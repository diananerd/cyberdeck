# Deck Components, Distribution, and Porting
**Version 1.0 — IDF Component Registry, Repo Strategy, and Cross-SoC Portability**

---

## 1. Purpose

Everything we invent for Deck — the runtime, the bridge layers, the service drivers, the optional capabilities (Markdown, future modules), the conformance suite, the tooling — is designed for **publication and reuse** by people who are not us. The goal is that a CyberDeck-class device, an unknown hobbyist's RP2350 build, or a Linux desktop simulator can all share most of the code, with each platform implementing only the parts that touch its specific hardware.

This document specifies:

- The **component model** we publish under: ESP-IDF Component Registry with a `idf_component.yml` manifest and `idf_component_manager` v3 dependency resolution.
- The **repository strategy**: each shippable artifact lives in its own GitHub repository with its own versioning, CI, releases, and license. The CyberDeck firmware repo consumes them as **git submodules** and via `idf.py add-dependency`.
- The **catalog of components** we will publish, what each one provides, and how they relate.
- The **cross-SoC porting guide**: what an implementer needs to do to bring Deck to a new SoC family (ESP32-C6, ESP32-P4, RP2350, STM32, hosted Linux/macOS for development).
- **Versioning, release, and CI** conventions across all the repos.
- The **conformance suite** that validates any platform implementation against the SDI.

What is **out of scope** here: the language semantics (`01–02`), the runtime backend (`11`), the SDI itself (`12`), and the CyberDeck-specific implementation (`13`). This document is about the meta — how the work is packaged and distributed.

### Companion documents

| Doc | Role |
|---|---|
| `11-deck-implementation` | The portable runtime — the largest single component |
| `12-deck-service-drivers` | The contract every platform implements |
| `13-deck-cyberdeck-platform` | The reference implementation that consumes these components |

---

## 2. The Component Model

### 2.1 What an ESP-IDF Component Is

ESP-IDF defines a **component** as a directory tree containing at minimum:

- `CMakeLists.txt` — declares dependencies, sources, includes, compile options
- `idf_component.yml` — the manifest read by `idf_component_manager` (publish, version, registry deps)
- `include/<name>/` — public C headers
- Source files, examples, tests

Components are consumed by adding them to a project's `idf_component.yml` (or a parent component's). The manager fetches them from the **ESP Component Registry** at `components.espressif.com` (or any compatible registry mirror) and places them under `managed_components/` at build time.

This is exactly the model the CyberDeck repo already uses — it pulls `espressif/esp_lcd_touch_gt911`, `espressif/esp_lcd_touch`, and `lvgl/lvgl` via the registry today. Every Deck component follows the same model.

### 2.2 Manifest example (`idf_component.yml`)

```yaml
name: deck-runtime
version: "1.0.0"
description: |
  Portable Deck language runtime. Implements the lexer, parser, loader,
  evaluator, effect dispatcher, scheduler, navigation manager, snapshot/restore,
  and DVC wire format per the Deck specification (deck-lang/11-deck-implementation).

  This component is hardware-agnostic and depends only on a Service Driver
  Interface implementation (deck-service-drivers v1.x) provided by the host.
url: https://github.com/<org>/deck-runtime
documentation: https://github.com/<org>/deck-runtime/tree/main/docs
license: MIT
maintainers:
  - <user> <email@example.com>
tags:
  - deck
  - runtime
  - interpreter
  - dsl

targets:
  # Empty targets list = all targets supported. The runtime is portable C99.

dependencies:
  idf:
    version: ">=5.0"     # works back to v5.0 even though our reference build is v6.0
  <org>/deck-service-drivers:
    version: "^1.0.0"
    git: https://github.com/<org>/deck-service-drivers

examples:
  - path: examples/mock_bridge
    description: Runs the runtime against a mock bridge (no hardware required)
  - path: examples/conformance
    description: Runs the conformance suite

files:
  exclude:
    - tests/**
    - benchmarks/**
```

