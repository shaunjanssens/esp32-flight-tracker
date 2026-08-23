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
#include "model/aircraft.h"
#include "net/adsb_client.h"

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
<style>
:root{color-scheme:dark}
body{margin:0;padding:24px 16px;background:#0a0e14;color:#dbe4ef;
     font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
main{max-width:460px;margin:0 auto}
h1{font-size:20px;letter-spacing:.14em;text-transform:uppercase;color:#38e08a;margin:0 0 4px}
p.sub{margin:0 0 24px;color:#6e8aa6;font-size:13px}
fieldset{border:1px solid #1d2a3a;border-radius:10px;margin:0 0 16px;padding:14px 16px}
legend{padding:0 6px;color:#8aa6c0;font-size:12px;letter-spacing:.1em;text-transform:uppercase}
label{display:block;margin:10px 0 4px;font-size:13px;color:#9fb3c8}
input,select{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;
     border:1px solid #24344a;background:#111823;color:#dbe4ef;font-size:15px}
.row{display:flex;gap:10px}.row>div{flex:1}
button{width:100%;margin-top:8px;padding:12px;border:0;border-radius:8px;
     background:#38e08a;color:#04070d;font-size:16px;font-weight:600}
button.ghost{background:#1d2a3a;color:#9fb3c8;margin-top:10px}
.hint{font-size:12px;color:#5c7080;margin-top:6px}
.stat{display:flex;justify-content:space-between;padding:5px 0;border-bottom:1px solid #131c28;font-size:14px}
.stat span:last-child{color:#8aa6c0;font-variant-numeric:tabular-nums}
</style>
)CSS";

void appendSettingsFields(String &html)
{
    const app::Settings &settings = app::settings();
    html += F("<fieldset><legend>Home position</legend>");
    html += F("<div class=row><div><label>Latitude</label>"
              "<input name=lat id=lat type=number step=0.00001 min=-90 max=90 value='");
    html += String(settings.home_lat, 5);
    html += F("'></div><div><label>Longitude</label>"
              "<input name=lon id=lon type=number step=0.00001 min=-180 max=180 value='");
    html += String(settings.home_lon, 5);
    html += F("'></div></div>"
              "<button type=button class=ghost onclick='useLocation()'>Use my browser location</button>"
              "<p class=hint>The radar is drawn around this point. Five decimals is about a metre.</p>"
              "</fieldset>");

    html += F("<fieldset><legend>Radar</legend><label>Range</label><select name=radius>");
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
    html += F("</select><label>Data source</label><select name=provider>");
    html += (settings.provider == app::Provider::AdsbLol)
        ? F("<option value=0 selected>adsb.lol</option><option value=1>adsb.fi</option>")
        : F("<option value=0>adsb.lol</option><option value=1 selected>adsb.fi</option>");
    html += F("</select></fieldset>");

    html += F("<fieldset><legend>Display</legend><div class=row>"
              "<div><label>Day brightness %</label><input name=day type=number min=5 max=100 value='");
    html += String(settings.day_brightness);
    html += F("'></div><div><label>Night brightness %</label>"
              "<input name=night type=number min=0 max=100 value='");
    html += String(settings.night_brightness);
    html += F("'></div></div><div class=row>"
              "<div><label>Night from (h)</label><input name=nstart type=number min=0 max=23 value='");
    html += String(settings.night_start_hour);
    html += F("'></div><div><label>Night until (h)</label>"
              "<input name=nend type=number min=0 max=23 value='");
    html += String(settings.night_end_hour);
    html += F("'></div></div></fieldset>");
}

const char kGeoScript[] PROGMEM = R"JS(
<script>
function useLocation(){
  if(!navigator.geolocation){alert('This browser has no geolocation.');return}
  navigator.geolocation.getCurrentPosition(function(p){
    document.getElementById('lat').value=p.coords.latitude.toFixed(5);
    document.getElementById('lon').value=p.coords.longitude.toFixed(5);
  },function(e){alert('Location failed: '+e.message)},{enableHighAccuracy:true});
}
</script>
)JS";

void handleSetupPage()
{
    String html = F("<!doctype html><title>Flight Tracker setup</title>");
    html += FPSTR(kPageStyle);
    html += F("<main><h1>Flight Tracker</h1>"
              "<p class=sub>First-time setup</p><form method=POST action=/save>"
              "<fieldset><legend>Wi-Fi</legend><label>Network</label><input name=ssid list=nets value='");
    html += WiFi.SSID();
    html += F("'><datalist id=nets>");

    const int found = WiFi.scanComplete();
    for (int i = 0; i < found; i++) {
        html += F("<option value='");
        html += WiFi.SSID(i);
        html += F("'>");
    }
    html += F("</datalist><label>Password</label><input name=pass type=password>"
              "<p class=hint>2.4 GHz only — the ESP32-S3 has no 5 GHz radio.</p></fieldset>");
    appendSettingsFields(html);
    html += F("<button type=submit>Save and restart</button></form></main>");
    html += FPSTR(kGeoScript);
    g_server.send(200, "text/html", html);
}

void handleSettingsPage()
{
    const net::FeedStats stats = net::adsbStats();
    const uint32_t age_s = stats.last_success_ms == 0
        ? 0 : (millis() - stats.last_success_ms) / 1000;

    String html = F("<!doctype html><title>Flight Tracker</title>");
    html += FPSTR(kPageStyle);
    html += F("<main><h1>Flight Tracker</h1><p class=sub>");
    html += g_network;
    html += F(" · ");
    html += g_address;
    html += F("</p><fieldset><legend>Feed</legend>");

    html += F("<div class=stat><span>Aircraft shown</span><span>");
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
              "<form method=POST action=/forget onsubmit=\"return confirm('Forget Wi-Fi and restart?')\">"
              "<button class=ghost type=submit>Forget Wi-Fi</button></form></main>");
    html += FPSTR(kGeoScript);
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
    if (g_server.hasArg("day")) {
        settings.day_brightness = (uint8_t)constrain(g_server.arg("day").toInt(), 5, 100);
    }
    if (g_server.hasArg("night")) {
        settings.night_brightness = (uint8_t)constrain(g_server.arg("night").toInt(), 0, 100);
    }
    if (g_server.hasArg("nstart")) {
        settings.night_start_hour = (uint8_t)constrain(g_server.arg("nstart").toInt(), 0, 23);
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
    doc["i2c_devices"] = app::i2cReport();

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

    Serial.printf("[wifi] setup portal up: %s -> http://%s/\n", ap_name, g_address);
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

    Serial.printf("[wifi] connecting to %s\n", ssid.c_str());
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
        Serial.println("[wifi] connection failed");
        return false;
    }

    strlcpy(g_address, WiFi.localIP().toString().c_str(), sizeof(g_address));
    g_state = WifiState::Connected;
    Serial.printf("[wifi] connected, http://%s/ (%s.local)\n", g_address, app::settings().hostname);

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
            Serial.println("[wifi] link lost, reconnecting");
            WiFi.reconnect();
        } else if (!g_portal && g_state == WifiState::Connecting && WiFi.status() == WL_CONNECTED) {
            strlcpy(g_address, WiFi.localIP().toString().c_str(), sizeof(g_address));
            g_state = WifiState::Connected;
            Serial.printf("[wifi] reconnected: %s\n", g_address);
        }

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
