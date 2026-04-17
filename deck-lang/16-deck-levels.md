# Deck Conformance Levels
**Version 1.0 — Progressive Profiles for Embedded Targets**

---

## 1. Why This Document Exists

Deck is a language *and* an operating system. A full reference implementation on a flagship MCU (ESP32-S3, 8 MB flash, 8 MB PSRAM) is one target — but not the only one. We want a community to grow around Deck, and that community includes people on cheaper silicon: ESP32-C3 with no PSRAM, RP2350 with a few hundred KB of SRAM, hobbyists porting to exotic boards. A spec that forces the full surface on every port locks out those builders; a spec that is "whatever fits" produces fragmentation nobody can reason about.

This document defines **three conformance levels** — **DL1**, **DL2**, **DL3** — each a *layered subset* of the full Deck specification. Each level is:

- **Complete by itself.** A DL1 runtime runs DL1 apps deterministically, on tiny targets, with no holes.
- **A strict subset of the next level.** Every DL1 app runs on a DL2 or DL3 runtime unchanged. DL3 never breaks DL2.
- **Testable.** Each level has a conformance profile in the reference test suite; a platform claims a level only if it passes every test at that level and below.
- **Discoverable from code.** An app declares the minimum level it needs via `@requires.deck_level`. The loader rejects the app with a structured error if the runtime can't satisfy it.

The model is modeled on CSS 1 / 2.1 / 3 — a layered standard where each level is a snapshot that implementations can claim conformance with, rather than a checklist of cherry-picked features. Unlike CSS the levels here are **not temporal** (DL1 is not "older"); they are **capacity bands**.

