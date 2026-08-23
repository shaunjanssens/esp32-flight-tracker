#include "aircraft.h"

#include <esp_heap_caps.h>
#include <math.h>

namespace model {
namespace {

Store g_store;

constexpr float kMaxExtrapolationS = 20.0f;   // never dead-reckon further than this

} // namespace

Store &store()
{
    return g_store;
}

void Aircraft::positionAt(uint32_t now_ms, float &x, float &y) const
{
    float dt = (float)(int32_t)(now_ms - last_update_ms) / 1000.0f;
    if (dt < 0.0f) {
        dt = 0.0f;
    } else if (dt > kMaxExtrapolationS) {
        dt = kMaxExtrapolationS;
    }
    x = x_nm + vx * dt;
    y = y_nm + vy * dt;
}

void Aircraft::trailPoint(size_t index, float &x, float &y) const
{
    // trail_head points one past the newest sample, so the oldest sample sits
    // trail_len slots behind it.
    const size_t slot = (trail_head + kTrailPoints - trail_len + index) % kTrailPoints;
    x = (float)trail[slot][0] / kTrailScale;
    y = (float)trail[slot][1] / kTrailScale;
}

void Aircraft::pushTrail()
{
    const float limit = 32767.0f / kTrailScale;
    if (fabsf(x_nm) > limit || fabsf(y_nm) > limit) {
        return;
    }
    trail[trail_head][0] = (int16_t)lroundf(x_nm * kTrailScale);
    trail[trail_head][1] = (int16_t)lroundf(y_nm * kTrailScale);
    trail_head = (trail_head + 1) % kTrailPoints;
    if (trail_len < kTrailPoints) {
        trail_len++;
    }
}

void Aircraft::clearTrail()
{
    trail_head = 0;
    trail_len = 0;
}

bool Store::begin()
{
    if (items_ != nullptr) {
        return true;
    }

    items_ = (Aircraft *)heap_caps_calloc(kMaxAircraft, sizeof(Aircraft), MALLOC_CAP_SPIRAM);
    if (items_ == nullptr) {
        // Fall back to internal RAM: ~20 kB, tight but survivable.
        items_ = (Aircraft *)calloc(kMaxAircraft, sizeof(Aircraft));
    }
    mutex_ = xSemaphoreCreateRecursiveMutex();
    return items_ != nullptr && mutex_ != nullptr;
}

bool Store::lock(int timeout_ms)
{
    if (mutex_ == nullptr) {
        return false;
    }
    const TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(mutex_, ticks) == pdTRUE;
}

void Store::unlock()
{
    if (mutex_ != nullptr) {
        xSemaphoreGiveRecursive(mutex_);
    }
}

Aircraft *Store::find(uint32_t icao)
{
    for (size_t i = 0; i < count_; i++) {
        if (items_[i].icao == icao) {
            return &items_[i];
        }
    }
    return nullptr;
}

Aircraft *Store::upsert(uint32_t icao, uint32_t now_ms)
{
    if (items_ == nullptr) {
        return nullptr;
    }
    if (Aircraft *existing = find(icao)) {
        return existing;
    }
    if (count_ >= kMaxAircraft) {
        return nullptr;
    }

    Aircraft &slot = items_[count_++];
    memset(&slot, 0, sizeof(slot));
    slot.icao = icao;
    slot.first_seen_ms = now_ms;
    slot.last_update_ms = now_ms;
    return &slot;
}

size_t Store::expire(uint32_t now_ms)
{
    size_t removed = 0;
    for (size_t i = 0; i < count_;) {
        if ((uint32_t)(now_ms - items_[i].last_update_ms) > kExpiryMs) {
            items_[i] = items_[count_ - 1];
            count_--;
            removed++;
        } else {
            i++;
        }
    }
    return removed;
}

void Store::clear()
{
    count_ = 0;
    reported_count_ = 0;
}

int Store::nearest(uint32_t now_ms, float x_nm, float y_nm, float max_nm) const
{
    int best = -1;
    float best_distance = max_nm;

    for (size_t i = 0; i < count_; i++) {
        if (!items_[i].has_position) {
            continue;
        }
        float x, y;
        items_[i].positionAt(now_ms, x, y);
        const float distance = sqrtf((x - x_nm) * (x - x_nm) + (y - y_nm) * (y - y_nm));
        if (distance < best_distance) {
            best_distance = distance;
            best = (int)i;
        }
    }
    return best;
}

} // namespace model
