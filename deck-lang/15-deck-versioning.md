# Deck Versioning and Compatibility
**Version 1.0 — One Policy, Three Audiences, Five Versions**

---

## 1. Why This Document Exists

Deck is unusual: it is a language and an operating system at the same time. Most language projects can hide their compiler version from end users; most OS projects can pretend the user-facing API is stable forever. We can do neither. An app written in Deck depends on:

- The **language** itself (syntax, type system, semantics)
- The **OS surface** (which capabilities exist, what they accept, what they return)
- The **interpreter** that runs the app (how it implements the language)
- The **drivers** behind the OS surface (how each capability is actually realized on this hardware)
- The **app's own version** (its semver string)

A change to any of these can break or extend what an app can do. A naive approach — "just one big version number" — collapses all this into a coarse pin that breaks for the wrong reasons. A maximalist approach — "version everything individually, declare everything explicitly" — burdens the app developer with concerns that aren't theirs.

This document specifies a **layered, audience-aware** versioning scheme that gives each stakeholder exactly the controls they need, no more.

---

## 2. The Three Audiences

Every versioning decision is evaluated against what these three people experience.

### 2.1 OS Maintainer Dev

The people who write and evolve the language, the runtime, the SDI, and the official drivers. (Right now: us.) They own the spec, the reference implementation, the test suite, the documentation site.

What they bump: editions, surface API level, runtime semver, SDI semver, official driver semver, official component semver.

What they care about: governance ("when is it OK to bump?"), backward compatibility ("don't break existing apps without a deprecation cycle"), forward signaling ("tell apps about new capabilities cleanly").

### 2.2 Platform Implementer

People porting Deck to a new SoC, building drivers for new sensors, or shipping a board with a custom set of services. Examples: a community port to RP2350, an industrial vendor adding a CANbus driver, a hobbyist wiring up an e-paper display.

What they bump: per-platform driver semver, per-platform component semver, the platform's `.deck-os` extension version.

What they care about: knowing what they have to implement to claim conformance, knowing what they can extend without breaking apps, getting clear errors from the conformance suite.

### 2.3 App Developer

People writing apps in Deck. They edit `.deck` files. They never look inside the runtime, never read SDI headers, never touch driver C code. They pack their bundle and publish it.

What they bump: their app's semver.

What they care about: knowing which language version their app targets, knowing which capabilities they can rely on, getting clear "won't run on this device" errors with actionable fixes.

---

## 3. The Five Version Concepts

Deck has **five** distinct version concepts. Each captures a different axis of change. They are intentionally orthogonal — bumping one does not require bumping any other.

| # | Concept | Format | Bumped by | What it means |
|---|---|---|---|---|
| 1 | **Edition** | calendar year (`2026`) | OS Maintainer | A coherent snapshot of the language's syntax and semantics |
| 2 | **Surface API level** | integer (`1`, `2`, …) | OS Maintainer | Which capabilities exist in the canonical OS surface |
| 3 | **Runtime version** | semver (`1.0.0`) | OS Maintainer | Which interpreter implementation is running |
| 4 | **SDI version** | major.minor (`1.0`) | OS Maintainer | The C-level driver vtable contract |
| 5 | **App version** | semver (`1.0.0`) | App Developer | An app's own release version |

Per-driver implementation versions and per-IDF-component versions also exist (semver, owned by their respective publishers); they fall out of the SDI version concept and are described in §11.

### 3.1 Why five and not one

Each concept changes for a different reason and at a different cadence:

- An app developer who writes a bug fix bumps **app version** only — no language change, no OS change.
- A platform vendor who improves an HTTP driver bumps **driver version** only — same SDI, same surface.
- The runtime team adding a new pattern-matching optimization bumps **runtime version** only — same edition, same surface, same SDI.
- The OS team adding a new sensor capability bumps **surface API level** only — same edition, same runtime ABI.
- The OS team rewriting closure semantics bumps **edition** — the most disruptive change; orthogonal to everything else.

Trying to compress these into one version inevitably breaks the principle that "you should only have to update what actually changed."

### 3.2 The relationship to dependencies

Each version flows into the others through documented compatibility:

```
            ┌─────────────────────┐
   App ─────│ edition: 2026       │── language frontend
            │ requires:           │
            │   deck_os: ">= 2"   │── capability availability
            │   runtime: ">= 1.0" │── interpreter
            └─────────────────────┘
                       ↓
            Runtime checks all three at load time.
            Drivers and components are "below the line" —
            invisible to the app, but their versions
            determine what the surface can offer.
```