The long-term goal is for CyberDeck (the 4.3" ESP32-S3 board this repo targets) to be **DL3-conforming** as the reference implementation, while cheap or constrained ports can claim **DL1** or **DL2** and still run a meaningful fraction of the app ecosystem.

---

## 2. The Three Levels at a Glance

| | **DL1 — Core** | **DL2 — Standard** | **DL3 — Full** |
|---|---|---|---|
| **Purpose** | Minimum viable Deck OS | Expected embedded profile | Reference / flagship |
| **Target hardware example** | ESP32-C3 (400 KB SRAM / 4 MB flash), RP2350 | ESP32-S3 without PSRAM, ESP32-S3 4 MB flash | ESP32-S3 8 MB flash + 8 MB PSRAM (CyberDeck) |
| **Runtime flash** | ≤ 120 KB | ≤ 280 KB | ≤ 520 KB |
| **Runtime heap** | ≤ 64 KB (no PSRAM required) | ≤ 512 KB SRAM, PSRAM optional | ≥ 2 MB PSRAM |
| **Concurrent Deck apps** | 1 (singleton runtime) | 2–3 (foreground + background) | 4+ (full task stack) |
| **UI** | Optional; text or none | Single-screen panel + touch | 800×480 RGB + touch + rotation |
| **Network** | None required | WiFi + HTTP | WiFi + HTTP + MQTT + WebSocket + BLE |
| **Persistence** | NVS + FS read-only | + FS write + cache | + SQLite + OTA dual-track |
| **Apps runnable** | Launcher (stub) + hello-world | Launcher + Task Manager + simple apps | All bundled apps + Bluesky kitchen-sink |

DL levels are **orthogonal** to the five version concepts in `15-deck-versioning.md`: an edition (`2026`), a runtime semver (`1.0.0`), and a surface API level (integer) are things that bump over time. A DL level is a **horizontal slice** of the surface at a given edition, carving out what must be present for the runtime to claim that tier.

Mapping to existing version concepts:

```
Edition 2026  →  defines the language (DL1/2/3 all use the same edition)
Surface API level 1  →  the full capability catalog
    ├── DL1 subset  →  required capabilities for Core profile
    ├── DL2 subset  →  required capabilities for Standard profile
    └── DL3 subset  →  the full catalog (every DL3 runtime must satisfy all of surface level 1)
SDI 1.0  →  vtable contract; DL1 implements only a prefix, DL3 implements it all
```

---

## 3. Design Principles

### 3.1 Strict subset

Every feature in DL*n* must also be in DL*n+1*. There is no "DL1 has this and DL2 removed it." Apps targeting DL1 run unmodified on DL3 hardware.

### 3.2 Complete, not partial

DL1 must be *useful on its own*. The whole point of tiered conformance is that a DL1 device is a functional Deck OS — not a stub. We do not ship DL1 with missing load-time checks, crashing pattern matchers, or half-implemented modules.

### 3.3 Levels are capacity bands, not roadmap phases

DL1/2/3 describe what a runtime *supports*, not the order in which we *implement*. The reference implementation targets DL3 from day one; we simply first ship the subset that *also* constitutes DL1, then the DL2 superset, then fill DL3. A platform implementer can claim DL1 forever, they are not expected to race toward DL3.

### 3.4 One line declares conformance

An app opts in with a single line:

```deck
@app
  name:    "Notes"
  id:      "local.notes"
  version: "1.0.0"
  edition: 2026
  requires:
    deck_level: 2         # requires a DL2 or higher runtime
    deck_os:    ">= 1"
    runtime:    ">= 1.0"
```

The loader refuses to run this app on a DL1 runtime and produces error `E_LEVEL_BELOW_REQUIRED` (see §10).

### 3.5 Capabilities, not knobs

We avoid granular feature flags ("requires SQLite", "requires WiFi"). Those belong to `@use` and `@requires.capabilities` from the existing model. DL level is the *coarse-grained* category that tells the app developer which *classes of feature* are available at all.

### 3.6 The platform decides, the app requests

Platform implementers declare the level they conform to in their component manifest (`idf_component.yml` extension, see §11). Apps request the level they need. The loader mediates.

---

## 4. DL1 — Core Profile

**Mission:** prove Deck can run on a cheap MCU. A DL1 runtime is enough to load a `.deck` file from flash, evaluate a single-app state machine, persist a few keys to NVS, and emit log output. It may have no screen. It may have no network. It has no SD card.

### 4.1 Language features required at DL1

From `01-deck-lang.md`:

| Feature | Section | Required because |
|---|---|---|
| Lexical structure (UTF-8, comments, indentation, identifiers, literals) | §2 | No parser without it |
| Primitive types: `int`, `float`, `bool`, `str`, `byte`, `unit` | §3.1 | Any computation |
| Composite types: `list T`, `map K V`, tuples | §3.2 | Data shapes |
| `Optional T` (`T?`) | §3.3 | Absence without null |
| `let` bindings | §5 | Core binding form |
| Pure functions | §6.1 | Core abstraction |
| Pattern matching — basic (atoms, literals, wildcards) + exhaustiveness check | §8.1–8.3 | The only branching construct |
| Arithmetic, comparison, logical operators | §7.1–7.3 | Base operators |
| String concatenation + interpolation | §7.4, §2.6 | Built-in |
| Module system (file = module, imports) | §10 | Multi-file programs |
| Type conversion builtins (`str`, `int`, `float`, `bool`) | §11.1 | Required for interop |

### 4.2 App model features required at DL1

From `02-deck-app.md`:

| Feature | Section | Required because |
|---|---|---|
| `@app` header (id, name, version, edition) | §3 | App identity |
| `@app.entry` pointing to a minimal state target | §3 | Entry point |
| `@use` of *local* modules | §4.2 | Module graph resolution |
| `@machine` with at least two states + at least one transition | §8.1–8.4 | Any stateful behavior |
| `@machine.on enter / leave` lifecycle hooks (`on enter:` / `on leave:`) | §8.5 | Minimal lifecycle |
| `@on launch` hook | §11 | App startup callback |

**Not required at DL1:** `@use` of OS capabilities, `@requires` blocks beyond `edition` and `deck_level`, `@permissions`, `@config`, `@errors`, `@flow` sugar, `@stream`, `@task`, `@migration`, `@handles`, `@assets`, `content =` view bodies.

### 4.3 OS surface required at DL1

From `03-deck-os.md` (capabilities) and `05-deck-os-api.md` (services):

| Capability | Required surface |
|---|---|
| `math`, `text`, `bytes`, `log` (pure builtins) | Full |
| `time.now`, `time.duration`, `time.to_iso` | Full |
| `system.info` (device_id, free_heap, app_id) | Full |
| `nvs.get`, `nvs.set`, `nvs.delete` | Full |
| `fs.read`, `fs.exists`, `fs.list` | Read-only subset |
| `os.resume`, `os.suspend`, `os.terminate` | Full |
| Event bus core events: `HOME`, `BACK`, `os.app_launched`, `os.app_terminated` | Full |

### 4.4 Runtime required at DL1

From `04-deck-runtime.md` and `11-deck-implementation.md`:

- Lexer, LL(1) parser, AST tree-walking evaluator
- Loader stages 0–9 (lexical, parse, type, capability-bind, linkage) — no snapshot stage
- Pattern-matching switch compilation
- Effect dispatcher core routing (no continuations, synchronous effects only)
- Immutable values with reference counting, no GC
- String interning
- Single evaluator thread
- Crash handler (panic → safe unwind → reboot or restart app)
- Stack depth cap (512 frames default)
- Heap soft/hard limits with *hard-limit only* enforcement (no `os.memory_pressure` event at DL1)

### 4.5 SDI required at DL1

From `12-deck-service-drivers.md`, the following drivers are *mandatory* for a platform to claim DL1:

- `storage.nvs`
- `storage.fs` (read-only is acceptable)
- `system.info`
- `system.time` (monotonic only; wall clock optional)
- `system.shell` (minimal: push/pop one app)

Display and touch drivers are **optional** at DL1. A headless DL1 device is valid (useful for sensor nodes, actuators).

### 4.6 Bridge UI required at DL1

From `10-deck-bridge-ui.md`:

- **Optional at DL1.** A DL1 device may ship with no display.
- *If* a display is present, the required subset is:
  - Bridge UI core renderer + DVC decoder
  - Components: `LABEL`, `BUTTON`, `CONTAINER`, `LIST`, `ROW`
  - Services: toast

No `content =` view bodies in `@machine` at DL1 (content bodies are DL3). DL1 apps with UI use a simpler `render:` hook that returns a DVC tree directly.

### 4.7 Shell required at DL1

From `09-deck-shell.md`:

- Launcher stub (single-app device — launcher can be a no-op that auto-launches the one installed app)
- Event routing for `HOME` / `BACK`
- App lifecycle (launch, suspend, resume, terminate)
- No statusbar, no navbar, no lockscreen, no notifications

### 4.8 Apps runnable at DL1

- **Launcher** (slot 0) in "single-app" mode — no grid, just a hand-off to the installed app.
- Any user app written to DL1 constraints.

Bundled apps that are **not** runnable at DL1:
- Task Manager (needs live task/CPU streams → DL2)
- Settings (needs WiFi + crypto + OTA → DL3)
- Files (needs FS write + per-app sandbox introspection → DL3)
- Bluesky (kitchen sink → DL3)

### 4.9 Footprint target

| Segment | Budget |
|---|---|
| Runtime code (flash) | ≤ 120 KB |
| Runtime data (heap) | ≤ 64 KB |
| One loaded app (code + state) | ≤ 48 KB |
| **Total platform image** | ≤ 256 KB flash, ≤ 128 KB SRAM |

---

## 5. DL2 — Standard Profile

**Mission:** the profile a serious embedded Deck device ships with. WiFi, HTTP, a real UI, a lockscreen, a Task Manager, settings that make sense. DL2 is where most community ports aim.

### 5.1 Language features added at DL2

*(added on top of DL1)*

| Feature | Section | Added because |
|---|---|---|
| Union types `T1 \| T2` | §3.4 | Needed for Result and advanced matching |
| `Result T E` | §3.5 | Canonical error handling |
| Named record types (`@type`) | §4 + §9 | Schemas used by capabilities |
| Effect annotations (`!alias`) | §3.9, §6.2 | Required to declare side effects |
| `where` bindings | §5.2 | Local dependencies in functions |
| Functions with effects | §6.2 | Calling capabilities |
| Recursion + tail calls | §6.4–6.5 | Trampolining |
| Lambdas | §6.5 | Higher-order operations |
| Advanced pattern matching (records, variants with payload, guards) | §8.2 | Real data destructuring |
| `do` blocks | §7.5 | Sequencing effects |
| Pipe operators (`\|>`, `\|>?`) | §7.7, §7.9 | Ergonomic composition |
| `is` operator | §7.8 | Atom / state testing |
| List / map / tuple helpers (`len`, `head`, `map`, `filter`, `reduce`, …) | §11.2–11.4 | Standard library |
| Result / Optional helpers (`ok`, `err`, `unwrap`, `map_ok`, `and_then`, …) | §11.5–11.6 | Canonical error ergonomics |
| Type inspection (`type_of`, `is_int`, …) | §11.8 | Dynamic introspection |
| `@private` | §10.2 | Module encapsulation |

### 5.2 App model features added at DL2

| Feature | Section |
|---|---|
| `@use` of OS capabilities | §4.1 |
| `@use.optional` | §4.1 |
| `@requires` (full: deck_os, runtime, capabilities) | §4A |
| `@permissions` | §5 |
| `@config` | §6 |
| `@errors` | §7 |
| `@machine.before / after` transition hooks | §8.5 |
| `@flow` and `@flow.step` sugar | §9 |
| `@on` lifecycle hooks (launch, resume, pause, suspend, terminate, low_memory, network_change) | §11 |
| `@migration` | §15 |
| `@test` inline tests | §16 |
| `@assets` bundle | §19 |

### 5.3 OS surface added at DL2

| Capability / service | Source |
|---|---|
| `fs` — write, create, delete, mkdir | §05.2 |
| `http` — sync request with headers, body, TLS | §05.3 |
| `api_client` — configured session with retry + caching | §05.3 |
| `cache` — in-memory TTL'd cache | §05.5 |
| `crypto.aes` (encrypt/decrypt, gen_key) | §05.6 |
| `network.wifi` — scan, connect, status | §05.4 |
| `system.battery` — level, charging, low-battery event | §03 |
| `system.time` (with NTP) | §03 |
| `system.security` — PIN hash, lock/unlock, permission_get/set | §03 |
| `system.tasks` — list_processes, kill_process (no CPU stream) | §03 |
| `display.notify` — toast | §03 |
| `display.screen` — brightness, power | §03 |
| `system.locale` — language, timezone, formatters | §03 |
| System events: `EVT_BATTERY_UPDATED`, `EVT_WIFI_CONNECTED/DISCONNECTED`, `EVT_SETTINGS_CHANGED`, `EVT_DISPLAY_ROTATED`, `os.network_change`, `os.permission_change`, `os.memory_pressure` | §03, §09 |

### 5.4 Runtime added at DL2

- Scheduler with timers + condition tracking
- Stream buffers (ring, distinct, throttle, debounce) for selected services that expose watches
- Continuation table for async effect dispatch
- Memory pressure monitor → `os.memory_pressure` event
- IPC framing between runtime and bridge (async UI updates, queued snapshots)
- DVC wire format and render arena for `content =` in DL3 (DL2 apps still use direct render hooks)
- Single-evaluator-thread model retained; multi-app concurrency achieved via time-sliced scheduling

### 5.5 SDI added at DL2

Mandatory for a platform claiming DL2:

- `storage.fs` (read + write)
- `display.panel` (bitmap rendering)
- `display.touch` (poll-based, no multi-touch required)
- `network.wifi`
- `network.http`
- `system.battery`
- `system.time` (wall clock via NTP or RTC)
- `system.security`
- `bridge.ui` — the LVGL (or equivalent) decoder of DVC trees

### 5.6 Bridge UI added at DL2

| Additions | Notes |
|---|---|
| DVC components: `TEXT`, `PASSWORD`, `CHOICE`, `DATE`, `TOGGLE`, `RANGE`, `CONFIRM`, `LOADING` | Form inputs + modal dialogs |
| UI services: confirm, loading, keyboard (TEXT_UPPER), choice | Overlay system |
| Statusbar (minimal: time, WiFi, battery) | No BT / SD icons at DL2 |
| Navbar (BACK + HOME only) | No TASKS long-press |
| Activity stack (max 4) | Managed by shell |

### 5.7 Shell added at DL2

- Lockscreen with PIN numpad (basic)
- Statusbar service
- Navbar service (portrait + landscape routing)
- Intent-based navigation (`ui_intent_navigate`)
- Settings sub-screen conventions (screen_id namespacing)
- Display rotation (via `ui_activity_recreate_all`)

### 5.8 Apps runnable at DL2

- Launcher (full grid)
- Task Manager (functional — shows running apps, kill dialog)
- User apps using WiFi / HTTP / NVS / FS / cache / crypto
- **Not yet:** Settings (needs OTA + crypto.aes + BT detection → DL3), Files (needs FS sandbox inspection), Bluesky (kitchen sink).

### 5.9 Footprint target

| Segment | Budget |
|---|---|
| Runtime code (flash) | ≤ 280 KB |
| Runtime data (heap) | ≤ 512 KB SRAM (PSRAM optional) |
| Concurrent apps | 2–3 |
| **Total platform image** | ≤ 2 MB flash, ≤ 512 KB SRAM |

---

## 6. DL3 — Full Profile

**Mission:** the full Deck standard, the target CyberDeck (this repo) commits to. Every bundled app runs. Every capability is exposed. An app written to DL3 can use everything the spec defines.

### 6.1 Language features added at DL3

| Feature | Section |
|---|---|
| `Stream T` type | §01.3.10 |
| `Fragment` type | §01.3.11 |
| View content functions | §01.6.3 |
| `when` / `for` in content bodies | §01.7.6 |
| Functional utilities (`compose`, `flip`) | §01.11.9 |
| `random` (impure builtin) | §01.11.10 |

### 6.2 App model features added at DL3

| Feature | Section |
|---|---|
| `@use.when` (conditional capability activation) | §02.4.1 |
| `@machine.content =` | §02.8.2 |
| `@machine.watch:` (reactive watches) | §02.8.6 |
| `@machine` delegated (`machine:` / `flow:` composition) | §02.8.3 |
| `@stream` declarations | §02.10 |
| Content bodies: `group`, `text`, `button`, `input`, `list`, `markdown`, `image`, `canvas`, `when`, `for` | §02.12 |
| Content intents (action / toggle / navigate / send / emit) | §02.12.4 |
| `@task` and `@task.every` | §02.14 |
| `@doc` / `@example` | §02.17 |
| `@handles` deep links | §02.20 |

### 6.3 OS surface added at DL3

| Capability / service | Source |
|---|---|
| `db` (SQLite: exec, query, transaction) | §05.6 |
| `network.socket` (raw TCP/UDP) | §05.4 |
| `network.ws` (WebSocket) | §05.4 |
| `mqtt` | §05.5 |
| `network.downloader` (queued + resumable) | §05 |
| `network.notifications` (HTTP poll + MQTT push + badge streams) | §05 |
| `ble` (central, GATT, notifications) | §03 |
| `bt_classic` (stubbed if unsupported) | §03 |
| `sensors.<kind>` (temp, humidity, pressure, accel, gyro, light, GPS) | §03 |
| `system.audio` (PCM playback, volume, codecs) | §03 |
| `hardware.gpio`, `.i2c`, `.spi`, `.uart`, `.button`, `.door_opened` | §03 |
| `system.apps` (list_installed, config_schema, launch) | §03 |
| `system.crashes` | §03 |
| `system.shell` (privileged: push/pop any app, app switcher) | §03 |
| `ota.firmware` (dual-bank, anti-rollback, signature verify) | §05 |
| `ota.app` (per-app bundle install + migrations + rollback) | §05 |
| `background_fetch` | §03 |
| `markdown`, `markdown_editor` | §03 |
| `notifications` (history + remote source polling) | §03 |

### 6.4 Runtime added at DL3

- VM snapshot/restore (serialize heap + frames; used for suspend/resume of background apps)
- Hot reload (code replacement with state migration)
- Full IPC framing between runtime and bridge (multiple UI contexts)
- Background fetch scheduler integration with OS
- Multi-app concurrent evaluator (time-sliced; reference uses a single evaluator thread with cooperative scheduling)

### 6.5 SDI added at DL3

Mandatory for DL3 platforms (reference: CyberDeck):

- Every DL2 driver, **plus**:
- `storage.db` (SQLite)
- `storage.cache`
- `network.socket`
- `network.ws`
- `network.mqtt`
- `network.downloader`
- `network.notifications`
- `ble`
- `crypto.aes` (hardware-accelerated where available)
- `sensors.*` (at minimum, a stub that reports "not present")
- `system.audio`
- `hardware.*` (full set of pin-level drivers)
- `ota.firmware` + `ota.app` (dual-track)
- `bt_classic` (may be a stub on platforms without classic BT — ESP32-S3 included)

### 6.6 Bridge UI added at DL3

| Additions |
|---|
| DVC components: `SEARCH`, `PIN`, `PROGRESS`, `CANVAS`, `MARKDOWN`, `SHARE` |
| UI services: date picker, badge, progress overlay, share sheet, permission dialog |
| Statusbar complete: BT icon, SD icon, audio indicator |
| Navbar complete: BACK + HOME + TASKS (long-press app switcher) |
| Display rotation (`ui_activity_recreate_all`) |
| Theme switching (green / amber / neon) |

### 6.7 Shell added at DL3

- Lockscreen with auto-lock timer, low-battery trigger, PIN timeout
- Full Task Manager (CPU + heap streams, per-app storage inspection)
- Cross-app deep links via `@handles`
- App multi-instance via PSRAM-backed heap snapshots
- OTA upgrade flow with rollback UI

### 6.8 Apps runnable at DL3

All bundled apps: Launcher, Task Manager, Settings, Files, plus the Bluesky kitchen-sink reference.

### 6.9 Footprint target

| Segment | Budget |
|---|---|
| Runtime code (flash) | ≤ 520 KB |
| Runtime data (heap) | ≥ 2 MB PSRAM |
| Concurrent apps | 4+ |
| **Total platform image** | ≤ 8 MB flash, ≤ 8 MB PSRAM |

The reference implementation on CyberDeck uses the full 8 MB / 8 MB budget, leaving headroom for user apps loaded from SD.

---

## 7. Conformance Matrix

Consolidated view — quick reference when porting or writing apps.

### 7.1 Language

| Feature | DL1 | DL2 | DL3 |
|---|---|---|---|
| Primitives, composites, pattern matching core, modules | ● | ● | ● |
| Optional | ● | ● | ● |
| Union, Result, `@type` | | ● | ● |
| `!effects`, `do`, lambdas, recursion, where | | ● | ● |
| Advanced pattern matching (records, guards, variant payloads) | | ● | ● |
| List/Map/Tuple/Result/Optional stdlib | | ● | ● |
| Stream type, Fragment type, view content funcs | | | ● |
| Random, compose, flip | | | ● |

### 7.2 App model

| Feature | DL1 | DL2 | DL3 |
|---|---|---|---|
| `@app`, `@use` locals, `@machine` basic, `@on launch` | ● | ● | ● |
| `@use` OS + optional, `@requires`, `@permissions`, `@config`, `@errors`, `@flow`, `@on` full, `@migration`, `@test`, `@assets` | | ● | ● |
| `@use.when`, `@stream`, `content =`, `@machine.watch:`, `@task`, `@handles`, `@doc` | | | ● |

### 7.3 OS surface (capabilities)

| Capability | DL1 | DL2 | DL3 |
|---|---|---|---|
| math, text, bytes, log, time | ● | ● | ● |
| nvs, fs (read), system.info, os.{resume,suspend,terminate} | ● | ● | ● |
| fs (write), http, api_client, cache, crypto.aes, wifi, battery, system.time (NTP), system.security, system.tasks (list/kill), display.notify, display.screen, locale | | ● | ● |
| db, socket, ws, mqtt, downloader, notifications, ble, sensors.*, audio, hardware.*, system.apps, system.crashes, ota.firmware, ota.app, background_fetch, markdown | | | ● |

### 7.4 SDI drivers required

| Driver | DL1 | DL2 | DL3 |
|---|---|---|---|
| storage.nvs | ● | ● | ● |
| storage.fs (read) | ● | ● | ● |
| system.info | ● | ● | ● |
| system.time (monotonic) | ● | ● | ● |
| system.shell (minimal) | ● | ● | ● |
| storage.fs (write), display.panel, display.touch, network.wifi, network.http, system.battery, system.time (wall), system.security, bridge.ui | | ● | ● |
| storage.db, storage.cache, network.socket, network.ws, network.mqtt, network.downloader, network.notifications, ble, crypto.aes, sensors.*, system.audio, hardware.*, ota.firmware, ota.app, bt_classic (stub ok) | | | ● |

### 7.5 Bridge UI components

| Component | DL1 (optional) | DL2 | DL3 |
|---|---|---|---|
| LABEL, BUTTON, CONTAINER, LIST, ROW | ● | ● | ● |
| TEXT, PASSWORD, CHOICE, DATE, TOGGLE, RANGE, CONFIRM, LOADING | | ● | ● |
| SEARCH, PIN, PROGRESS, CANVAS, MARKDOWN, SHARE | | | ● |

### 7.6 Shell features

| Feature | DL1 | DL2 | DL3 |
|---|---|---|---|
| App lifecycle routing, HOME/BACK, single-app launcher | ● | ● | ● |
| Full launcher grid, lockscreen PIN, statusbar minimal, navbar, intent navigation, rotation | | ● | ● |
| Task switcher (long-press), auto-lock, statusbar full, deep links, app snapshot suspend, OTA flow | | | ● |

---

## 8. App-to-Level Mapping (bundled apps)

| App | Minimum DL | Why |
|---|---|---|
| Launcher (single-app mode) | DL1 | Must boot the OS |
| Launcher (full grid) | DL2 | Needs DVC grid + app streams |
| Task Manager | DL2 | Live process list + kill dialog |
| Settings | DL3 | OTA + crypto + BT detection |
| Files | DL3 | FS write + cross-app sandbox inspection |
| Bluesky (Annex XX) | DL3 | Uses 7+ capabilities + `@task` + `@stream` + content bodies |

The rule: the app declares the minimum in `@requires.deck_level`, and platforms that support the app ship that level or higher.

---

## 9. How Apps Declare and Runtimes Enforce

### 9.1 App declaration

```deck
@app
  name:    "Notes"
  id:      "local.notes"
  version: "1.2.0"
  edition: 2026
  requires:
    deck_level: 2          # requires DL2 or higher
    deck_os:    ">= 1"     # which surface API level
    runtime:    ">= 1.0"
    capabilities:
      http:      ">= 1"
      storage.fs: ">= 1"
```

`deck_level` is a *separate* key from `deck_os`. `deck_os` tells you *which capabilities* are defined. `deck_level` tells you *whether the runtime is big enough to host them*.

### 9.2 Runtime declaration

Every runtime advertises its level through the `system.info` capability at DL1:

```deck
system.info.deck_level     → 1 | 2 | 3
system.info.deck_os        → 1
system.info.runtime        → "1.0.0"
system.info.edition        → 2026
```

### 9.3 Load-time check

The loader evaluates `@requires.deck_level` against `system.info.deck_level` in stage 6 (compatibility check) of `04-deck-runtime.md`. On mismatch it emits:

```
E_LEVEL_BELOW_REQUIRED
  required: 2
  runtime:  1
  message:  "App 'local.notes' requires Deck Level 2 runtime; this device runs Level 1."
  hint:     "This app cannot run on this device. Install on a DL2+ device, or use a DL1-targeted build."
```

See `15-deck-versioning.md` §9 for the full structured error model. `E_LEVEL_BELOW_REQUIRED` is additive to the existing error codes.

### 9.4 Runtime probing (optional capabilities)

Apps using `@use.optional` or checking `@use.when` are expected to call `system.info.deck_level` at runtime when they adapt behavior to the host profile — e.g., a music player might offer waveform visualization only on DL3 (canvas) and fall back to a progress bar on DL2.

```deck
@machine player
  state playing:
    content =
      when system.info.deck_level >= 3 then
        canvas(draw: draw_waveform)
      else
        progress(value: position_pct)
```

---

## 10. Errors and Diagnostics

Three new structured errors are added to the `15-deck-versioning.md` catalog:

| Code | Meaning | Raised where |
|---|---|---|
| `E_LEVEL_BELOW_REQUIRED` | App wants level *n*, runtime provides *n−1* or lower | Loader stage 6 |
| `E_LEVEL_UNKNOWN` | App declares a level that doesn't exist in the spec this edition | Loader stage 6 |
| `E_LEVEL_INCONSISTENT` | App declares `deck_level: 1` but uses a DL2+ capability | Loader stage 4 (capability bind) |

All three carry `required`, `runtime`, `message`, `hint`, and are surfaced through the installer UI (Settings app on DL3 devices) with a human-readable explanation.

---

## 11. Platform Declaration

A platform component (reference: `deck-platform-cyberdeck` in the component registry, cf. `14-deck-components.md`) declares its conformance in the component manifest:

```yaml
# idf_component.yml
name: deck-platform-cyberdeck
version: 1.0.0

deck:
  level: 3                 # this platform claims DL3 conformance
  edition: 2026
  os_surface: 1
  runtime_range: ">=1.0, <2.0"
  sdi: "1.0"
  drivers:
    storage.nvs:    { version: "1.0.0", impl: "deck-driver-esp32-nvs" }
    storage.fs:     { version: "1.0.0", impl: "deck-driver-esp32-fatfs" }
    storage.db:     { version: "1.0.0", impl: "deck-driver-esp32-sqlite" }
    network.wifi:   { version: "1.0.0", impl: "deck-driver-esp32-wifi" }
    # … full list
```

The conformance suite (`deck-conformance-suite` from `14-deck-components.md` §9) runs the corresponding DL*n* battery and refuses to certify a higher level if any lower-level test fails.

### 11.1 Conformance suite tiers

- `conformance-DL1` — language core, module loader, pattern matching exhaustiveness, NVS read/write, fs read, event routing
- `conformance-DL2` — everything in DL1 plus wifi/http flow, lockscreen cycle, DVC form rendering, app lifecycle including suspend/resume, task list/kill
- `conformance-DL3` — everything in DL2 plus SQLite CRUD, MQTT pub/sub, BLE scan, OTA dry-run, content body reconciliation, deep link routing, rotation

A platform is the highest level whose test battery it passes end-to-end. Platforms claim a level by publishing a signed conformance report with the component release.

### 11.2 Partial conformance ("DL2 + X")

Platforms that do not claim DL3 but implement some DL3 capabilities (e.g., a DL2 platform that happens to have BLE) can list them under `deck.extras`:

```yaml
deck:
  level: 2
  extras:
    - ble
    - sensors.accel
```

Apps that use `@use.optional` see those capabilities at runtime. Apps that require them via `@requires.capabilities` are still rejected on DL2 even if the extras include them — extras do not raise the declared level.

---

## 12. Governance — Promoting Features Between Levels

A feature lives at a specific DL level in a given edition. Changing its level is a **spec change**. Rules:

1. **Never demote.** A DL2 feature cannot become DL1 in the same edition — that would retroactively require DL1 runtimes to implement it, breaking existing DL1 platforms.
2. **Promotion (DL3 → DL2) requires an edition bump.** Moving a feature into a lower profile means more platforms must implement it; that is a breaking change at the platform level and requires an edition cycle + deprecation warning period.
3. **Introduction always happens at the highest level.** A new capability lands in DL3 first and percolates down in future editions if the implementation footprint proves small enough for a smaller profile.
4. **Removal follows the deprecation pipeline from `15-deck-versioning.md` §8.** A feature cannot be removed from any level without going through deprecated → discouraged → removed across two editions minimum.

---

## 13. Relationship to Existing Documents

| Document | Interaction with DL levels |
|---|---|
| `01-deck-lang.md` | Features classified DL1/2/3 in §7.1 above |
| `02-deck-app.md` | `@requires.deck_level` added to the `@requires` block |
| `03-deck-os.md` | Capability catalog annotated per-capability with its level |
| `04-deck-runtime.md` | Loader stage 6 checks `deck_level`; `system.info.deck_level` is added |
| `05-deck-os-api.md` | Each service's sub-features tagged with level thresholds |
| `09-deck-shell.md` | Shell features layered — single-app launcher at DL1, full shell at DL2, multi-instance at DL3 |
| `10-deck-bridge-ui.md` | DVC component catalog split into DL1-required (optional screens), DL2, DL3 |
| `11-deck-implementation.md` | Runtime mechanisms (snapshot, hot reload, scheduler) mapped to DL2/3 |
| `12-deck-service-drivers.md` | SDI vtable split into DL1 prefix, DL2 middle, DL3 tail |
| `13-deck-cyberdeck-platform.md` | CyberDeck reference implementation declares DL3 |
| `14-deck-components.md` | Component manifests carry `deck.level` |
| `15-deck-versioning.md` | DL level is a new orthogonal dimension; structured errors extended |

When a future edit to any of those documents adds or moves a feature, this document's tables are the source of truth for which level it lives at.

---

## 14. Example — Three Devices, Three Stories

### 14.1 ESP32-C3 Sensor Node (DL1)

A minimal board that reads a temperature sensor every minute and writes the reading to NVS. No screen. No WiFi (for this example; WiFi would be DL2).

```deck
@app
  name:    "Temp Log"
  id:      "local.templog"
  version: "1.0.0"
  edition: 2026
  requires:
    deck_level: 1
  use:
    nvs
    time
    log

@machine logger
  state running:
    on enter:
      do
        let t = read_temp()               # platform-provided pure function
        nvs.set("last", to_str(t))
        log.info("Logged: " <> to_str(t))
```

Runs on a DL1 platform with **≤ 256 KB flash, ≤ 128 KB SRAM**.

### 14.2 ESP32-S3 Notes Device (DL2)

A pocket device with a small screen, WiFi, and the Notes app.

```deck
@app
  name:    "Notes"
  id:      "local.notes"
  version: "1.2.0"
  edition: 2026
  requires:
    deck_level: 2
  use:
    nvs
    fs
    http
    crypto.aes
```

Runs on a DL2 platform — 4 MB flash, 512 KB SRAM, optional PSRAM.

### 14.3 CyberDeck (DL3)

Full reference board. Runs every bundled app plus user apps loaded from SD. Reference implementation of the entire spec.

---

## 15. Roadmap for This Repository

This repository (`cyberdeck`) is the reference DL3 implementation. We ship in this order:

1. **v0.1 — DL1 core** on the CyberDeck hardware (proves the profile boundary).
2. **v0.5 — DL2** (full shell, WiFi, lockscreen, Task Manager).
3. **v1.0 — DL3** (all bundled apps including Bluesky, full SDI, OTA dual-track, conformance-DL3 green).

Each minor release between those milestones adds a contiguous chunk of DL*n* features (see the separate `DEVELOPMENT-PLAN.md` for the atomic breakdown).

---

## 16. Summary

- Three conformance levels: **DL1 (Core) / DL2 (Standard) / DL3 (Full)**.
- Each is a strict subset of the next; apps declare `@requires.deck_level`; runtimes declare their level via `system.info.deck_level`.
- Designed so DL1 runs on tiny MCUs (~256 KB flash) and DL3 is the CyberDeck target (~8 MB).
- Classification is spec-driven (every feature in documents 01–15 has a level assignment in the conformance matrix §7).
- Promotion between levels requires an edition bump; demotion is forbidden within an edition.
- Conformance is tested via `deck-conformance-suite` tiered batteries, not self-claimed.
