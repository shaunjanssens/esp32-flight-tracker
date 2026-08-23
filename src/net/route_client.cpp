#include "route_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

namespace net {
namespace {

// adsbdb redirects HTTP to HTTPS, so these lookups pay the TLS cost. They are
// only made when an aircraft is tapped, one at a time, and cached — including
// the misses, which are common for GA traffic with no filed route.
constexpr char kCallsignUrl[] = "https://api.adsbdb.com/v0/callsign/%s";
constexpr char kAircraftUrl[] = "https://api.adsbdb.com/v0/aircraft/%06lx";

constexpr size_t kCacheSize = 24;
constexpr uint32_t kRetryAfterMs = 60000;
constexpr uint32_t kHttpTimeoutMs = 8000;
constexpr int kTaskCore = 0;
constexpr int kTaskPriority = 2;
constexpr int kTaskStack = 10 * 1024;

const char kUserAgent[] =
    "esp32-flight-tracker/0.1 (+https://github.com/shaunjanssens/esp32-flight-tracker)";

struct CacheEntry {
    RouteInfo info;
    uint32_t last_used_ms = 0;
    uint32_t last_attempt_ms = 0;
    bool used = false;
};

CacheEntry *g_cache = nullptr;
SemaphoreHandle_t g_mutex = nullptr;
QueueHandle_t g_queue = nullptr;

struct Request {
    uint32_t icao;
    char callsign[9];
};

CacheEntry *findEntry(uint32_t icao)
{
    for (size_t i = 0; i < kCacheSize; i++) {
        if (g_cache[i].used && g_cache[i].info.icao == icao) {
            return &g_cache[i];
        }
    }
    return nullptr;
}

CacheEntry *claimEntry(uint32_t icao)
{
    CacheEntry *victim = nullptr;
    for (size_t i = 0; i < kCacheSize; i++) {
        if (!g_cache[i].used) {
            victim = &g_cache[i];
            break;
        }
        if (victim == nullptr || g_cache[i].last_used_ms < victim->last_used_ms) {
            victim = &g_cache[i];
        }
    }
    *victim = CacheEntry{};
    victim->used = true;
    victim->info.icao = icao;
    return victim;
}

void copyField(char *dest, size_t size, const char *source)
{
    if (source == nullptr) {
        dest[0] = '\0';
        return;
    }
    strlcpy(dest, source, size);
}

bool fetchJson(const char *url, JsonDocument &doc, const JsonDocument &filter)
{
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(kHttpTimeoutMs / 1000);

    HTTPClient http;
    http.setUserAgent(kUserAgent);
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    http.useHTTP10(true);
    if (!http.begin(client, url)) {
        return false;
    }

    const int status = http.GET();
    if (status != HTTP_CODE_OK) {
        // 404 is the normal answer for an unknown callsign or airframe.
        http.end();
        return false;
    }

    const DeserializationError error =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    return !error;
}

void lookupRoute(RouteInfo &info)
{
    if (info.callsign[0] == '\0') {
        return;
    }

    char url[80];
    snprintf(url, sizeof(url), kCallsignUrl, info.callsign);

    JsonDocument filter;
    JsonObject route = filter["response"]["flightroute"].to<JsonObject>();
    route["airline"]["name"] = true;
    for (const char *end : {"origin", "destination"}) {
        JsonObject airport = route[end].to<JsonObject>();
        airport["iata_code"] = true;
        airport["municipality"] = true;
        airport["name"] = true;
    }

    JsonDocument doc;
    if (!fetchJson(url, doc, filter)) {
        return;
    }

    JsonObjectConst flightroute = doc["response"]["flightroute"];
    if (flightroute.isNull()) {
        return;
    }
    copyField(info.airline, sizeof(info.airline), flightroute["airline"]["name"] | "");
    copyField(info.origin_iata, sizeof(info.origin_iata), flightroute["origin"]["iata_code"] | "");
    copyField(info.origin_city, sizeof(info.origin_city),
              flightroute["origin"]["municipality"] | (flightroute["origin"]["name"] | ""));
    copyField(info.dest_iata, sizeof(info.dest_iata), flightroute["destination"]["iata_code"] | "");
    copyField(info.dest_city, sizeof(info.dest_city),
              flightroute["destination"]["municipality"] | (flightroute["destination"]["name"] | ""));
    info.route_known = info.origin_iata[0] != '\0' || info.dest_iata[0] != '\0';
}

void lookupAircraft(RouteInfo &info)
{
    char url[80];
    snprintf(url, sizeof(url), kAircraftUrl, (unsigned long)info.icao);

    JsonDocument filter;
    JsonObject aircraft = filter["response"]["aircraft"].to<JsonObject>();
    aircraft["type"] = true;
    aircraft["manufacturer"] = true;
    aircraft["registration"] = true;
    aircraft["registered_owner"] = true;

    JsonDocument doc;
    if (!fetchJson(url, doc, filter)) {
        return;
    }

    JsonObjectConst record = doc["response"]["aircraft"];
    if (record.isNull()) {
        return;
    }

    const char *manufacturer = record["manufacturer"] | "";
    const char *type = record["type"] | "";
    if (manufacturer[0] != '\0' && type[0] != '\0') {
        snprintf(info.type_name, sizeof(info.type_name), "%s %s", manufacturer, type);
    } else {
        copyField(info.type_name, sizeof(info.type_name), type[0] != '\0' ? type : manufacturer);
    }
    copyField(info.owner, sizeof(info.owner), record["registered_owner"] | "");
    copyField(info.registration, sizeof(info.registration), record["registration"] | "");
    info.aircraft_known = info.type_name[0] != '\0' || info.owner[0] != '\0';
}

void routeTask(void *)
{
    Request request;
    while (true) {
        if (xQueueReceive(g_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (WiFi.status() != WL_CONNECTED) {
            continue;
        }

        // Work on a copy, then publish: the UI reads the cache continuously.
        RouteInfo scratch;
        scratch.icao = request.icao;
        strlcpy(scratch.callsign, request.callsign, sizeof(scratch.callsign));

        lookupAircraft(scratch);
        lookupRoute(scratch);
        scratch.failed = !scratch.route_known && !scratch.aircraft_known;
        scratch.pending = false;

        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            CacheEntry *entry = findEntry(request.icao);
            if (entry == nullptr) {
                entry = claimEntry(request.icao);
            }
            entry->info = scratch;
            entry->last_used_ms = millis();
            entry->last_attempt_ms = millis();
            xSemaphoreGive(g_mutex);
        }

        Serial.printf("[route] %s: %s%s%s\n", request.callsign,
                      scratch.route_known ? "route " : "",
                      scratch.aircraft_known ? "aircraft " : "",
                      scratch.failed ? "nothing found" : "");
    }
}

} // namespace

bool routeStart()
{
    if (g_cache != nullptr) {
        return true;
    }
    g_cache = (CacheEntry *)heap_caps_calloc(kCacheSize, sizeof(CacheEntry), MALLOC_CAP_SPIRAM);
    if (g_cache == nullptr) {
        g_cache = (CacheEntry *)calloc(kCacheSize, sizeof(CacheEntry));
    }
    g_mutex = xSemaphoreCreateMutex();
    g_queue = xQueueCreate(4, sizeof(Request));
    if (g_cache == nullptr || g_mutex == nullptr || g_queue == nullptr) {
        return false;
    }
    return xTaskCreatePinnedToCore(routeTask, "route", kTaskStack, nullptr,
                                   kTaskPriority, nullptr, kTaskCore) == pdPASS;
}

const RouteInfo *routeLookup(uint32_t icao, const char *callsign)
{
    if (g_cache == nullptr || icao == 0) {
        return nullptr;
    }
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return nullptr;
    }

    CacheEntry *entry = findEntry(icao);
    const uint32_t now = millis();
    bool queue_request = false;

    if (entry == nullptr) {
        entry = claimEntry(icao);
        strlcpy(entry->info.callsign, callsign != nullptr ? callsign : "",
                sizeof(entry->info.callsign));
        entry->info.pending = true;
        entry->last_attempt_ms = now;
        queue_request = true;
    } else if (entry->info.failed && (now - entry->last_attempt_ms) > kRetryAfterMs) {
        entry->info.failed = false;
        entry->info.pending = true;
        entry->last_attempt_ms = now;
        queue_request = true;
    }
    entry->last_used_ms = now;

    RouteInfo *result = &entry->info;
    Request request{icao, ""};
    strlcpy(request.callsign, result->callsign, sizeof(request.callsign));
    xSemaphoreGive(g_mutex);

    if (queue_request) {
        xQueueSend(g_queue, &request, 0);
    }
    return result;
}

} // namespace net
