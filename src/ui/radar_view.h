#pragma once

#include <Arduino.h>
#include <lvgl.h>

namespace ui {

/** Build the radar face on the active screen. Call with the LVGL lock held. */
bool radarCreate();

/** ICAO address of the tapped aircraft, or 0 when nothing is selected. */
uint32_t radarSelected();
void radarSelect(uint32_t icao);

/** Current display range in nautical miles. */
uint16_t radarRadiusNm();

} // namespace ui
