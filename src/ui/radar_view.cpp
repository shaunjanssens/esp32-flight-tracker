#include "radar_view.h"

#include <math.h>
#include <time.h>

#include "app/config.h"
#include "app/log.h"
#include "model/aircraft.h"
#include "net/adsb_client.h"
#include "net/wifi_manager.h"
#include "ui/detail_sheet.h"
#include "ui/lvgl_port.h"

namespace ui {
namespace {

// Screen geometry. The panel is 480x480 and round, so everything is polar
// around the centre; the outer band is reserved for the range gesture.
constexpr int32_t kCentre     = 240;
constexpr int32_t kPlotRadius = 196;   // outermost range ring
constexpr int32_t kBezelInner = 200;   // drags starting beyond this zoom
constexpr int32_t kTapSlopPx  = 34;    // how close a tap must land to a blip

constexpr uint32_t kRefreshMs = 100;   // 10 fps: blips move slowly, PSRAM does not
constexpr size_t   kMaxLabels = 8;     // label only the nearest few, or it is soup
constexpr size_t   kTrailChunks = 4;   // fade steps along a trail

// Palette
constexpr uint32_t kColourBackground = 0x04070D;
constexpr uint32_t kColourRing       = 0x14352A;
constexpr uint32_t kColourRingMajor  = 0x1E4D3B;
constexpr uint32_t kColourRingLabel  = 0x3E5C50;
constexpr uint32_t kColourNorth      = 0xE8F0A0;
constexpr uint32_t kColourCardinal   = 0x5C7A6E;
constexpr uint32_t kColourHome       = 0x38E08A;
constexpr uint32_t kColourLabel      = 0xAFC4D4;
constexpr uint32_t kColourStale      = 0xE0603C;
constexpr uint32_t kColourSelected   = 0xFFFFFF;

lv_obj_t *g_face = nullptr;
lv_timer_t *g_timer = nullptr;
uint32_t g_selected_icao = 0;

// Bezel gesture state
bool  g_bezel_drag = false;
float g_bezel_last_angle = 0.0f;
float g_bezel_accumulated = 0.0f;
uint32_t g_range_hint_until = 0;

/** Altitude bands, coloured the way sectional charts and radar displays do. */
lv_color_t altitudeColour(const model::Aircraft &aircraft)
{
    if (aircraft.on_ground) {
        return lv_color_hex(0x7E8CA0);
    }
    if (aircraft.alt_ft < 5000) {
        return lv_color_hex(0x46D3FF);
    }
    if (aircraft.alt_ft < 15000) {
        return lv_color_hex(0x38E08A);
    }
    if (aircraft.alt_ft < 25000) {
        return lv_color_hex(0xD8D24A);
    }
    return lv_color_hex(0xEFF3F8);
}

/** Nautical miles to pixels for the current range. */
float scale()
{
    const uint16_t radius_nm = app::settings().radius_nm;
    return (float)kPlotRadius / (float)(radius_nm == 0 ? 1 : radius_nm);
}

void toScreen(float x_nm, float y_nm, float &sx, float &sy)
{
    const float s = scale();
    sx = (float)kCentre + x_nm * s;
    sy = (float)kCentre - y_nm * s;      // north is up
}

bool onScreen(float sx, float sy)
{
    const float dx = sx - kCentre;
    const float dy = sy - kCentre;
    return (dx * dx + dy * dy) <= (float)(kPlotRadius * kPlotRadius);
}

void drawRings(lv_layer_t *layer)
{
    const uint16_t radius_nm = app::settings().radius_nm;

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center.x = kCentre;
    arc.center.y = kCentre;
    arc.start_angle = 0;
    arc.end_angle = 360;

    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = &lv_font_montserrat_12;
    label.color = lv_color_hex(kColourRingLabel);

    for (int step = 1; step <= 4; step++) {
        const int32_t radius_px = kPlotRadius * step / 4;
        arc.radius = radius_px;
        arc.width = (step == 4) ? 2 : 1;
        arc.color = lv_color_hex((step == 4) ? kColourRingMajor : kColourRing);
        lv_draw_arc(layer, &arc);

        // Range figure on the 45-degree diagonal, where blips rarely sit.
        char text[8];
        const int value = radius_nm * step / 4;
        snprintf(text, sizeof(text), "%d", value);
        const float diagonal = (float)radius_px * 0.7071f;
        lv_area_t area;
        area.x1 = (int32_t)(kCentre + diagonal) - 14;
        area.y1 = (int32_t)(kCentre - diagonal) - 7;
        area.x2 = area.x1 + 28;
        area.y2 = area.y1 + 14;
        label.text = text;
        label.align = LV_TEXT_ALIGN_CENTER;
        lv_draw_label(layer, &label, &area);
    }
}

void drawCardinals(lv_layer_t *layer)
{
    static const char *kNames[] = {"N", "E", "S", "W"};
    static const float kAngles[] = {0.0f, 90.0f, 180.0f, 270.0f};

    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = &lv_font_montserrat_20;
    label.align = LV_TEXT_ALIGN_CENTER;

    lv_draw_line_dsc_t tick;
    lv_draw_line_dsc_init(&tick);
    tick.color = lv_color_hex(kColourRing);
    tick.width = 1;

    // Minor ticks every 30 degrees.
    for (int degrees = 0; degrees < 360; degrees += 30) {
        const float radians = degrees * (float)DEG_TO_RAD;
        const float sin_a = sinf(radians);
        const float cos_a = cosf(radians);
        tick.p1.x = kCentre + sin_a * (kPlotRadius - 8);
        tick.p1.y = kCentre - cos_a * (kPlotRadius - 8);
        tick.p2.x = kCentre + sin_a * kPlotRadius;
        tick.p2.y = kCentre - cos_a * kPlotRadius;
        lv_draw_line(layer, &tick);
    }

    for (int i = 0; i < 4; i++) {
        const float radians = kAngles[i] * (float)DEG_TO_RAD;
        const int32_t x = (int32_t)(kCentre + sinf(radians) * (kPlotRadius + 22));
        const int32_t y = (int32_t)(kCentre - cosf(radians) * (kPlotRadius + 22));
        lv_area_t area = {x - 14, y - 12, x + 14, y + 12};
        label.text = kNames[i];
        label.color = lv_color_hex(i == 0 ? kColourNorth : kColourCardinal);
        lv_draw_label(layer, &label, &area);
    }
}

void drawTrail(lv_layer_t *layer, const model::Aircraft &aircraft, lv_color_t colour)
{
    if (aircraft.trail_len < 2) {
        return;
    }

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = colour;
    line.width = 2;
    line.round_start = 1;
    line.round_end = 1;

    // Fade in chunks rather than per segment: one draw call per segment for
    // every aircraft would cost more than the rest of the frame put together.
    const size_t count = aircraft.trail_len;
    for (size_t chunk = 0; chunk < kTrailChunks; chunk++) {
        const size_t start = count * chunk / kTrailChunks;
        const size_t end = count * (chunk + 1) / kTrailChunks;
        if (end <= start + 1) {
            continue;
        }
        line.opa = (lv_opa_t)(40 + 45 * chunk);      // oldest faintest

        for (size_t i = start; i + 1 < end + 1 && i + 1 < count; i++) {
            float ax, ay, bx, by, sx1, sy1, sx2, sy2;
            aircraft.trailPoint(i, ax, ay);
            aircraft.trailPoint(i + 1, bx, by);
            toScreen(ax, ay, sx1, sy1);
            toScreen(bx, by, sx2, sy2);
            if (!onScreen(sx1, sy1) || !onScreen(sx2, sy2)) {
                continue;
            }
            line.p1.x = sx1;
            line.p1.y = sy1;
            line.p2.x = sx2;
            line.p2.y = sy2;
            lv_draw_line(layer, &line);
        }
    }
}

void drawBlip(lv_layer_t *layer, float sx, float sy, float track_deg, lv_color_t colour,
              bool selected)
{
    // A chevron pointing along the track: nose, then the two trailing corners.
    const float radians = track_deg * (float)DEG_TO_RAD;
    const float sin_a = sinf(radians);
    const float cos_a = cosf(radians);
    auto rotate = [&](float x, float y, lv_point_precise_t &point) {
        point.x = sx + (x * cos_a + y * sin_a);
        point.y = sy + (x * sin_a - y * cos_a);
    };

    lv_draw_triangle_dsc_t triangle;
    lv_draw_triangle_dsc_init(&triangle);
    triangle.color = colour;
    triangle.opa = LV_OPA_COVER;
    rotate(0.0f, 9.0f, triangle.p[0]);      // nose
    rotate(-6.0f, -6.0f, triangle.p[1]);
    rotate(6.0f, -6.0f, triangle.p[2]);
    lv_draw_triangle(layer, &triangle);

    if (selected) {
        lv_draw_arc_dsc_t ring;
        lv_draw_arc_dsc_init(&ring);
        ring.center.x = (int32_t)sx;
        ring.center.y = (int32_t)sy;
        ring.radius = 18;
        ring.width = 2;
        ring.start_angle = 0;
        ring.end_angle = 360;
        ring.color = lv_color_hex(kColourSelected);
        lv_draw_arc(layer, &ring);
    }
}

void drawAircraft(lv_layer_t *layer)
{
    model::StoreGuard guard(20);
    if (!guard) {
        return;                 // the feed task is mid-update; skip a frame
    }

    model::Store &store = model::store();
    const uint32_t now_ms = millis();

    // Nearest first, so the label budget goes to the closest aircraft.
    size_t order[model::kMaxAircraft];
    size_t count = 0;
    for (size_t i = 0; i < store.size(); i++) {
        if (store.at(i).has_position) {
            order[count++] = i;
        }
    }
    for (size_t i = 1; i < count; i++) {
        const size_t key = order[i];
        const float key_distance = store.at(key).dst_nm;
        size_t j = i;
        while (j > 0 && store.at(order[j - 1]).dst_nm > key_distance) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }

    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = &lv_font_montserrat_12;
    label.align = LV_TEXT_ALIGN_LEFT;

    size_t labelled = 0;
    for (size_t index = 0; index < count; index++) {
        const model::Aircraft &aircraft = store.at(order[index]);
        float x_nm, y_nm, sx, sy;
        aircraft.positionAt(now_ms, x_nm, y_nm);
        toScreen(x_nm, y_nm, sx, sy);
        if (!onScreen(sx, sy)) {
            continue;
        }

        const lv_color_t colour = altitudeColour(aircraft);
        const bool selected = (aircraft.icao == g_selected_icao);

        drawTrail(layer, aircraft, colour);
        drawBlip(layer, sx, sy, aircraft.track_deg, colour, selected);

        if (labelled < kMaxLabels || selected) {
            labelled++;
            char text[24];
            const char *name = aircraft.flight[0] != '\0' ? aircraft.flight : aircraft.type;
            if (aircraft.on_ground) {
                snprintf(text, sizeof(text), "%s\nGND", name);
            } else {
                snprintf(text, sizeof(text), "%s\nFL%03d", name, (int)(aircraft.alt_ft / 100));
            }
            label.text = text;
            label.color = lv_color_hex(selected ? kColourSelected : kColourLabel);
            lv_area_t area = {(int32_t)sx + 12, (int32_t)sy - 8, (int32_t)sx + 92, (int32_t)sy + 22};
            lv_draw_label(layer, &label, &area);
        }
    }
}

void drawCentre(lv_layer_t *layer)
{
    lv_draw_arc_dsc_t dot;
    lv_draw_arc_dsc_init(&dot);
    dot.center.x = kCentre;
    dot.center.y = kCentre;
    dot.radius = 4;
    dot.width = 4;
    dot.start_angle = 0;
    dot.end_angle = 360;
    dot.color = lv_color_hex(kColourHome);
    lv_draw_arc(layer, &dot);

    const net::FeedStats feed = net::adsbStats();
    const uint32_t age_s = feed.last_success_ms == 0
        ? 0 : (millis() - feed.last_success_ms) / 1000;
    const bool stale = feed.last_success_ms == 0 || age_s > 20;

    char clock_text[12] = "--:--";
    time_t now = time(nullptr);
    if (now > 1700000000) {                 // NTP has landed
        struct tm local;
        localtime_r(&now, &local);
        strftime(clock_text, sizeof(clock_text), "%H:%M", &local);
    }

    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.align = LV_TEXT_ALIGN_CENTER;
    label.font = &lv_font_montserrat_20;
    label.color = lv_color_hex(kColourLabel);
    label.text = clock_text;
    lv_area_t clock_area = {kCentre - 60, kCentre + 16, kCentre + 60, kCentre + 42};
    lv_draw_label(layer, &label, &clock_area);

    char status[48];
    if (net::wifiState() == net::WifiState::PortalActive) {
        snprintf(status, sizeof(status), "setup: %s", net::wifiNetwork());
    } else if (stale) {
        snprintf(status, sizeof(status), "no data %us", (unsigned)age_s);
    } else {
        snprintf(status, sizeof(status), "%u aircraft  %u nm",
                 (unsigned)feed.accepted, (unsigned)app::settings().radius_nm);
    }
    label.font = &lv_font_montserrat_12;
    label.color = lv_color_hex(stale ? kColourStale : kColourRingLabel);
    label.text = status;
    lv_area_t status_area = {kCentre - 90, kCentre + 44, kCentre + 90, kCentre + 60};
    lv_draw_label(layer, &label, &status_area);

    // Transient range readout while the bezel is being dragged.
    if (millis() < g_range_hint_until) {
        char range[16];
        snprintf(range, sizeof(range), "%u nm", (unsigned)app::settings().radius_nm);
        label.font = &lv_font_montserrat_28;
        label.color = lv_color_hex(kColourHome);
        label.text = range;
        lv_area_t range_area = {kCentre - 80, kCentre - 70, kCentre + 80, kCentre - 34};
        lv_draw_label(layer, &label, &range_area);
    }
}

void drawFace(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    drawRings(layer);
    drawCardinals(layer);
    drawAircraft(layer);
    drawCentre(layer);
}

void changeRange(int direction)
{
    app::Settings &settings = app::settings();
    const size_t current = app::radiusPresetIndex(settings.radius_nm);
    int next = (int)current + direction;
    next = constrain(next, 0, (int)app::kRadiusPresetCount - 1);
    if ((size_t)next == current) {
        return;
    }
    settings.radius_nm = app::kRadiusPresets[next];
    settings.save();          // writes NVS, which stalls the panel's PSRAM reads
    displayResync();
    net::adsbRefreshNow();
    g_range_hint_until = millis() + 1500;
    app::logf("[ui] range %u nm", (unsigned)settings.radius_nm);
}

void onPressed(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    const float dx = (float)(point.x - kCentre);
    const float dy = (float)(point.y - kCentre);
    const float distance = sqrtf(dx * dx + dy * dy);

    g_bezel_drag = (distance >= kBezelInner);
    g_bezel_accumulated = 0.0f;
    g_bezel_last_angle = atan2f(dx, -dy) * (float)RAD_TO_DEG;
    LV_UNUSED(event);
}

void onPressing(lv_event_t *event)
{
    if (!g_bezel_drag) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    const float angle = atan2f((float)(point.x - kCentre), (float)(kCentre - point.y))
                        * (float)RAD_TO_DEG;
    float delta = angle - g_bezel_last_angle;
    while (delta > 180.0f)  delta -= 360.0f;
    while (delta < -180.0f) delta += 360.0f;
    g_bezel_last_angle = angle;
    g_bezel_accumulated += delta;

    // ~50 degrees of sweep per range step: far enough not to trigger by accident.
    while (g_bezel_accumulated >= 50.0f) {
        g_bezel_accumulated -= 50.0f;
        changeRange(1);
    }
    while (g_bezel_accumulated <= -50.0f) {
        g_bezel_accumulated += 50.0f;
        changeRange(-1);
    }
    LV_UNUSED(event);
}

void onClicked(lv_event_t *event)
{
    if (g_bezel_drag) {
        g_bezel_drag = false;
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    const float s = scale();
    const float x_nm = (float)(point.x - kCentre) / s;
    const float y_nm = (float)(kCentre - point.y) / s;

    uint32_t hit = 0;
    {
        model::StoreGuard guard(50);
        if (guard) {
            const int index = model::store().nearest(millis(), x_nm, y_nm, kTapSlopPx / s);
            if (index >= 0) {
                hit = model::store().at(index).icao;
            }
        }
    }

    g_selected_icao = hit;
    if (hit != 0) {
        detailSheetShow(hit);
    } else {
        detailSheetHide();
    }
    LV_UNUSED(event);
}

void refreshTimer(lv_timer_t *timer)
{
    if (g_face != nullptr) {
        lv_obj_invalidate(g_face);
    }
    detailSheetRefresh();
    LV_UNUSED(timer);
}

} // namespace

bool radarCreate()
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(kColourBackground), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    g_face = lv_obj_create(screen);
    lv_obj_remove_style_all(g_face);
    lv_obj_set_size(g_face, 480, 480);
    lv_obj_set_pos(g_face, 0, 0);
    lv_obj_add_flag(g_face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_face, drawFace, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(g_face, onPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(g_face, onPressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(g_face, onClicked, LV_EVENT_CLICKED, nullptr);

    detailSheetCreate(screen);

    g_timer = lv_timer_create(refreshTimer, kRefreshMs, nullptr);
    return g_face != nullptr;
}

uint32_t radarSelected()      { return g_selected_icao; }
void radarSelect(uint32_t icao) { g_selected_icao = icao; }
uint16_t radarRadiusNm()      { return app::settings().radius_nm; }

} // namespace ui