The `examples/` directory is auto-discovered by the registry, providing one-line `idf.py create-project-from-example <component>:<example_name>` flows.

### 2.3 Manifest fields we use (and their conventions)

| Field | Value pattern |
|---|---|
| `name` | Lowercase with hyphens. Matches the GitHub repo name. |
| `version` | Strict semver. No leading `v`. Pre-release tags allowed (`1.0.0-beta.2`). |
| `description` | Multi-paragraph. First paragraph is a one-line summary; subsequent paragraphs explain when to use this vs alternatives. |
| `url` | The GitHub repo URL. |
| `documentation` | A docs URL — typically a `docs/` directory in the repo, or a GitHub Pages site. |
| `license` | SPDX identifier. Default for our components: **MIT**. Apache-2.0 acceptable for components that re-export Apache code. |
| `maintainers` | The list of GitHub handles + emails. |
| `tags` | Always include `deck`. Add per-component tags (e.g. `wifi`, `lvgl`, `markdown`). |
| `targets` | Empty for portable components. Pinned (e.g. `[esp32s3]`) for platform-specific drivers. |
| `dependencies` | Other registry components OR git URLs (for org-internal pre-publication dev). |
| `examples` | Each example is a complete project; `idf.py create-project-from-example` works. |

### 2.4 Versioning (semver, strict)

Every component is versioned per **Semantic Versioning 2.0.0**:

- **Major** bump: any breaking change to public C headers, SDI vtable layout, on-disk formats, behavior contracts. Consumers must update their dependency range.
- **Minor** bump: backward-compatible additions (new functions, new optional fields, new `capability_flags`).
- **Patch** bump: bug fixes, doc-only changes, internal refactors.

Pre-1.0 components MAY break on minor bumps; once a component reaches 1.0, the strict rules apply.

Dependencies use the `^` caret operator (`^1.2.0` = `>= 1.2.0, < 2.0.0`) by default. Pinning to exact versions is reserved for known incompatibilities documented in the component README.

> **Component versioning is one of five concepts** in the broader Deck versioning model. The full picture — editions, surface API levels, runtime versions, SDI versions, app versions, and how they all interact — lives in `15-deck-versioning`. When publishing a driver component, refer to §11 of that doc to understand how `impl_version` propagates to apps via `system.info.versions()`, and to §16 (the compatibility matrix) for who-must-do-what when each version bumps.

### 2.5 Registry namespace

Components are published under a single org namespace on the ESP Component Registry. Convention: `<org>/<name>` where `<org>` is the GitHub organization name (chosen at first publication). The runtime, drivers, conformance suite, and tools all live under the same namespace so users can discover them with one search.

Examples (placeholder org `cyberdeck`):

- `cyberdeck/deck-runtime`
- `cyberdeck/deck-service-drivers`
- `cyberdeck/deck-bridge-lvgl`
- `cyberdeck/deck-driver-esp32-storage-fat`
- `cyberdeck/deck-conformance-suite`

---

## 3. Repository Strategy

### 3.1 One repo per component

Each shippable component is **its own GitHub repository**. Reasons:

- **Independent versioning.** A bug fix in `deck-driver-esp32-storage-fat` ships immediately as a 0.1.4 patch without bumping anything else.
- **Independent CI.** Each repo runs only the tests it owns. A flaky test in `deck-markdown` does not block a release of `deck-runtime`.
- **Independent issue trackers.** Users who hit a problem with the touch driver file an issue in the touch driver repo; the runtime repo stays focused on language/runtime issues.
- **Independent licenses.** Each component declares its own license. We default to MIT but a future component could be Apache-2.0 or LGPL if it integrates code with that constraint.
- **Discoverability.** Each repo's README is the primary documentation surface. Cross-repo navigation through GitHub or the Component Registry is straightforward.

