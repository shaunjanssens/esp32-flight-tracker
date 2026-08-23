#include "config.h"

#include <Preferences.h>

namespace app {
namespace {

constexpr char kNamespace[] = "tracker";
Settings g_settings;

} // namespace

Settings &settings()
{
    return g_settings;
}

size_t radiusPresetIndex(uint16_t radius_nm)
{
    for (size_t i = 0; i < kRadiusPresetCount; i++) {
        if (radius_nm <= kRadiusPresets[i]) {
            return i;
        }
    }
    return kRadiusPresetCount - 1;
}

bool Settings::load()
{
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
        // Namespace does not exist yet — first boot, defaults stand.
        return false;
    }

    home_lat = prefs.getFloat("lat", home_lat);
    home_lon = prefs.getFloat("lon", home_lon);
    position_set = prefs.getBool("pos_set", position_set);
    radius_nm = prefs.getUShort("radius", radius_nm);
    provider = static_cast<Provider>(prefs.getUChar("provider", static_cast<uint8_t>(provider)));
    day_brightness = prefs.getUChar("bright_day", day_brightness);
    night_brightness = prefs.getUChar("bright_night", night_brightness);
    night_start_hour = prefs.getUChar("night_start", night_start_hour);
    night_end_hour = prefs.getUChar("night_end", night_end_hour);
    night_dimming = prefs.getBool("night_dim", night_dimming);
    imu_orientation = prefs.getBool("imu", imu_orientation);
    prefs.getString("host", hostname, sizeof(hostname));
    prefs.getString("tz", timezone, sizeof(timezone));
    prefs.getString("ota_pw", ota_password, sizeof(ota_password));
    prefs.end();

    // A stored radius from an older build might not be a preset any more.
    radius_nm = kRadiusPresets[radiusPresetIndex(radius_nm)];
    return true;
}

bool Settings::save() const
{
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        return false;
    }

    prefs.putFloat("lat", home_lat);
    prefs.putFloat("lon", home_lon);
    prefs.putBool("pos_set", position_set);
    prefs.putUShort("radius", radius_nm);
    prefs.putUChar("provider", static_cast<uint8_t>(provider));
    prefs.putUChar("bright_day", day_brightness);
    prefs.putUChar("bright_night", night_brightness);
    prefs.putUChar("night_start", night_start_hour);
    prefs.putUChar("night_end", night_end_hour);
    prefs.putBool("night_dim", night_dimming);
    prefs.putBool("imu", imu_orientation);
    prefs.putString("host", hostname);
    prefs.putString("tz", timezone);
    prefs.putString("ota_pw", ota_password);
    prefs.end();
    return true;
}

void Settings::reset()
{
    Preferences prefs;
    if (prefs.begin(kNamespace, false)) {
        prefs.clear();
        prefs.end();
    }
    *this = Settings{};
}

} // namespace app
