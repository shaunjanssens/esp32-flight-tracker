#pragma once

#include <Arduino.h>

namespace net {

/** What adsbdb knows about one aircraft. Filled in asynchronously. */
struct RouteInfo {
    uint32_t icao = 0;
    char callsign[9] = "";

    bool pending = false;      // a lookup is queued or in flight
    bool route_known = false;
    bool aircraft_known = false;
    bool failed = false;       // network error; worth retrying later

    char airline[28] = "";
    char origin_iata[5] = "";
    char origin_city[26] = "";
    char dest_iata[5] = "";
    char dest_city[26] = "";

    char type_name[34] = "";   // "Airbus A320-214"
    char owner[28] = "";
    char registration[9] = "";
};

/** Start the lookup task. */
bool routeStart();

/**
 * Cached entry for `icao`, queueing a lookup on first ask. Never blocks; the
 * returned pointer stays valid until the entry is evicted, so copy anything
 * you need to keep. Null only if the cache could not be allocated.
 */
const RouteInfo *routeLookup(uint32_t icao, const char *callsign);

} // namespace net