This is the same model Espressif themselves use: `esp-lcd-touch`, `esp_lcd_touch_gt911`, `network_provisioning`, `mqtt`, `cjson` are all separate repos under their `espressif/` org. We mirror that pattern.

### 3.2 The CyberDeck firmware repo composes via submodules

The CyberDeck firmware repo (this one) consumes Deck components two ways:

**Via `idf.py add-dependency` (registry, recommended for stable releases):**

```yaml
# main/idf_component.yml
dependencies:
  cyberdeck/deck-runtime: "^1.0"
  cyberdeck/deck-bridge-lvgl: "^1.0"
  cyberdeck/deck-driver-esp32-storage-fat: "^1.0"
  cyberdeck/deck-driver-esp32-network: "^1.0"
  # ...
```

This pulls them from the registry into `managed_components/` at build time. Reproducible via `dependencies.lock`.

**Via git submodule (development, recommended for cross-repo work):**

```bash
git submodule add https://github.com/cyberdeck/deck-runtime components/deck_runtime
git submodule add https://github.com/cyberdeck/deck-bridge-lvgl components/deck_bridge_lvgl
git submodule add https://github.com/cyberdeck/deck-driver-esp32-storage-fat components/deck_drivers_storage_fat
# ...
```

Submodules let you check out a specific commit (or branch) of a component, edit it in place, and run the firmware against your edits — useful when fixing bugs or adding features that span multiple repos. After verifying, the change is committed in the component's own repo, released, and the firmware switches back to the registry version.

We recommend submodules for **active development** and registry deps for **stable builds**. The build system handles both — a component found at `components/<name>/` shadows any registry version with the same name.

### 3.3 The standard component repo layout

```
deck-runtime/                     (one example; same shape for every component)
├── README.md                     # Markdown; primary documentation
├── LICENSE                       # SPDX-tagged
├── CHANGELOG.md                  # human-readable version history
├── CONTRIBUTING.md               # how to file issues / PRs
├── idf_component.yml             # registry manifest
├── CMakeLists.txt                # idf_component_register(...)
├── Kconfig                       # any per-component CONFIG_ keys
├── include/
│   └── deck/
│       └── runtime/              # public headers go here; subdirectory matches component name
├── src/                          # implementation files (.c)
│   └── ...
├── examples/
│   ├── mock_bridge/              # full project; idf.py create-project-from-example works
│   │   ├── README.md
│   │   ├── CMakeLists.txt
│   │   ├── main/
│   │   └── sdkconfig.defaults
│   └── ...
├── test_apps/                    # ESP-IDF test_app pattern
│   └── unit_test/
│       ├── CMakeLists.txt
│       └── main/
├── docs/                         # extended documentation
│   ├── architecture.md
│   └── ...
└── .github/
    └── workflows/
        ├── ci.yml                # build + test on every PR
        └── release.yml           # publish to registry on tag
```

Public C headers live under `include/deck/<component>/` so consumers `#include <deck/runtime/runtime.h>` (or equivalent). This is the discoverable namespace.

### 3.4 Naming convention

All published Deck components have names starting with `deck-`:

- `deck-runtime` — the portable runtime
- `deck-service-drivers` — the SDI headers (no implementation; just contracts)
- `deck-bridge-lvgl` — bridge UI implementation using LVGL
- `deck-driver-<platform>-<service>` — platform-specific service driver

For platform-specific drivers, the convention is `deck-driver-<platform>-<service>`:

- `deck-driver-esp32-storage-fat` — FAT-FS storage driver for ESP-IDF
- `deck-driver-esp32-network` — WiFi + HTTP + downloader for ESP-IDF
- `deck-driver-rp2350-storage-littlefs` — LittleFS-based storage on RP2350
- `deck-driver-linux-network` — POSIX sockets + curl-based driver for hosted dev

This makes it obvious from the name what platform a component targets and which SDI driver it implements.

### 3.5 The umbrella repo

