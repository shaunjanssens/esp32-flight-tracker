#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace model {

constexpr size_t   kMaxAircraft = 64;      // nearest N are kept; the rest are dropped
constexpr size_t   kTrailPoints = 60;      // 60 x 5 s poll interval = 5 minutes
constexpr uint32_t kExpiryMs    = 30000;   // drop an aircraft unseen for this long
constexpr float    kTrailScale  = 64.0f;   // trail fixed point: 1/64 nm per unit

/**
 * One tracked aircraft, in a local flat-earth frame centred on the home
 * position: +x is east, +y is north, both in nautical miles.
 *
 * The feed already provides distance and bearing from the query point, so no
 * great-circle maths happens on the device.
 */
struct Aircraft {
    uint32_t icao;                 // 24-bit Mode-S address, the identity key
    char     flight[9];            // callsign, whitespace trimmed
    char     reg[9];               // registration
    char     type[5];              // ICAO type designator, e.g. "A320"

    float    x_nm, y_nm;           // position at last_update_ms
    float    vx, vy;               // nm per second, from ground speed + track
    float    dst_nm, dir_deg;      // as reported by the feed
    float    track_deg;
    int32_t  alt_ft;
    int16_t  gs_kt;
    int16_t  vs_fpm;

    bool     on_ground;
    bool     mlat;                 // position is multilaterated, not ADS-B
    bool     has_position;

    uint32_t last_update_ms;
    uint32_t first_seen_ms;

    int16_t  trail[kTrailPoints][2];
    uint8_t  trail_head;           // index one past the newest point
    uint8_t  trail_len;

    /** Dead-reckoned position at `now_ms`, extrapolated from the last fix. */
    void positionAt(uint32_t now_ms, float &x, float &y) const;

    /** Oldest-to-newest trail point `index`, in nm. */
    void trailPoint(size_t index, float &x, float &y) const;

    void pushTrail();
    void clearTrail();
};

/**
 * Fixed-size aircraft table in PSRAM, shared between the network task (writer)
 * and the UI task (reader). Every access must hold the lock.
 */
class Store {
public:
    bool begin();

    bool lock(int timeout_ms = -1);
    void unlock();

    /** Find an existing entry or claim a slot for `icao`. Null when full. */
    Aircraft *upsert(uint32_t icao, uint32_t now_ms);
    Aircraft *find(uint32_t icao);

    /** Drop everything unseen for longer than kExpiryMs. */
    size_t expire(uint32_t now_ms);
    void clear();

    /** Index of the aircraft closest to (x_nm, y_nm), or -1 beyond max_nm. */
    int nearest(uint32_t now_ms, float x_nm, float y_nm, float max_nm) const;

    size_t size() const { return count_; }
    const Aircraft &at(size_t index) const { return items_[index]; }
    Aircraft &at(size_t index) { return items_[index]; }

    /** millis() of the last successful feed update, 0 if never. */
    uint32_t lastFeedMs() const { return last_feed_ms_; }
    void setLastFeedMs(uint32_t ms) { last_feed_ms_ = ms; }

    /** Aircraft the feed reported before the kMaxAircraft cap was applied. */
    uint16_t reportedCount() const { return reported_count_; }
    void setReportedCount(uint16_t count) { reported_count_ = count; }

private:
    Aircraft *items_ = nullptr;
    size_t count_ = 0;
    uint32_t last_feed_ms_ = 0;
    uint16_t reported_count_ = 0;
    SemaphoreHandle_t mutex_ = nullptr;
};

Store &store();

/** RAII lock for the store. */
class StoreGuard {
public:
    explicit StoreGuard(int timeout_ms = -1) : held_(store().lock(timeout_ms)) {}
    ~StoreGuard() { if (held_) store().unlock(); }
    explicit operator bool() const { return held_; }
    StoreGuard(const StoreGuard &) = delete;
    StoreGuard &operator=(const StoreGuard &) = delete;
private:
    bool held_;
};

} // namespace model
