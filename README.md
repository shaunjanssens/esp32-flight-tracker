# esp32-flight-tracker

A desk radar for aircraft overhead, on a 2.1" round touchscreen.

Your home position sits at the centre. Live aircraft appear as chevrons at their real bearing
and distance, trailing the last five minutes of their track behind them. No map, no tiles —
just range rings, N/E/S/W markers, and the planes. Tap one to see who it is and where it's
going. Drag around the bezel to change the range.

Data comes straight from community ADS-B feeds over your Wi-Fi. No receiver hardware, no
server of your own, no API keys.

> **Status: in development.** The design is settled and the hardware is confirmed — see
> [PLAN.md](PLAN.md) for the full build plan. There is no flashable release yet; the
> [Flashing](#flashing) section below describes how it will work and what already works today.

## Hardware

One board, nothing to solder:

**[Waveshare ESP32-S3-Touch-LCD-2.1](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.1)**

| | |
|---|---|
| MCU | ESP32-S3R8, dual-core Xtensa LX7 @ 240 MHz, Wi-Fi + BLE 5 |
| Memory | 512 KB SRAM, 8 MB PSRAM (octal), 16 MB flash |
| Display | 2.1" round IPS, 480×480, ST7701 over 16-bit RGB parallel |
| Touch | CST820 capacitive, I²C — **single touch only** |
| Also onboard | QMI8658 6-axis IMU, PCF85063 RTC, TCA9554 IO expander, TF slot, battery charging |

The single-touch controller is why range is changed with a bezel drag rather than a pinch —
the panel physically cannot report two fingers.

You will also need a USB-C cable that carries data, and 2.4 GHz Wi-Fi (the ESP32-S3 has no
5 GHz radio).

## What it does

**Radar face**
- North-up polar plot: range rings at ¼, ½, ¾ and full range, labelled in nm
- N / E / S / W bezel markers with minor ticks every 30°
- Aircraft drawn as chevrons rotated to their true track, coloured by altitude band
- Callsign + flight level labelled on the nearest few, so the face stays readable when busy
- Centre shows local time, aircraft count, and the age of the last update — which goes amber,
  then red, if the feed stalls
- Positions interpolate between updates from ground speed and track, so blips glide instead
  of hopping every five seconds

**Trails**
- Five minutes of history per aircraft, fading out along its length

**Tap an aircraft**
- Callsign and airline
- Route: origin → destination, with airport codes and cities
- Aircraft type, registration, and registered operator
- Altitude with climb/descent rate, ground speed, distance, and bearing
- Details are fetched only when you tap, then cached

**Range control**
- Drag a finger around the outer bezel to step through 5 / 10 / 25 / 50 / 100 nm
- Your choice is remembered across reboots

**Setup and upkeep**
- Browser-based flashing and Wi-Fi provisioning — no IDE needed to install it
- Captive-portal fallback if provisioning doesn't take
- Small web UI on your LAN for settings, plus over-the-air firmware updates
- Backlight dims on a schedule at night; any touch wakes it to full brightness
- IMU detects when the device is turned, and re-orients the detail panel text

## Flashing

### Install from the browser *(planned, once the first release ships)*

The intended path — no toolchain, no checkout:

1. Plug the board into your computer with a USB-C data cable.
2. Open the install page (link will be published here) in **Chrome or Edge** on a desktop.
   Safari and Firefox do not implement WebSerial and cannot flash.
3. Click **Install**, pick the serial port that appears, and wait.
4. When it finishes, the same page offers to send your Wi-Fi credentials over the serial
   connection (Improv). Enter your network and password there.
5. The device connects and asks for your home latitude and longitude. There's a button to fill
   them from your browser's location.

If Wi-Fi provisioning doesn't complete, the device raises its own access point named
`FlightTracker-XXXX`. Join it and a setup page opens automatically.

### Build and flash it yourself *(works today)*

Requires [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html).

```bash
git clone https://github.com/shaunjanssens/esp32-flight-tracker.git
cd esp32-flight-tracker
pio run -t upload
```

The first build downloads the ESP32 toolchain and libraries — a few hundred megabytes and
several minutes. Later builds are quick.

To watch the serial output:

```bash
pio device monitor
```

The board exposes a native USB serial port (`303A:1001`, "USB JTAG/serial debug unit") and
resets into the bootloader on its own, so there are no buttons to hold while flashing.

### Updating later

Once the device is on your network, firmware updates are uploaded through its own web page —
no cable. Point a browser at `http://flighttracker.local/` (or the device's IP, shown on
screen during setup) for settings and OTA updates.

## Configuration

Everything is set through the browser and stored on the device; nothing needs recompiling.

| Setting | Notes |
|---|---|
| Home latitude / longitude | The centre of the radar. Decimal degrees. |
| Default range | 5 / 10 / 25 / 50 / 100 nm |
| Data provider | adsb.lol (default) or adsb.fi |
| Night dimming | Start and end time, dimmed brightness level |
| Timezone | Defaults to `Europe/Brussels`, from NTP |

## Data sources

This project is a client for infrastructure other people pay for and volunteers feed. Please
be a good guest: don't lower the poll interval, and don't point a fleet of these at it.

- Aircraft positions: [adsb.lol](https://adsb.lol) (default) or [adsb.fi](https://adsb.fi)
  — both free and unauthenticated, both fed by volunteer receivers.
- Routes and aircraft details: [adsbdb](https://www.adsbdb.com/), queried only when you tap
  an aircraft, and cached.

Position data is licensed **ODbL**; the device carries the attribution on its about screen.

Nothing here decodes radio. If you'd rather feed the network than only consume it, an RTL-SDR
and [these instructions](https://adsb.lol/docs/feeding/) will get you a receiver — and a
future version can read from your own `dump1090` instead of the internet.

## Documentation

- [PLAN.md](PLAN.md) — architecture, UI design, memory budgets, risks, and build order

## License

Not yet chosen — MIT is the intent, but no `LICENSE` file has been added.
