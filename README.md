# CyberDeck

ESP32-S3 firmware for the [Waveshare ESP32-S3-Touch-LCD-4.3](https://www.waveshare.com/esp32-s3-touch-lcd-4.3.htm) board — an OS-like launcher with a terminal-aesthetic UI built on ESP-IDF and LVGL 8.

The long-term goal is a fully self-contained handheld OS that runs apps written in **Deck**, a purpose-built embedded DSL that lives on the SD card and runs on a sandboxed interpreter — no recompilation, no flashing.

**Version:** 0.5.0 — **DL1 hardened** · **Target:** ESP32-S3 · **Display:** 800×480 RGB LCD (inactivo en DL1)

El firmware es un runtime Deck DL1 completo y endurecido para producción. Al boot arranca automáticamente `hello.deck` desde la partición SPIFFS `apps`. La batería de conformance (`deck_conformance_run`) pasa **55 checks verdes** en hardware: 5 suites C-side + 42 `.deck` tests (28 positivos + 14 negativos exhaustivos cubriendo todo el error surface DL1) + 8 stress/memory/perf (incluye concurrencia de log hook, rechazo de input corrupto, 100 reruns bajo churn, heap pressure con recuperación). Ver [CHANGELOG.md](CHANGELOG.md) y [tests/conformance/README.md](tests/conformance/README.md).

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
| Bitchat, Meshtastic, Mail | Planned |

---

## Deck Language

Apps in CyberDeck are designed to be written in **Deck** — a sandboxed, interpreted DSL that loads from the SD card without reflashing. Deck specs live in [`deck-lang/`](deck-lang/).

| File | Contents |
|---|---|
| [`01-deck-lang.md`](deck-lang/01-deck-lang.md) | Core language — syntax, types, pattern matching, effects |
| [`02-deck-app.md`](deck-lang/02-deck-app.md) | App model — `@app`, `@use`, `@on`, `@nav`, lifecycle annotations |
| [`03-deck-os.md`](deck-lang/03-deck-os.md) | OS interface — bridge protocol, `.deck-os` surface file, sandboxing |
| [`04-deck-runtime.md`](deck-lang/04-deck-runtime.md) | Interpreter internals — lexer, parser, evaluator, effect dispatcher |
| [`05-deck-os-api.md`](deck-lang/05-deck-os-api.md) | High-level OS services — SQLite, NVS, FS, HTTP client, MQTT, OTA, crypto |
| [`06-deck-native.md`](deck-lang/06-deck-native.md) | Native bindings — extending the runtime with C capabilities and events |
| [`07-deck-bluesky.md`](deck-lang/07-deck-bluesky.md) | Annex A — complete Bluesky ATProto client in Deck |
| [`08-deck-markdown.md`](deck-lang/08-deck-markdown.md) | Annex C — first-class Markdown rendering and editing capability |

See also [`GROUND-STATE.md`](GROUND-STATE.md) for a full audit of the current C API surface and what needs to be built before Deck can run, and [`APPS.md`](APPS.md) for the planned app catalog.

---

## License

[MIT](LICENSE) © 2026 Diana Nerd
