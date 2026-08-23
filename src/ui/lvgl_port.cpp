#include "lvgl_port.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_rgb.h>

#include "app/log.h"

using namespace esp_panel::drivers;

namespace ui {
namespace {

// Draw buffers live in *internal* RAM: the RGB panel already saturates PSRAM
// bandwidth streaming the framebuffer, and reading the flush source from PSRAM
// too is what tears the left edge of the picture. 10 lines keeps the pair at
// ~19 kB — internal RAM has to leave ~40 kB contiguous free for a TLS session.
constexpr int kBufferLines = 10;
constexpr int kTaskCore = 1;          // UI on core 1, networking on core 0
constexpr int kTaskPriority = 2;
constexpr int kTaskStack = 8 * 1024;
constexpr int kMaxDelayMs = 100;
constexpr int kMinDelayMs = 2;
constexpr uint32_t kTouchFailureLimit = 10;
constexpr uint32_t kTouchRetryMs = 5000;

SemaphoreHandle_t g_mutex = nullptr;
lv_display_t *g_display = nullptr;
LCD *g_lcd = nullptr;
TaskHandle_t g_lvgl_task = nullptr;
RenderMode g_mode = RenderMode::Partial;
const char *g_mode_name = "partial";
constexpr uint32_t kResyncIntervalMs = 10000;

/** Fires when the panel finishes showing a frame; releases the LVGL task. */
bool IRAM_ATTR onRefreshFinish(void *)
{
    BaseType_t higher_priority_woken = pdFALSE;
    if (g_lvgl_task != nullptr) {
        vTaskNotifyGiveFromISR(g_lvgl_task, &higher_priority_woken);
    }
    return higher_priority_woken == pdTRUE;
}

/**
 * Direct mode: LVGL renders straight into one of the panel's two framebuffers
 * and we swap them at the end of a frame.
 *
 * The alternative - LVGL drawing into its own buffer and flushing it into the
 * live framebuffer - means writing to memory the panel is scanning out at the
 * same time, which is what the horizontal flashing was. With two buffers, the
 * one being displayed is never written to, and the swap happens between
 * frames. It also removes the copy entirely: the render *is* the flush.
 */
/**
 * Full-frame mode: LVGL composes the entire screen in PSRAM, and one call
 * pushes it to the panel in a single sequential pass. No partially-drawn
 * frame is ever visible, which is what an erase-then-redraw flicker is.
 */
void flushFull(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    auto *lcd = static_cast<LCD *>(lv_display_get_user_data(disp));
    const int32_t width = area->x2 - area->x1 + 1;
    const int32_t height = area->y2 - area->y1 + 1;
    lcd->drawBitmap(area->x1, area->y1, width, height, px_map, -1);
    lv_display_flush_ready(disp);
}

void flushDirect(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    LV_UNUSED(area);
    if (!lv_display_flush_is_last(disp)) {
        lv_display_flush_ready(disp);
        return;
    }

    auto *lcd = static_cast<LCD *>(lv_display_get_user_data(disp));
    lcd->switchFrameBufferTo(px_map);

    // Wait until that buffer is actually on screen before LVGL starts drawing
    // into the other one.
    ulTaskNotifyValueClear(nullptr, ULONG_MAX);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    lv_display_flush_ready(disp);
}

void flushCallback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    auto *lcd = static_cast<LCD *>(lv_display_get_user_data(disp));
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;