Some users will want a one-shot install — a single repo or single component that pulls in "everything Deck for ESP32-S3." We provide that as an **umbrella component**:

```
deck-platform-esp32s3/
├── README.md
├── idf_component.yml             # depends on every relevant deck-* component
└── CMakeLists.txt                # empty INTERFACE component
```

Consumers add `cyberdeck/deck-platform-esp32s3: "^1.0"` and get the runtime, the LVGL bridge, the storage drivers, the network drivers, the OTA drivers, the system drivers, the crypto driver, the BLE driver, and the OS-surface blob — all as a single dependency.

For other platforms (`deck-platform-rp2350-littlefs`, etc.), an umbrella analog can be assembled from the appropriate component set.

The umbrella is **just a manifest** — no code of its own. Users with custom needs (e.g. a non-LVGL bridge) can skip the umbrella and depend on the individual components.

---

## 4. Component Catalog (v1)

The minimum set we will publish. New components can be added without breaking existing ones.

### 4.1 Core (platform-agnostic)

| Component | What it provides | Depends on |
|---|---|---|
| `deck-runtime` | The portable interpreter. Lexer, parser, loader, evaluator, dispatcher, scheduler, navigation manager, snapshot/restore, DVC wire format. Pure C99. | `deck-service-drivers` (interface only) |
| `deck-service-drivers` | The SDI headers — every `Deck*Driver` struct, every lifecycle / threading / memory contract. No implementation. | — |
| `deck-os-surface-base` | A minimal `.deck-os` text covering builtins and the standard capability set. Platforms include this and append their own platform-specific surface entries. | — |
| `deck-conformance-suite` | A test app that loads any registered SDI driver set and validates behavior. Used by platform implementers to certify their drivers. | `deck-runtime`, `deck-service-drivers` |
| `deck-markdown` | Implementation of the core markdown surface (`@builtin md` per `03-deck-os §3`, `@capability markdown` per §4.4, `DVC_MARKDOWN`/`DVC_MARKDOWN_EDITOR` rendering per `02-deck-app §12.3`). Markdown is part of the standard library; this component is the canonical implementation, always shipped by official platforms. | `deck-runtime`, md4c (managed component) |

### 4.2 Bridge UI implementations

| Component | Renders against | Uses |
|---|---|---|
| `deck-bridge-lvgl` | LVGL 8.4+ | esp_lvgl_port (when on ESP-IDF), or any LVGL port |
| `deck-bridge-sdl` (future) | SDL2 / SDL3 | Hosted desktop dev |
| `deck-bridge-terminal` (future) | ANSI terminal | CI / headless test |

A platform composes one bridge UI driver. Apps render identically against all of them (subject to natural surface differences: a terminal cannot show images).

### 4.3 ESP-IDF service drivers (one repo per bundle)

Bundled by service category to keep the dependency graph manageable. Each repo provides one or more `deck.driver.*` SDI implementations for ESP-IDF v5.x+ targets.

