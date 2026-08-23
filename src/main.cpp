/**
 * ESP32 Flight Tracker — Waveshare ESP32-S3-Touch-LCD-2.1
 *
 * Milestone 1: board bring-up. Proves the RGB panel timing, PSRAM budget,
 * touch mapping and LVGL frame rate before any application logic exists.
 */

#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <driver/i2c_master.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "ui/lvgl_port.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

namespace {

Board *g_board = nullptr;
lv_obj_t *g_sweep = nullptr;
lv_obj_t *g_touch_dot = nullptr;
lv_obj_t *g_stats_label = nullptr;

/**
 * The touch controller, the IO expander and the RTC all share I2C0
 * (SCL 7 / SDA 15). Scanning before the panel driver claims the bus tells us
 * which chips this particular board revision actually carries.
 *
 * This uses the IDF 5 i2c_master driver, not Arduino's Wire: the panel library
 * uses the new driver, and mixing the two aborts at boot with
 * "driver_ng is not allowed to be used with this old driver".
 */
void scanI2C()
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = GPIO_NUM_15;
    bus_config.scl_io_num = GPIO_NUM_7;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&bus_config, &bus) != ESP_OK) {
        Serial.println("[i2c] scan skipped: bus busy");
        return;
    }

    Serial.print("[i2c] devices:");
    int found = 0;
    for (uint8_t address = 0x08; address < 0x78; address++) {
        if (i2c_master_probe(bus, address, 50) == ESP_OK) {
            Serial.printf(" 0x%02X", address);
            found++;
        }
    }
    Serial.println(found == 0 ? " none!" : "");
    i2c_del_master_bus(bus);
}

void logMemory(const char *stage)
{
    Serial.printf("[mem] %-12s internal free %6u (largest %6u)  psram free %8u\n",
                  stage,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void onScreenPressed(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_obj_set_pos(g_touch_dot, point.x - 10, point.y - 10);
    lv_obj_clear_flag(g_touch_dot, LV_OBJ_FLAG_HIDDEN);
    Serial.printf("[touch] %d,%d\n", (int)point.x, (int)point.y);
    LV_UNUSED(event);
}

void onScreenReleased(lv_event_t *event)
{
    lv_obj_add_flag(g_touch_dot, LV_OBJ_FLAG_HIDDEN);
    LV_UNUSED(event);
}

/** Refresh the frame-rate / memory readout once a second. */
void statsTimer(lv_timer_t *timer)
{
    static uint32_t last_ms = 0;
    const uint32_t now = millis();
    const uint32_t uptime_s = now / 1000;

    lv_label_set_text_fmt(
        g_stats_label, "%us up\nSRAM %uk  PSRAM %uk",
        (unsigned)uptime_s,
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024)
    );
    last_ms = now;
    LV_UNUSED(timer);
    LV_UNUSED(last_ms);
}

/**
 * A stand-in for the radar face: concentric rings, cardinal marks and a sweeping
 * arc. Its only job is to show that large areas redraw smoothly and that the
 * circle is actually centred on the round panel.
 */
void buildBringUpScreen()
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x04070D), 0);
    lv_obj_add_event_cb(screen, onScreenPressed, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(screen, onScreenReleased, LV_EVENT_RELEASED, nullptr);

    static const int kRings[] = {200, 150, 100, 50};
    for (int radius : kRings) {
        lv_obj_t *ring = lv_arc_create(screen);
        lv_obj_set_size(ring, radius * 2, radius * 2);
        lv_obj_center(ring);
        lv_arc_set_bg_angles(ring, 0, 360);
        lv_arc_set_value(ring, 0);
        lv_obj_remove_style(ring, nullptr, LV_PART_INDICATOR);
        lv_obj_remove_style(ring, nullptr, LV_PART_KNOB);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_color(ring, lv_color_hex(0x1E4D3B), LV_PART_MAIN);
        lv_obj_set_style_arc_width(ring, 2, LV_PART_MAIN);
    }

    static const char *kCardinals[] = {"N", "E", "S", "W"};
    static const lv_align_t kAligns[] = {
        LV_ALIGN_TOP_MID, LV_ALIGN_RIGHT_MID, LV_ALIGN_BOTTOM_MID, LV_ALIGN_LEFT_MID
    };
    static const int kOffsets[][2] = {{0, 26}, {-30, 0}, {0, -26}, {30, 0}};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *label = lv_label_create(screen);
        lv_label_set_text(label, kCardinals[i]);
        lv_obj_set_style_text_color(label, i == 0 ? lv_color_hex(0xE8F0A0) : lv_color_hex(0x5C7A6E), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_obj_align(label, kAligns[i], kOffsets[i][0], kOffsets[i][1]);
    }

    // Sweeping arc, animated below.
    g_sweep = lv_arc_create(screen);
    lv_obj_set_size(g_sweep, 400, 400);
    lv_obj_center(g_sweep);
    lv_arc_set_bg_angles(g_sweep, 0, 0);
    lv_arc_set_angles(g_sweep, 270, 315);
    lv_obj_remove_style(g_sweep, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(g_sweep, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(g_sweep, lv_color_hex(0x38E08A), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_sweep, 6, LV_PART_INDICATOR);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "FLIGHT TRACKER");
    lv_obj_set_style_text_color(title, lv_color_hex(0xDCE6F0), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *subtitle = lv_label_create(screen);
    lv_label_set_text_fmt(subtitle, "bring-up  ·  LVGL %d.%d.%d",
                          LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x6E8AA6), 0);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_12, 0);
    lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, -14);

    g_stats_label = lv_label_create(screen);
    lv_obj_set_style_text_align(g_stats_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_stats_label, lv_color_hex(0x8AA6C0), 0);
    lv_obj_set_style_text_font(g_stats_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(g_stats_label, "");
    lv_obj_align(g_stats_label, LV_ALIGN_CENTER, 0, 30);
    lv_timer_create(statsTimer, 1000, nullptr);

    g_touch_dot = lv_obj_create(screen);
    lv_obj_set_size(g_touch_dot, 20, 20);
    lv_obj_set_style_radius(g_touch_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_touch_dot, lv_color_hex(0xFF5C5C), 0);
    lv_obj_set_style_border_width(g_touch_dot, 0, 0);
    lv_obj_add_flag(g_touch_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_touch_dot, LV_OBJ_FLAG_CLICKABLE);

    // Rotate the sweep once every 4 seconds.
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, g_sweep);
    lv_anim_set_values(&anim, 0, 3600);
    lv_anim_set_duration(&anim, 4000);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&anim, [](void *object, int32_t value) {
        lv_arc_set_angles(static_cast<lv_obj_t *>(object), value / 10, (value / 10 + 45) % 360);
    });
    lv_anim_start(&anim);
}

} // namespace

