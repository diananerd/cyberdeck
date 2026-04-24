# ARCHITECTURE.md — CyberDeck firmware

Reference overview of the C firmware stack that hosts the Deck runtime on the Waveshare ESP32-S3-Touch-LCD-4.3 board. For the language / OS / bridge contract, see the pillar specs in [`deck-lang/`](deck-lang/).

---

## Stack of layers

```
┌─────────────────────────────────────────────────────────────┐
│  apps/  (bundles .deck on SD or SPIFFS)                     │
│    · user apps authored against the Deck spec               │
│    · apps/demo.deck — combinatorial stress test             │
│    · apps/conformance/*.deck — conformance fixtures         │
├─────────────────────────────────────────────────────────────┤
│  components/deck_shell/          Shell                       │
│    · boot sequence + app discovery                          │
│    · activity stack                                         │
│    · HOME/BACK/TASK-SWITCH event routing                    │
│    · app lifecycle (launch / suspend / resume / terminate)  │
├─────────────────────────────────────────────────────────────┤
│  components/deck_bridge_ui/      UI bridge (BRIDGE.md)      │
│    · LVGL 8.4 on Core 1                                     │
│    · DVC snapshot decoder + presentation                    │
│    · UI services (toast, confirm, loading, progress, …)     │
│    · statusbar + navbar + rotation + theme                  │
├─────────────────────────────────────────────────────────────┤
│  components/deck_runtime/        Interpreter (LANG.md)      │
│    · types + refcount allocator + string interning          │
│    · lexer + parser (indent-sensitive)                      │
│    · loader (multi-stage: lex/parse/type/cap-bind)          │
│    · tree-walking evaluator + continuation scheduler        │
│    · @machine runtime (states, transitions, hooks)          │
│    · dependency-tracked @on watch re-evaluation             │
│    · DVC snapshot encoder                                   │
├─────────────────────────────────────────────────────────────┤
│  components/deck_sdi/            Service Driver Interface   │
│    · driver registry + typed vtables                        │
│    · drivers back the services catalogued in SERVICES.md    │
│    · bridge.ui slot implemented by deck_bridge_ui           │
├─────────────────────────────────────────────────────────────┤
│  components/deck_conformance/    Conformance harness        │
│    · runs apps/conformance/*.deck fixtures                  │
│    · reports per-suite results to console                   │
├─────────────────────────────────────────────────────────────┤
│  components/board/               HAL                         │
│    · I2C bus + CH422G expander                              │
│    · RGB LCD + GT911 touch + backlight                      │
│    · RTC (PCF85063A), battery ADC, SD card                  │
├─────────────────────────────────────────────────────────────┤
│  ESP-IDF v6.0 + FreeRTOS + xtensa-esp32s3 toolchain         │
└─────────────────────────────────────────────────────────────┘
```

### Dependency rules

- A layer references only the layer immediately below (or ESP-IDF directly for primitives).
- `deck_runtime` never touches `board/` — all hardware access routes through SDI.
- `deck_bridge_ui` consumes `display.panel` + `display.touch` through SDI; it does not call `hal_*` directly.
- Apps touch nothing C-level; they talk to services via capabilities.

---

## Core design decisions

1. **The spec is the contract.** The five pillar docs in `deck-lang/` define what this firmware must implement. Code catches up to spec, not the reverse.
2. **SDI is the only bridge between runtime and hardware.** No runtime code includes `board/` headers or ESP-IDF primitives directly — always through a typed driver.
3. **The UI bridge is not a capability.** Apps never `@use` the bridge. They declare `content = …`; the runtime pushes snapshots to the bridge; the bridge presents. See `BRIDGE.md` §0.
4. **Apps declare semantic intent, never layout.** No `column` / `row` / `card` / `grid` / `status_bar` / `nav_bar` in app code. The bridge infers presentation per substrate.
5. **Content pipeline is unidirectional.** Render produces no events; intents fire only on genuine user activation or declared external sources. See `BRIDGE.md` §6.1 for the no-loop invariant.
6. **Event bus lives in the runtime.** `@on os.*` subscriptions and internal event routing are part of the scheduler, not a separate component.

---

## Boot sequence

```
ESP-IDF brings up PSRAM, heap, FreeRTOS, USB-JTAG
  └─ app_main() (main/main.c)
        ├─ nvs_flash_init()
        ├─ board HAL init (I2C, expander, LCD, touch, SD, RTC, battery)
        ├─ deck_sdi_registry_init()
        │    └─ register every driver the platform provides
        ├─ deck_bridge_ui_register_lvgl()   (LVGL on Core 1)
        ├─ deck_runtime_init()
        │    └─ alloc limits, intern table, builtin registration
        └─ deck_shell_init()
             └─ discover installed apps (SD + SPIFFS + bundled)
             └─ launch the Launcher app (slot 0)
                  ├─ loader evaluates @on launch
                  └─ @machine enters initial state
                        └─ content tree → DVC snapshot → bridge presents
```

Touch / gesture events arrive at the bridge, get pre-filtered for HOME/BACK/TASK-SWITCH gestures (per `BRIDGE.md §39`), and otherwise flow to the content's intent system via `intent_fn` → runtime dispatcher → `Machine.send(…)`.

---

## Partition & storage layout

- **Internal flash (8 MB):** ESP-IDF partitions + bundled system apps in SPIFFS.
- **PSRAM (8 MB):** framebuffer (double-buffered), runtime heap, LVGL draw buffers.
- **SD card:** user-installed `.deck` apps, `@config` storage per app, `@assets` cache.
- **NVS:** `@config` keys for apps that declare them, user settings (theme, PIN, WiFi creds, brightness, timeout).

The bridge owns its own heap budget separate from the runtime heap; neither budget is shared with apps. Per-app `@needs.max_heap` is enforced by the runtime allocator.

---

## Testing

Conformance fixtures live at [`apps/conformance/*.deck`](apps/conformance/). The harness in `components/deck_conformance/` runs them and reports pass/fail per suite.

There are no on-host unit tests — every test runs on hardware. Results are emitted to the UART console.

See [`CLAUDE.md`](CLAUDE.md) for the flash workflow, JTAG debug setup, and hardware-specific notes.