| Component | Drivers provided |
|---|---|
| `deck-driver-esp32-storage-fat` | `deck.driver.storage.fs` (SD via FAT-FS), `deck.driver.storage.local` (KV over fs), `deck.driver.storage.db` (SQLite over fs) |
| `deck-driver-esp32-storage-nvs` | `deck.driver.storage.nvs` (HMAC-encrypted), `deck.driver.storage.cache` (in-memory LRU) |
| `deck-driver-esp32-network` | `deck.driver.network.wifi` (esp_wifi), `deck.driver.network.http` (esp_http_client + PSA TLS), `deck.driver.network.socket` (lwIP), `deck.driver.network.downloader` (queued + Range), `deck.driver.network.notifications` (poller) |
| `deck-driver-esp32-mqtt` | `deck.driver.network.mqtt` (espressif/mqtt) |
| `deck-driver-esp32-display-rgb` | `deck.driver.display.panel` for `esp_lcd_panel_rgb`, `deck.driver.display.screen` |
| `deck-driver-esp32-display-i80` | (alt) `deck.driver.display.panel` for `esp_lcd_panel_i80` (parallel-bus displays without RGB controller) |
| `deck-driver-esp32-touch-gt911` | `deck.driver.display.touch` over `esp_lcd_touch_gt911` |
| `deck-driver-esp32-touch-cst820` | (alt) `deck.driver.display.touch` over `esp_lcd_touch_cst820` |
| `deck-driver-esp32-ota` | `deck.driver.ota.firmware` (esp_https_ota), `deck.driver.ota.app` (downloader + atomic SD swap) |
| `deck-driver-esp32-system` | `deck.driver.system.info`, `.locale`, `.time`, `.battery`, `.security`, `.shell`, `.apps`, `.tasks`, `.crashes`, `.audio`, `.theme`, `.notify` |
| `deck-driver-esp32-crypto-psa` | `deck.driver.crypto.aes` over PSA Crypto |
| `deck-driver-esp32-ble-nimble` | `deck.driver.ble` over NimBLE |
| `deck-driver-esp32-bt-classic-uart` | `deck.driver.bt_classic` over an external UART module |
| `deck-driver-esp32-hardware` | `deck.driver.hardware.uart`, `.gpio` (rare), other raw-hardware caps |

Each repo can pin minimum ESP-IDF version (`>=5.2`, `>=6.0`, etc.) per the underlying API requirements.

### 4.4 Board-surface blobs

The `.deck-os` surface text varies per board (which capabilities exist, which sensors, which buttons). One repo per board:

| Component | Board |
|---|---|
| `deck-os-surface-cyberdeck` | Waveshare ESP32-S3-Touch-LCD-4.3 |
| `deck-os-surface-esp32-s3-devkit-c-1` | Espressif's reference DevKit (no display) |
| `deck-os-surface-rp2350-pico-display` | RP2350 + small color display |

A board-surface component depends on the relevant driver components and assembles the right combination for its hardware.

### 4.5 Tooling repos (not ESP-IDF components, but related)

