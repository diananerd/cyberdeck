# CyberDeck

ESP32-S3 firmware for the [Waveshare ESP32-S3-Touch-LCD-4.3](https://www.waveshare.com/esp32-s3-touch-lcd-4.3.htm) board — an OS-like launcher with a terminal-aesthetic UI built on ESP-IDF and LVGL 8.

The long-term goal is a fully self-contained handheld OS that runs apps written in **Deck**, a purpose-built embedded DSL that lives on the SD card and runs on a sandboxed interpreter — no recompilation, no flashing.

**Version:** 0.10.0 — **DL2 certified** · **Target:** ESP32-S3 · **Display:** 800×480 RGB LCD activo

El firmware es un runtime Deck DL2 completo: lenguaje extendido (fn/lambda/list/tuple/map/recursion/match/pipe/where), efectos enforced, async dispatcher, los 12 drivers SDI DL2 (NVS/FS read+write/Info/Time SNTP/Shell/WiFi/HTTP/Battery/Security PIN/Bridge UI/Display/Touch), bridge UI sobre LVGL 8.4 con DVC wire format y activity stack, shell con lockscreen + intent navigation + statusbar/navbar, 5 system apps bundled (launcher, counter, taskman, net_hello, settings) y conformance harness con `deck_level=2 deck_os=2`. Conformance reporta **96/96 verdes** en hardware (5/5 suites + 76/76 deck tests + 15/15 stress). Ver [CHANGELOG.md](CHANGELOG.md) y [DEVELOPMENT-PLAN-DL2.md](DEVELOPMENT-PLAN-DL2.md).

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

## Architecture (DL2)

```
┌──────────────────────────────────────────────────────────────┐
│  deck_shell  · launcher · counter · taskman · net_hello ·   │  System apps + shell
│              · settings · lockscreen · intent navigation     │
├──────────────────────────────────────────────────────────────┤
│  deck_bridge_ui  · LVGL 8.4 task on Core 1                   │  UI bridge
│                  · DVC decoder + activity stack              │
│                  · statusbar + navbar + overlays + rotation  │
├──────────────────────────────────────────────────────────────┤
│  deck_runtime  · DL2 lexer + parser + loader + interpreter   │  Deck language
│                · DVC wire format encoder/decoder              │
├──────────────────────────────────────────────────────────────┤
│  deck_sdi  · 12 drivers: storage.nvs, storage.fs, system.    │  SDI contract
│            info/time/shell/battery/security, network.wifi/    │
│            http, bridge.ui, display.panel, display.touch     │
├──────────────────────────────────────────────────────────────┤
│  board  · HAL: LCD (RGB), GT911 touch, CH422G, RTC, SD,     │  Hardware
│         · battery ADC, backlight                              │
└──────────────────────────────────────────────────────────────┘
```

Navigation uses a **push/pop activity stack** (max 4 levels). The launcher is always at the bottom. Each activity owns its LVGL screen and local state; transitions are instant with no slide animations. Statusbar (top, 36px) + navbar (bottom, 48px) live on `lv_layer_top` so they survive activity swaps + rotation.

---

## Project Status

**DL2 certified — v0.10.0 (2026-04-17).** El runtime + bridge UI + shell + 5 system apps corren en hardware sin panic. Conformance suite verde.

| Component | Status |
|---|---|
| Deck DL1 language (lexer/parser/loader/interp) | Done (v0.6.0) |
| Deck DL2 language (fn/lambda/list/map/match/pipe/where/types) | Done (v0.7.x) |
| SDI DL2 drivers (12 total) | Done (v0.8.0) |
| Bridge UI + DVC + LVGL | Done (v0.8.5) |
| Shell DL2 (lockscreen, intent nav, statusbar/navbar) | Done (v0.9.0) |
| App model DL2 parser (machine.before/after, flow, migration, assets) | Parser-only (v0.9.1); runtime semantics post-DL2 |
| System apps (launcher, counter, taskman, net_hello, settings) | Done (v0.9.5) |
| Conformance DL2 | Done (v0.9.9) |
| .deck source apps loaded from SD | Planned (post-DL2) |

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
