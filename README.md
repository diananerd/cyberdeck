# CyberDeck

ESP32-S3 firmware for the [Waveshare ESP32-S3-Touch-LCD-4.3](https://www.waveshare.com/esp32-s3-touch-lcd-4.3.htm) board — an OS-like launcher with a terminal-aesthetic UI built on ESP-IDF and LVGL 8.

**Version:** 0.1.0 · **Target:** ESP32-S3 · **Display:** 800×480 RGB LCD

---

## Hardware

| Component | Part |
|---|---|
| SoC | ESP32-S3 @ 240 MHz, 8 MB Flash, 8 MB PSRAM (Octal) |
| Display | 4.3" RGB LCD, 800×480, ST7701 driver |
| Touch | GT911 capacitive |
| I/O expander | CH422G (backlight, LCD reset, SD CS, USB mux) |
| RTC | PCF85063A |
| SD card | SPI via CH422G chip-select |
| Console | UART0 via CH343 USB-Serial |
| Flash/JTAG | ESP32-S3 native USB (USB Serial/JTAG) |

---

## Features

- **Launcher** — grid of app cards with touch navigation
- **Settings** — WiFi provisioning, display (brightness, theme, rotation, timeout), time (SNTP), storage, security (PIN lock), audio
- **Three color themes** — Green (Matrix), Amber (Retro), Neon (Cyberpunk)
- **Display rotation** — landscape and portrait, all screens adapt
- **PIN lockscreen** — 4-digit PIN with hash stored in NVS
- **OTA firmware updates** — via HTTPS
- **Touch gestures** — swipe up = HOME, swipe down = BACK

---

## Building

**Requirements:** ESP-IDF ≥ 5.1 (tested with 6.0.0)

```bash
# One-time: set IDF_PATH and activate the toolchain
. $IDF_PATH/export.sh

# Build
idf.py build

# Flash (USB native port)
idf.py -p /dev/cu.usbmodem1101 flash

# Serial console (CH343 UART port)
idf.py -p /dev/cu.usbmodem58A60705271 monitor
```

### Flash sequence

The board has two USB-C ports. Always flash via the **USB native** port — the CH343 UART port fails with checksum errors.

1. Hold **BOOT** + press **RESET** → download mode
2. `idf.py -p /dev/cu.usbmodem1101 flash`
3. Disconnect both cables, wait a few seconds, reconnect USB native first then UART

---

## Architecture

```
┌──────────────────────────────────────┐
│     apps/launcher · apps/settings    │  Concrete apps
├──────────────────────────────────────┤
│            app_framework             │  App registry, lifecycle, navigation
├──────────────────────────────────────┤
│             ui_engine                │  LVGL task, activity stack, theme, effects
├──────────────────────────────────────┤
│            sys_services              │  WiFi, battery, NVS settings, OTA, events
├──────────────────────────────────────┤
│               board                  │  HAL: LCD, touch, CH422G, RTC, SD card
└──────────────────────────────────────┘
```

Navigation uses a **push/pop activity stack** (max 4 levels). The launcher is always at the bottom. Each activity owns its LVGL screen and local state; transitions are instant with no slide animations.

---

## Project Status

| App slot | Status |
|---|---|
| Launcher | Done |
| Settings (WiFi, Display, Time, Storage, Security, Audio, About) | Done |
| Books, Notes, Tasks, Music, Podcasts, Calculator, Bluesky, Files | Planned |

---

## License

[MIT](LICENSE) © 2026 Diana Nerd
