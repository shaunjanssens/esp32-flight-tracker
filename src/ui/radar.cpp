#include "ui/radar.h"

#include <math.h>
#include <time.h>

#include "app/config.h"
#include "app/log.h"
#include "hw/display.h"
#include "hw/touch.h"
#include "model/aircraft.h"
#include "net/adsb_client.h"
#include "net/route_client.h"
#include "net/wifi_manager.h"

namespace ui {
namespace {

constexpr int32_t kCentre     = 240;
constexpr int32_t kPlotRadius = 196;
constexpr int32_t kBezelInner = 200;   // drags starting outside this change range
constexpr int32_t kTapSlopPx  = 34;
constexpr float   kDegreesPerStep = 50.0f;
constexpr size_t  kMaxLabels  = 8;

/** 24-bit hex to RGB565, so the palette reads like CSS. */
constexpr uint16_t rgb(uint32_t value)
{
    return (uint16_t)(((value & 0xF80000) >> 8) | ((value & 0x00FC00) >> 5) |
                      ((value & 0x0000F8) >> 3));
}

constexpr uint16_t kColourBackground = rgb(0x04070D);
constexpr uint16_t kColourRing       = rgb(0x14352A);
constexpr uint16_t kColourRingMajor  = rgb(0x1E4D3B);
constexpr uint16_t kColourRingLabel  = rgb(0x3E5C50);
constexpr uint16_t kColourNorth      = rgb(0xE8F0A0);
constexpr uint16_t kColourCardinal   = rgb(0x5C7A6E);
constexpr uint16_t kColourHome       = rgb(0x38E08A);
constexpr uint16_t kColourLabel      = rgb(0xAFC4D4);
constexpr uint16_t kColourDim        = rgb(0x6E8AA6);
constexpr uint16_t kColourStale      = rgb(0xE0603C);
constexpr uint16_t kColourSelected   = rgb(0xFFFFFF);
constexpr uint16_t kColourPanel      = rgb(0x0C121C);
constexpr uint16_t kColourPanelEdge  = rgb(0x24344A);

LGFX_Sprite g_grid(&display);
LGFX_Sprite g_frame(&display);
bool g_ready = false;
bool g_grid_dirty = true;
bool g_paused = false;

uint16_t g_refresh_ms = 250;
uint32_t g_next_draw_ms = 0;
uint32_t g_selected_icao = 0;
uint32_t g_range_hint_until = 0;

/**
 * Network details on the glass.
 *
 * Shown for the first 20 s of every boot, and on demand by tapping the centre
 * dot. Without it the only way to find the device is mDNS, which is exactly
 * what fails on the networks where you most need to know its address.
 */
uint32_t g_info_until = 0;
constexpr uint32_t kInfoBootMs = 20000;
constexpr uint32_t kInfoTapMs = 20000;
constexpr int32_t  kCentreTapRadius = 40;

// Touch gesture state
bool  g_pressed = false;
bool  g_bezel_drag = false;
float g_bezel_last_angle = 0.0f;
float g_bezel_accumulated = 0.0f;
int16_t g_press_x = 0, g_press_y = 0;
uint32_t g_press_ms = 0;

uint16_t altitudeColour(const model::Aircraft &aircraft)
{
    if (aircraft.on_ground)        return rgb(0x7E8CA0);
    if (aircraft.alt_ft < 5000)    return rgb(0x46D3FF);
    if (aircraft.alt_ft < 15000)   return rgb(0x38E08A);
    if (aircraft.alt_ft < 25000)   return rgb(0xD8D24A);
    return rgb(0xEFF3F8);
}

float scale()
{
    const uint16_t radius_nm = app::settings().radius_nm;
    return (float)kPlotRadius / (float)(radius_nm == 0 ? 1 : radius_nm);
}

void toScreen(float x_nm, float y_nm, float &sx, float &sy)
{
    const float s = scale();
    sx = (float)kCentre + x_nm * s;
    sy = (float)kCentre - y_nm * s;      // north up
}

bool onScreen(float sx, float sy)
{
    const float dx = sx - kCentre;
    const float dy = sy - kCentre;
    return (dx * dx + dy * dy) <= (float)(kPlotRadius * kPlotRadius);
}

/**
 * The rings, ticks and cardinal letters never change between range steps, so
 * they are drawn once into their own sprite and copied in as the background of
 * every frame.
 */
void drawGrid()
{
    const uint16_t radius_nm = app::settings().radius_nm;

    g_grid.fillScreen(kColourBackground);

    for (int step = 1; step <= 4; step++) {
        const int32_t radius_px = kPlotRadius * step / 4;
        const uint16_t colour = (step == 4) ? kColourRingMajor : kColourRing;
        // Smooth variants throughout: plain drawCircle/drawLine are aliased,
        // and on a round face every ring is a diagonal somewhere.
        g_grid.drawArc(kCentre, kCentre, radius_px - (step == 4 ? 1 : 0), radius_px,
                       0.0f, 360.0f, colour);

        const float diagonal = (float)radius_px * 0.7071f;
        g_grid.setFont(&fonts::FreeSans9pt7b);
        g_grid.setTextDatum(textdatum_t::middle_center);
        g_grid.setTextColor(kColourRingLabel, kColourBackground);
        g_grid.drawNumber(radius_nm * step / 4,
                          (int32_t)(kCentre + diagonal), (int32_t)(kCentre - diagonal));
    }

    for (int degrees = 0; degrees < 360; degrees += 30) {
        const float radians = degrees * (float)DEG_TO_RAD;
        const float sin_a = sinf(radians);
        const float cos_a = cosf(radians);
        g_grid.drawWideLine((int32_t)(kCentre + sin_a * (kPlotRadius - 8)),
                            (int32_t)(kCentre - cos_a * (kPlotRadius - 8)),
                            (int32_t)(kCentre + sin_a * kPlotRadius),
                            (int32_t)(kCentre - cos_a * kPlotRadius), 0.6f, kColourRing);
    }

    static const char *kNames[] = {"N", "E", "S", "W"};
    static const int kAngles[] = {0, 90, 180, 270};
    g_grid.setFont(&fonts::FreeSansBold12pt7b);
    g_grid.setTextDatum(textdatum_t::middle_center);
    for (int i = 0; i < 4; i++) {
        const float radians = kAngles[i] * (float)DEG_TO_RAD;
        g_grid.setTextColor(i == 0 ? kColourNorth : kColourCardinal, kColourBackground);
        g_grid.drawString(kNames[i],
                          (int32_t)(kCentre + sinf(radians) * (kPlotRadius + 22)),
                          (int32_t)(kCentre - cosf(radians) * (kPlotRadius + 22)));
    }

    g_grid.fillSmoothCircle(kCentre, kCentre, 4, kColourHome);
}

/*
 * Per-frame drawing uses plain aliased primitives on purpose.
 *
 * Anti-aliased drawing blends against what is already there, so every pixel is
 * a PSRAM read *and* a write. The panel is simultaneously streaming its
 * framebuffer out of that same PSRAM, and the contention starves the refresh:
 * smooth trails made the flicker dramatically worse. The static grid below is
 * drawn once per range change, so it can afford the smooth versions.
 */
void drawTrail(const model::Aircraft &aircraft, uint16_t colour)
{
    if (aircraft.trail_len < 2) {
        return;
    }
    // Older points are drawn darker by blending toward the background.
    const size_t stride = aircraft.trail_len > 24 ? 2 : 1;
    for (size_t i = 0; i + stride < aircraft.trail_len; i += stride) {
        float ax, ay, bx, by, sx1, sy1, sx2, sy2;
        aircraft.trailPoint(i, ax, ay);
        aircraft.trailPoint(i + stride, bx, by);
        toScreen(ax, ay, sx1, sy1);
        toScreen(bx, by, sx2, sy2);
        if (!onScreen(sx1, sy1) || !onScreen(sx2, sy2)) {
            continue;
        }
        const uint8_t fade = (uint8_t)(40 + (200 * i) / aircraft.trail_len);
        g_frame.drawLine((int32_t)sx1, (int32_t)sy1, (int32_t)sx2, (int32_t)sy2,
                         g_frame.color565((fade * ((colour >> 11) & 0x1F)) / 31,
                                          (fade * ((colour >> 5) & 0x3F)) / 63,
                                          (fade * (colour & 0x1F)) / 31));
    }
}

void drawBlip(float sx, float sy, float track_deg, uint16_t colour, bool selected)
{
    const float radians = track_deg * (float)DEG_TO_RAD;
    const float sin_a = sinf(radians);
    const float cos_a = cosf(radians);
    auto rotate = [&](float x, float y, int32_t &out_x, int32_t &out_y) {
        out_x = (int32_t)(sx + (x * cos_a + y * sin_a));
        out_y = (int32_t)(sy + (x * sin_a - y * cos_a));
    };

    int32_t nose_x, nose_y, left_x, left_y, right_x, right_y;
    rotate(0.0f, 9.0f, nose_x, nose_y);
    rotate(-6.0f, -6.0f, left_x, left_y);
    rotate(6.0f, -6.0f, right_x, right_y);
    g_frame.fillTriangle(nose_x, nose_y, left_x, left_y, right_x, right_y, colour);

    if (selected) {
        g_frame.drawCircle((int32_t)sx, (int32_t)sy, 18, kColourSelected);
    }
}

void drawAircraft()
{
    model::StoreGuard guard(20);
    if (!guard) {
        return;
    }
    model::Store &store = model::store();
    const uint32_t now_ms = millis();

    size_t order[model::kMaxAircraft];
    size_t count = 0;
    for (size_t i = 0; i < store.size(); i++) {
        if (store.at(i).has_position) {
            order[count++] = i;
        }
    }
    for (size_t i = 1; i < count; i++) {                 // nearest first
        const size_t key = order[i];
        const float key_distance = store.at(key).dst_nm;
        size_t j = i;
        while (j > 0 && store.at(order[j - 1]).dst_nm > key_distance) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }

    g_frame.setFont(&fonts::FreeSans9pt7b);
    g_frame.setTextDatum(textdatum_t::top_left);

    size_t labelled = 0;
    for (size_t index = 0; index < count; index++) {
        const model::Aircraft &aircraft = store.at(order[index]);
        float x_nm, y_nm, sx, sy;
        aircraft.positionAt(now_ms, x_nm, y_nm);
        toScreen(x_nm, y_nm, sx, sy);
        if (!onScreen(sx, sy)) {
            continue;
        }

        const uint16_t colour = altitudeColour(aircraft);
        const bool selected = (aircraft.icao == g_selected_icao);
        drawTrail(aircraft, colour);
        drawBlip(sx, sy, aircraft.track_deg, colour, selected);

        if (labelled < kMaxLabels || selected) {
            labelled++;
            const char *name = aircraft.flight[0] != '\0' ? aircraft.flight : aircraft.type;
            char altitude[12];
            if (aircraft.on_ground) {
                snprintf(altitude, sizeof(altitude), "GND");
            } else {
                snprintf(altitude, sizeof(altitude), "FL%03d", (int)(aircraft.alt_ft / 100));
            }
            g_frame.setTextColor(selected ? kColourSelected : kColourLabel);
            g_frame.drawString(name, (int32_t)sx + 13, (int32_t)sy - 11);
            g_frame.setTextColor(kColourDim);
            g_frame.drawString(altitude, (int32_t)sx + 13, (int32_t)sy + 4);
        }
    }
}

/**
 * Only the things that mean something is wrong.
 *
 * The clock, the aircraft count and the range readout all lived here and are
 * gone on purpose: the rings are already labelled in nm, and a count is
 * information the picture itself carries. What is left is the state you cannot
 * infer by looking - the feed being stale, or the device waiting for setup.
 */
/** SSID, address and signal, so the device can be reached without mDNS. */
void drawNetworkInfo()
{
    const bool booting = millis() < kInfoBootMs;
    if (!booting && (g_info_until == 0 || millis() > g_info_until)) {
        return;
    }

    const char *network = net::wifiNetwork();
    const char *address = net::wifiAddress();

    g_frame.setTextDatum(textdatum_t::middle_center);
    g_frame.setFont(&fonts::FreeSansBold12pt7b);
    g_frame.setTextColor(kColourHome);
    g_frame.drawString(address[0] != '\0' ? address : "no address", kCentre, kCentre - 96);

    g_frame.setFont(&fonts::FreeSans9pt7b);
    g_frame.setTextColor(kColourDim);
    char line[64];
    if (net::wifiState() == net::WifiState::PortalActive) {
        snprintf(line, sizeof(line), "join %s to set up", network);
    } else {
        snprintf(line, sizeof(line), "%s   %d dBm", network, net::wifiRssi());
    }
    g_frame.drawString(line, kCentre, kCentre - 72);
}

void drawCentre()
{
    const net::FeedStats feed = net::adsbStats();
    const uint32_t age_s = feed.last_success_ms == 0
        ? 0 : (millis() - feed.last_success_ms) / 1000;
    const bool stale = feed.last_success_ms == 0 || age_s > 20;

    if (net::wifiState() != net::WifiState::PortalActive && !stale) {
        return;
    }

    char status[48];
    if (net::wifiState() == net::WifiState::PortalActive) {
        snprintf(status, sizeof(status), "setup: %s", net::wifiNetwork());
    } else {
        snprintf(status, sizeof(status), "no data %us", (unsigned)age_s);
    }

    g_frame.setTextDatum(textdatum_t::middle_center);
    g_frame.setFont(&fonts::FreeSans9pt7b);
    g_frame.setTextColor(kColourStale);
    g_frame.drawString(status, kCentre, kCentre + 34);
}

/**
 * The detail panel is the circular segment below a chord: its sides and bottom
 * are the screen edge itself, so the background runs out to the glass with no
 * sliver of radar left showing around it.
 */
constexpr int32_t kPanelTop = 288;
constexpr int32_t kScreenRadius = 240;

/** Half-width of the panel at row `y`, following the screen edge exactly. */
int32_t panelHalfWidth(int32_t y)
{
    const float dy = (float)(y - kCentre);
    const float squared = (float)(kScreenRadius * kScreenRadius) - dy * dy;
    return squared <= 0.0f ? 0 : (int32_t)sqrtf(squared);
}

bool insidePanel(int32_t x, int32_t y)
{
    if (y < kPanelTop) {
        return false;
    }
    return abs(x - kCentre) <= panelHalfWidth(y);
}

/**
 * Detail panel for the selected aircraft.
 *
 * Drawn as horizontal spans whose width follows the circle rather than as a
 * rectangle: on a round 480x480 panel a box wide enough to read runs off the
 * glass at this height, taking its text with it.
 */
void drawDetail()
{
    if (g_selected_icao == 0) {
        return;
    }

    model::Aircraft copy;
    bool found = false;
    {
        model::StoreGuard guard(20);
        if (guard) {
            if (const model::Aircraft *aircraft = model::store().find(g_selected_icao)) {
                copy = *aircraft;
                found = true;
            }
        }
    }
    if (!found) {
        return;
    }

    for (int32_t y = kPanelTop; y < hw::kScreenHeight; y++) {
        const int32_t half = panelHalfWidth(y);
        if (half <= 0) {
            continue;
        }
        g_frame.drawFastHLine(kCentre - half, y, half * 2, kColourPanel);
    }
    // A bright chord along the top is the only edge it needs; the other three
    // sides are the bezel.
    const int32_t top_half = panelHalfWidth(kPanelTop);
    g_frame.drawFastHLine(kCentre - top_half, kPanelTop, top_half * 2, kColourPanelEdge);

    const net::RouteInfo *route = net::routeLookup(copy.icao, copy.flight);

    g_frame.setTextDatum(textdatum_t::middle_center);
    g_frame.setFont(&fonts::FreeSansBold12pt7b);
    g_frame.setTextColor(kColourSelected);
    g_frame.drawString(copy.flight[0] != '\0' ? copy.flight : "unknown", kCentre, kPanelTop + 22);

    g_frame.setFont(&fonts::FreeSans9pt7b);
    if (route != nullptr && route->route_known) {
        char line[48];
        snprintf(line, sizeof(line), "%s  >  %s", route->origin_iata, route->dest_iata);
        g_frame.setTextColor(kColourHome);
        g_frame.drawString(line, kCentre, kPanelTop + 50);

        snprintf(line, sizeof(line), "%s - %s", route->origin_city, route->dest_city);
        g_frame.setTextColor(kColourDim);
        g_frame.drawString(line, kCentre, kPanelTop + 72);
    } else if (route != nullptr && route->pending) {
        g_frame.setTextColor(kColourDim);
        g_frame.drawString("looking up route...", kCentre, kPanelTop + 50);
    } else {
        g_frame.setTextColor(kColourDim);
        g_frame.drawString("no route on file", kCentre, kPanelTop + 50);
    }

    char identity[52];
    if (route != nullptr && route->aircraft_known && route->type_name[0] != '\0') {
        snprintf(identity, sizeof(identity), "%s  %s", route->type_name, copy.reg);
    } else {
        snprintf(identity, sizeof(identity), "%s  %s",
                 copy.type[0] != '\0' ? copy.type : "type unknown", copy.reg);
    }
    g_frame.setTextColor(kColourLabel);
    g_frame.drawString(identity, kCentre, kPanelTop + 96);

    char figures[64];
    const char *trend = copy.vs_fpm > 200 ? "climbing" : (copy.vs_fpm < -200 ? "descending" : "level");
    snprintf(figures, sizeof(figures), "FL%03d %s   %d kt   %.1f nm   %03d",
             (int)(copy.alt_ft / 100), trend, (int)copy.gs_kt, copy.dst_nm, (int)copy.dir_deg);
    g_frame.setTextColor(kColourDim);
    g_frame.drawString(figures, kCentre, kPanelTop + 120);
}

void renderFrame()
{
    if (g_grid_dirty) {
        drawGrid();
        g_grid_dirty = false;
    }
    g_grid.pushSprite(&g_frame, 0, 0);
    drawAircraft();
    drawCentre();
    drawNetworkInfo();
    drawDetail();
    g_frame.pushSprite(0, 0);
}

void changeRange(int direction)
{
    app::Settings &settings = app::settings();
    const size_t current = app::radiusPresetIndex(settings.radius_nm);
    int next = constrain((int)current + direction, 0, (int)app::kRadiusPresetCount - 1);
    if ((size_t)next == current) {
        return;
    }
    settings.radius_nm = app::kRadiusPresets[next];
    settings.saveSoon();
    net::adsbRefreshNow();
    g_grid_dirty = true;               // ring labels changed
    g_range_hint_until = millis() + 1500;
    app::logf("[ui] range %u nm", (unsigned)settings.radius_nm);
}

void handleTouch()
{
    hw::TouchState touch;
    if (!hw::touchRead(touch)) {
        return;
    }

    if (touch.down && !g_pressed) {
        g_pressed = true;
        g_press_x = touch.x;
        g_press_y = touch.y;
        g_press_ms = millis();
        const float dx = (float)(touch.x - kCentre);
        const float dy = (float)(touch.y - kCentre);
        g_bezel_drag = sqrtf(dx * dx + dy * dy) >= (float)kBezelInner;
        g_bezel_accumulated = 0.0f;
        g_bezel_last_angle = atan2f(dx, -dy) * (float)RAD_TO_DEG;
        return;
    }

    if (touch.down && g_pressed && g_bezel_drag) {
        const float angle = atan2f((float)(touch.x - kCentre), (float)(kCentre - touch.y))
                            * (float)RAD_TO_DEG;
        float delta = angle - g_bezel_last_angle;
        while (delta > 180.0f)  delta -= 360.0f;
        while (delta < -180.0f) delta += 360.0f;
        g_bezel_last_angle = angle;
        g_bezel_accumulated += delta;

        while (g_bezel_accumulated >= kDegreesPerStep) {
            g_bezel_accumulated -= kDegreesPerStep;
            changeRange(1);
        }
        while (g_bezel_accumulated <= -kDegreesPerStep) {
            g_bezel_accumulated += kDegreesPerStep;
            changeRange(-1);
        }
        return;
    }

    if (!touch.down && g_pressed) {
        g_pressed = false;
        if (g_bezel_drag) {
            g_bezel_drag = false;
            return;
        }
        // A tap on the open panel dismisses it, without hunting for an
        // aircraft underneath it.
        if (g_selected_icao != 0 && insidePanel(g_press_x, g_press_y)) {
            g_selected_icao = 0;
            return;
        }

        // Tapping the home dot summons the network details.
        const int32_t dx = g_press_x - kCentre;
        const int32_t dy = g_press_y - kCentre;
        if (dx * dx + dy * dy <= kCentreTapRadius * kCentreTapRadius) {
            const bool showing = millis() < kInfoBootMs ||
                                 (g_info_until != 0 && millis() < g_info_until);
            g_info_until = showing ? 0 : millis() + kInfoTapMs;
            return;
        }

        const float s = scale();
        const float x_nm = (float)(g_press_x - kCentre) / s;
        const float y_nm = (float)(kCentre - g_press_y) / s;

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
        g_selected_icao = (hit != 0 && hit != g_selected_icao) ? hit : 0;
    }
}

} // namespace

bool radarBegin()
{
    g_refresh_ms = app::settings().refresh_ms;

    g_grid.setColorDepth(16);
    g_grid.setPsram(true);
    g_frame.setColorDepth(16);
    g_frame.setPsram(true);

    if (g_grid.createSprite(hw::kScreenWidth, hw::kScreenHeight) == nullptr ||
        g_frame.createSprite(hw::kScreenWidth, hw::kScreenHeight) == nullptr) {
        app::logf("[radar] sprite allocation failed (free PSRAM %u)",
                  (unsigned)ESP.getFreePsram());
        return false;
    }

    g_ready = true;
    g_grid_dirty = true;
    app::logf("[radar] two %dx%d sprites in PSRAM", hw::kScreenWidth, hw::kScreenHeight);
    return true;
}

void radarTick()
{
    if (!g_ready) {
        return;
    }
    handleTouch();

    if (g_paused || (int32_t)(millis() - g_next_draw_ms) < 0) {
        return;
    }
    g_next_draw_ms = millis() + g_refresh_ms;
    renderFrame();
}

void radarPause(bool paused)  { g_paused = paused; }
bool radarPaused()            { return g_paused; }
void radarSetRefreshMs(uint16_t ms) { g_refresh_ms = constrain(ms, (uint16_t)40, (uint16_t)2000); }
uint32_t radarSelected()      { return g_selected_icao; }

} // namespace ui
