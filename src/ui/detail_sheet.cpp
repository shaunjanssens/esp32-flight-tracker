#include "detail_sheet.h"

#include "model/aircraft.h"
#include "net/route_client.h"
#include "ui/radar_view.h"

namespace ui {
namespace {

// The panel covers the lower part of the round screen. Anything below y=440 is
// in the narrow part of the circle, so the layout keeps its distance.
constexpr int32_t kPanelTop = 168;
constexpr int32_t kPanelHeight = 480 - kPanelTop;
constexpr uint32_t kSlideMs = 220;

lv_obj_t *g_panel = nullptr;
lv_obj_t *g_callsign = nullptr;
lv_obj_t *g_operator = nullptr;
lv_obj_t *g_route = nullptr;
lv_obj_t *g_cities = nullptr;
lv_obj_t *g_airframe = nullptr;
lv_obj_t *g_telemetry = nullptr;
lv_obj_t *g_hint = nullptr;

uint32_t g_icao = 0;
bool g_visible = false;

lv_obj_t *addLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t colour)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(colour), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, 320);
    lv_label_set_text(label, "");
    return label;
}

void onPanelClicked(lv_event_t *event)
{
    detailSheetHide();
    radarSelect(0);
    LV_UNUSED(event);
}

void slideTo(int32_t y)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, g_panel);
    lv_anim_set_values(&anim, lv_obj_get_y(g_panel), y);
    lv_anim_set_duration(&anim, kSlideMs);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, [](void *object, int32_t value) {
        lv_obj_set_y(static_cast<lv_obj_t *>(object), value);
    });
    lv_anim_start(&anim);
}

} // namespace

void detailSheetCreate(lv_obj_t *parent)
{
    g_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(g_panel);
    lv_obj_set_size(g_panel, 480, kPanelHeight);
    lv_obj_set_pos(g_panel, 0, 480);
    lv_obj_set_style_bg_color(g_panel, lv_color_hex(0x080D15), 0);
    lv_obj_set_style_bg_opa(g_panel, LV_OPA_90, 0);
    lv_obj_set_style_radius(g_panel, 44, 0);
    lv_obj_set_style_border_color(g_panel, lv_color_hex(0x1E4D3B), 0);
    lv_obj_set_style_border_width(g_panel, 1, 0);
    lv_obj_set_style_border_side(g_panel, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(g_panel, 14, 0);
    lv_obj_set_flex_flow(g_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_panel, 4, 0);
    lv_obj_add_flag(g_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_panel, onPanelClicked, LV_EVENT_CLICKED, nullptr);

    g_callsign  = addLabel(g_panel, &lv_font_montserrat_28, 0xEFF3F8);
    g_operator  = addLabel(g_panel, &lv_font_montserrat_16, 0x8AA6C0);
    g_route     = addLabel(g_panel, &lv_font_montserrat_20, 0x38E08A);
    g_cities    = addLabel(g_panel, &lv_font_montserrat_12, 0x6E8AA6);
    g_airframe  = addLabel(g_panel, &lv_font_montserrat_16, 0xAFC4D4);
    g_telemetry = addLabel(g_panel, &lv_font_montserrat_16, 0xD8D24A);
    g_hint      = addLabel(g_panel, &lv_font_montserrat_12, 0x3E5C50);
    lv_label_set_text(g_hint, "tap to close");
}

void detailSheetShow(uint32_t icao)
{
    if (g_panel == nullptr) {
        return;
    }
    g_icao = icao;
    g_visible = true;
    lv_obj_remove_flag(g_panel, LV_OBJ_FLAG_HIDDEN);
    slideTo(kPanelTop);
    detailSheetRefresh();
}

void detailSheetHide()
{
    if (g_panel == nullptr || !g_visible) {
        return;
    }
    g_visible = false;
    g_icao = 0;
    slideTo(480);
    lv_obj_add_flag(g_panel, LV_OBJ_FLAG_HIDDEN);
}

bool detailSheetVisible()
{
    return g_visible;
}

void detailSheetRefresh()
{
    if (!g_visible || g_icao == 0) {
        return;
    }

    // Copy what we need out of the store, then release it: the feed task must
    // not wait on the UI.
    model::Aircraft snapshot;
    bool present = false;
    {
        model::StoreGuard guard(20);
        if (!guard) {
            return;
        }
        if (const model::Aircraft *aircraft = model::store().find(g_icao)) {
            snapshot = *aircraft;
            present = true;
        }
    }

    if (!present) {
        // It left the radar. Keep the panel up but say so, rather than yanking
        // it away from under a finger.
        lv_obj_set_style_text_color(g_callsign, lv_color_hex(0x6E8AA6), 0);
        lv_label_set_text(g_telemetry, "out of range");
        return;
    }
    lv_obj_set_style_text_color(g_callsign, lv_color_hex(0xEFF3F8), 0);

    char hex[8];
    snprintf(hex, sizeof(hex), "%06lX", (unsigned long)snapshot.icao);
    lv_label_set_text(g_callsign, snapshot.flight[0] != '\0' ? snapshot.flight : hex);

    if (snapshot.on_ground) {
        lv_label_set_text_fmt(g_telemetry, "on ground  %d kt  %.1f nm  %03d deg",
                              (int)snapshot.gs_kt, snapshot.dst_nm, (int)snapshot.dir_deg);
    } else {
        const char *arrow = snapshot.vs_fpm > 200 ? " climbing" :
                            (snapshot.vs_fpm < -200 ? " descending" : "");
        lv_label_set_text_fmt(g_telemetry, "FL%03d%s  %d kt  %.1f nm  %03d deg",
                              (int)(snapshot.alt_ft / 100), arrow, (int)snapshot.gs_kt,
                              snapshot.dst_nm, (int)snapshot.dir_deg);
    }

    const net::RouteInfo *info = net::routeLookup(snapshot.icao, snapshot.flight);
    if (info == nullptr || info->pending) {
        lv_label_set_text(g_route, "...");
        lv_label_set_text(g_cities, "looking up route");
        lv_label_set_text_fmt(g_airframe, "%s %s",
                              snapshot.type[0] != '\0' ? snapshot.type : "unknown type",
                              snapshot.reg);
        lv_label_set_text(g_operator, "");
        return;
    }

    if (info->route_known) {
        lv_label_set_text_fmt(g_route, "%s  >  %s",
                              info->origin_iata[0] != '\0' ? info->origin_iata : "?",
                              info->dest_iata[0] != '\0' ? info->dest_iata : "?");
        lv_label_set_text_fmt(g_cities, "%s to %s", info->origin_city, info->dest_city);
    } else {
        lv_label_set_text(g_route, "no filed route");
        lv_label_set_text(g_cities, "");
    }

    lv_label_set_text(g_operator, info->airline[0] != '\0' ? info->airline : info->owner);

    if (info->type_name[0] != '\0') {
        lv_label_set_text_fmt(g_airframe, "%s  %s", info->type_name,
                              info->registration[0] != '\0' ? info->registration : snapshot.reg);
    } else {
        lv_label_set_text_fmt(g_airframe, "%s  %s",
                              snapshot.type[0] != '\0' ? snapshot.type : "unknown type",
                              snapshot.reg);
    }
}

} // namespace ui