---

## 4. Edition

### 4.1 What an edition is

An **edition** is a stable, named snapshot of the language. It fixes:

- Lexical syntax (keywords, operators, indentation rules, literal forms)
- Grammar
- Type system rules
- Pattern matching semantics
- The set of allowed and forbidden constructs (e.g. "edition 2026 forbids `if/then/else`")
- The set of standard builtins available without `@use`

Within an edition, the language **cannot break**. Any future runtime that supports edition 2026 must accept any well-formed edition-2026 program written today and produce equivalent behavior.

Editions are how we get to make breaking language changes without breaking existing apps. The `@view`/`@nav` → `@machine`/`@flow` migration we did in 2026 will, retrospectively, become "edition 2026 introduced @machine/@flow as the canonical syntax; edition 2025 (if it existed) used @view/@nav."

### 4.2 Naming and cadence

Editions use the **calendar year** (`2026`). A new edition is published **at most once per year**, and only if there are language changes worth coordinating into a release. We are not obligated to bump every year; if a year passes with no breaking language changes, no new edition is published.

The current edition at this writing is **2026**.

### 4.3 What triggers a new edition

These are the only changes that require a new edition:

- A keyword is added (existing identifiers might collide)
- An operator's precedence or associativity changes
- A reserved syntax form is removed (e.g. `if/then/else`)
- The semantics of an existing form changes (e.g. closure capture rules)
- A previously-allowed program becomes a load error
- A predeclared atom name is reused for a different meaning

Adding a builtin function, adding a new node type, adding a new annotation that introduces no syntactic conflict — none of these require an edition bump. They go into the **next surface API level** instead.

### 4.4 What an edition does NOT include

