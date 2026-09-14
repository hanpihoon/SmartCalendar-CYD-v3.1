# v3.1 Easy Connect

Setup portal now prioritizes Google/Microsoft Connect buttons, cleans encoding artifacts, and moves ICS fields to Advanced/Optional. Note: true OAuth sign-in still requires registering Google/Microsoft applications and a secure redirect/token service; the firmware does not fake or embed account passwords.

## v3 Portrait fix

- Native portrait UI 240x320
- Correct ILI9341 MADCTL orientation (0x48)
- Portrait touch mapping and hit zones
- Portrait Today / Month / Settings layouts

# Smart Calendar Pro v3 Portrait — ESP32-2432S028R / CYD 2.8"

A polished desk-calendar firmware for the classic CYD 2.8" board (ESP32-WROOM-32, ILI9341 320×240, XPT2046 touch).

## What is new in v2

- Commercial-style dark dashboard UI
- Touch navigation: **Today / Month / Settings**
- Google Calendar + Outlook Calendar merged into one agenda
- Monthly calendar with event dots
- Wi‑Fi status + sync status
- NTP time sync
- Weather card using Open-Meteo (no API key required)
- Captive Wi‑Fi setup portal
- Event cache in SPIFFS so the agenda remains visible when Wi‑Fi is unavailable
- Local OTA firmware update page
- Configurable brightness, timezone, refresh interval and weather location
- Touch calibration values editable from the setup portal
- Hold BOOT for ~3 seconds to reopen setup mode

## First boot

If Wi‑Fi is not configured, the board creates:

- Wi‑Fi: `SmartCalendar-Setup`
- Password: `12345678`
- Portal: `http://192.168.4.1`

The portal lets you enter Wi‑Fi, Google/Outlook ICS links, timezone, weather location, brightness and touch calibration.

## Calendar connections

### Google Calendar
Google Calendar → Settings → your calendar → **Integrate calendar** → **Secret address in iCal format**.

### Outlook / Microsoft 365
Outlook Calendar → Settings → **Shared calendars** → **Publish a calendar** → choose **Can view all details** → copy the ICS link.

This firmware intentionally does not store your Google or Microsoft account password. Treat private ICS links as secrets.

## Build with PlatformIO

```bash
pio run -e esp32dev
```

Expected binaries:

- `.pio/build/esp32dev/bootloader.bin` → `0x1000`
- `.pio/build/esp32dev/partitions.bin` → `0x8000`
- `.pio/build/esp32dev/firmware.bin` → `0x10000`

Depending on the Arduino core build, `boot_app0.bin` may also be needed at `0xE000`. See `FLASH_OFFSETS.txt`.

## Flash with esptool.spacehuhn.com

Select **ESP32**, choose the binaries and offsets above, then flash.

## Local OTA after the first USB flash

Once the device is connected to your LAN, open its IP address in a browser. Under **Local Firmware Update**, upload a new `firmware.bin`. You no longer need USB for ordinary firmware updates.

## Touch calibration

CYD clones vary. Defaults target the common XPT2046 revision. If touch positions are mirrored or shifted, open Setup and adjust X/Y min/max and Swap/Invert options.

## Hardware note

This source targets the common **ESP32-2432S028R + ILI9341** revision. Some visually similar clones use another display controller and require driver changes.

## Build without installing PlatformIO locally (GitHub Actions)

This package includes `.github/workflows/build-firmware.yml`.

1. Create a GitHub repository and upload the project contents.
2. Open **Actions → Build Smart Calendar Firmware → Run workflow**.
3. Download the artifact named **SmartCalendar_CYD_Pro_v2_BIN**.
4. Flash those `.bin` files with esptool.spacehuhn.com using the offsets in `FLASH_OFFSETS.txt`.