| Repo | What it does |
|---|---|
| `deck-tools` | Standalone Python: `pack_app.py`, `sign_bundle.py`, `decode_panic.py`, `provision.py` |
| `deck-vscode-extension` | Syntax highlighting, type-aware completions, `idf.py monitor` integration |
| `deck-language-server` | LSP implementation; consumed by the VS Code extension and any LSP-capable editor |
| `deck-docs-site` | The `https://deck-lang.org` documentation site (sources for what we're writing now) |

---

## 5. App Bundle Tooling

App bundles (per `13-deck-cyberdeck-platform §6`) are produced by `pack_app.py` from `deck-tools`. The tool:

1. Reads `app.deck` to discover `app.id` and `app.version`.
2. Walks `@use ./...` paths to identify all source files in the bundle.
3. Walks `@assets` to identify required asset files.
4. Validates that no path escapes the bundle directory.
5. Optionally invokes `sign_bundle.py` to attach an Ed25519 signature.
6. Produces `<app_id>-<version>.tar.zst`.

Reverse: `unpack_app.py` reads a bundle and prints its `@app` metadata, signature status, and file manifest. Useful for debugging an OTA update.

The OTA-distribution **manifest** (the JSON descriptor at the URL given to `ota.app.check()`) is generated separately by `make_manifest.py`:

```bash
deck-tools/make_manifest.py \
  --app-id social.bsky.app \
  --version 1.0.0 \
  --bundle-url https://example.com/bsky-1.0.0.tar.zst \
  --bundle-file bsky-1.0.0.tar.zst \
  --signing-key ~/.deck/keys/community.pem \
  > manifest.json
```

Output matches the format in `13-deck-cyberdeck-platform §13.3`.

---

## 6. Conformance Suite

`deck-conformance-suite` is the canonical test harness any platform implementer runs to certify their SDI implementation. It is itself an ESP-IDF component containing a test app.

### 6.1 What it tests

| Suite | What it covers |
|---|---|
| `runtime/` | The runtime against a mock bridge: every value type round-trips, every dispatcher path, every scheduler edge case. ~500 tests. |
| `sdi-storage/` | Each `deck.driver.storage.*` driver: read/write semantics, error atoms, lifecycle, sandbox enforcement. |
| `sdi-network/` | Each `deck.driver.network.*` driver against a local mock HTTP server (lwIP loopback). |
| `sdi-display/` | Panel + touch drivers: geometry, frame buffer correctness, touch coordinate mapping. |
| `sdi-system/` | All `deck.driver.system.*` drivers. |
| `sdi-ota/` | Firmware + app OTA against a mock manifest server. |
| `sdi-crypto/` | KAT (known-answer tests) for AES. |
| `e2e/` | A complete Deck app launches, renders, suspends, restores, terminates. |

### 6.2 How a platform implementer uses it

```bash
# In a fresh ESP-IDF project for the target platform:
idf.py add-dependency cyberdeck/deck-conformance-suite
idf.py add-dependency cyberdeck/deck-runtime
idf.py add-dependency cyberdeck/<your-platform-driver-bundle>

# Build and flash the conformance test app:
idf.py -p /dev/cu.usbmodem... flash monitor

# The test app prints PASS/FAIL per suite and overall.
```

A platform that passes all suites can be listed in the official **certified platforms** page on the docs site.

### 6.3 Mock implementations for desktop

A `deck-mock-bridge` component (also published) provides a minimal SDI implementation that runs on a hosted Linux/macOS dev machine. Tests run there as a sanity check before flashing to real hardware. The mock implements:

- `storage.local`, `nvs`, `fs`, `db`, `cache` over an in-memory tmpfs
- `network.http` via libcurl
- `display.panel` to a PNG file (one frame per call)
- All `system.*` drivers as no-op stubs
- `crypto.aes` via OpenSSL

This is **not** the same as `deck-bridge-sdl` — the mock is for tests, not for end-user GUI apps.

---

## 7. Cross-SoC Porting Guide

This section is what an implementer reads when they want to bring Deck to a SoC family we don't yet officially support. Examples: ESP32-C6 (we'd inherit most of the ESP-IDF drivers), RP2350 (Pico SDK based), STM32 (Zephyr or HAL-based), hosted Linux (POSIX).

### 7.1 What you implement

To bring Deck to a new platform, you write:

1. **Service driver implementations** for at least the minimum set:
   - `deck.driver.storage.fs` and either `deck.driver.storage.nvs` or another KV store
   - `deck.driver.system.info` (device id, model)
   - `deck.driver.system.time` (clock + NTP if available)
   - `deck.driver.display.panel` and `deck.driver.display.touch` (or render-only without input for kiosk-style devices)
   - `deck.driver.bridge.ui` — typically you adopt `deck-bridge-lvgl` or write your own
   - `deck.driver.system.security` (PIN + lockscreen state)
   - `deck.driver.system.shell`, `.apps`, `.tasks`, `.crashes`
   - `deck.driver.crypto.aes`

2. **A `.deck-os` surface** describing what your platform exposes (which capabilities, which events). Start from `deck-os-surface-base` and add platform-specific entries.

3. **A boot harness** that:
   - Initializes your platform's HAL
   - Constructs each driver instance and registers it via `deck_registry_register()`
   - Calls `deck_bridge_bind_capabilities(bridge, registry)` to wire drivers to the runtime
   - Calls `deck_runtime_load(rt, bundled_launcher_dir)` and `deck_runtime_start(rt)`

4. **Build glue**: a CMakeLists.txt (for ESP-IDF and STM32CubeIDE), a `Cargo.toml` (for Rust+Embassy), or a plain Makefile (for hosted POSIX) that produces a flashable image.

