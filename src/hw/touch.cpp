#include "hw/touch.h"

#include <Wire.h>

#include "app/log.h"
#include "hw/display.h"

namespace hw {
namespace {

bool g_ready = false;
uint32_t g_failures = 0;
uint32_t g_rejected = 0;

bool writeRegister(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(kTouchAddress);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool readRegisters(uint8_t reg, uint8_t *data, size_t length)
{
    Wire.beginTransmission(kTouchAddress);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom(kTouchAddress, (uint8_t)length) != length) {
        while (Wire.available()) {
            Wire.read();
        }
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        data[i] = (uint8_t)Wire.read();
    }
    return true;
}

} // namespace

bool touchInit()
{
    pinMode((uint8_t)kTouchInt, INPUT_PULLUP);

    expanderSet(kExpanderTouchReset, false);
    delay(10);
    expanderSet(kExpanderTouchReset, true);
    delay(50);

    // Auto-sleep makes the controller stop answering after a quiet spell,
    // which reads exactly like a dead chip.
    g_ready = writeRegister(kTouchSleepReg, 0xFF);
    app::logf("[touch] CST820 %s", g_ready ? "ready" : "not responding");
    return g_ready;
}

bool touchReady()
{
    return g_ready;
}

uint32_t touchRejectedSamples()
{
    return g_rejected;
}

bool touchRead(TouchState &state)
{
    state.down = false;
    if (!g_ready) {
        return false;
    }

    uint8_t buffer[6] = {};
    if (!readRegisters(kTouchReadReg, buffer, sizeof(buffer))) {
        if (++g_failures == 20) {
            app::logf("[touch] controller stopped answering");
        }
        return false;
    }
    g_failures = 0;

    // The finger-count register is 0 or 1 on this controller; anything else is
    // a corrupt read, not a touch.
    if (buffer[1] == 0 || buffer[1] > 1) {
        return true;
    }

    const int16_t x = (int16_t)(((buffer[2] & 0x0F) << 8) | buffer[3]);
    const int16_t y = (int16_t)(((buffer[4] & 0x0F) << 8) | buffer[5]);

    /*
     * Reject bad samples rather than clamping them.
     *
     * The CST820 periodically returns out-of-range coordinates - 0x0FFF and
     * friends. Clamping turned those into a plausible tap at 479,479, which is
     * the bottom-right corner: on a round panel there is no glass there, and
     * every phantom tap landed on empty space and cleared the user's selection.
     * Two guards: the reported point must be on the panel at all, and it must
     * be inside the circle that physically exists.
     */
    if (x < 0 || x >= kScreenWidth || y < 0 || y >= kScreenHeight) {
        g_rejected++;
        return true;
    }

    constexpr int32_t kRadius = kScreenWidth / 2;
    const int32_t dx = x - kRadius;
    const int32_t dy = y - kRadius;
    if (dx * dx + dy * dy > kRadius * kRadius) {
        g_rejected++;
        return true;
    }

    state.down = true;
    state.x = x;
    state.y = y;
    return true;
}

} // namespace hw
