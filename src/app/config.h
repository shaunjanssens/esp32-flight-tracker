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

    Provider provider = Provider::AdsbLol;

    // Backlight, in percent, and the night window it applies to (local hours).
    uint8_t day_brightness = 100;
    uint8_t night_brightness = 20;
    uint8_t night_start_hour = 22;
    uint8_t night_end_hour = 7;
    bool    night_dimming = true;

    bool imu_orientation = true;

    char hostname[24] = "flighttracker";
    char timezone[40] = "CET-1CEST,M3.5.0,M10.5.0/3";   // Europe/Brussels
    char ota_password[33] = "";

    bool load();
    bool save() const;
    void reset();
};

constexpr uint16_t kRadiusPresets[] = {5, 10, 25, 50, 100};
constexpr size_t kRadiusPresetCount = sizeof(kRadiusPresets) / sizeof(kRadiusPresets[0]);

/** Index of the preset at or above `radius_nm`, clamped to the ends. */
size_t radiusPresetIndex(uint16_t radius_nm);

/** The process-wide settings instance. */
Settings &settings();

} // namespace app
