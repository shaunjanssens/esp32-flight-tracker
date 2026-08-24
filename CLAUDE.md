# ESP32 Flight Tracker

Live ADS-B aircraft on a 480x480 round touch screen: a north-up polar plot with range
rings, cardinal marks, trails and tap-for-detail. No map tiles.

Hardware: **Waveshare ESP32-S3-Touch-LCD-2.1** (ESP32-S3R8, 8 MB octal PSRAM, 16 MB flash).

## Working on this project

**Flash over Wi-Fi. Do not reach for the USB cable.**

```bash
pio run -e ota -t upload                      # OTA to flighttracker.local
curl -s http://flighttracker.local/api/status # feed state + aircraft table
curl -s http://flighttracker.local/api/log    # last ~4 kB of device log
```

The device serves its own diagnostics, which is the only sane way to work on it: serial
means a cable, and a cable on this board means the BOOT-button dance below. `/api/log` is
a RAM ring buffer written by `app::logf()` - use that, not `Serial.printf`, for anything
worth reading later.

### Endpoints

| Route | Purpose |
|---|---|
| `GET /` | Settings page (position, range, provider, brightness, redraw, pixel clock) |
| `GET /api/status` | JSON: uptime, memory, feed stats, full aircraft table |
| `GET /api/log` | Retained log |
| `POST /api/refresh` `ms=250` | Redraw period, live |
| `POST /api/pclk` `mhz=8` | Pixel clock; saves and reboots |
| `POST /api/freeze` `on=1` | Stop drawing entirely (diagnostic) |
| `POST /api/diag` `seconds=45` | Reboot with radios off and a static frame, then resume |
| `POST /reboot` | Restart |
| `POST /forget` | Clear Wi-Fi credentials, restart into the setup portal |

`/api/freeze` and `/api/diag` exist because "is it the panel or is it us?" is otherwise
unanswerable remotely. They earned their keep - see the flicker story below.

## Layout

```
src/
  main.cpp              boot order, crash-loop safe mode, PSRAM benchmark
  app/config.*          NVS-backed settings; saveSoon() coalesces writes
  app/log.*             logf() -> serial + RAM ring buffer for /api/log
  hw/board_pins.h       pin map, panel timings, I2C addresses
  hw/display.*          LovyanGFX LGFX device, ST7701 init list, TCA9554 expander
  hw/touch.*            CST820 over Wire
  model/aircraft.*      PSRAM aircraft table, trails, dead reckoning, expiry
  net/adsb_client.*     feed poll, PSRAM body buffer, filtered parse, failover
  net/route_client.*    adsbdb route/type lookups, LRU + negative caching
  net/wifi_manager.*    STA + captive portal, settings pages, OTA, mDNS
  ui/radar.*            sprite compositing, radar face, touch gestures
```

**Boot order matters.** Settings, Wi-Fi, OTA and the feed start *before* the display, and
nothing in `setup()` returns early on a display failure. A panel bug must never take the
network with it. `g_display_attempts` in RTC memory drops the device to headless mode after
two failed display inits, so a bad display setting costs an OTA push rather than a cable.

## Architecture

- **Core 0**: feed poll (5 s), route lookups, web server, OTA.
- **Core 1** (Arduino loop): touch polling and radar compositing.
- Shared state is `model::store()` behind a recursive mutex; hold it briefly.

Rendering follows the pattern that works on this panel: a **static grid sprite** (rings,
ticks, cardinals) composited into a **frame sprite**, aircraft drawn on top, then one
`pushSprite` to the panel. Both sprites are 480x480x16 in PSRAM.

Positions come from the feed as `dst` (nm) and `dir` (bearing) relative to the query point,
so there is no great-circle maths on device - just polar to screen. Between polls, blips are
dead-reckoned from ground speed and track.

## Data sources

- **adsb.fi** (default): `https://opendata.adsb.fi/api/v2/lat/{lat}/lon/{lon}/dist/{nm}`
- **adsb.lol** (fallback): `http://api.adsb.lol/v2/lat/{lat}/lon/{lon}/dist/{nm}`
- **adsbdb.com** for routes and types: `/v0/callsign/{callsign}`, `/v0/aircraft/{hex}`

