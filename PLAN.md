# ESP32 Round Radar — Build Plan

A desk radar for the **Waveshare ESP32-S3-Touch-LCD-2.1**: live aircraft around your home
position on a north-up polar plot, with trails, touch-to-inspect, and a bezel-drag zoom.
No map tiles — just rings, cardinal markers, and blips.

## 0. Decisions locked in

| Topic | Choice |
|---|---|
| Board | Waveshare ESP32-S3-Touch-LCD-2.1 (ESP32-S3R8, 8 MB PSRAM, 16 MB flash) |
| Stack | PlatformIO + Arduino-ESP32 3.x + LVGL 9 |
| Data | Direct from public API — adsb.lol v2, fallback adsb.fi — no proxy server |
| Detail view | Route + aircraft type/operator, **no photos** |
| Units | Aviation — nm, ft, kt |
| Radius UI | Circular drag along the outer bezel, snapping to presets |
| Home position | Typed into the setup portal, stored in NVS |
| Cadence | Poll every 5 s, 5 minutes of trail (60 points/aircraft) |
| Extras used | Night dimming (PWM backlight), IMU orientation |
| Setup/flash | ESP Web Tools install page + Improv-Serial + captive-portal fallback |
| Reconfigure | Small LAN web UI (`flighttracker.local`) with settings + OTA |
| Scope | Full feature set in one v1 |

## 1. Hardware facts (verified against Waveshare's wiki)

- **Display**: ST7701 (ST7701S), 480×480 round IPS, driven over the ESP32-S3 **16-bit RGB parallel**
  interface. Framebuffer = 480×480×2 = **460 KB → must live in PSRAM**.
- **Touch**: **CST820**, I²C, **single touch only** → no pinch-zoom (this is why radius is a bezel drag).
- **IO expander**: TCA9554PWR (panel reset / backlight / misc lines).
- **Sensors**: QMI8658 6-axis IMU, PCF85063 RTC. TF card slot, battery charge circuit.
- **Memory**: 512 KB SRAM, 8 MB PSRAM (octal), 16 MB flash.

The pin map and panel timings do **not** need to be reverse-engineered: Espressif's
`ESP32_Display_Panel` library ships a board definition
(`src/board/supported/waveshare/BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_2_1.h`), so bring-up is
one `#define` plus `lv_conf.h`.

