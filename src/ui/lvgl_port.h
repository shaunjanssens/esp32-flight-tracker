#pragma once

#include <esp_display_panel.hpp>
#include <lvgl.h>

namespace ui {

/**
 * Bring LVGL up on top of an initialised ESP32_Display_Panel board.
 *
 * Starts the LVGL timer task pinned to `kLvglTaskCore`; every LVGL call made
 * from another task must be wrapped in lock()/unlock().
 */
bool lvglPortInit(esp_panel::drivers::LCD *lcd, esp_panel::drivers::Touch *touch);

/** Take the LVGL mutex. `timeout_ms < 0` waits forever. */
bool lvglLock(int timeout_ms = -1);
void lvglUnlock();

/** RAII helper for the mutex above. */
class LvglGuard {
public:
    explicit LvglGuard(int timeout_ms = -1) : held_(lvglLock(timeout_ms)) {}
    ~LvglGuard() { if (held_) lvglUnlock(); }
    explicit operator bool() const { return held_; }
    LvglGuard(const LvglGuard &) = delete;
    LvglGuard &operator=(const LvglGuard &) = delete;
private:
    bool held_;
};

} // namespace ui