void setup()
{
    Serial.begin(115200);
    // Long enough for the native USB CDC to enumerate before the first print,
    // and to leave esptool a window to grab the board if the app ever crash-loops.
    delay(1500);
    Serial.println("\n\n=== ESP32 Flight Tracker :: bring-up ===");
    logMemory("boot");
    scanI2C();

    g_board = new Board();
    if (!g_board->init()) {
        Serial.println("[board] init failed");
        return;
    }

    // An RGB panel streams its framebuffer out of PSRAM continuously; Wi-Fi and
    // TLS compete for that same bandwidth and the picture shears. Bounce buffers
    // in internal SRAM are what keep the image stable under network load.
    LCD *lcd = g_board->getLCD();
    if (lcd != nullptr) {
        Bus *bus = lcd->getBus();
        if (bus != nullptr && bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
            static_cast<BusRGB *>(bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 10);
            Serial.println("[board] RGB bounce buffer: 10 lines");
        }
    }

    if (!g_board->begin()) {
        Serial.println("[board] begin failed");
        return;
    }
    logMemory("panel");

    Backlight *backlight = g_board->getBacklight();
    if (backlight != nullptr) {
        backlight->setBrightness(100);
    }

    if (!ui::lvglPortInit(g_board->getLCD(), g_board->getTouch())) {
        Serial.println("[lvgl] port init failed");
        return;
    }
    logMemory("lvgl");

    {
        ui::LvglGuard guard;
        buildBringUpScreen();
    }

    Serial.println("[app] bring-up screen up");
    logMemory("ui");
}

void loop()
{
    static uint32_t last_report = 0;
    if (millis() - last_report > 10000) {
        last_report = millis();
        logMemory("idle");
    }
    delay(100);
}
