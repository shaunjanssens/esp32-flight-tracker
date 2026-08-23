#include "adsb_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <math.h>

#include "app/config.h"
#include "app/log.h"
#include "model/aircraft.h"

namespace net {
namespace {

constexpr uint32_t kPollIntervalMs = 5000;
constexpr uint32_t kMinGapMs       = 2500;    // hard floor between requests, whatever asks
constexpr uint32_t kRateLimitMs    = 60000;   // stand down this long after an HTTP 429
constexpr uint32_t kBackoffMaxMs   = 120000;
constexpr uint32_t kHttpTimeoutMs  = 20000;   // whole-response budget
constexpr uint32_t kConnectTimeoutMs = 5000;  // a healthy node answers in well under a second
constexpr size_t   kMaxRecords     = 512;     // dst scratch, bigger than any 100 nm response
constexpr size_t   kBodyCapacity   = 192 * 1024;  // 100 nm returns ~150 kB
constexpr uint32_t kStallTimeoutMs = 6000;
constexpr int      kTaskCore       = 0;
constexpr int      kTaskPriority   = 3;
constexpr int      kTaskStack      = 10 * 1024;

const char kUserAgent[] =
    "esp32-flight-tracker/0.1 (+https://github.com/shaunjanssens/esp32-flight-tracker)";

FeedStats g_stats;
uint32_t g_last_request_ms = 0;
uint32_t g_rate_limited_until_ms = 0;
portMUX_TYPE g_stats_mux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t g_task = nullptr;
float *g_distances = nullptr;
char  *g_body = nullptr;

/** ArduinoJson allocator that keeps the parsed document out of internal RAM. */
struct PsramAllocator : ArduinoJson::Allocator {
    void *allocate(size_t size) override
    {
        void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        return ptr != nullptr ? ptr : malloc(size);
    }
    void deallocate(void *pointer) override
    {
        heap_caps_free(pointer);
    }
    void *reallocate(void *pointer, size_t size) override
    {
        void *ptr = heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM);
        return ptr != nullptr ? ptr : realloc(pointer, size);
    }
};

PsramAllocator g_allocator;

struct ProviderInfo {
    const char *name;
    const char *url_format;
    bool tls;
};

/**
 * adsb.lol is reachable over plain HTTP, which matters more than it looks: a
 * TLS session needs ~40 kB of *contiguous* internal RAM, and after Wi-Fi and
 * the RGB panel have taken their share the largest free block hovers around
 * that figure. Aircraft positions are public data being read, not sent, so the
 * exposure is a bad actor on the path feeding fake aircraft to a desk toy.
 * adsb.fi redirects HTTP to HTTPS, so the fallback pays the TLS cost.
 */
const ProviderInfo kProviders[] = {
    {"adsb.lol", "http://api.adsb.lol/v2/lat/%.5f/lon/%.5f/dist/%u", false},
    {"adsb.fi",  "https://opendata.adsb.fi/api/v2/lat/%.5f/lon/%.5f/dist/%u", true},
};
constexpr size_t kProviderCount = sizeof(kProviders) / sizeof(kProviders[0]);

// Which provider we are actually using; may differ from the configured one
// after a failover.
size_t g_active_provider = 0;
uint32_t g_consecutive_failures = 0;
constexpr uint32_t kFailoverAfter = 2;

void buildUrl(char *out, size_t out_size)
{
    const app::Settings &settings = app::settings();
    snprintf(out, out_size, kProviders[g_active_provider].url_format,
             settings.home_lat, settings.home_lon, (unsigned)settings.radius_nm);
}

/** Move to the other provider after repeated failures. */
void considerFailover()
{
    if (g_consecutive_failures < kFailoverAfter) {
        return;
    }
    g_active_provider = (g_active_provider + 1) % kProviderCount;
    g_consecutive_failures = 0;
    app::logf("[adsb] switching to %s after %u consecutive failures",
              kProviders[g_active_provider].name, (unsigned)kFailoverAfter);
}

void configureTls(WiFiClientSecure &client)
{
    // TODO(tls): swap for setCACertBundle() once the installed core version is
    // pinned. Certificate rotation on a device with no console is worse than the
    // risk this carries on a home network.
    client.setInsecure();
    client.setTimeout(kHttpTimeoutMs / 1000);
}

void copyTrimmed(char *dest, size_t dest_size, const char *source)
{
    if (source == nullptr) {
        dest[0] = '\0';
        return;
    }
    while (*source == ' ') {
        source++;
    }
    size_t length = strlen(source);
    while (length > 0 && (source[length - 1] == ' ' || source[length - 1] == '\t')) {
        length--;
    }
    if (length >= dest_size) {
        length = dest_size - 1;
    }
    memcpy(dest, source, length);
    dest[length] = '\0';
}

/** The aircraft array is `ac` on adsb.lol and `aircraft` on adsb.fi. */
JsonArrayConst aircraftArray(JsonDocument &doc)
{
    JsonArrayConst array = doc["ac"].as<JsonArrayConst>();
    if (array.isNull()) {
        array = doc["aircraft"].as<JsonArrayConst>();
    }
    return array;
}

void buildFilter(JsonDocument &filter)
{
    static const char *kFields[] = {
        "hex", "flight", "r", "t", "alt_baro", "gs", "track", "baro_rate",
        "geom_rate", "lat", "lon", "dst", "dir", "mlat", "desc"
    };
    for (const char *root : {"ac", "aircraft"}) {
        JsonObject element = filter[root].add<JsonObject>();
        for (const char *field : kFields) {
            element[field] = true;
        }
    }
}

/** kMaxAircraft-th smallest distance, so only the nearest are stored. */
float distanceCutoff(JsonArrayConst array, uint16_t &total)
{
    size_t count = 0;
    total = 0;
    for (JsonObjectConst record : array) {
        total++;
        if (count >= kMaxRecords) {
            continue;
        }
        if (!record["dst"].is<float>()) {
            continue;
        }
        g_distances[count++] = record["dst"].as<float>();
    }
    if (count <= model::kMaxAircraft) {
        return INFINITY;
    }

    // Partial selection: repeatedly pull the smallest to position k.
    for (size_t i = 0; i <= model::kMaxAircraft && i < count; i++) {
        size_t smallest = i;
        for (size_t j = i + 1; j < count; j++) {
            if (g_distances[j] < g_distances[smallest]) {
                smallest = j;
            }
        }
        const float swap = g_distances[i];
        g_distances[i] = g_distances[smallest];
        g_distances[smallest] = swap;
    }
    return g_distances[model::kMaxAircraft - 1];
}

void applyRecord(model::Aircraft &aircraft, JsonObjectConst record, uint32_t now_ms)
{
    copyTrimmed(aircraft.flight, sizeof(aircraft.flight), record["flight"] | "");
    copyTrimmed(aircraft.reg, sizeof(aircraft.reg), record["r"] | "");
    copyTrimmed(aircraft.type, sizeof(aircraft.type), record["t"] | "");

    // alt_baro is "ground" for aircraft on the surface, a number otherwise.
    aircraft.on_ground = record["alt_baro"].is<const char *>();
    aircraft.alt_ft = aircraft.on_ground ? 0 : (record["alt_baro"] | 0);
    aircraft.gs_kt = (int16_t)(record["gs"] | 0.0f);
    aircraft.vs_fpm = (int16_t)(record["baro_rate"] | (record["geom_rate"] | 0));
    aircraft.track_deg = record["track"] | 0.0f;
    aircraft.mlat = record["mlat"].is<JsonArrayConst>() && record["mlat"].size() > 0;

    const float dst = record["dst"] | -1.0f;
    const float dir = record["dir"] | 0.0f;
    if (dst >= 0.0f) {
        const float radians = dir * (float)DEG_TO_RAD;
        aircraft.dst_nm = dst;
        aircraft.dir_deg = dir;
        aircraft.x_nm = dst * sinf(radians);     // east
        aircraft.y_nm = dst * cosf(radians);     // north
        aircraft.has_position = true;

        const float track_radians = aircraft.track_deg * (float)DEG_TO_RAD;
        const float speed_nm_s = (float)aircraft.gs_kt / 3600.0f;
        aircraft.vx = speed_nm_s * sinf(track_radians);
        aircraft.vy = speed_nm_s * cosf(track_radians);
        aircraft.pushTrail();
    }

    aircraft.last_update_ms = now_ms;
}

/**
 * These feeds are volunteer-funded and shared. A poll every 5 s is fine; a
 * burst is not, and the range gesture can ask for a refresh on every step.
 * This is the one place that decides whether a request actually goes out.
 */
bool requestAllowed()
{
    const uint32_t now = millis();
    if (g_rate_limited_until_ms != 0 && (int32_t)(now - g_rate_limited_until_ms) < 0) {
        return false;
    }
    if (g_last_request_ms != 0 && (now - g_last_request_ms) < kMinGapMs) {
        return false;
    }
    return true;
}

/** Big radii mean big responses; ask for them less often. */
uint32_t pollIntervalMs()
{
    const uint16_t radius = app::settings().radius_nm;
    if (radius >= 100) {
        return 20000;      // ~150 kB a go: four times a minute is plenty
    }
    if (radius >= 50) {
        return 10000;
    }
    return kPollIntervalMs;
}

bool fetchOnce()
{
    if (WiFi.status() != WL_CONNECTED || !requestAllowed()) {
        return false;
    }
    g_last_request_ms = millis();

    char url[160];
    buildUrl(url, sizeof(url));

    const ProviderInfo &provider = kProviders[g_active_provider];
    WiFiClient plain_client;
    WiFiClientSecure tls_client;
    if (provider.tls) {
        configureTls(tls_client);
    }
    WiFiClient &client = provider.tls ? static_cast<WiFiClient &>(tls_client) : plain_client;

    HTTPClient http;
    http.setUserAgent(kUserAgent);
    http.setTimeout(kHttpTimeoutMs);
    // Short: adsb.lol publishes five A records and some of them refuse
    // connections. Discovering that must be quick, because the cost is paid
    // before every failover.
    http.setConnectTimeout(kConnectTimeoutMs);
    http.useHTTP10(true);              // stream the body instead of chunk-decoding it
    if (!http.begin(client, url)) {
        app::logf("[adsb] %s: connection setup failed", provider.name);
        portENTER_CRITICAL(&g_stats_mux);
        g_stats.failure_count++;
        portEXIT_CRITICAL(&g_stats_mux);
        return false;
    }

    const uint32_t started_ms = millis();
    const int status = http.GET();
    if (status == HTTP_CODE_TOO_MANY_REQUESTS) {
        g_rate_limited_until_ms = millis() + kRateLimitMs;
        app::logf("[adsb] %s: rate limited (429), standing down %us",
                  provider.name, (unsigned)(kRateLimitMs / 1000));
        http.end();
        portENTER_CRITICAL(&g_stats_mux);
        g_stats.last_http_code = status;
        g_stats.failure_count++;
        portEXIT_CRITICAL(&g_stats_mux);
        return false;
    }

    if (status != HTTP_CODE_OK) {
        // -1 covers everything from a refused connection to a failed DNS
        // lookup, so resolve the host separately to tell them apart.
        IPAddress resolved;
        const bool dns_ok = WiFi.hostByName(provider.tls ? "opendata.adsb.fi" : "api.adsb.lol",
                                            resolved);
        app::logf("[adsb] %s: HTTP %d (%s) dns=%s gw=%s rssi=%d", provider.name, status,
                  HTTPClient::errorToString(status).c_str(),
                  dns_ok ? resolved.toString().c_str() : "FAILED",
                  WiFi.gatewayIP().toString().c_str(), (int)WiFi.RSSI());
        http.end();
        portENTER_CRITICAL(&g_stats_mux);
        g_stats.last_http_code = status;
        g_stats.failure_count++;
        portEXIT_CRITICAL(&g_stats_mux);
        return false;
    }

    const int content_length = http.getSize();

    // Read the whole body into PSRAM before parsing.
    //
    // Parsing straight from the socket is tempting and was the first cut, but
    // ArduinoJson treats a read that returns nothing as end-of-input: one pause
    // longer than the stream timeout - routine on TLS over marginal Wi-Fi -
    // ends the document early and the parse fails with IncompleteInput. Here a
    // stall is just a stall, and only a real close or a long silence ends it.
    WiFiClient *stream = http.getStreamPtr();
    size_t received = 0;
    uint32_t last_data_ms = millis();
    while (received + 1 < kBodyCapacity) {
        const size_t available = stream->available();
        if (available > 0) {
            const size_t room = kBodyCapacity - received - 1;
            const int read = stream->readBytes(g_body + received,
                                               available < room ? available : room);
            if (read > 0) {
                received += (size_t)read;
                last_data_ms = millis();
            }
        } else {
            if (content_length > 0 && received >= (size_t)content_length) {
                break;                       // whole body in hand
            }
            if (!http.connected() && stream->available() == 0) {
                break;                       // server closed, HTTP/1.0 style
            }
            if (millis() - last_data_ms > kStallTimeoutMs) {
                app::logf("[adsb] %s: stalled after %u of %d bytes",
                          provider.name, (unsigned)received, content_length);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    g_body[received] = '\0';
    http.end();

    JsonDocument filter;
    buildFilter(filter);

    JsonDocument doc(&g_allocator);
    const DeserializationError error =
        deserializeJson(doc, g_body, received, DeserializationOption::Filter(filter));

    if (error) {
        // IncompleteInput means the body stopped arriving mid-JSON, which on a
        // marginal link happens to a 14 kB response often enough to matter.
        // Report it distinctly from malformed JSON so the log stays honest.
        app::logf("[adsb] %s: parse failed after %u of %d bytes: %s",
                  provider.name, (unsigned)received, content_length, error.c_str());
        portENTER_CRITICAL(&g_stats_mux);
        g_stats.failure_count++;
        portEXIT_CRITICAL(&g_stats_mux);
        return false;
    }

    JsonArrayConst array = aircraftArray(doc);
    if (array.isNull()) {
        app::logf("[adsb] %s: response has no aircraft array", provider.name);
        portENTER_CRITICAL(&g_stats_mux);
        g_stats.failure_count++;
        portEXIT_CRITICAL(&g_stats_mux);
        return false;
    }

    uint16_t total = 0;
    const float cutoff = distanceCutoff(array, total);
    const uint32_t now_ms = millis();
    uint16_t accepted = 0;

    {
        model::StoreGuard guard;
        model::Store &store = model::store();
        for (JsonObjectConst record : array) {
            const char *hex = record["hex"] | "";
            if (hex[0] == '\0') {
                continue;
            }
            const float dst = record["dst"] | -1.0f;
            if (dst < 0.0f || dst > cutoff) {
                continue;
            }
            const uint32_t icao = strtoul(hex, nullptr, 16);
            model::Aircraft *aircraft = store.upsert(icao, now_ms);
            if (aircraft == nullptr) {
                continue;      // table full
            }
            applyRecord(*aircraft, record, now_ms);
            accepted++;
        }
        store.expire(now_ms);
        store.setLastFeedMs(now_ms);
        store.setReportedCount(total);
    }

    portENTER_CRITICAL(&g_stats_mux);
    g_stats.last_success_ms = now_ms;
    g_stats.success_count++;
    g_stats.reported_total = total;
    g_stats.accepted = accepted;
    g_stats.last_http_code = status;
    g_stats.last_bytes = (uint32_t)received;
    g_stats.last_duration_ms = now_ms - started_ms;
    portEXIT_CRITICAL(&g_stats_mux);

    app::logf("[adsb] %s: %u/%u aircraft, %u bytes, %ums",
              provider.name, (unsigned)accepted, (unsigned)total, (unsigned)received,
              (unsigned)(now_ms - started_ms));
    return true;
}

void adsbTask(void *)
{
    uint32_t backoff_ms = 0;

    while (true) {
        const uint32_t started = millis();
        portENTER_CRITICAL(&g_stats_mux);
        g_stats.last_attempt_ms = started;
        portEXIT_CRITICAL(&g_stats_mux);

        // A settings change resets the failover choice.
        const size_t configured = (size_t)app::settings().provider;
        static size_t last_configured = SIZE_MAX;
        if (configured != last_configured) {
            last_configured = configured;
            g_active_provider = configured % kProviderCount;
            g_consecutive_failures = 0;
        }

        bool ok = app::settings().position_set && fetchOnce();
        if (!ok && app::settings().position_set && g_rate_limited_until_ms == 0) {
            // A truncated body or a dropped connection is usually transient;
            // one prompt retry beats waiting out the whole poll interval. Never
            // retry into a rate limit, though - that is how you earn a longer one.
            vTaskDelay(pdMS_TO_TICKS(kMinGapMs));
            ok = fetchOnce();
        }
        if (ok) {
            g_consecutive_failures = 0;
        } else {
            g_consecutive_failures++;
            considerFailover();
        }
        backoff_ms = ok ? 0 : (backoff_ms == 0 ? kPollIntervalMs
                                               : min(backoff_ms * 2, kBackoffMaxMs));

        const uint32_t elapsed = millis() - started;
        uint32_t wait_ms = pollIntervalMs() + backoff_ms;
        if (g_rate_limited_until_ms != 0 && (int32_t)(millis() - g_rate_limited_until_ms) < 0) {
            wait_ms = g_rate_limited_until_ms - millis();
        }
        wait_ms = (wait_ms > elapsed) ? wait_ms - elapsed : kMinGapMs;

        // A refresh request (radius change) cuts the wait short.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

} // namespace

bool adsbStart()
{
    if (g_task != nullptr) {
        return true;
    }
    if (g_body == nullptr) {
        g_body = (char *)heap_caps_malloc(kBodyCapacity, MALLOC_CAP_SPIRAM);
        if (g_body == nullptr) {
            return false;
        }
    }
    if (g_distances == nullptr) {
        g_distances = (float *)heap_caps_malloc(kMaxRecords * sizeof(float), MALLOC_CAP_SPIRAM);
        if (g_body == nullptr) {
        g_body = (char *)heap_caps_malloc(kBodyCapacity, MALLOC_CAP_SPIRAM);
        if (g_body == nullptr) {
            return false;
        }
    }
    if (g_distances == nullptr) {
            g_distances = (float *)malloc(kMaxRecords * sizeof(float));
        }
        if (g_body == nullptr) {
        g_body = (char *)heap_caps_malloc(kBodyCapacity, MALLOC_CAP_SPIRAM);
        if (g_body == nullptr) {
            return false;
        }
    }
    if (g_distances == nullptr) {
            return false;
        }
    }
    return xTaskCreatePinnedToCore(adsbTask, "adsb", kTaskStack, nullptr,
                                   kTaskPriority, &g_task, kTaskCore) == pdPASS;
}

void adsbRefreshNow()
{
    if (g_task != nullptr) {
        xTaskNotifyGive(g_task);
    }
}

FeedStats adsbStats()
{
    portENTER_CRITICAL(&g_stats_mux);
    FeedStats copy = g_stats;
    portEXIT_CRITICAL(&g_stats_mux);
    return copy;
}

} // namespace net
