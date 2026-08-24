/**
 * ESP32 Flight Tracker - Waveshare ESP32-S3-Touch-LCD-2.1
 *
 * Live ADS-B aircraft on a north-up polar plot: range rings, cardinal marks,
 * trails, tap an aircraft for its route. No map tiles.
 */

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <string.h>

#include "app/config.h"
#include "app/log.h"
#include "hw/display.h"
#include "hw/touch.h"
#include "model/aircraft.h"
#include "net/adsb_client.h"
#include "net/route_client.h"
#include "net/wifi_manager.h"
#include "ui/radar.h"

namespace {

/**
 * Counts display-init attempts across soft resets, in RTC memory so that
 * counting costs no flash write and a power cycle clears it. Two crashes in a
 * row and the third boot skips the display, staying reachable over OTA.
 */
RTC_NOINIT_ATTR uint32_t g_display_attempts;
constexpr uint32_t kDisplayAttemptLimit = 2;

/**
 * Set by /api/diag: boot the panel with the radios off and hold a static
 * image, so the display can be judged with nothing competing for PSRAM. Wi-Fi
 * starts by itself afterwards, so the device always comes back on its own.
 */
RTC_NOINIT_ATTR uint32_t g_diag_seconds;
RTC_NOINIT_ATTR uint32_t g_diag_magic;
constexpr uint32_t kDiagMagic = 0x0FF1CE01;

bool g_headless = false;

/**
 * Measure PSRAM throughput once at boot.
 *
 * The panel reads its framebuffer straight out of PSRAM - 27.6 MB/s at a
 * 16 MHz pixel clock - and flicker turned out to scale with exactly that. If
 * the measured ceiling is near those figures, the pixel clock is the honest
 * limit; if it is far below what octal PSRAM at 80 MHz should manage, the
 * memory is misconfigured and that is the real bug.
 */
void benchmarkPsram()
{
    constexpr size_t kBytes = 512 * 1024;
    uint8_t *source = (uint8_t *)heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM);
    uint8_t *destination = (uint8_t *)heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM);
    if (source == nullptr || destination == nullptr) {
        heap_caps_free(source);
        heap_caps_free(destination);
        return;
    }
    memset(source, 0xA5, kBytes);

    const uint32_t write_start = micros();
    memset(destination, 0x5A, kBytes);
    const uint32_t write_us = micros() - write_start;

    const uint32_t copy_start = micros();
    memcpy(destination, source, kBytes);
    const uint32_t copy_us = micros() - copy_start;

    app::logf("[psram] %u MB/s write, %u MB/s copy (panel needs %u MB/s at %u MHz)",
              (unsigned)(kBytes / (write_us ? write_us : 1)),
              (unsigned)(kBytes / (copy_us ? copy_us : 1)),
              (unsigned)((480u * 480u * 2u * app::settings().pclk_mhz * 1000000u)
                         / (548u * 499u) / 1000000u),
              (unsigned)app::settings().pclk_mhz);

    heap_caps_free(source);
    heap_caps_free(destination);
}

void logMemory(const char *stage)
{
    app::logf("[mem] %-8s internal %u (largest %u)  psram %u", stage,
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

} // namespace

void setup()
{
    Serial.begin(115200);
    delay(300);
    app::logf("=== ESP32 Flight Tracker ===");
    logMemory("boot");

    const bool diagnostic = (g_diag_magic == kDiagMagic && g_diag_seconds > 0);
    g_diag_magic = 0;

    // Settings and networking come up first, and nothing here returns early:
    // a device whose panel fails must still join Wi-Fi and accept an OTA
    // update, or a display bug strands it behind the USB cable.
    app::settings().load();
    benchmarkPsram();

    if (!model::store().begin()) {
        app::logf("[app] aircraft store allocation failed");
    }
    if (diagnostic) {
        app::logf("[app] diagnostic boot: radios off for %us", (unsigned)g_diag_seconds);
    } else if (!net::wifiBegin()) {
        app::logf("[app] wifi task failed to start");
    }
    if (!diagnostic) {
        if (!net::adsbStart()) {
            app::logf("[app] feed task failed to start");
        }
        if (!net::routeStart()) {
            app::logf("[app] route lookup task failed to start");
        }
    }

    if (esp_reset_reason() == ESP_RST_POWERON) {
        g_display_attempts = 0;
    }
    if (g_display_attempts >= kDisplayAttemptLimit) {
        g_headless = true;
        app::logf("[app] display init crashed %u times - headless, OTA is open",
                  (unsigned)g_display_attempts);
        return;
    }
    g_display_attempts++;

    if (!hw::displayInit(app::settings().pclk_mhz)) {
        app::logf("[app] display unavailable - continuing headless");
        g_headless = true;
        return;
    }
    hw::displayBrightness(app::settings().day_brightness);
    hw::touchInit();
    logMemory("display");

    if (!ui::radarBegin()) {
        app::logf("[app] radar sprites unavailable - continuing headless");
        g_headless = true;
        return;
    }
    logMemory("radar");

    g_display_attempts = 0;      // survived display bring-up

    if (diagnostic) {
        // One frame, then nothing at all: no drawing, no radios, no feed.
        ui::radarTick();
        ui::radarPause(true);
        const uint32_t hold_ms = g_diag_seconds * 1000;
        const uint32_t started = millis();
        while (millis() - started < hold_ms) {
            delay(250);
        }
        app::logf("[app] diagnostic window over, starting Wi-Fi");
        ui::radarPause(false);
        net::wifiBegin();
        net::adsbStart();
        net::routeStart();
    }

    app::logf("[app] running");
}

/**
 * Backlight follows the clock, with a touch overriding it.
 *
 * The window wraps midnight (22 -> 7), which is the normal case, so the
 * comparison differs depending on whether start is before or after end.
 */
void applyBrightness()
{
    const app::Settings &settings = app::settings();
    uint8_t target = settings.day_brightness;

    const time_t now = time(nullptr);
    if (settings.night_dimming && now > 1700000000) {
        struct tm local;
        localtime_r(&now, &local);
        const int hour = local.tm_hour;
        const int start = settings.night_start_hour;
        const int end = settings.night_end_hour;
        const bool night = (start <= end) ? (hour >= start && hour < end)
                                          : (hour >= start || hour < end);
        if (night) {
            target = settings.night_brightness;
        }
    }

    // Any touch restores full brightness for a while, so a dimmed screen is
    // never a dead-looking one.
    if (millis() - ui::radarLastTouchMs() < 30000 && ui::radarLastTouchMs() != 0) {
        target = settings.day_brightness;
    }

    static uint8_t applied = 255;
    if (target != applied) {
        applied = target;
        hw::displayBrightness(target);
    }
}

void loop()
{
    if (!g_headless) {
        ui::radarTick();

        static uint32_t next_brightness_ms = 0;
        if ((int32_t)(millis() - next_brightness_ms) >= 0) {
            next_brightness_ms = millis() + 5000;
            applyBrightness();
        }
    }
    delay(5);
}

/** Called from the web handler to arm a diagnostic boot. */
void requestDiagnosticBoot(uint32_t seconds)
{
    g_diag_seconds = seconds;
    g_diag_magic = kDiagMagic;
}
