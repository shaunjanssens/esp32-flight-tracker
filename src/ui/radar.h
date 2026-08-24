#pragma once

#include <Arduino.h>

namespace ui {

/** Allocate the compositing sprites and draw the static grid. */
bool radarBegin();

/** Poll touch and redraw when due. Call from the main loop. */
void radarTick();

/** Stop redrawing (diagnostic). */
void radarPause(bool paused);
bool radarPaused();

void radarSetRefreshMs(uint16_t ms);

/** ICAO address of the selected aircraft, 0 for none. */
uint32_t radarSelected();

/** millis() of the last accepted touch, for the dimming schedule. */
uint32_t radarLastTouchMs();

} // namespace ui