5. **An umbrella component** (optional but encouraged): `deck-platform-<your-soc>` that depends on all the right drivers and surface so end users get one-line install.

### 7.2 What you do NOT implement

You do not modify `deck-runtime`, `deck-service-drivers`, or `deck-os-surface-base`. Those are shared. If you find a need to modify them, that's a sign of a portability bug — file an issue against `deck-runtime` and propose a fix (a new SDI flag, a new lifecycle hook) rather than forking.

You do not need to implement every driver. Drivers absent at boot mark their backing capabilities `:unavailable`. Apps that declared the missing capability with `optional` continue to run; apps that declared it as required fail to launch with a clear error.

### 7.3 Platform tiers

We use a tier model for officially-supported platforms:

| Tier | Criteria | Examples |
|---|---|---|
| **Tier 1** | All drivers in §4.3 implemented; passes full conformance suite; CI runs against it on every release; project maintains hardware in lab | CyberDeck (Waveshare ESP32-S3-Touch-LCD-4.3) |
| **Tier 2** | Most drivers implemented; passes most conformance suites; community-maintained; periodic compatibility check | (TBD; ESP32-C6 or future Espressif boards) |
| **Tier 3** | Subset of drivers; passes whatever applies; no formal commitment from project | (TBD; community ports) |

Tier 1 is reserved for boards the project itself develops on. Tier 2 expands as community contributors maintain other boards. Tier 3 is the long tail.

### 7.4 The hosted dev-loop platform

We ship a `deck-platform-hosted` umbrella for development on Linux/macOS:

- `deck-driver-hosted-storage` — uses temp directories
- `deck-driver-hosted-network` — uses libcurl + system DNS
- `deck-driver-hosted-display-sdl` — uses SDL3 for an 800×480 window
- `deck-driver-hosted-touch-mouse` — mouse maps to single-touch
- `deck-driver-hosted-system` — POSIX time, hardcoded device info
- `deck-driver-hosted-crypto-openssl` — OpenSSL EVP

Run a Deck app on your laptop:

```bash
git clone https://github.com/<org>/deck-runtime
cd deck-runtime/examples/hosted_run
cmake -B build && cmake --build build
./build/deck_run /path/to/your/app
```

Same `.deck` files, same behavior, but no hardware required. This is the recommended dev loop for app authors who don't want to flash a board on every iteration.

### 7.5 Adding a new SDI driver type

If your platform exposes hardware that doesn't fit any existing SDI driver — for example, an e-ink display with refresh modes that `deck.driver.display.panel` doesn't model, or a LoRa radio — propose a new driver type via an RFC issue against `deck-service-drivers`. The process:

1. Open an issue describing the use case and proposed driver vtable.
2. The maintainers review for fit (could it be a capability flag on an existing driver?), naming, threading, ownership.
3. On acceptance, the new driver type lands in `deck-service-drivers` with a minor-version bump.
4. You publish the implementation as `deck-driver-<your-platform>-<new-driver-name>`.

This is the same RFC pattern Espressif uses for new ESP-IDF APIs.

---

## 8. CI and Release Process

### 8.1 Per-component CI

Each component repo runs CI on every PR and every push to `main`:

```yaml
# .github/workflows/ci.yml (simplified)
name: CI
on: [push, pull_request]
jobs:
  build:
    runs-on: ubuntu-latest
    strategy:
      matrix:
        idf_version: ["v5.2", "v5.3", "v6.0"]
        target: ["esp32", "esp32s3", "esp32c6"]   # per-component matrix
    steps:
      - uses: actions/checkout@v4
      - uses: espressif/esp-idf-ci-action@v1
        with:
          esp_idf_version: ${{ matrix.idf_version }}
          target: ${{ matrix.target }}
          path: examples/mock_bridge
      - run: ./scripts/run-tests.sh
```