    // timeout_ms = -1 blocks until the copy into the panel framebuffer is done,
    // so the draw buffer is free to reuse the moment this returns.
    lcd->drawBitmap(area->x1, area->y1, w, h, px_map, -1);
    lv_display_flush_ready(disp);
}

void touchReadCallback(lv_indev_t *indev, lv_indev_data_t *data)
{
    auto *touch = static_cast<Touch *>(lv_indev_get_user_data(indev));
    TouchPoint point;

    data->state = LV_INDEV_STATE_RELEASED;

    // A touch controller that never answers must not be polled 30 times a
    // second: each failure logs three lines and the console flood costs more
    // time than the UI itself. Back off to one retry every 5 s instead.
    static uint32_t consecutive_failures = 0;
    static uint32_t next_retry_ms = 0;
    if (consecutive_failures >= kTouchFailureLimit) {
        if (millis() < next_retry_ms) {
            return;
        }
        next_retry_ms = millis() + kTouchRetryMs;
    }

    const int read = touch->readPoints(&point, 1, 0);
    if (read < 0) {
        consecutive_failures++;
        if (consecutive_failures == kTouchFailureLimit) {
            Serial.println("[touch] controller not responding, backing off to 1 poll / 5 s");
        }
        return;
    }

    if (consecutive_failures >= kTouchFailureLimit) {
        Serial.println("[touch] controller answered again");
    }
    consecutive_failures = 0;

    if (read > 0) {
        data->point.x = point.x;
        data->point.y = point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
}

const char *displayRenderModeName();

uint32_t tickCallback()
{
    return millis();
}

void lvglTask(void *)
{
    uint32_t next_resync_ms = 0;
    while (true) {
        if (millis() >= next_resync_ms) {
            next_resync_ms = millis() + kResyncIntervalMs;
            displayResync();
        }
        uint32_t delay_ms = kMaxDelayMs;
        if (lvglLock(-1)) {
            delay_ms = lv_timer_handler();
            lvglUnlock();
        }
        delay_ms = constrain(delay_ms, (uint32_t)kMinDelayMs, (uint32_t)kMaxDelayMs);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

} // namespace

bool lvglLock(int timeout_ms)
{
    if (g_mutex == nullptr) {
        return false;
    }
    const TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(g_mutex, ticks) == pdTRUE;
}

void lvglUnlock()
{
    if (g_mutex != nullptr) {
        xSemaphoreGiveRecursive(g_mutex);
    }
}

void displayResync()
{
    if (g_lcd == nullptr) {
        return;
    }
    esp_lcd_panel_handle_t panel = g_lcd->getRefreshPanelHandle();
    if (panel != nullptr) {
        esp_lcd_rgb_panel_restart(panel);
    }
}

bool displaySetPixelClock(uint32_t mhz)
{
    if (g_lcd == nullptr || mhz < 6 || mhz > 30) {
        return false;
    }
    esp_lcd_panel_handle_t panel = g_lcd->getRefreshPanelHandle();
    if (panel == nullptr) {
        return false;
    }
    const esp_err_t err = esp_lcd_rgb_panel_set_pclk(panel, mhz * 1000 * 1000);
    app::logf("[panel] pixel clock -> %u MHz (%s)", (unsigned)mhz, esp_err_to_name(err));
    return err == ESP_OK;
}

bool lvglPortInit(LCD *lcd, Touch *touch, RenderMode mode)
{
    if (lcd == nullptr) {
        Serial.println("[lvgl] no LCD");
        return false;
    }

    g_lcd = lcd;
    g_mutex = xSemaphoreCreateRecursiveMutex();
    if (g_mutex == nullptr) {
        Serial.println("[lvgl] mutex allocation failed");
        return false;
    }

    lv_init();
    lv_tick_set_cb(tickCallback);

    const int width = lcd->getFrameWidth();
    const int height = lcd->getFrameHeight();

    g_display = lv_display_create(width, height);
    if (g_display == nullptr) {
        Serial.println("[lvgl] display creation failed");
        return false;
    }
    lv_display_set_user_data(g_display, lcd);
    lv_display_set_color_format(g_display, LV_COLOR_FORMAT_RGB565);

    const size_t frame_bytes = (size_t)width * height * sizeof(lv_color16_t);
    g_mode = mode;

    if (mode == RenderMode::Direct) {
        void *fb0 = lcd->getFrameBufferByIndex(0);
        void *fb1 = lcd->getFrameBufferByIndex(1);
        if (fb0 != nullptr && fb1 != nullptr) {
            lv_display_set_flush_cb(g_display, flushDirect);
            lv_display_set_buffers(g_display, fb0, fb1, frame_bytes,
                                   LV_DISPLAY_RENDER_MODE_DIRECT);
            lcd->attachRefreshFinishCallback(onRefreshFinish, nullptr);
            g_mode_name = "direct";
        } else {
            app::logf("[lvgl] only one framebuffer available, falling back to full");
            mode = RenderMode::Full;
        }
    }

    if (mode == RenderMode::Full) {
        void *buffer = heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buffer != nullptr) {
            lv_display_set_flush_cb(g_display, flushFull);
            lv_display_set_buffers(g_display, buffer, nullptr, frame_bytes,
                                   LV_DISPLAY_RENDER_MODE_FULL);
            g_mode_name = "full";
        } else {
            app::logf("[lvgl] no PSRAM for a full frame buffer, falling back to partial");
            mode = RenderMode::Partial;
        }
    }

    if (mode == RenderMode::Partial) {
        const size_t buffer_bytes = (size_t)width * kBufferLines * sizeof(lv_color16_t);
        void *buf1 = heap_caps_malloc(buffer_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        void *buf2 = heap_caps_malloc(buffer_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (buf1 == nullptr || buf2 == nullptr) {
            heap_caps_free(buf1);
            heap_caps_free(buf2);
            buf1 = heap_caps_malloc(buffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            buf2 = heap_caps_malloc(buffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (buf1 == nullptr || buf2 == nullptr) {
            app::logf("[lvgl] draw buffer allocation failed");
            return false;
        }
        lv_display_set_flush_cb(g_display, flushCallback);
        lv_display_set_buffers(g_display, buf1, buf2, buffer_bytes,
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        g_mode_name = "partial";
    }
    g_mode = mode;
    app::logf("[lvgl] %dx%d, %s rendering", width, height, g_mode_name);

    if (touch != nullptr) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touchReadCallback);
        lv_indev_set_user_data(indev, touch);
        lv_indev_set_display(indev, g_display);
    } else {
        Serial.println("[lvgl] warning: no touch device");
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        lvglTask, "lvgl", kTaskStack, nullptr, kTaskPriority, &g_lvgl_task, kTaskCore
    );
    if (created != pdPASS) {
        Serial.println("[lvgl] task creation failed");
        return false;
    }

    return true;
}

const char *displayRenderMode()
{
    return g_mode_name;
}

} // namespace ui
