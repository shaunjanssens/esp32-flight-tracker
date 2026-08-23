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
enum class RenderMode : uint8_t {
    Partial = 0,
    Direct  = 1,
    Full    = 2,
};

bool lvglPortInit(esp_panel::drivers::LCD *lcd, esp_panel::drivers::Touch *touch,
                  RenderMode mode);

/** Which mode actually came up (allocation can force a fallback). */
const char *displayRenderMode();

/**
 * Restart the RGB panel's DMA at the next VSYNC.
 *
 * The ESP32-S3 streams the framebuffer out of PSRAM continuously. Whenever the
 * cache is disabled - which every SPI-flash write does, including the NVS
 * writes Wi-Fi makes while associating - the refresh FIFO starves, and the
 * picture stays permanently shifted afterwards. Restarting resynchronises it;
 * it is a no-op to look at when nothing has drifted.
 */
void displayResync();

/**
 * Change the RGB pixel clock while running. Lower means less PSRAM bandwidth
 * spent on refresh, at the cost of frame rate. Returns false if the panel
 * rejects the value.
 */
bool displaySetPixelClock(uint32_t mhz);

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