### The one real hardware risk
On ESP32-S3, an RGB panel streams its framebuffer out of PSRAM continuously. Wi-Fi + TLS +
a 150 KB JSON parse compete for that same PSRAM bandwidth, which shows up as **tearing or a
permanently drifting/sheared image**. Mitigations, applied from day one:
- Enable **bounce buffers** in internal SRAM (the board config's default path) so the LCD
  DMA reads from SRAM, not PSRAM.
- Keep networking on core 0, LVGL + flush on core 1.
- Cap the JSON payload (radius cap + response filtering, see §4).

## 2. Data sources (tested live while writing this plan)

**Aircraft — adsb.lol v2**, free, no key, no auth header:
```
GET https://api.adsb.lol/v2/lat/{lat}/lon/{lon}/dist/{nm}     (max 250 nm)
```
Measured from a Belgian home position:

| radius | payload | aircraft |
|---|---|---|
| 10 nm | 6 KB | 11 |
| 25 nm | 15 KB | 28 |
| 50 nm | 56 KB | 102 |
| 100 nm | 152 KB | 279 |

Crucially, every record already carries **`dst` (distance in nm)** and **`dir` (true bearing
in degrees)** relative to the query point — the polar coordinates the radar needs, with no
great-circle math on device. Also present: `hex`, `flight`, `r` (registration), `t` (type),
`alt_baro`, `gs`, `track`, `baro_rate`, `lat`, `lon`, `category`, `mlat`, `seen`.

**Route + aircraft details — adsbdb.com**, free, no key, small responses:
```
GET https://api.adsbdb.com/v0/callsign/{callsign}   → airline, origin/destination airport (IATA/ICAO, city, country)
GET https://api.adsbdb.com/v0/aircraft/{hex}        → type, icao_type, manufacturer, registration, owner, owner country
```
Both verified returning correct data for a live Brussels Airlines flight. 404 = unknown,
which must be cached as "unknown" so we don't re-ask every 5 s.

**Etiquette / licensing** (this is community-funded infrastructure, not a commercial API):
- Descriptive `User-Agent` identifying the project + a contact URL.
- Never poll faster than 5 s; exponential backoff on 429/5xx; hard radius cap of 100 nm.
- Data is ODbL — an attribution line on the about/settings screen.
- Config field for an alternate provider — **adsb.fi**, verified as a drop-in (see §9) — so
  the device survives one provider going away.

## 3. Firmware architecture

Two FreeRTOS tasks, one snapshot between them:

```
core 0  net_task ─── every 5 s ── HTTPS GET adsb.lol ── streaming JSON parse ──┐
                └── lazy queue ── adsbdb lookups (route/type) ── LRU cache      │
                                                                    mutex ↓ snapshot
core 1  ui_task  ─── LVGL tick + timer_handler @ 200 Hz ── radar draw @ ~20 fps ┘
                     dead-reckoning between polls (gs + track)
```

### Aircraft store
Fixed-size table in PSRAM, nearest-first, capped at **64 aircraft** (beyond that the round
screen is unreadable anyway):

```c
typedef struct {
  uint32_t icao;            // hex as int, the identity key
  char     flight[9];       // callsign, trimmed
  char     reg[8], type[5];
  float    x_nm, y_nm;      // local flat-earth, from dst/dir; +y = north
  float    vx, vy;          // nm/s, from gs + track — used for interpolation
  int32_t  alt_ft;  int16_t gs_kt, vs_fpm;  float track_deg;
  bool     on_ground, mlat;
  uint32_t last_seen_ms;
  uint8_t  trail_head, trail_len;
  int16_t  trail[60][2];    // 1/64 nm fixed point → 240 B/aircraft, 5 min @ 5 s
} aircraft_t;
```
~19 KB total in PSRAM. Entries expire 30 s after last sighting; trails fade out rather than
vanishing instantly.

Between polls the UI advances each blip by `v * dt` so motion is smooth at 20 fps instead of
jumping every 5 s. Each poll snaps back to truth (with a short lerp to hide the correction).

### Parsing 150 KB on a microcontroller
`HTTPClient` + `client.useHTTP10(true)` (avoids chunked-encoding stalls when streaming),
then ArduinoJson 7 `deserializeJson(doc, stream, DeserializationOption::Filter(filter))`
with a **PSRAM allocator** — the filter drops ~70% of each record before it is ever
allocated. Never buffer the whole response as a String.

### Geometry
```
screen_x = 240 + (x_nm / radius_nm) * R_px
screen_y = 240 - (y_nm / radius_nm) * R_px      // R_px = 200, leaving a 40 px bezel ring
```

## 4. UI (480×480, round, north-up)

**Radar face** — one full-screen LVGL object with a custom `LV_EVENT_DRAW_MAIN` handler
(`lv_draw_arc` / `lv_draw_line` / `lv_draw_label` directly). This keeps the LVGL object
count near zero; 64 aircraft × 60 trail points as widgets would not hold 20 fps.

- Range rings at ¼, ½, ¾, 1 × radius, each labelled in nm at the 45° diagonal.
- Cardinal markers **N / E / S / W** on the bezel, N accented; minor ticks every 30°.
- Home marker at centre; under it: local time (RTC), aircraft count, and the age of the
  last successful fetch — which turns amber, then red, when the feed goes stale.
- **Blips**: chevrons rotated to `track`, coloured by altitude band (ground → deep blue,
  FL100 → cyan, FL300+ → warm white), squared-off marker for MLAT/ground.
- **Labels**: callsign + flight level, drawn only for the nearest 8 (and always for the
  selected aircraft) — otherwise the face turns to soup at 100 nm.
- **Trails**: polyline behind each blip, alpha ramping 100% → 0% over the 5 minutes.

**Detail sheet** — tap a blip (hit radius ~30 px, nearest wins) and a panel animates up over
the lower two-thirds:
> **BEL4OZ** · Brussels Airlines
> **BRU → EDI** — Brussels ✈ Edinburgh
> A320 (Airbus A320-214) · OO-SNB
> FL067 ↑ 2176 fpm · 334 kt · 12.4 nm · 291°

Route/type are fetched lazily on first tap and cached (LRU, ~64 entries in PSRAM, negative
results cached too). Values keep updating live while the sheet is open; if the aircraft
leaves range the sheet greys out rather than closing under your finger. Dismiss by tapping
outside or swiping down.

**Bezel zoom** — a drag that starts in the outer 40 px annulus accumulates swept angle;
~60° of sweep steps one preset: **5 / 10 / 25 / 50 / 100 nm** (capped at 100 for API
etiquette). A transient arc + big centre readout shows the new range, and the ring labels
re-animate. Radius persists to NVS (debounced).

**Night dimming** — LEDC PWM on the backlight; schedule from the RTC (e.g. 22:00–07:00 at
20%), any touch restores full brightness for 30 s. Configurable in the web UI.

**IMU orientation** — QMI8658 accelerometer picks a 90° step with hysteresis. The radar
itself stays north-up (rotating it would defeat the purpose and full-frame software rotation
is expensive on an RGB panel); only the detail-sheet text layout re-orients. Low priority
inside v1 — it is the one feature I would cut first if the panel fights back.

## 5. Setup, flashing and reconfiguration

**A. Install page (`web/install/`, GitHub Pages)**
`esp-web-tools` `<esp-web-install-button>` + a `manifest.json` pointing at a merged binary
(`esptool merge_bin` of bootloader + partitions + app + littlefs), built and published by a
GitHub Actions release workflow. Chrome/Edge only — WebSerial doesn't exist in Safari or
Firefox, and the page says so.

**B. Improv-Serial** — right after flashing, the install page hands Wi-Fi credentials to the
device over the same serial connection. Best case, you never see an AP.

**C. Captive portal fallback** — no credentials, or 20 s without a connection, and the device
raises AP `FlightTracker-XXXX` + DNS catch-all. Setup page: Wi-Fi scan list + password,
**home lat/lon** (with a "use my browser location" button filling the fields via
`navigator.geolocation`), default radius, timezone. Saved to NVS via `Preferences`.

**D. LAN web UI + OTA** — once online, an `ESPAsyncWebServer` on port 80, `flighttracker.local`
via mDNS:
- `/` settings (position, radius presets, brightness schedule, data provider, units)
- `/api/status` live JSON of the current aircraft table — the debugging window into the device
- `/update` OTA firmware upload, gated by a password stored in NVS
Partition table for 16 MB: `ota_0` 4 MB / `ota_1` 4 MB / `littlefs` ~7 MB. Portal assets are
gzipped into LittleFS.

*Security note*: this is an unauthenticated box on your LAN. Settings are harmless, OTA is
not — hence the password on `/update`, and it should never be port-forwarded.

## 6. Repository layout

```
platformio.ini                  # env:device (arduino), env:native (unit tests)
partitions_16mb_ota.csv
src/
  main.cpp                      # task setup, watchdogs
  hw/board.cpp                  # ESP32_Display_Panel init, backlight PWM, IMU, RTC
  app/config.cpp                # NVS-backed settings struct + defaults
  net/wifi_manager.cpp          # STA connect, AP portal, mDNS, reconnection
  net/adsb_client.cpp           # poll loop, streaming filtered parse, backoff
  net/route_client.cpp          # adsbdb lookups + LRU cache (incl. negative caching)
  model/aircraft_store.cpp      # table, trails, expiry, dead reckoning
  ui/radar_view.cpp             # custom-draw radar face
  ui/detail_sheet.cpp           # tap target → detail panel
  ui/bezel_zoom.cpp             # angular drag gesture
  ui/theme.cpp                  # palette, altitude colour ramp, fonts
web/portal/                     # setup + settings pages (gzipped → LittleFS)
web/install/                    # esp-web-tools install page → GitHub Pages
tools/merge_firmware.py
test/                           # native tests: geo math, JSON fixtures, trail ring buffer
.github/workflows/build.yml     # build → merged bin → release → Pages
```

The parsing and geometry code is written so it compiles for the `native` env too — real
adsb.lol responses saved as fixtures, asserted on a laptop, so bugs in the fiddly parts
never need a reflash to find.

## 7. Build order

Everything ships in v1, but this is the order the work happens in, because each step de-risks
the next:

1. **Bring-up** — PlatformIO env, `ESP32_Display_Panel` board define, LVGL 9 `lv_conf.h`,
   bounce buffers, backlight PWM, touch coordinates verified. A rotating test arc proves
   frame timing before any app logic exists.
2. **Config + network** — NVS settings, Wi-Fi STA/AP, captive portal, mDNS, web settings page, OTA.
3. **Data pipeline** — adsb.lol client with filtered streaming parse, aircraft store, expiry;
   validated over serial and via `/api/status` before it's drawn.
4. **Radar face** — rings, cardinals, blips, dead reckoning, altitude colouring, labels.
5. **Trails** — ring buffer, fading polylines, behaviour on expiry.
6. **Touch** — hit testing, detail sheet, adsbdb lookups + caching.
7. **Bezel zoom** — angular gesture, presets, persistence, ring relabelling.
8. **Polish** — night dimming, IMU orientation, stale-feed and no-Wi-Fi states, error toasts.
9. **Release** — merged bin, install page, GitHub Actions, README with photos.

## 8. Risks and how they're handled

| Risk | Handling |
|---|---|
| RGB panel tears/drifts under Wi-Fi load | Bounce buffers, core split, radius cap, no big String buffers |
| Heap exhaustion (TLS ~40 KB + JSON) | PSRAM allocator for ArduinoJson, response filter, `setBufferSizes`, 100 nm cap |
| CST820 is single-touch and gesture-quirky | No multi-touch gestures anywhere; explicit release timeout handling |
| Free API changes/disappears/rate-limits | Configurable provider, backoff, cached last-good frame, visible stale indicator |
| TLS root cert expiry | Bundle ISRG Root X1 + a "skip verification" escape hatch in settings, OTA to update |
| esp-web-tools browser support | Documented on the install page; `pio run -t upload` always works |
| Detail lookups hammering adsbdb | Lazy (tap-triggered only), LRU + negative caching, one request in flight |

## 9. Resolved since first draft

- **Board is on the desk**: `/dev/cu.usbmodem1101`, VID:PID `303A:1001` (ESP32-S3 native
  USB JTAG/serial), MAC `28:84:85:91:CE:08`. PlatformIO Core 6.1.19 is installed, so bring-up
  can start immediately and every step is verified on real hardware.
- **Repo is public** → the install page ships to GitHub Pages from this repo via Actions,
  and `gh` is available locally for the release workflow.
- **Fallback provider is adsb.fi** (`https://opendata.adsb.fi/api/v2/lat/{lat}/lon/{lon}/dist/{nm}`).
  Verified: HTTP 200, no key, same record shape *including* `dst`/`dir`, and it adds a `desc`
  field (full type name, e.g. "DE HAVILLAND DHC-1 Chipmunk") that saves an adsbdb lookup when
  present. Only difference: the aircraft array is under `aircraft` instead of adsb.lol's `ac` —
  the parser accepts either key, so switching providers is one config string.
  *airplanes.live was rejected*: it now returns HTTP 403 demanding email approval for API access.
- **Timezone**: NTP + `Europe/Brussels` POSIX TZ string, overridable in the web settings.