- The set of OS capabilities (that's the surface API level)
- The interpreter implementation (that's the runtime version)
- Performance characteristics
- The wire format of snapshots or DVC trees (those have their own format_version fields)

### 4.5 How apps target an edition

```deck
@app
  name:    "Bluesky"
  id:      "social.bsky.app"
  version: "1.0.0"
  edition: 2026
```

`edition` is **mandatory** in `@app`. The loader rejects any app that omits it.

### 4.6 Edition support window

Every shipping runtime declares the set of editions it supports. The current rule:

- A runtime MUST support the current edition.
- A runtime SHOULD support the immediately previous edition (if one exists).
- A runtime MAY support older editions; the project commits to supporting **at least** the two most recent editions in the official runtime.

Apps targeting an edition not supported by the running runtime fail to load with the error code `:incompatible_edition` (§9.1).

### 4.7 No silent edition migration

The runtime never silently treats an edition-N program as edition-(N+1). The edition declared in `@app` is honored exactly. If the developer wants to upgrade an app from edition 2026 to edition 2027, they update the `edition:` field, run `deck migrate` (a tool that flags constructs needing changes), and accept that the new app version may not run on older runtimes.

### 4.8 Edition vs Rust editions

The model is closely inspired by Rust editions. Two differences worth noting:

- Rust's compiler is one process; ours is many independent runtimes shipped with different firmware versions. Edition support windows therefore matter more in our world.
- Rust uses three-year cadence (2015, 2018, 2021, 2024). We use one-year cadence as the *upper bound* but only publish when warranted.

---

## 5. Surface API Level (`deck_os_version`)

### 5.1 What it is

The **surface API level** is a strictly increasing integer that names a specific snapshot of the OS surface — the set of `@capability`, `@event`, `@type`, `@opaque`, and `@builtin` declarations in `.deck-os`. Currently `1`.

Each increment denotes a forwards-compatible expansion of the surface: new capabilities, new events, new types. Apps written against API level N continue to work against any API level ≥ N. The OS surface never silently shrinks within a major edition.

### 5.2 Why a single integer (and not semver)

The surface only ever grows or, in the case of formal removals, follows the deprecation cycle in §10. A single integer mirrors how Android's API levels work and is easy to reason about: "this device is at API level 3; my app needs ≥ 2; ✓."

A semver would suggest more nuance than exists. There is no concept of a "patch surface change" — a surface change is, by definition, observable to apps.

### 5.3 What triggers an increment

| Change | Bumps surface? | Notes |
|---|---|---|
| Add a new `@capability` | Yes | Always. New capabilities are net-new. |
| Add a method to an existing `@capability` | Yes | Apps targeting older levels can't see it. |
| Add an `@event` | Yes | Same reason. |
| Add an atom variant or `@type` | Yes | Same reason. |
| Add a flag bit to a capability `capability_flags` | No | Flags are advertised through `system.info`; apps probe at runtime. |
| Internal rewrite of a capability's implementation | No | Same surface = no bump. |
| Make an existing method more permissive (accept new arg combinations) | No | Old behavior preserved. |
| Tighten validation (reject inputs previously accepted) | Yes | Observable behavior change. |
| Remove a capability or method | Yes (after deprecation) | Per §10. |
| Rename a capability | Yes | Treated as remove-and-add. |

### 5.4 How apps target a surface level

```deck
@requires
  deck_os: ">= 2"
```

If absent, the loader assumes the lowest surface level satisfying every capability the app uses (computed from `@use` declarations). Explicit `@requires` is recommended for production apps because it makes the contract visible.

### 5.5 Detection at runtime

```deck
@use system.info as sysinfo

let info = sysinfo.versions()
log.info("running on surface API level " ++ str(info.deck_os))
```

The full `versions()` shape is in §12.

### 5.6 Optional surface extensions

A platform may add capabilities **beyond** the canonical surface. These are platform-specific and live in their own namespace:

```
# In a platform's .deck-os addendum:
@capability ext.acme.fancy_radio
  ...

@const ext.acme.fancy_radio.api_level: int = 1
```

The `ext.<vendor>.<name>` namespace is reserved for non-canonical capabilities. Apps that depend on extensions declare them like any other capability:

```deck
@requires
  deck_os: ">= 1"
  capabilities:
    ext.acme.fancy_radio: ">= 1"
```

The canonical `deck_os_version` does not bump for extension additions. Each extension carries its own `api_level` constant.

---

## 6. Runtime Version

### 6.1 What it is

The **runtime version** is a strict semver string identifying the interpreter implementation. Currently the reference implementation is at `1.0.0` (when first shipped).

It captures everything below the language and surface: the lexer, the parser, the loader, the evaluator, the dispatcher, the scheduler, the navigation manager, snapshot/restore, the DVC wire format, the bridge IPC.

### 6.2 Compatibility with apps

An app declares the **minimum** runtime it accepts:

```deck
@requires
  runtime: ">= 1.0"
```

If absent, the default is `>= 1.0` (the lowest version this spec was ever released for).

Major version bumps signal incompatible changes: a new opcode the app might rely on, a snapshot format that's no longer backward-readable, an evaluator quirk that affected behavior. Minor and patch bumps are always backward-compatible — an app that ran on `1.2` runs on `1.3`.

### 6.3 What triggers each bump

| Change | Bump |
|---|---|
| Bug fix that does not change observable behavior | patch |
| Performance improvement | patch |
| Documentation-only change | patch |
| New optional configuration option | minor |
| New observable behavior (e.g. previously-unspecified ordering becomes guaranteed) | minor |
| New telemetry counter, new optional `system.info` field | minor |
| Bump to a new SDI major version | major |
| Snapshot format breaking change | major |
| Edition support window shrinks (e.g. dropping support for an old edition) | major |
| Default behavior changes that observably affect apps | major |

### 6.4 Multiple runtimes per platform

A single platform release may, in principle, ship multiple runtime versions side-by-side (e.g. for development vs. production). In practice we expect one. The compatibility check is per-VM, so a future "compat shim" runtime that emulates an older version is allowed by this spec.

---

## 7. SDI Version

Defined in `12-deck-service-drivers §11`. Briefly:

- Format: 16-bit `(major << 8) | minor` (e.g. `0x0100` = 1.0).
- Major bumps when the C vtable changes incompatibly.
- Minor bumps for backward-compatible additions.
- Drivers carry their own `sdi_version`; the runtime refuses incompatible drivers at registration.

The SDI version concerns only Platform Implementers and OS Maintainers. App Developers never see it.

---

## 8. App Version

Strict semver per app, in the `@app version:` field. Owned entirely by the App Developer. The OS does not impose semver discipline — an app could call its 1.0 release "27.4.0-banana" and the OS would not care. The ecosystem (the registry, the docs site, the OTA tooling) is what enforces semver conventions.

The app version flows into:

- The bundle filename (`{app_id}-{version}.tar.zst`)
- The OTA manifest
- The signature block
- `system.info.app_version()` for the running app
- The migration chain (`@migration { from: vN }` declarations)

---

## 9. Compatibility Check at Load Time

Every app launch begins with a compatibility check. The check is **fail-fast and structured** — the OS knows exactly why an app cannot run and can present that reason to the user.

### 9.1 Check sequence

The loader performs these checks in order:

```
1. Edition check
   app declares: edition: E
   runtime supports: editions_supported = {E1, E2, …}
   if E ∉ editions_supported: fail with :incompatible_edition

2. Surface API level check
   app declares: requires.deck_os ranges
   runtime exposes: deck_os = N
   if N does not satisfy ranges: fail with :incompatible_surface

3. Runtime version check
   app declares: requires.runtime ranges
   runtime exposes: runtime = R
   if R does not satisfy ranges: fail with :incompatible_runtime

4. Capability presence check
   for each (cap, range) in requires.capabilities ∪ implicit_from_use:
     driver = registry.get(cap)
     if driver = absent and entry not optional: fail with :missing_capability
     if driver.version does not satisfy range: fail with :incompatible_capability

5. Permission resolution
   for each cap in requires.capabilities and @permissions:
     state = security.permission_state(app_id, cap)
     if state = denied and not optional: fail with :permission_denied

6. Type-check & loader stages 3..12 (per 11-deck-implementation §9.1)
```

Steps 1–4 are version-related. Steps 5–6 are documented elsewhere; included here for completeness.

### 9.2 Error model

Every load failure produces a `LoadError` with at least these fields:

```
LoadError {
  code:     atom               # machine-readable; see §9.3 for the catalog
  actor:    atom               # who can fix this; see §9.4
  message:  str                # one-sentence human-readable description
  fix:      str                # one-sentence instruction to the actor
  detail:   {str: any}         # structured context for the OS to render
  fix_url:  str?               # link to documentation
  severity: atom               # :fatal | :warning
}
```

The OS shell uses this structure to render error screens with the right actionable next step shown to the right person.

### 9.3 Error code catalog

```
:incompatible_edition       — App declares an edition this runtime does not support
:incompatible_surface       — App requires a surface API level this device cannot satisfy
:incompatible_runtime       — App requires a runtime version this device cannot satisfy
:missing_capability         — A required capability has no driver registered on this device
:incompatible_capability    — A capability is present but its version does not satisfy the app
:permission_denied          — A required permission was denied by the user / policy
:signature_invalid          — The bundle signature does not validate
:unknown_signer             — The bundle is signed by an unrecognized key
:bundle_corrupt             — The bundle hash does not match what the manifest declared
:migration_failed           — A required @migration step failed
:loader_error               — A type or topology check failed (load stages 3..12)
:no_memory                  — Insufficient heap to load the app
:internal                   — A defect in the runtime; should never happen
```

### 9.4 Actor catalog

```
:app_developer              — The author of the app must fix this
:user                       — The end user of the device must take action (grant permission, etc.)
:platform_dev               — The board / driver maintainer must fix this
:os_maintainer              — A bug in the language, runtime, or surface; file an issue
```

### 9.5 Example: incompatible edition

```
LoadError {
  code:    :incompatible_edition
  actor:   :app_developer
  message: "social.bsky.app v1.0.0 declares edition 2027, but this device's runtime supports only editions {2025, 2026}."
  fix:     "Bring the app to edition 2026 (run `deck migrate --to 2026`), or update the device firmware to a build that supports edition 2027."
  detail:  {
    app_id:                 "social.bsky.app",
    app_version:            "1.0.0",
    declared_edition:       2027,
    editions_supported:     [2025, 2026],
  }
  fix_url: "https://deck-lang.org/docs/errors/incompatible_edition"
  severity: :fatal
}
```

The shell can render this with both the technical detail (visible in a "Show details" expansion) and the human-readable fix prominent.

### 9.6 Example: missing capability

```
LoadError {
  code:    :missing_capability
  actor:   :platform_dev
  message: "social.bsky.app requires `network.http` but no driver for that capability is registered on this device."
  fix:     "If you are the platform implementer: register a `deck.driver.network.http` implementation. If you are the user: this app cannot run on this device until the firmware adds network support."
  detail:  {
    capability:             "network.http",
    optional:               false,
    drivers_registered:     ["network.wifi", "storage.fs", "storage.nvs", "system.info"],
  }
  severity: :fatal
}
```

### 9.7 Example: incompatible capability

```
LoadError {
  code:    :incompatible_capability
  actor:   :user
  message: "social.bsky.app needs `network.http >= 2` (HTTP/2 streaming). This device provides `network.http v1.4`."
  fix:     "Update the device firmware to a release that bundles `deck-driver-esp32-network` >= 2.0."
  detail:  {
    capability:                "network.http",
    required_version_range:    ">= 2",
    available_version:         "1.4.0",
    available_capability_flags: ["KEEP_ALIVE"],
    needed_capability_flags:    ["HTTP2", "STREAM_BODY"],
    upgrade_path:               "https://deck-lang.org/docs/upgrades/network-2.0"
  }
  severity: :fatal
}
```

### 9.8 Warnings, not errors

Some compatibility issues are warnings: the app can run, but the developer should know. Example:

```
LoadError {
  code:    :deprecated_capability
  actor:   :app_developer
  message: "social.bsky.app uses `display.notify`, which is deprecated since deck_os v3 and scheduled for removal in v5."
  fix:     "Migrate to the `notifications` capability (with `local_only: true` for transient toast-style messages)."
  severity: :warning
  detail:  {
    capability:        "display.notify",
    deprecated_since:  3,
    removal_target:    5,
    replacement:       "notifications.post_local",
  }
  fix_url: "https://deck-lang.org/docs/migrations/display-notify-to-notifications"
}
```

Warnings are surfaced to the developer (logs, dev console) but do not prevent launch.

---

## 10. Deprecation Policy

When a capability, method, atom, or behavior must change incompatibly, it goes through a three-stage deprecation pipeline. **Nothing is removed without going through all three stages.**

### 10.1 Stage 1: Deprecated

- The element is marked `@deprecated` in `.deck-os` (with `since:` and `replacement:` fields).
- A surface API level bump records the deprecation.
- Apps using it continue to work without warning during this stage's first release; from the second release, the loader emits a **warning** (severity `:warning`, code `:deprecated_capability`).
- Documentation moves the entry to a "Deprecated" section with a migration guide.
- The element MUST remain functionally identical to its non-deprecated behavior.

Minimum duration: **one full release of the surface** (at least one minor version of the runtime ships with the warning visible).

### 10.2 Stage 2: Discouraged

- The deprecation warning escalates: the loader still allows launch, but the OS shell may show a one-time notification to the user ("this app uses an outdated feature; please update").
- New apps using the deprecated element are rejected by `deck check` (the linting tool) — they cannot be published to a registry that uses official tooling.
- The element behavior MUST remain identical.

Minimum duration: **one additional surface API level**.

### 10.3 Stage 3: Removed

- The element is removed from `.deck-os` and the runtime no longer routes calls to it.
- A surface API level bump records the removal.
- Apps using it fail to load with `:missing_capability` or `:incompatible_capability` (depending on what was removed).

Total deprecation timeline: at minimum **two surface API levels** (~1 year at expected cadence).

### 10.4 Edition deprecations

Editions are never "deprecated" in the same sense — they remain readable forever in principle. What can happen:

- A specific runtime release may drop support for an old edition (with a major version bump on the runtime).
- The official runtime project commits to supporting at least two recent editions; community runtimes may go further or shorter.

An app on an old edition that the running runtime no longer supports fails with `:incompatible_edition`.

---

## 11. Driver and Component Versions

Every IDF Component (`14-deck-components`) and every concrete driver implementation has its own semver. The version flows through the SDI as:

```c
typedef struct {
  const char *driver_path;         /* "deck.driver.network.http" */
  uint16_t    sdi_version;          /* 0x0100 = SDI 1.0 */
  uint16_t    impl_version;         /* this driver's own semver, packed */
  const char *impl_name;            /* "esp_idf_v6.0_lwip", etc. */
  uint32_t    capability_flags;     /* per-driver feature bitmap */
} DeckServiceHeader;
```

`impl_version` is packed semver: `(major << 16) | (minor << 8) | patch`. So `1.4.0` packs as `0x010400`.

The runtime exposes `impl_version` per driver via `system.info.versions().drivers` so apps can probe and the OS shell can show "installed driver versions" in Settings → Diagnostics.

Apps never declare a specific driver implementation version — only the abstract capability version range:

```deck
@requires
  capabilities:
    network.http: ">= 2"        # capability version, NOT impl version
```

The capability version is a property of the **OS surface declaration**, not of the implementation. Two different driver implementations of `network.http v2` (one on ESP32, one on RP2350) have different `impl_version` values but both report capability version 2.

---

## 12. Runtime Probing — `system.info.versions()`

Every Deck runtime MUST implement `system.info.versions()` returning a complete picture of what's running:

```deck
@type Versions
  edition_current        : int                     # the active edition (e.g. 2026)
  editions_supported     : [int]
  deck_os                : int                     # surface API level
  runtime                : str                     # semver string
  runtime_build          : str                     # vendor build identifier (commit hash, etc.)
  sdi                    : (int, int)              # (major, minor)
  drivers                : [DriverVersionEntry]
  extensions             : [(name: str, level: int)]
  app_version            : str                     # the calling app's version
  app_id                 : str                     # the calling app's id

@type DriverVersionEntry
  capability             : str                     # e.g. "network.http"
  driver_path            : str                     # e.g. "deck.driver.network.http"
  capability_version     : int
  impl_name              : str                     # e.g. "esp_idf_v6.0_lwip"
  impl_version           : str                     # semver string
  capability_flags       : [atom]                  # human-readable flag names
  state                  : atom                    # :running | :degraded | :unavailable
```

Apps use this for:

- Telling the user what version they're on (about screen)
- Conditional feature use (`when http_driver.has_flag(:http2) → use_streaming`)
- Telemetry / crash reports

The OS shell uses it for:

- Settings → Device → Versions tab
- Crash reports include the full Versions snapshot
- Compatibility diagnostics ("Why won't app X run?")

---

## 13. Capability-Level Feature Flags

Within a single capability version, individual implementations may offer optional features advertised through `capability_flags`. These let apps detect optional features without requiring a surface bump for every variant.

### 13.1 In `.deck-os`

Each capability declares its possible flags:

```
@capability network.http
  @flag :keep_alive
  @flag :http2
  @flag :http3
  @flag :stream_body
  @flag :brotli

  request   (...)   -> ...
  ...
```

A specific driver implementation reports which flags it actually supports via `capability_flags` (per `12-deck-service-drivers §3`).

### 13.2 At runtime

```deck
@use network.http as http

let v = sysinfo.versions()
let http_driver = list.find(v.drivers, d -> d.capability == "network.http")
when http_driver.flags contains :http2
  -- use HTTP/2 specific code path
when not (http_driver.flags contains :http2)
  -- fall back
```

Or via a sugar method on the capability handle:

```deck
when http.has_flag(:http2)
  use_http2_path()
```

### 13.3 When to use a flag vs a version bump

- **Flag**: an optional feature that, if absent, the app can do without (the alternative is graceful degradation in the same code).
- **Version bump**: a feature that fundamentally changes the API contract or that the app cannot work without (the alternative is "this app can't run here").

Example: `:http2` is a flag (apps can fall back to HTTP/1.1). `:stream_body` is also a flag. But adding a new method `network.http.upload_with_progress` would be a version bump because old apps can't conditionally call a method that doesn't exist.

---

## 14. Tools

The CLI tool `deck` (shipped with `deck-tools`) provides version-aware commands.

### 14.1 `deck check <bundle>`

Static analysis. Reports:

- Edition declared and any constructs incompatible with it
- Capabilities used (computed from `@use`) and their inferred minimum versions
- Discrepancies with `@requires` (e.g., `@requires` says `>= 1` but the app uses a method that needs `>= 2`)
- Use of deprecated capabilities or methods
- Suggestions for `@requires` ranges

### 14.2 `deck info <bundle>`

Prints the full version envelope of a bundle:

```
$ deck info bsky-1.0.0.tar.zst

App:        social.bsky.app v1.0.0
Edition:    2026
Requires:
  deck_os:  >= 2
  runtime:  >= 1.0
  capabilities:
    network.http       >= 2
    storage.local      any
    notifications      >= 1
Bundle hash:    sha256:abcd1234...
Signature:      verified (cyberdeck/community-key-2026)
```

### 14.3 `deck info --device <port>`

Connects to a flashed device and dumps `system.info.versions()`:

```
$ deck info --device /dev/cu.usbmodem1101

Device:     CyberDeck v1.0
Edition:    2026 (supported: 2025, 2026)
deck_os:    2
runtime:    1.4.0 (build: a1b2c3d4)
SDI:        1.0
Drivers (32):
  network.http      v2.1.0    [keep_alive, http2, stream_body]   running
  network.wifi      v1.0.5    [—]                                running
  storage.fs        v1.2.0    [removable]                         running
  ...
Extensions:
  ext.cyberdeck.battery_curve   v1
```

### 14.4 `deck migrate <app_dir> --to <edition>`

Edition migration. Reports constructs that need to change and (where mechanical) rewrites them in-place. The tool refuses migrations that would alter app behavior; those are flagged for the developer to decide.

### 14.5 `deck deps <bundle>`

Resolves the dependency graph: which capabilities, which versions, which platforms can satisfy them. Used for "this app runs on the following devices: …" reports.

---

## 15. The Lock File

A bundle MAY include `deck.lock` recording the exact resolution used at build / test time:

```toml
edition = 2026
runtime = "1.3.2"
deck_os = 2

[[capability]]
path = "network.http"
version = 2
flags = ["keep_alive", "http2", "stream_body"]
impl_name = "esp_idf_v6.0_lwip"
impl_version = "2.1.0"

[[capability]]
path = "storage.local"
version = 1
flags = []
impl_name = "esp_idf_v6.0_fat"
impl_version = "1.2.0"

# ...
```

The file is informational. The runtime does NOT enforce that the lock matches at load time — that would be too strict, since the whole point of compatibility ranges is to allow drift. But:

- `deck info` shows lock vs current diffs.
- `deck check` warns if `@requires` ranges have drifted away from the lock.
- Reproducible builds use the lock to produce identical bundles across CI runs.

---

## 16. The Compatibility Matrix

A condensed view of who-must-do-what when each version bumps.

| Bumped element | App developer must… | Platform dev must… | OS Maintainer must… |
|---|---|---|---|
| **App version** | Choose new semver, follow project conventions | — | — |
| **Edition** | Decide whether to migrate (run `deck migrate`); existing apps keep running on old editions | — | Publish migration guide; runtime must still support previous edition for at least one release |
| **Surface API level** | Bump `@requires deck_os` if using new capabilities; otherwise nothing | Implement new capability drivers (or refuse them as optional) | Update `.deck-os`; document new capabilities; ship reference drivers |
| **Runtime version (minor/patch)** | Nothing | Nothing | Test against existing apps; publish release notes |
| **Runtime version (major)** | Bump `@requires runtime` if relying on changed behavior | Re-test platform drivers against new runtime | Document break; run conformance suite |
| **SDI version (minor)** | Nothing | Optionally adopt new optional methods | Update SDI headers; bump `sdi_version` |
| **SDI version (major)** | Nothing | Update all driver implementations | Coordinate cross-component release; bump every driver |
| **Driver impl version (patch/minor)** | Nothing | Publish new component release | — |
| **Driver impl version (major)** | Possibly: if capability version also bumped | Coordinate with consumers; publish breaking change notice | Update conformance suite |

---

## 17. Governance

### 17.1 Who can bump what

| Element | Who can bump |
|---|---|
| Edition | OS Maintainers (project core team), via the language RFC process |
| Surface API level | OS Maintainers, via the surface RFC process |
| Runtime version | OS Maintainers, on each runtime release |
| SDI version | OS Maintainers, via the SDI RFC process |
| Official driver impl version | OS Maintainers, on each driver release |
| Community driver impl version | Driver maintainer, on each release |
| App version | App author, on each app release |
| `ext.<vendor>.*` extension level | The extension's vendor |

### 17.2 RFC process for OS Maintainer-controlled bumps

For editions, surface API levels, and SDI majors:

1. An issue is opened in the relevant spec repo (`deck-lang` for edition / surface, `deck-service-drivers` for SDI).
2. The issue carries the proposed change, motivation, alternatives, and migration plan.
3. A two-week comment period (longer for editions).
4. If accepted, the spec PR lands; the new version takes effect at the next release.

For runtime semver and driver impl semver: routine releases follow the standard semver discipline; no RFC needed for non-breaking changes.

### 17.3 Release coordination

A coordinated release cycle ships the runtime, the surface, and the official drivers together every quarter:

- **Release-N month 1**: spec PRs land; new versions tagged but not yet built into firmware.
- **Release-N month 2**: official driver components release matching versions; `deck-platform-esp32s3` umbrella bumps.
- **Release-N month 3**: firmware OTA goes live; release notes published; tutorials updated.

Off-cycle patches (bug fixes, security) ship without coordination on a per-component basis.

---

## 18. Examples End-to-End

### 18.1 An app that runs on every shipping device

```deck
@app
  name:    "Hello"
  id:      "org.example.hello"
  version: "1.0.0"
  edition: 2026
  -- @requires omitted: defaults to whatever capabilities the app uses, current runtime, lowest deck_os

@on launch
  log.info("hello, world")
```

This app uses no capabilities. The loader infers `requires.capabilities = []`, `requires.deck_os = >= 1`, `requires.runtime = >= 1.0`. It runs on every device that supports edition 2026.

### 18.2 An app that needs specific capability features

```deck
@app
  name:    "VideoStream"
  id:      "org.example.video"
  version: "1.0.0"
  edition: 2026

@requires
  deck_os: ">= 3"
  runtime: ">= 1.5"
  capabilities:
    network.http: ">= 2"            # needs HTTP/2 + streaming
    display.video: ">= 1"           # a hypothetical future capability
    storage.fs:   ">= 1"

@use
  network.http   as http
  display.video  as video
  storage.fs     as fs
```

If installed on a device whose `network.http` driver is at version 1, the load fails with `:incompatible_capability` and the user sees:

> **VideoStream** requires HTTP/2 streaming, but this device's network driver is older. Update firmware (or install a newer build) to run this app.

### 18.3 An app with optional features

```deck
@app
  name:    "Sketch"
  id:      "org.example.sketch"
  version: "1.0.0"
  edition: 2026

@requires
  deck_os: ">= 2"
  capabilities:
    sensors.accelerometer: optional   # nice to have; degrades gracefully

@use
  sensors.accelerometer as accel  optional

@on launch
  match accel.is_available()
    | true  -> use_motion_input()
    | false -> use_touch_only()
```

Loads on devices without an accelerometer; the `sensors.accelerometer` capability returns `:err :unavailable` for any call.

### 18.4 An app declaring an extension

```deck
@app
  name:    "BatteryHealth"
  id:      "com.cyberdeck.battery_health"
  version: "1.0.0"
  edition: 2026

@requires
  deck_os: ">= 1"
  capabilities:
    ext.cyberdeck.battery_curve: ">= 1"

@use
  ext.cyberdeck.battery_curve as battery_curve
```

Uses a CyberDeck-specific extension. Will not load on platforms that do not register this extension.

---

## 19. Frequently Asked Questions

**Q: Why isn't `deck_os_version` semver?**
A: It only ever grows. Semver implies room for "patch surface changes," which is a contradiction in terms — a surface change is observable to apps. An integer is honest about that.

**Q: Why not put `min_deck_runtime` in `manifest.json` like before?**
A: We removed `manifest.json` entirely (the @app block IS the manifest). The new `@requires` annotation lives in `app.deck` alongside `@app`, keeping all version contracts in one source-controlled file.

**Q: What if my app needs runtime feature X added in 1.4 but X is not visible to apps?**
A: Then it doesn't matter — bump `@requires runtime: ">= 1.4"` only if some observable behavior depends on it. If X is purely internal, the app cannot tell and need not declare it.

**Q: Can a community driver use a capability path that conflicts with an official one?**
A: No. Official capability paths are reserved (the `ext.<vendor>.*` namespace exists for community/vendor capabilities). The conformance suite checks for path collisions.

**Q: What happens if two drivers register the same `deck.driver.X` path?**
A: The first one registered wins, unless one declares `DECK_DRIVER_FLAG_FALLBACK` (`12-deck-service-drivers §10.2`) — in which case the non-fallback wins. This lets a hardware-accelerated driver coexist with a software fallback.

**Q: Do I need to bump `app version` for a documentation fix?**
A: That's an App Developer's discretion. Most app developers follow strict semver (yes, patch bump). The OS does not care.

**Q: Can a runtime support more editions than the official one?**
A: Yes. Community runtimes can support any range of editions they want. They report this via `system.info.versions().editions_supported`. Apps can target whatever the consuming runtime supports.

**Q: What if the spec itself has a typo that affects compatibility?**
A: Errata are tracked per-version on the docs site. Spec text is normative; if behavior diverges, the implementation wins (and a spec patch follows).

**Q: How do I downgrade?**
A: You don't, automatically — anti-rollback (`13-deck-cyberdeck-platform §12.5`) prevents firmware downgrades. Apps can be downgraded via OTA app installer with `--allow-downgrade` (off by default) for development purposes.

---

## 20. Out of Scope for v1

- An on-device "compatibility advisor" that recommends app upgrades based on installed firmware.
- An automated semver-bump bot for official components.
- A formal language reference manual generator that includes per-edition syntax diffs.
- Multi-tenant runtime co-existence (one runtime serving multiple SoCs).
- Cross-edition interoperability of snapshots (a snapshot taken under edition 2026 may not restore on a 2027-only runtime).

These are roadmap items, not v1 commitments. The goal of v1 is a working five-version model that gives every actor exactly what they need.
