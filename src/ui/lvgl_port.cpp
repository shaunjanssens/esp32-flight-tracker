#include "lvgl_port.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

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

uint32_t tickCallback()
{
    return millis();
}

void lvglTask(void *)
{
    while (true) {
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

bool lvglPortInit(LCD *lcd, Touch *touch)
{
    if (lcd == nullptr) {
        Serial.println("[lvgl] no LCD");
        return false;
    }

    g_mutex = xSemaphoreCreateRecursiveMutex();
    if (g_mutex == nullptr) {
        Serial.println("[lvgl] mutex allocation failed");
        return false;
    }

    lv_init();
    lv_tick_set_cb(tickCallback);

    const int width = lcd->getFrameWidth();
    const int height = lcd->getFrameHeight();
    const size_t buffer_bytes = (size_t)width * kBufferLines * sizeof(lv_color16_t);

    void *buf1 = heap_caps_malloc(buffer_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void *buf2 = heap_caps_malloc(buffer_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf1 == nullptr || buf2 == nullptr) {
        // Internal RAM is tight; PSRAM still works, it just draws worse.
        heap_caps_free(buf1);
        heap_caps_free(buf2);
        buf1 = heap_caps_malloc(buffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        buf2 = heap_caps_malloc(buffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        Serial.println("[lvgl] falling back to PSRAM draw buffers");
    }
    if (buf1 == nullptr || buf2 == nullptr) {
        Serial.printf("[lvgl] could not allocate 2 x %u byte draw buffers\n", (unsigned)buffer_bytes);
        heap_caps_free(buf1);
        heap_caps_free(buf2);
        return false;
    }

    g_display = lv_display_create(width, height);
    if (g_display == nullptr) {
        Serial.println("[lvgl] display creation failed");
        return false;
    }
    lv_display_set_user_data(g_display, lcd);
    lv_display_set_color_format(g_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(g_display, flushCallback);
    lv_display_set_buffers(g_display, buf1, buf2, buffer_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

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
        lvglTask, "lvgl", kTaskStack, nullptr, kTaskPriority, nullptr, kTaskCore
    );
    if (created != pdPASS) {
        Serial.println("[lvgl] task creation failed");
        return false;
    }

    Serial.printf("[lvgl] up: %dx%d, 2 x %u byte draw buffers\n",
                  width, height, (unsigned)buffer_bytes);
    return true;
}

} // namespace ui
