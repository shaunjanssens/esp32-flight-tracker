#pragma once

#include <Arduino.h>

namespace app {

enum class Provider : uint8_t {
    AdsbLol = 0,
    AdsbFi  = 1,
};

/** Everything the user can change, persisted in NVS. */
struct Settings {
    // Position of the radar centre.
    float home_lat = 0.0f;
    float home_lon = 0.0f;
    bool  position_set = false;

    // Display range in nautical miles; must be one of kRadiusPresets.
    uint16_t radius_nm = 25;

    // adsb.fi first: adsb.lol's round-robin includes nodes that refuse
    // connections, and failing over costs a poll cycle every time.
    Provider provider = Provider::AdsbFi;

    // Backlight, in percent, and the night window it applies to (local hours).
    uint8_t day_brightness = 100;
    uint8_t night_brightness = 20;
    uint8_t night_start_hour = 22;
    uint8_t night_end_hour = 7;
    bool    night_dimming = true;

    bool imu_orientation = true;

    /**
     * RGB bounce buffer height in lines; 0 disables it.
     *
     * Not cosmetic: with a bounce buffer the LCD FIFO is refilled from an
     * interrupt, and these Arduino IDF libraries are built with
     * CONFIG_LCD_RGB_ISR_IRAM_SAFE off, so any flash write (Wi-Fi writing PHY
     * calibration to NVS, for one) can starve it and shift the picture for
     * good. Without one, GDMA streams straight from PSRAM instead. Which is
     * better depends on the board, so it is a setting, applied at boot.
     */
    uint16_t bounce_lines = 0;

    /**
     * RGB pixel clock in MHz (board default 16). At 480x480 with the porches
     * this board uses, 16 MHz is ~58 Hz refresh and 27.6 MB/s of continuous
     * PSRAM reads. Dropping it trades refresh rate for bandwidth headroom,
     * which is the other half of the tearing story.
     */
    uint8_t pclk_mhz = 16;

    /**
     * How LVGL gets pixels onto the panel. All three exist because RGB panels
     * on this SoC fail in different ways and the only honest test is the
     * screen itself:
     *   0 Partial - small buffers copied into the live framebuffer. Cheapest
     *     in RAM, but writes memory the panel is scanning out: tears.
     *   1 Direct  - LVGL renders into the panel's own second framebuffer and
     *     they swap on VSYNC.
     *   2 Full    - the whole frame is composed off-screen in PSRAM and blitted
     *     in a single pass. What the LovyanGFX projects on this board do.
     */
    uint8_t render_mode = 2;

    char hostname[24] = "flighttracker";
    char timezone[40] = "CET-1CEST,M3.5.0,M10.5.0/3";   // Europe/Brussels
    char ota_password[33] = "";

    bool load();
    bool save() const;
    void reset();

    /**
     * Queue a write instead of making one now.
     *
     * Every NVS write is a flash write, and a flash write disables the cache -
     * which on this board shifts the picture. Dragging the range ring fires a
     * change per step, so writing on each one produced a burst of flash writes
     * exactly while the user was looking at the screen. This coalesces them
     * into one write a few seconds after the last change.
     */
    void saveSoon();
};

constexpr uint16_t kRadiusPresets[] = {5, 10, 25, 50, 100};
constexpr size_t kRadiusPresetCount = sizeof(kRadiusPresets) / sizeof(kRadiusPresets[0]);

/** Index of the preset at or above `radius_nm`, clamped to the ends. */
size_t radiusPresetIndex(uint16_t radius_nm);

/** The process-wide settings instance. */
Settings &settings();

/** Flush a pending saveSoon() once it has settled. Call from a periodic task. */
void settingsTick();


} // namespace app