Both feeds return the same record shape; the array key differs (`aircraft` vs `ac`) and the
parser accepts either. These are volunteer-funded: keep the descriptive User-Agent, the
2.5 s floor between requests, the 60 s stand-down on HTTP 429, and the radius-scaled poll
interval. Data is ODbL.

## What went wrong, and what fixed it

Everything here cost real time. Read it before re-deriving it.

### The display flicker (the big one)

Symptom: horizontal tearing and a "hacking movie" shimmer whenever Wi-Fi was up and
aircraft were drawn. Chased for hours through **bounce buffer sizes, three LVGL render
modes (partial, direct, full-frame), pixel clocks downward, VSYNC swapping, and a complete
rewrite from LVGL to LovyanGFX**. Every one of those helped a little or not at all.

The actual cause: **PSRAM bandwidth**. The RGB panel reads its framebuffer out of PSRAM
continuously, and that demand scales with pixel clock:

| Pixel clock | Refresh | Panel needs | Result |
|---|---|---|---|
| 20 MHz | 73 Hz | 34 MB/s | very bad |
| 16 MHz (stock) | 58 Hz | 27.6 MB/s | flickers |
| 8 MHz | 29 Hz | 13.8 MB/s | rock steady |

Measured throughput on this board is **34 MB/s write, 22 MB/s copy** (`benchmarkPsram()`
logs it at boot). At the stock 16 MHz the refresh alone consumes the memory system, so
Wi-Fi DMA and drawing starve it. Default pixel clock is therefore **8 MHz**.

Two diagnostics settled it in minutes after hours of guessing:
- `/api/diag` - radios off, static frame: **no flicker at all**, proving the panel and its
  init were innocent.
- Flicker scaling monotonically with pixel clock, proving it was bandwidth, not tearing.

**The lesson**: when a symptom scales with a clock rate, measure the bus before changing
the software. A two-minute benchmark would have pointed straight at it.

Corollaries worth keeping:
- **Anti-aliased drawing is expensive on the wrong resource.** `drawWideLine` / `drawArc`
  blend against existing pixels, so every pixel is a PSRAM read *and* write. Anti-aliasing
  the per-frame layer made the flicker dramatically worse. Smoothing is confined to the
  static grid, drawn once per range change; the per-frame layer uses plain primitives.
- **Redraw rate is a bandwidth setting**, not a smoothness setting. 4 fps (250 ms) is
  plenty: an airliner crosses a 25 nm face in about three minutes, roughly 2 px/second.

### LVGL / ESP32_Display_Panel (removed)

The project started on LVGL 9 + `ESP32_Display_Panel` and moved to LovyanGFX. Notes if
anyone reconsiders:

- `configRGB_BounceBufferSize(0)` **divides by zero** - the library computes
  `half_total_pixels % size` to align the buffer. Disabling a bounce buffer means clearing
  `bounce_buffer_size_px` in the panel config directly.
- Bounce buffers are refilled **from an interrupt**, and the precompiled Arduino IDF
  libraries have `CONFIG_LCD_RGB_ISR_IRAM_SAFE` **off**. Any flash write disables the cache
  - including the NVS write Wi-Fi makes while associating, about two seconds into a boot -
  and the refresh FIFO underruns, leaving the image permanently shifted.
- Because of that, **every NVS write is a display glitch**. `Settings::saveSoon()` coalesces
  writes a few seconds after the last change; dragging the range ring used to write on every
  step.

### LovyanGFX

- The stock `Panel_ST7701` init sequence leaves this panel **blank white**. It needs the
  Waveshare register list (`PanelST7701Waveshare::getInitCommands`), which is byte-identical
  to the one Espressif ships in its board support - worth checking before assuming the
  sequence is the problem, as it was not.
- Panel CS is on the **TCA9554 expander**, not a GPIO, so it is asserted by hand around
  `init()`.
