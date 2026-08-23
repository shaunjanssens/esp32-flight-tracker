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
     * Radar redraw period in ms.
     *
     * Each redraw blits the whole 480x480 frame, so this is the single
     * biggest lever on how much the panel is disturbed. It buys almost
     * nothing visually: an airliner at 500 kt crosses a 25 nm face in about
     * three minutes, roughly 2 px per second, so even 4 fps moves a blip half
     * a pixel per frame.
     */
    uint16_t refresh_ms = 250;

    /**
     * RGB pixel clock in MHz, applied when the panel is initialised.
     *
     * This is the one setting that actually decides whether the picture is
     * stable, and the default is low on purpose.
     *
     * The panel reads its framebuffer out of PSRAM continuously: 480 x 480 x 2
     * bytes per frame, so bandwidth = pclk x 2 x 480x480 / (548 x 499), which
     * is 27.6 MB/s at 16 MHz and 34 MB/s at 20 MHz. Measured throughput on
     * this board is 34 MB/s write and 22 MB/s copy, so at 16 MHz the refresh
     * alone consumes the whole memory system and anything else - Wi-Fi DMA,
     * our own drawing - starves it and the image tears. 8 MHz needs 13.8 MB/s
     * and is rock steady with Wi-Fi up and aircraft moving.
     *
     * The cost is refresh rate (8 MHz is ~29 Hz), which on an LCD shows as
     * slightly less crisp motion rather than the brightness flicker a CRT
     * would give. Raise it if your board has bandwidth to spare; verify with
     * Wi-Fi connected and aircraft on screen, since an idle device looks fine
     * at any clock.
     */
    uint8_t pclk_mhz = 8;

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
