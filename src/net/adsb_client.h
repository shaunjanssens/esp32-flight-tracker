#pragma once

#include <Arduino.h>

namespace net {

struct FeedStats {
    uint32_t last_success_ms = 0;   // millis() of the last good response
    uint32_t last_attempt_ms = 0;
    uint32_t success_count = 0;
    uint32_t failure_count = 0;
    uint16_t reported_total = 0;    // aircraft the feed reported, before capping
    uint16_t accepted = 0;          // aircraft actually stored
    int      last_http_code = 0;
    uint32_t last_bytes = 0;
    uint32_t last_duration_ms = 0;
};

/** Start the polling task on core 0. Safe to call once Wi-Fi has been set up. */
bool adsbStart();

/** Ask the poller to fetch immediately (after a radius or position change). */
void adsbRefreshNow();

FeedStats adsbStats();

} // namespace net
