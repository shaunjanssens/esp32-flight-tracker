#pragma once

#include <Arduino.h>

namespace hw {

/** One sample from the touch controller. */
struct TouchState {
    bool down = false;
    int16_t x = 0;
    int16_t y = 0;
};

bool touchInit();

/** Current contact, if any. The CST820 reports a single point only. */
bool touchRead(TouchState &state);

bool touchReady();

} // namespace hw
