/**
 * ESP32 Flight Tracker — Waveshare ESP32-S3-Touch-LCD-2.1
 *
 * Milestone 1: board bring-up. Proves the RGB panel timing, PSRAM budget,
 * touch mapping and LVGL frame rate before any application logic exists.
 */

#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_system.h>
#include <lvgl.h>

#include "app/config.h"
#include "app/log.h"
#include "model/aircraft.h"
#include "net/adsb_client.h"
#include "net/route_client.h"
#include "net/wifi_manager.h"
#include "ui/lvgl_port.h"
#include "ui/radar_view.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

namespace {

Board *g_board = nullptr;

/**
 * Where setup() has got to. A heartbeat task prints this continuously, because
 * the USB CDC drops anything written before the host opens the port — one-shot
 * prints during boot are simply lost, and a hang looks identical to silence.
 */
volatile const char *g_phase = "start";

/**
 * Counts display-init attempts across soft resets. Kept in RTC memory rather
 * than NVS so that counting costs no flash write - and because a power cycle
 * clearing it is exactly the behaviour we want.
 *
 * If bringing the panel up crashes twice in a row, the third boot skips the
 * display entirely and stays on the network, where it can be fixed by OTA.
 * Without this, one bad display setting means the USB cable and the BOOT
 * button, which on this board is a genuinely painful loop.
 */
RTC_NOINIT_ATTR uint32_t g_display_attempts;
constexpr uint32_t kDisplayAttemptLimit = 2;
bool g_headless = false;

void heartbeatTask(void *)
{
    while (true) {
        Serial.printf("[phase] %s  up=%us  sram=%uk psram=%uk\n",
                      g_phase, (unsigned)(millis() / 1000),
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
lv_obj_t *g_sweep = nullptr;
lv_obj_t *g_touch_dot = nullptr;
lv_obj_t *g_stats_label = nullptr;

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

    const net::FeedStats feed = net::adsbStats();
    const char *network_line = net::wifiState() == net::WifiState::Connected
        ? net::wifiAddress()
        : (net::wifiState() == net::WifiState::PortalActive ? net::wifiNetwork() : "connecting...");

    if (feed.last_success_ms == 0) {
        lv_label_set_text_fmt(g_stats_label, "%s\n%us up  -  no feed yet",
                              network_line, (unsigned)uptime_s);
    } else {
        lv_label_set_text_fmt(g_stats_label, "%s\n%u aircraft  -  %us ago",
                              network_line, (unsigned)feed.accepted,
                              (unsigned)((now - feed.last_success_ms) / 1000));
    }
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
    lv_label_set_text_fmt(subtitle, "bring-up  -  LVGL %d.%d.%d",
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
    xTaskCreatePinnedToCore(heartbeatTask, "heartbeat", 4096, nullptr, 1, nullptr, 0);
    logMemory("boot");

    // Settings and networking come first, and nothing below is allowed to
    // return early. A device whose panel fails to start must still join Wi-Fi
    // and accept an OTA update - otherwise a display bug strands it, and the
    // only way back in is the USB cable and the BOOT button.
    g_phase = "settings";
    app::settings().load();

    g_phase = "store";
    if (!model::store().begin()) {
        Serial.println("[app] aircraft store allocation failed");
    }

    g_phase = "wifi";
    if (!net::wifiBegin()) {
        Serial.println("[app] wifi task failed to start");
    }

    g_phase = "adsb";
    if (!net::adsbStart()) {
        Serial.println("[app] adsb task failed to start");
    }
    if (!net::routeStart()) {
        Serial.println("[app] route lookup task failed to start");
    }

    if (esp_reset_reason() == ESP_RST_POWERON) {
        g_display_attempts = 0;
    }
    if (g_display_attempts >= kDisplayAttemptLimit) {
        g_headless = true;
        g_phase = "headless";
        app::logf("[board] display init crashed %u times - staying headless, OTA is open",
                  (unsigned)g_display_attempts);
        return;
    }
    g_display_attempts++;

    g_phase = "board-init";
    g_board = new Board();
    if (!g_board->init()) {
        Serial.println("[board] init failed - continuing headless");
        return;
    }

    const uint16_t bounce_lines = app::settings().bounce_lines;
    LCD *lcd = g_board->getLCD();
    if (lcd != nullptr) {
        Bus *bus = lcd->getBus();
        if (bus != nullptr && bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
            // Two framebuffers so LVGL can render into the one that is not
            // being scanned out (see the port's direct mode).
            if (app::settings().render_mode == (uint8_t)ui::RenderMode::Direct) {
                lcd->configFrameBufferNumber(2);
            }
            auto *rgb_bus = static_cast<BusRGB *>(bus);
            if (bounce_lines > 0) {
                rgb_bus->configRGB_BounceBufferSize(lcd->getFrameWidth() * bounce_lines);
            } else {
                // configRGB_BounceBufferSize(0) divides by the size to align it,
                // so it traps on zero. Clear the field in the panel config
                // instead, which is what "no bounce buffer" actually means.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
                auto *config = const_cast<esp_lcd_rgb_panel_config_t *>(rgb_bus->getRgbConfig());
#pragma GCC diagnostic pop
                config->bounce_buffer_size_px = 0;
            }
            Serial.printf("[board] RGB bounce buffer: %u lines\n", (unsigned)bounce_lines);
        }
    }

    g_phase = "board-begin";
    if (!g_board->begin()) {
        Serial.println("[board] begin failed - continuing headless");
        return;
    }
    logMemory("panel");

    Backlight *backlight = g_board->getBacklight();
    if (backlight != nullptr) {
        backlight->setBrightness(app::settings().day_brightness);
    }

    g_phase = "lvgl-init";
    if (!ui::lvglPortInit(g_board->getLCD(), g_board->getTouch(),
                          (ui::RenderMode)app::settings().render_mode)) {
        Serial.println("[lvgl] port init failed - continuing headless");
        return;
    }
    logMemory("lvgl");

    {
        g_phase = "build-ui";
        ui::LvglGuard guard;
        ui::radarCreate();
    }

    if (app::settings().pclk_mhz != 16) {
        ui::displaySetPixelClock(app::settings().pclk_mhz);
    }

    // Survived display bring-up: clear the crash counter.
    g_display_attempts = 0;

    g_phase = "running";
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
