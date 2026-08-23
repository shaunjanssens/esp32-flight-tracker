#pragma once

#include <Arduino.h>
#include <lvgl.h>

namespace ui {

void detailSheetCreate(lv_obj_t *parent);
void detailSheetShow(uint32_t icao);
void detailSheetHide();
bool detailSheetVisible();

/** Refresh the live figures; called from the radar's redraw timer. */
void detailSheetRefresh();

} // namespace ui
