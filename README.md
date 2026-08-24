# ESP32 Flight Tracker

Live ADS-B aircraft on a 2.1&Prime; round touchscreen: a north-up polar plot with range
rings, cardinal marks, trails, and tap-for-detail. No map tiles, no receiver, no antenna —
aircraft data comes from community feeds over Wi-Fi.

Runs on the **Waveshare ESP32-S3-Touch-LCD-2.1** (ESP32-S3R8, 8 MB octal PSRAM, 16 MB flash).

## Install

**[Flash it from your browser →](https://shaunjanssens.github.io/esp32-flight-tracker/)**

Chrome or Edge on a desktop, and a USB cable. No toolchain, no IDE. Safari and Firefox have
no Web Serial support and cannot flash.

If the board is not detected, hold **BOOT** while plugging the cable in, then release it.
After flashing, **unplug and replug**: this board has no USB-to-serial chip, so the reset
esptool performs cannot clear the ROM's download-mode latch, and it will sit at "waiting for
download" until it is power-cycled.

## First run

1. The device starts its own Wi-Fi network, `FlightTracker-XXXX`.
2. Join it; the setup page opens by itself. Enter your Wi-Fi and press **Connect**.
3. It restarts onto your network. Drop a pin on the map for your home position — the radar
   is drawn around that point.

Everything after that is over Wi-Fi. The cable is only needed once.

## Using it

| Gesture | What happens |
|---|---|
| Tap an aircraft | Detail panel: callsign, route, aircraft type, registration, altitude, speed, distance |
| Tap the panel | Dismiss it |
| Long press | On-device settings: range, and the address for everything else |

Blips are coloured by altitude band and point along their track. Trails show roughly five
minutes of history. Aircraft squawking **7500, 7600 or 7700** are drawn in red with a ring
and are never hidden by any filter.

## Settings

Everything lives at `http://flighttracker.local/` (or the device's IP, shown on the
on-device settings page).

- **Position** — latitude and longitude, with a map picker
- **Radar** — range, compass rotation, hide aircraft on the ground, hide above a flight
  level, trails, how many aircraft get labels, units, data source
- **Display** — screen rotation in 45° steps, day and night brightness, night schedule,
  redraw period, pixel clock

Two settings are worth explaining:

**Compass rotation** turns the rose without moving the device: set it to 270 and north
points left, so the plot matches how the thing sits on your desk.

**Screen rotation** turns the whole face, text included, in 45° steps — the USB port on this
board is at 135°, and a printed case usually wants it at the bottom. The screen is round, so
rotating the frame only throws away corners that were never visible. Quarter turns stay
pixel-sharp; other angles resample and soften slightly.

## Updating

Over the air, once it is on your network:

```bash
pio run -e ota -t upload                          # to flighttracker.local
pio run -e ota -t upload --upload-port 192.168.1.x  # or by address
```

## Building from source

```bash
pio run -e device                                    # build
pio run -e device -t upload --upload-port /dev/cu.usbmodemXXXX   # first flash, over USB
```

Diagnostics without a cable:

```bash
curl -s http://flighttracker.local/api/status   # uptime, memory, feed stats, aircraft table
curl -s http://flighttracker.local/api/log      # retained log
```

See [CLAUDE.md](CLAUDE.md) for the architecture, and for a record of what went wrong during
development and what fixed it — the PSRAM bandwidth limit that governs the pixel clock, the
download-mode trap, the touch controller's phantom samples. Worth reading before changing
anything that looks odd; several things that look like mistakes are deliberate.

## Data sources

Aircraft from [adsb.fi](https://adsb.fi) and [adsb.lol](https://adsb.lol), routes and
aircraft types from [adsbdb.com](https://adsbdb.com). Data is ODbL.

These are volunteer-funded and free to use. The firmware sends a descriptive User-Agent,
never polls faster than every 5 seconds, scales the interval with the requested radius, and
stands down for a minute on an HTTP 429. Please keep it that way.
