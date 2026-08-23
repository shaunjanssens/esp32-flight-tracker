#include "hw/touch.h"

#include <Wire.h>

#include "app/log.h"
#include "hw/display.h"

namespace hw {
namespace {

bool g_ready = false;
uint32_t g_failures = 0;

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

    if (buffer[1] == 0) {
        return true;            // a valid read, just nothing touching
    }

    state.down = true;
    state.x = (int16_t)(((buffer[2] & 0x0F) << 8) | buffer[3]);
    state.y = (int16_t)(((buffer[4] & 0x0F) << 8) | buffer[5]);
    state.x = constrain(state.x, 0, kScreenWidth - 1);
    state.y = constrain(state.y, 0, kScreenHeight - 1);
    return true;
}

} // namespace hw