Hosted-dev components use plain `cmake` + `ctest`; no ESP-IDF needed.

### 8.2 Releases

Releases follow the standard registry flow:

1. Update `version` in `idf_component.yml`.
2. Update `CHANGELOG.md` with the new entry.
3. Tag the release: `git tag v1.2.3 && git push --tags`.
4. The `release.yml` workflow runs `compote component upload` (the official ESP-IDF Component Registry CLI) to publish to `components.espressif.com/<org>/<name>/<version>`.

A pre-release flow (`v1.2.3-beta.1`) is supported and useful for cross-component coordination.

### 8.3 Coordinating cross-component changes

When a change spans multiple repos (e.g. adding a new SDI driver type that the runtime, the LVGL bridge, and three platform drivers all need), the workflow is:

1. Open a draft PR in each affected repo.
2. Use git submodules in your local working firmware to test the combination end-to-end.
3. Merge in dependency order: leaf-first (`deck-service-drivers`), then consumers (`deck-runtime`, `deck-bridge-lvgl`, drivers).
4. Each repo releases independently with bumped versions.
5. Update the umbrella component's manifest to require the new minor versions.

There is **no monorepo**. Coordination overhead is the price for independent versioning, and we accept it.

---

## 9. License

Default license for all Deck components: **MIT**. Components that integrate code under different licenses (e.g. md4c is MIT, NimBLE is Apache-2.0, SQLite is public domain) MAY be Apache-2.0 if the bundled code requires it.

Each repo's `LICENSE` file is the source of truth. The `idf_component.yml` `license:` field uses the SPDX identifier and MUST match.

Contributors retain copyright on their contributions; the project does not require a CLA for v1. (A CLA may be introduced if a foundation or commercial entity later steward the project.)

---

## 10. Documentation Practices

Each repo's `README.md` is the user-facing entry point. Standard structure:

```markdown
# <component-name>

One-line summary.

[![CI](badge)] [![Registry](badge)]

## What this is
2–4 paragraphs of context and use case.

## When to use
Bullet list of fitting scenarios.

## When NOT to use
Bullet list with pointers to alternatives.

## Installation

`idf.py add-dependency <namespace>/<name>^<version>`

## Quick start
A 10-line example that does something interesting.

## Documentation
Links to `docs/` for deeper material; specific deck-lang spec sections that this implements.

## License
MIT (or Apache-2.0).
```

`docs/` contains long-form material:

- `architecture.md` — how the component is structured internally
- `api.md` — full API reference (could be auto-generated from headers)
- `examples.md` — annotated walkthrough of each example project
- `migration.md` — when there's been a breaking version change

A central `https://deck-lang.org` site (`deck-docs-site`) aggregates everything: the language spec (this `deck-lang/` directory), per-component docs, the platform porting guide, the conformance suite report.

---

## 11. Governance and Contribution

For v1, governance is informal — the original maintainers approve PRs and cut releases. As the project matures, we will adopt:

- **CODEOWNERS** files in each repo identifying maintainers.
- **A weekly issue triage cadence** for the core repos.
- **An RFC process** (described in §7.5) for substantial changes.
- **A community Slack/Discord/Matrix channel** for synchronous discussion (TBD).

Contributions follow the repo's `CONTRIBUTING.md`. The defaults:

- Sign-off (`Signed-off-by:` line) is required.
- Tests for any behavior change.
- Docs update for any user-visible change.
- A passing CI is mandatory for merge.

---

## 12. Out of Scope for v1

- A web-based registry browser of our own (we use components.espressif.com).
- Automated dependency-update bots across the component constellation (manual for v1).
- Pre-built binary distribution for any component (everything is built from source via `idf.py`).
- A formal trademark policy.
- A code-of-conduct beyond the standard contributor covenant.
- Translations of the documentation (English only for v1).

These are roadmap items, not v1 commitments. The goal of v1 is a working component constellation that other developers can adopt without our involvement.
