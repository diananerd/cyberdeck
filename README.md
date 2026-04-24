# CyberDeck

ESP32-S3 firmware for the [Waveshare ESP32-S3-Touch-LCD-4.3](https://www.waveshare.com/esp32-s3-touch-lcd-4.3.htm) board — a self-contained handheld that runs apps written in **Deck**, a purpose-built embedded DSL that loads from the SD card without reflashing.

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

## The Deck Spec

The authoritative specification of the Deck language, its OS surface, and the UI bridge lives in [`deck-lang/`](deck-lang/):

| File | Contents |
|---|---|
| [`LANG.md`](deck-lang/LANG.md) | Language — lexical structure, types, all 14 annotations, error model, runtime envelope |
| [`SERVICES.md`](deck-lang/SERVICES.md) | OS foundation + catalog of 31 system services across 5 tiers |
| [`CAPABILITIES.md`](deck-lang/CAPABILITIES.md) | Consumer protocol — how apps `@use` services, declare grants, handle errors |
| [`BUILTINS.md`](deck-lang/BUILTINS.md) | 14 in-VM modules (`math`, `text`, `list`, `map`, `stream`, `bytes`, `option`, `result`, `record`, `json`, `time`, `log`, `rand`, `type_of`) |
| [`BRIDGE.md`](deck-lang/BRIDGE.md) | UI bridge — semantic-to-presentation contract, inference rules, UI services, CyberDeck reference implementation |

Authority order: `LANG > SERVICES > CAPABILITIES = BUILTINS = BRIDGE`. The five pillars are self-contained and internally consistent; there is no previous version to migrate from.

---

## Implementation Status

Firmware code (in `components/`, `main/`, `apps/`) may lag the spec. When spec and code diverge, the spec wins — code is brought up to spec, not the reverse. See [`CLAUDE.md`](CLAUDE.md) for the repo conventions, flash workflow, and hardware notes, and [`REPORTS.md`](REPORTS.md) for the iteration log that captures how the spec and code evolved.

---

## License

[MIT](LICENSE) © 2026 Diana Nerd
