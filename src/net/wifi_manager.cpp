#include "wifi_manager.h"

#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "app/config.h"
#include "app/log.h"
#include "model/aircraft.h"
#include "net/adsb_client.h"
#include "hw/display.h"
#include "hw/touch.h"
#include "ui/radar.h"

/** Defined in main.cpp: arms a radios-off diagnostic boot. */
void requestDiagnosticBoot(uint32_t seconds);

namespace net {
namespace {

constexpr char     kWifiNamespace[] = "wifi";
constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint8_t  kDnsPort = 53;
constexpr int      kTaskCore = 0;
constexpr int      kTaskPriority = 1;
constexpr int      kTaskStack = 8 * 1024;

WebServer  g_server(80);
DNSServer  g_dns;
WifiState  g_state = WifiState::Idle;
char       g_network[33] = "";
char       g_address[20] = "";
bool       g_portal = false;
bool       g_ota_ready = false;

const char kPageStyle[] PROGMEM = R"CSS(
<meta name=viewport content="width=device-width,initial-scale=1">
<link rel=stylesheet href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<style>
:root{color-scheme:dark}
body{margin:0;padding:24px 16px 48px;background:#0a0e14;color:#dbe4ef;
     font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
main{max-width:460px;margin:0 auto}
h1{font-size:20px;letter-spacing:.14em;text-transform:uppercase;color:#38e08a;margin:0 0 4px}
p.sub{margin:0 0 24px;color:#6e8aa6;font-size:13px}
fieldset{border:1px solid #1d2a3a;border-radius:10px;margin:0 0 16px;padding:14px 16px}
legend{padding:0 6px;color:#8aa6c0;font-size:12px;letter-spacing:.1em;text-transform:uppercase}
label{display:block;margin:14px 0 4px;font-size:13px;color:#9fb3c8}
label:first-of-type{margin-top:4px}
input[type=text],input[type=password],input[type=number],select{
     width:100%;box-sizing:border-box;padding:10px;border-radius:8px;
     border:1px solid #24344a;background:#111823;color:#dbe4ef;font-size:15px}
input[type=range]{width:100%;margin:6px 0}
.check{display:flex;align-items:center;gap:10px;margin:14px 0 4px;
     font-size:14px;color:#dbe4ef}
.check input{width:20px;height:20px;flex:0 0 auto;accent-color:#38e08a;margin:0}
.row{display:flex;gap:10px}.row>div{flex:1}
button{width:100%;margin-top:14px;padding:12px;border:0;border-radius:8px;
     background:#38e08a;color:#04070d;font-size:16px;font-weight:600}
button.ghost{background:#1d2a3a;color:#9fb3c8}
.hint{font-size:12px;color:#5c7080;margin:6px 0 0}
.stat{display:flex;justify-content:space-between;padding:5px 0;border-bottom:1px solid #131c28;font-size:14px}
.stat span:last-child{color:#8aa6c0;font-variant-numeric:tabular-nums}
#map{height:220px;border-radius:10px;overflow:hidden;margin:10px 0;background:#111823}
.leaflet-container{background:#111823}
</style>
)CSS";

/** Map picker. Tiles come from the browser's own internet access, not ours. */
const char kMapScript[] PROGMEM = R"HTML(
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script>
(function(){
  var latEl=document.getElementById('lat'), lonEl=document.getElementById('lon');
  var box=document.getElementById('map');
  if(!window.L||!latEl||!box){ if(box) box.style.display='none'; return; }
  var lat=parseFloat(latEl.value)||51.0, lon=parseFloat(lonEl.value)||4.0;
  var map=L.map(box,{attributionControl:false}).setView([lat,lon],10);
  L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:18}).addTo(map);
  var pin=L.marker([lat,lon],{draggable:true}).addTo(map);
  var ring=L.circle([lat,lon],{radius:46300,color:'#38e08a',weight:1,fillOpacity:0.05}).addTo(map);
  function put(ll){
    latEl.value=ll.lat.toFixed(5); lonEl.value=ll.lng.toFixed(5);
    ring.setLatLng(ll);
  }
  pin.on('drag',function(){ put(pin.getLatLng()); });
  map.on('click',function(e){ pin.setLatLng(e.latlng); put(e.latlng); });
  function fromFields(){
    var a=parseFloat(latEl.value), b=parseFloat(lonEl.value);
    if(isNaN(a)||isNaN(b)) return;
    pin.setLatLng([a,b]); ring.setLatLng([a,b]); map.panTo([a,b]);
  }
  latEl.addEventListener('change',fromFields);
  lonEl.addEventListener('change',fromFields);
})();
</script>
)HTML";

void appendNumber(String &html, const char *name, const char *label, long value,
                  long minimum, long maximum, const char *hint = nullptr)
{
    html += F("<label for=");
    html += name;
    html += F(">");
    html += label;
    html += F("</label><input id=");
    html += name;
    html += F(" name=");
    html += name;
    html += F(" type=number min=");
    html += String(minimum);
    html += F(" max=");
    html += String(maximum);
    html += F(" value='");
    html += String(value);
    html += F("'>");
    if (hint != nullptr) {
        html += F("<p class=hint>");
        html += hint;
        html += F("</p>");
    }
}

void appendCheckbox(String &html, const char *name, const char *label, bool checked)
{
    html += F("<div class=check><input type=checkbox id=");
    html += name;
    html += F(" name=");
    html += name;
    html += F(" value=1");
    html += checked ? F(" checked") : F("");
    html += F("><label for=");
    html += name;
    html += F(" style='margin:0;color:inherit;font-size:14px'>");
    html += label;
    html += F("</label></div>");
}

/** `options` is a NULL-terminated list; the value posted is the index. */
void appendSelect(String &html, const char *name, const char *label,
                  const char *const *options, size_t count, size_t selected,
                  const char *hint = nullptr)
{
    html += F("<label for=");
    html += name;
    html += F(">");
    html += label;
    html += F("</label><select id=");
    html += name;
    html += F(" name=");
    html += name;
    html += F(">");
    for (size_t i = 0; i < count; i++) {
        html += F("<option value=");
        html += String(i);
        if (i == selected) {
            html += F(" selected");
        }
        html += F(">");
        html += options[i];
        html += F("</option>");
    }
    html += F("</select>");
    if (hint != nullptr) {
        html += F("<p class=hint>");
        html += hint;
        html += F("</p>");
    }
}

void appendPositionFields(String &html)
{
    const app::Settings &settings = app::settings();
    html += F("<fieldset><legend>Home position</legend>"
              "<div class=row>"
              "<div><label for=lat>Latitude</label>"
              "<input id=lat name=lat type=number step=0.00001 min=-90 max=90 value='");
    html += String(settings.home_lat, 5);
    html += F("'></div><div><label for=lon>Longitude</label>"
              "<input id=lon name=lon type=number step=0.00001 min=-180 max=180 value='");
    html += String(settings.home_lon, 5);
    html += F("'></div></div><div id=map></div>"
              "<p class=hint>Drag the pin or tap the map. The circle is roughly 25 nm. "
              "Needs internet in this browser; the fields work without it.</p></fieldset>");
}

void appendRadarFields(String &html)
{
    const app::Settings &settings = app::settings();

    html += F("<fieldset><legend>Radar</legend><label for=radius>Range</label>"
              "<select id=radius name=radius>");
    for (size_t i = 0; i < app::kRadiusPresetCount; i++) {
        html += F("<option value=");
        html += String(app::kRadiusPresets[i]);
        if (app::kRadiusPresets[i] == settings.radius_nm) {
            html += F(" selected");
        }
        html += F(">");
        html += String(app::kRadiusPresets[i]);
        html += F(" nm</option>");
    }
    html += F("</select>");

    html += F("<label for=north>Compass rotation: <span id=northval>");
    html += String(settings.north_offset_deg);
    html += F("</span>&deg;</label>"
              "<input id=north name=north type=range min=0 max=359 step=1 value='");
    html += String(settings.north_offset_deg);
    html += F("' oninput=\"document.getElementById('northval').textContent=this.value\">"
              "<p class=hint>0 puts north at the top. Set it to match how the device sits "
              "on your desk - 270 makes north point left.</p>");

    appendCheckbox(html, "hidegnd", "Hide aircraft on the ground", settings.hide_ground);
    appendNumber(html, "maxfl", "Hide above flight level", settings.max_flight_level, 0, 600,
                 "0 means no limit. FL250 hides high overflights. "
                 "Emergency squawks are never hidden.");

    static const char *kTrailNames[] = {"Off", "Selected aircraft only", "All aircraft"};
    appendSelect(html, "trails", "Trails", kTrailNames, 3, settings.trail_mode);

    appendNumber(html, "labels", "Labelled aircraft (nearest N)", settings.label_count, 0, 24);
    appendCheckbox(html, "labelalt", "Show altitude under the callsign",
                   settings.label_altitude);

    static const char *kUnitNames[] = {"Aviation (nm, ft, kt)", "Metric (km, m, km/h)"};
    appendSelect(html, "metric", "Units", kUnitNames, 2, settings.metric ? 1 : 0);

    static const char *kProviderNames[] = {"adsb.lol", "adsb.fi"};
    appendSelect(html, "provider", "Data source", kProviderNames, 2,
                 settings.provider == app::Provider::AdsbFi ? 1 : 0,
                 "Whichever is picked, the device falls back to the other one after "
                 "repeated failures.");
    html += F("</fieldset>");
}

void appendDisplayFields(String &html)
{
    const app::Settings &settings = app::settings();
    html += F("<fieldset><legend>Display</legend>");

    static const char *kRotationNames[] = {"0&deg;", "45&deg;", "90&deg;", "135&deg;",
                                          "180&deg;", "225&deg;", "270&deg;", "315&deg;"};
    appendSelect(html, "rot", "Screen rotation", kRotationNames, 8,
                 (settings.display_rotation_deg % 360) / 45,
                 "Rotates the whole face, text included, so the USB port can sit "
                 "wherever your case needs it. The corners it clips are outside the "
                 "round glass anyway.");

    html += F("<div class=row><div>");
    appendNumber(html, "day", "Day brightness %", settings.day_brightness, 5, 100);
    html += F("</div><div>");
    appendNumber(html, "night", "Night brightness %", settings.night_brightness, 0, 100);
    html += F("</div></div>");

    appendCheckbox(html, "nightdim", "Dim the screen at night", settings.night_dimming);

    html += F("<div class=row><div>");
    appendNumber(html, "nstart", "Night from (hour)", settings.night_start_hour, 0, 23);
    html += F("</div><div>");
    appendNumber(html, "nend", "Night until (hour)", settings.night_end_hour, 0, 23);
    html += F("</div></div>"
              "<p class=hint>0% night brightness turns the backlight off entirely. "
              "A touch restores full brightness for 30 s.</p>");

    appendNumber(html, "refresh", "Redraw period (ms)", settings.refresh_ms, 40, 2000,
                 "Every redraw composes and pushes a whole frame. 250 ms is 4 fps, "
                 "smooth enough for aircraft.");
    appendNumber(html, "pclk", "Pixel clock (MHz)", settings.pclk_mhz, 8, 30,
                 "8 is the default and the safe one. Higher makes the panel read PSRAM "
                 "faster than it can deliver, and the picture tears. Applied on restart.");
    html += F("</fieldset>");
}

void appendSettingsFields(String &html)
{
    appendPositionFields(html);
    appendRadarFields(html);
    appendDisplayFields(html);
}

void handleSetupPage()
{
    String html = F("<!doctype html><html><head><meta charset=utf-8>"
                    "<title>Flight Tracker setup</title>");
    html += FPSTR(kPageStyle);
    html += F("</head><body><main><h1>Flight Tracker</h1>"
              "<p class=sub>First-time setup</p><form method=POST action=/save>"
              "<fieldset><legend>Wi-Fi</legend><label for=ssid>Network</label>"
              "<input id=ssid name=ssid list=nets value='");
    html += WiFi.SSID();
    html += F("'><datalist id=nets>");

    const int found = WiFi.scanComplete();
    for (int i = 0; i < found; i++) {
        html += F("<option value='");
        html += WiFi.SSID(i);
        html += F("'>");
    }
    html += F("</datalist><label for=pass>Password</label>"
              "<input id=pass name=pass type=password>"
              "<p class=hint>2.4 GHz only - the ESP32-S3 has no 5 GHz radio.</p>"
              "</fieldset>");
    appendSettingsFields(html);
    html += F("<button type=submit>Save and restart</button></form></main>");
    html += FPSTR(kMapScript);
    html += F("</body></html>");
    g_server.send(200, "text/html", html);
}

void handleSettingsPage()
{
    const net::FeedStats stats = net::adsbStats();
    const uint32_t age_s = stats.last_success_ms == 0
        ? 0 : (millis() - stats.last_success_ms) / 1000;

    String html = F("<!doctype html><html><head><meta charset=utf-8>"
                    "<title>Flight Tracker</title>");
    html += FPSTR(kPageStyle);
    html += F("</head><body><main><h1>Flight Tracker</h1><p class=sub>");
    html += g_network;
    html += F(" &middot; ");
    html += g_address;
    html += F("</p><fieldset><legend>Feed</legend>"
              "<div class=stat><span>Aircraft shown</span><span>");
    html += String(stats.accepted);
    html += F("</span></div><div class=stat><span>Reported in range</span><span>");
    html += String(stats.reported_total);
    html += F("</span></div><div class=stat><span>Last update</span><span>");
    html += stats.last_success_ms == 0 ? String("never") : (String(age_s) + " s ago");
    html += F("</span></div><div class=stat><span>Response</span><span>");
    html += String(stats.last_bytes / 1024);
    html += F(" kB in ");
    html += String(stats.last_duration_ms);
    html += F(" ms</span></div><div class=stat><span>Success / failure</span><span>");
    html += String(stats.success_count);
    html += F(" / ");
    html += String(stats.failure_count);
    html += F("</span></div></fieldset><form method=POST action=/save>");
    appendSettingsFields(html);
    html += F("<button type=submit>Save</button></form>"
              "<form method=POST action=/reboot><button class=ghost type=submit>"
              "Restart</button></form>"
              "<form method=POST action=/forget onsubmit=\"return confirm("
              "'Forget Wi-Fi and restart into setup?')\">"
              "<button class=ghost type=submit>Forget Wi-Fi</button></form>"
              "<p class=hint>Aircraft data from adsb.lol and adsb.fi, routes from "
              "adsbdb.com. Data is ODbL.</p></main>");
    html += FPSTR(kMapScript);
    html += F("</body></html>");
    g_server.send(200, "text/html", html);
}

void handleSave()
{
    app::Settings &settings = app::settings();
    bool wifi_changed = false;

    if (g_server.hasArg("ssid") && g_server.arg("ssid").length() > 0) {
        Preferences prefs;
        if (prefs.begin(kWifiNamespace, false)) {
            prefs.putString("ssid", g_server.arg("ssid"));
            prefs.putString("pass", g_server.arg("pass"));
            prefs.end();
            wifi_changed = true;
        }
    }
    if (g_server.hasArg("lat") && g_server.hasArg("lon")) {
        settings.home_lat = g_server.arg("lat").toFloat();
        settings.home_lon = g_server.arg("lon").toFloat();
        settings.position_set = true;
    }
    if (g_server.hasArg("radius")) {
        settings.radius_nm = app::kRadiusPresets[
            app::radiusPresetIndex((uint16_t)g_server.arg("radius").toInt())];
    }
    if (g_server.hasArg("provider")) {
        settings.provider = g_server.arg("provider").toInt() == 1
            ? app::Provider::AdsbFi : app::Provider::AdsbLol;
    }
    // Checkboxes only appear in the POST when ticked, and the settings form
    // always posts every section, so absence means "off".
    settings.hide_ground = g_server.hasArg("hidegnd");
    settings.night_dimming = g_server.hasArg("nightdim");
    settings.label_altitude = g_server.hasArg("labelalt");
    if (g_server.hasArg("maxfl")) {
        settings.max_flight_level = (uint16_t)constrain(g_server.arg("maxfl").toInt(), 0, 600);
    }
    if (g_server.hasArg("trails")) {
        settings.trail_mode = (uint8_t)constrain(g_server.arg("trails").toInt(), 0, 2);
    }
    if (g_server.hasArg("labels")) {
        settings.label_count = (uint8_t)constrain(g_server.arg("labels").toInt(), 0, 24);
    }
    if (g_server.hasArg("rot")) {
        settings.display_rotation_deg =
            (uint16_t)(constrain(g_server.arg("rot").toInt(), 0, 7) * 45);
    }
    if (g_server.hasArg("metric")) {
        settings.metric = g_server.arg("metric").toInt() == 1;
    }
    if (g_server.hasArg("north")) {
        settings.north_offset_deg = (uint16_t)constrain(g_server.arg("north").toInt(), 0, 359);
    }
    if (g_server.hasArg("day")) {
        settings.day_brightness = (uint8_t)constrain(g_server.arg("day").toInt(), 5, 100);
    }
    if (g_server.hasArg("night")) {
        settings.night_brightness = (uint8_t)constrain(g_server.arg("night").toInt(), 0, 100);
    }
    if (g_server.hasArg("nstart")) {
        settings.night_start_hour = (uint8_t)constrain(g_server.arg("nstart").toInt(), 0, 23);
    }
    if (g_server.hasArg("pclk")) {
        settings.pclk_mhz = (uint8_t)constrain(g_server.arg("pclk").toInt(), 8, 30);
    }
    if (g_server.hasArg("refresh")) {
        settings.refresh_ms = (uint16_t)constrain(g_server.arg("refresh").toInt(), 40, 2000);
        ui::radarSetRefreshMs(settings.refresh_ms);
    }
    if (g_server.hasArg("nend")) {
        settings.night_end_hour = (uint8_t)constrain(g_server.arg("nend").toInt(), 0, 23);
    }
    settings.save();

    String html = F("<!doctype html><title>Saved</title>");
    html += FPSTR(kPageStyle);
    html += F("<main><h1>Saved</h1><p class=sub>");
    html += wifi_changed ? F("Restarting to join the network…")
                         : F("Settings applied.");
    html += F("</p>");
    if (!wifi_changed) {
        html += F("<p><a href=/ style=color:#38e08a>Back</a></p>");
    }
    html += F("</main>");
    g_server.send(200, "text/html", html);

    if (wifi_changed) {
        delay(400);
        ESP.restart();
    } else {
        adsbRefreshNow();
    }
}

void handleForget()
{
    wifiForget();
}

void handleStatusApi()
{
    const net::FeedStats stats = net::adsbStats();
    const app::Settings &settings = app::settings();

    JsonDocument doc;
    doc["uptime_s"] = millis() / 1000;
    doc["network"] = g_network;
    doc["address"] = g_address;
    doc["rssi"] = WiFi.RSSI();
    doc["free_sram"] = ESP.getFreeHeap();
    doc["largest_sram_block"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    doc["free_psram"] = ESP.getFreePsram();
    doc["touch_rejected"] = hw::touchRejectedSamples();

    JsonObject home = doc["home"].to<JsonObject>();
    home["lat"] = settings.home_lat;
    home["lon"] = settings.home_lon;
    home["radius_nm"] = settings.radius_nm;
    home["provider"] = settings.provider == app::Provider::AdsbFi ? "adsb.fi" : "adsb.lol";

    JsonObject feed = doc["feed"].to<JsonObject>();
    feed["last_success_age_s"] = stats.last_success_ms == 0
        ? -1 : (int)((millis() - stats.last_success_ms) / 1000);
    feed["reported"] = stats.reported_total;
    feed["accepted"] = stats.accepted;
    feed["successes"] = stats.success_count;
    feed["failures"] = stats.failure_count;
    feed["http_code"] = stats.last_http_code;
    feed["bytes"] = stats.last_bytes;
    feed["duration_ms"] = stats.last_duration_ms;

    JsonArray aircraft = doc["aircraft"].to<JsonArray>();
    {
        model::StoreGuard guard(200);
        if (guard) {
            model::Store &store = model::store();
            for (size_t i = 0; i < store.size(); i++) {
                const model::Aircraft &item = store.at(i);
                JsonObject entry = aircraft.add<JsonObject>();
                char hex[8];
                snprintf(hex, sizeof(hex), "%06lx", (unsigned long)item.icao);
                entry["hex"] = hex;
                entry["flight"] = item.flight;
                entry["reg"] = item.reg;
                entry["type"] = item.type;
                entry["dst_nm"] = round(item.dst_nm * 10) / 10.0;
                entry["dir"] = (int)item.dir_deg;
                entry["alt_ft"] = item.alt_ft;
                entry["gs_kt"] = item.gs_kt;
                entry["track"] = (int)item.track_deg;
                entry["vs_fpm"] = item.vs_fpm;
                entry["trail"] = item.trail_len;
            }
        }
    }

    String out;
    serializeJson(doc, out);
    g_server.send(200, "application/json", out);
}

void handleNotFound()
{
    if (g_portal) {
        // Captive portal: bounce everything back to the setup page.
        g_server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
        g_server.send(302, "text/plain", "");
        return;
    }
    g_server.send(404, "text/plain", "not found");
}

void startPortal()
{
    char ap_name[32];
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(ap_name, sizeof(ap_name), "FlightTracker-%02X%02X", mac[4], mac[5]);

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ap_name);
    g_dns.start(kDnsPort, "*", WiFi.softAPIP());
    g_portal = true;
    g_state = WifiState::PortalActive;
    strlcpy(g_network, ap_name, sizeof(g_network));
    strlcpy(g_address, WiFi.softAPIP().toString().c_str(), sizeof(g_address));

    app::logf("[wifi] setup portal up: %s -> http://%s/", ap_name, g_address);
}

void startOta()
{
    if (g_ota_ready) {
        return;
    }
    ArduinoOTA.setHostname(app::settings().hostname);
    if (app::settings().ota_password[0] != '\0') {
        ArduinoOTA.setPassword(app::settings().ota_password);
    }
    ArduinoOTA.onStart([]() { Serial.println("[ota] update starting"); });
    ArduinoOTA.onEnd([]() { Serial.println("[ota] done, rebooting"); });
    ArduinoOTA.onError([](ota_error_t error) { Serial.printf("[ota] error %u\n", error); });
    ArduinoOTA.begin();
    g_ota_ready = true;
    Serial.printf("[ota] listening on %s.local:3232\n", app::settings().hostname);
}

bool connectStation()
{
    Preferences prefs;
    String ssid, pass;
    if (prefs.begin(kWifiNamespace, true)) {
        ssid = prefs.getString("ssid", "");
        pass = prefs.getString("pass", "");
        prefs.end();
    }
    if (ssid.isEmpty()) {
        Serial.println("[wifi] no stored credentials");
        return false;
    }

    app::logf("[wifi] connecting to %s", ssid.c_str());
    g_state = WifiState::Connecting;
    strlcpy(g_network, ssid.c_str(), sizeof(g_network));

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);          // latency matters more than the few mA here
    WiFi.setHostname(app::settings().hostname);
    WiFi.begin(ssid.c_str(), pass.c_str());

    const uint32_t deadline = millis() + kConnectTimeoutMs;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        delay(250);
    }
    if (WiFi.status() != WL_CONNECTED) {
        app::logf("[wifi] connection failed");
        return false;
    }

    strlcpy(g_address, WiFi.localIP().toString().c_str(), sizeof(g_address));
    g_state = WifiState::Connected;
    app::logf("[wifi] connected, http://%s/ (%s.local)", g_address, app::settings().hostname);

    if (MDNS.begin(app::settings().hostname)) {
        MDNS.addService("http", "tcp", 80);
    }
    configTzTime(app::settings().timezone, "pool.ntp.org", "time.google.com");
    startOta();
    return true;
}

void networkTask(void *)
{
    while (true) {
        if (g_portal) {
            g_dns.processNextRequest();
        }
        g_server.handleClient();
        if (g_ota_ready) {
            ArduinoOTA.handle();
        }

        // Rejoin if the access point goes away.
        if (!g_portal && g_state == WifiState::Connected && WiFi.status() != WL_CONNECTED) {
            g_state = WifiState::Connecting;
            app::logf("[wifi] link lost, reconnecting");
            WiFi.reconnect();
        } else if (!g_portal && g_state == WifiState::Connecting && WiFi.status() == WL_CONNECTED) {
            strlcpy(g_address, WiFi.localIP().toString().c_str(), sizeof(g_address));
            g_state = WifiState::Connected;
            app::logf("[wifi] reconnected: %s", g_address);
        }

        app::settingsTick();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

} // namespace

bool wifiBegin()
{
    WiFi.scanNetworks(true);       // async scan, results feed the portal's picker

    if (!connectStation()) {
        startPortal();
    }

    g_server.on("/", HTTP_GET, []() {
        g_portal ? handleSetupPage() : handleSettingsPage();
    });
    g_server.on("/save", HTTP_POST, handleSave);
    g_server.on("/forget", HTTP_POST, handleForget);
    g_server.on("/api/status", HTTP_GET, handleStatusApi);
    g_server.on("/api/pclk", HTTP_POST, []() {
        const uint8_t mhz = (uint8_t)constrain(g_server.arg("mhz").toInt(), 8, 30);
        app::settings().pclk_mhz = mhz;
        app::settings().save();
        g_server.send(200, "text/plain",
                      String("pixel clock ") + mhz + " MHz on next boot, restarting");
        delay(300);
        ESP.restart();
    });
    g_server.on("/api/diag", HTTP_POST, []() {
        const uint32_t seconds = (uint32_t)constrain(g_server.arg("seconds").toInt(), 5, 120);
        ::requestDiagnosticBoot(seconds);
        g_server.send(200, "text/plain",
                      String("rebooting: static image, radios off for ") + seconds + "s");
        delay(300);
        ESP.restart();
    });
    g_server.on("/api/freeze", HTTP_POST, []() {
        const bool freeze = g_server.arg("on") != "0";
        ui::radarPause(freeze);
        g_server.send(200, "text/plain", freeze ? "drawing paused" : "drawing resumed");
    });
    g_server.on("/api/log", HTTP_GET, []() {
        g_server.send(200, "text/plain", app::logDump());
    });
    g_server.on("/reboot", HTTP_POST, []() {
        g_server.send(200, "text/plain", "rebooting");
        delay(300);
        ESP.restart();
    });
    // Captive-portal probes used by iOS, Android and Windows.
    g_server.on("/generate_204", HTTP_GET, handleNotFound);
    g_server.on("/hotspot-detect.html", HTTP_GET, handleNotFound);
    g_server.on("/ncsi.txt", HTTP_GET, handleNotFound);
    g_server.onNotFound(handleNotFound);
    g_server.begin();

    return xTaskCreatePinnedToCore(networkTask, "net", kTaskStack, nullptr,
                                   kTaskPriority, nullptr, kTaskCore) == pdPASS;
}

WifiState wifiState()       { return g_state; }
const char *wifiNetwork()   { return g_network; }
const char *wifiAddress()   { return g_address; }
int wifiRssi()              { return WiFi.RSSI(); }

void wifiForget()
{
    Preferences prefs;
    if (prefs.begin(kWifiNamespace, false)) {
        prefs.clear();
        prefs.end();
    }
    g_server.send(200, "text/html",
                  "<!doctype html><meta charset=utf-8><body style='background:#0a0e14;color:#dbe4ef;"
                  "font-family:sans-serif;padding:24px'>Wi-Fi forgotten. Restarting into setup…");
    delay(400);
    ESP.restart();
}

} // namespace net
