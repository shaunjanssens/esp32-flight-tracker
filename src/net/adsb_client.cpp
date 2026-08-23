#include "adsb_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <math.h>

#include "app/config.h"
#include "model/aircraft.h"

namespace net {
namespace {

constexpr uint32_t kPollIntervalMs = 5000;
constexpr uint32_t kMinIntervalMs  = 4000;    // never hammer the feed harder than this
constexpr uint32_t kBackoffMaxMs   = 120000;
constexpr uint32_t kHttpTimeoutMs  = 15000;
constexpr size_t   kMaxRecords     = 512;     // dst scratch, bigger than any 100 nm response
constexpr int      kTaskCore       = 0;
constexpr int      kTaskPriority   = 3;
constexpr int      kTaskStack      = 10 * 1024;

const char kUserAgent[] =
    "esp32-flight-tracker/0.1 (+https://github.com/shaunjanssens/esp32-flight-tracker)";

FeedStats g_stats;
portMUX_TYPE g_stats_mux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t g_task = nullptr;
float *g_distances = nullptr;

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
constexpr uint32_t kFailoverAfter = 3;

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
    Serial.printf("[adsb] switching to %s after %u failures\n",
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

bool fetchOnce()
{
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

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
    http.setConnectTimeout(kHttpTimeoutMs);
    http.useHTTP10(true);              // stream the body instead of chunk-decoding it
    if (!http.begin(client, url)) {
        Serial.printf("[adsb] %s: connection setup failed\n", provider.name);
        portENTER_CRITICAL(&g_stats_mux);
        g_stats.failure_count++;
        portEXIT_CRITICAL(&g_stats_mux);
        return false;
    }

    const uint32_t started_ms = millis();
    const int status = http.GET();
    if (status != HTTP_CODE_OK) {
        Serial.printf("[adsb] %s: HTTP %d (%s)\n", provider.name, status,
                      HTTPClient::errorToString(status).c_str());
        http.end();
        portENTER_CRITICAL(&g_stats_mux);
        g_stats.last_http_code = status;
        g_stats.failure_count++;
        portEXIT_CRITICAL(&g_stats_mux);
        return false;
    }

    const int content_length = http.getSize();

    JsonDocument filter;
    buildFilter(filter);

    JsonDocument doc(&g_allocator);
    const DeserializationError error =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();

    if (error) {
        Serial.printf("[adsb] parse failed: %s\n", error.c_str());
        portENTER_CRITICAL(&g_stats_mux);
        g_stats.failure_count++;
        portEXIT_CRITICAL(&g_stats_mux);
        return false;
    }

    JsonArrayConst array = aircraftArray(doc);
    if (array.isNull()) {
        Serial.printf("[adsb] %s: response has no aircraft array\n", provider.name);
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
    g_stats.last_bytes = content_length > 0 ? (uint32_t)content_length : 0;
    g_stats.last_duration_ms = now_ms - started_ms;
    portEXIT_CRITICAL(&g_stats_mux);

    Serial.printf("[adsb] %s: %u/%u aircraft, %d bytes, %ums\n",
                  provider.name, (unsigned)accepted, (unsigned)total, content_length,
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

        const bool ok = app::settings().position_set && fetchOnce();
        if (ok) {
            g_consecutive_failures = 0;
        } else {
            g_consecutive_failures++;
            considerFailover();
        }
        backoff_ms = ok ? 0 : (backoff_ms == 0 ? kPollIntervalMs
                                               : min(backoff_ms * 2, kBackoffMaxMs));

        const uint32_t elapsed = millis() - started;
        uint32_t wait_ms = kPollIntervalMs + backoff_ms;
        wait_ms = (wait_ms > elapsed) ? wait_ms - elapsed : kMinIntervalMs;

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
    if (g_distances == nullptr) {
        g_distances = (float *)heap_caps_malloc(kMaxRecords * sizeof(float), MALLOC_CAP_SPIRAM);
        if (g_distances == nullptr) {
            g_distances = (float *)malloc(kMaxRecords * sizeof(float));
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