- `use_psram = 2` (double framebuffer) is required; single-buffering tears.

### Flashing and recovery

- **This board cannot leave ROM download mode over USB.** It has no USB-UART bridge:
  esptool's reset is emulated through the USB-serial-JTAG peripheral (`rst:0x15
  USB_UART_CHIP_RESET`) and that does not clear the download-mode latch. After a USB flash
  the board sits in "waiting for download" until it is **physically unplugged and replugged**.
  Every cable flash therefore costs two manual steps. Use OTA.
- Arduino's `Wire` and the IDF `i2c_master` driver **cannot coexist**: mixing them aborts at
  boot with `CONFIG_I2C driver_ng is not allowed to be used with this old driver`. With
  ESP32_Display_Panel gone, `Wire` is used throughout.
- USB CDC output written **before the host opens the port is discarded**, so one-shot
  `setup()` prints vanish and a hang looks identical to a healthy boot. The heartbeat task
  and `/api/log` exist because of this.
- Reading serial with **DTR asserted pulls GPIO0 low**, which drops the board into download
  mode on the next reset. Read with DTR clear.

### Touch

- **Reject impossible samples, do not clamp them.** The CST820 returns out-of-range
  coordinates now and then. Clamping turned those into a tap at 479,479 - the bottom-right
  corner, where a round panel has no glass - and each phantom landed on empty space and
  cleared the user's selection. `touchRead()` now discards anything off-panel or outside
  the circle and counts it in `/api/status` as `touch_rejected`.
- **Debounce the release.** The controller drops samples while a finger is still down, so
  one tap arrives as release-press-release. The second tap re-hit the same aircraft
  (toggling it off) or landed on the panel that had just opened underneath. A release only
  counts after 60 ms of continuous "up".
- Those two between them explain a range that used to change to 100 nm by itself, and the
  HTTP 429s that came with it: phantom touches at the screen edge were driving the old
  range-drag gesture. That gesture is gone; range lives on the settings page.

### Rotation and the round screen

- **Rotate the composed frame, not both sprites.** The grid is drawn into its own sprite and
  pushed into the frame sprite; if both carry a rotation the grid gets transformed twice and
  ends up upright while the aircraft move. Only the final push rotates.
- **45-degree steps are affordable here only because the screen is round.** `pushRotated`
  clips the frame's corners, and on this panel those corners are not glass. Cost measured at
  135 degrees: 29 ms versus 26 ms unrotated, against a 250 ms redraw period.
- Non-orthogonal angles resample, so text and thin lines soften; quarter turns map pixels
  one to one and stay sharp. Non-orthogonal angles therefore use `pushRotatedWithAA`, and
  quarter turns deliberately do not.

### The feed

- **Do not parse straight from the TLS socket.** ArduinoJson treats a read that returns
  nothing as end-of-input, so any pause longer than the stream timeout ends the document
  early - `IncompleteInput` on perfectly good responses. The body is read into a PSRAM
  buffer first, then parsed.
- **Connect timeout must be short.** adsb.lol publishes five A records and some refuse
  connections; at a 20 s timeout and three failures before failover, that was over a minute
  of dead air. Now 5 s and two failures.
- **Rate limits are real.** Dragging the range ring fired a refresh per step and earned
  HTTP 429s. There is now a hard 2.5 s floor between requests regardless of who asks.
- `useHTTP10(true)` avoids chunked-encoding stalls; keep it.

## Conventions

- Comments explain **why**, especially where the code looks wrong but is not (the 8 MHz
  clock, the aliased per-frame drawing, the deferred NVS writes). Those three all look like
  mistakes and are all deliberate.
- Anything the panel might reject is a **setting with a safe default**, not a constant -
  the failure mode is only visible on the glass, so it has to be changeable without a build.
- Aviation units throughout: nm, ft, kt.

## Still to build

- Web-flash install page (ESP Web Tools on GitHub Pages) + release workflow
- Night dimming schedule (setting exists, not yet applied to the backlight)
- IMU orientation (QMI8658)
- README flashing instructions, written around OTA
