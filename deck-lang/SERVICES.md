# Deck — Services Catalog

Foundation document for OS-mediated functionality. Companion to `LANG.md` (language), `BUILTINS.md` (in-VM modules), `CAPABILITIES.md` (consumer protocol), `BRIDGE.md` (UI bridge).

**Edition:** 2027.

A **service** is the atomic unit of OS-mediated functionality in Deck. Every cross-boundary operation an app performs — reading a file, fetching a URL, observing battery state, persisting config, displaying a notification, calling another app — goes through a service.

This document defines:

- **Part I** — the meta-spec: what a service IS, the ten dimensions every service declares, the three implementation kinds.
- **Part II** — the v1 catalog: every service the reference platform provides, organised by abstraction tier with explicit guidance to prefer the highest tier.
- **Part III** — language-service integration: how `@config`, `@assets`, `@machine`, `@on`, `@service`, `@handles`, `log.*`, `time.*` are themselves implemented over services. The runtime is a service consumer too.
- **Part IV** — authoring app services: how a Deck app declares and exposes its own services for other apps.
- **Part V** — the extension contract: how a platform adds native services or replaces existing ones.

The consumer-side protocol — how apps `@use`, configure, call, and handle errors — lives in the companion document `CAPABILITIES.md`. The two are complementary: SERVICES defines what exists; CAPABILITIES defines how an app talks to what exists.

---

# Part I — Meta-spec: the shape of a service

## 0 · Philosophy

The Unix philosophy applied to embedded OS-level functionality, with three claims specific to services:

1. **Services are the OS atoms.** Every observable boundary an app crosses is a service call. No "magic" capabilities, no implicit OS state, no hidden side effects in the runtime that aren't somebody's service.
2. **Implementation is invisible to consumers.** Whether a service is implemented in C/Rust by the platform, in Deck by another app, or by the runtime itself on the consuming app's behalf, the consumer code is identical. `@use service "X" as x; x.method(args)` works the same regardless.
3. **One provider per service ID.** Service IDs are namespaced under the providing entity (`system.*` for platform, `<app-id>/<name>` for apps). Two providers cannot claim the same ID; the second is rejected at install with `LoadError :resource`. Cross-app extensibility for generic concepts (multiple browsers, multiple email apps) is handled by `@handles` URL pattern matching, not by service-ID competition.

A consequence of (1) and (2): the catalog enumerated in Part II is the complete OS surface. Anything the platform provides outside this catalog is either a builtin (in-VM, no IPC) or a kernel-level driver feeding an existing service (not directly app-callable).

## 1 · What IS a service

A service is a **singleton process per service ID** that exposes a typed method interface and may emit streams. Concretely:

- **Singleton**: at any time, at most one instance of a service is running on the device. Multiple consumers calling concurrently share that instance; the runtime serialises their calls into the service's cooperative scheduler.
- **Process**: the service has its own scheduler queue, error domain, and configuration. Calls cannot interleave within a single method body; they queue and dequeue in order.
- **Per service ID**: identity is the dotted path. Two different service IDs (`storage.fs` and `storage.cache`) are distinct services even if their implementation shares code.

A service is fully described by ten dimensions; nothing else belongs to its definition.

## 2 · The ten dimensions of a service

| # | Dimension | Description |
|---|---|---|
| 1 | **Identity** | Service ID (dotted path). For system services: `<group>.<name>` (`storage.fs`, `system.apps`). For app-provided: `<app-id>/<service-name>` (`social.bsky.app/feed`). |
| 2 | **Tier** | Abstraction layer (1: primitives, 2: network primitives, 3: system OS, 4: high-level domain, 5: sensors). See §12. |
| 3 | **Implementation kind** | `:native` (C/Rust in platform) / `:deck-app` (in another app's VM) / `:language-integrated` (runtime-mediated; see Part III). |
| 4 | **State** | The `@type <Cap>State` record describing observable state, or `unit` if stateless. |
| 5 | **Methods** | Each declared as `on :name (params) -> ReturnType ! = body` (provider side) or by signature alone (consumer side). |
| 6 | **Error domain** | `@errors <name>` enumerating the consumer-visible failure atoms, including the universals (§6 below). |
| 7 | **Configuration schema** | Alias-level config keys + types, evaluated once at bind. |
| 8 | **Permission model** | `allow:` (provider-side hard policy) + `@grants.services.<alias>` schema (consumer-side rationale). |
| 9 | **Versioning** | Single integer `version:`. Multiple major versions can coexist for migration. |
| 10 | **Lifecycle** | `keep:` modifier + per-method `max_run_ms:`. Cold/spawning/resident/idle-eviction/quarantined per impl kind (§9). |

Optional eleventh dimension: **Events**. Services that emit cross-cutting OS-level signals (not tied to any single capability's state) declare them as `os.*` events. Most services don't need this — per-service state changes go through watch streams (kind: watch-stream).

## 3 · Service ID schema

Service IDs follow strict namespace rules:

| Prefix pattern | Meaning | Example |
|---|---|---|
| `<group>.<name>` (no slash) | System service provided by the platform | `storage.fs`, `network.http`, `system.apps`, `media.image` |
| `<app-id>/<service-name>` (with slash) | App-provided service | `social.bsky.app/feed`, `media.notes.dn/inbox` |
| `system.*/<service-name>` | System app-provided service (special: provider's `@app.id` starts with `system.`) | `system.share/target` |

**System service groups** (all reserved by the platform; apps cannot register IDs in these groups):

- `storage.*` — persistence primitives
- `network.*` — network primitives
- `system.*` — OS services (apps, security, scheduler, events, etc.)
- `media.*` — media services (image, audio)
- `api.*` — API client abstractions
- `auth.*` — authentication helpers
- `data.*` — structured data utilities
- `sensors.*` — physical-world sensors

**App service IDs:**
- `<app-id>` matches the providing app's `@app.id` exactly (lowercase reverse-DNS-style).
- `<service-name>` matches `[a-z][a-z0-9_-]*` (no slashes, no dots, no uppercase).
- Each app may declare any number of services under its own ID.

**Conflict resolution**: an app whose `@app.serves` claims an ID already registered by another installed app is rejected with `LoadError :resource`. System apps (whose ID starts with `system.`) take precedence over third-party apps for IDs starting with `system.*` — a third-party app cannot register `system.share/target` if a system app already does.

## 4 · Implementation kinds

A service declares which implementation kind it uses; this affects lifecycle and discovery, but not consumer code:

### 4.1 `:native` — implemented in C/Rust by the platform

- The implementation is part of the platform binary (firmware).
- The "VM" is not a Deck VM — it's runtime-internal code with the same single-thread cooperative model.
- No cold-spawn cost; the service is "always resident" from the consumer's perspective.
- `@on launch` for the service runs once at platform boot.
- Error: native code panics surface as `:err :service_unavailable` (severe internal bug, logged at `:error`); restart on next boot.
- Examples: `storage.fs`, `network.http`, `system.platform`.

### 4.2 `:deck-app` — implemented by another Deck app

- The implementation is the providing app's `@service "id" = …` block.
- The provider has its own VM; consumers' calls IPC into it.
- Cold-spawn on first call; idle-evicted unless `keep: true`.
- Provider's panics quarantine after 3 in 5 min (§9.5).
- Examples: `social.bsky.app/feed`, `system.share/target` (provided by the system share app, which is itself a Deck app).

### 4.3 `:language-integrated` — implemented by the runtime on app's behalf

- Special category: the runtime mediates the call without a separate VM.
- Used for services that back language features (`@config` ↔ `storage.nvs` with schema validation; `log.*` ↔ `system.logs` with trace stamping).
- The consumer's app code may *also* `@use` the underlying service directly for non-language-feature use (`@config` declares schema'd keys; `storage.nvs` directly handles dynamic keys).
- Examples: the runtime's behaviour when persisting `@config`, when emitting `log.info`, when serialising `@machine` state at suspend.

The implementation kind is metadata exposed by `system.apps.service(id) -> ServiceInfo`. Apps don't typically check; the runtime cares for lifecycle accounting.

## 5 · Method kinds

Every service method falls into exactly one of five kinds. Naming conventions let the consumer predict behaviour from the method name.

| Kind | Return type | Naming | Lifecycle |
|---|---|---|---|
| **Query** | `Result T E` (or `T?` when absence is routine) | Noun phrase: `fs.exists`, `apps.info` | Synchronous request-response |
| **Mutation** | `Result unit E` or `Result T' E` | Verb or `set_<noun>`: `fs.write`, `theme.set` | Synchronous; observable state change |
| **Action stream** | `Stream <EventT>` (event variant with `:done` / `:failed`) | Verb, no `watch` suffix: `fs.copy`, `wifi.scan`, `ota.download` | Long-running; cancelled by subscription drop |
| **Watch stream** | `Stream T` (infinite, reactive) | `<state>_watch` suffix or bare `watch`: `theme.watch`, `wifi.status_watch` | Infinite; first emission immediate; subsequent only on structural change |
| **Handle-producing** | `Result Handle E` (paired with handle-taking lifecycle methods) | Verb: `audio.play`, `fs.open_write` | Long-lived resource; explicit close required |

**Discipline rules:**
- A method that changes observable state is **not** a query, regardless of return type.
- A query never causes a side effect a subsequent query would observe.
- Action streams emit exactly one terminator (`:done` / `:failed`); after the terminator, no further emissions.
- Watch streams never terminate on their own; cancellation is only by subscription drop.
- Handles are values; they may be stored in `@type` fields and passed across fns. They do **not** persist across suspend (per §22.4 of `DRAFT.md`).
- Every Handle-producing method has a paired close method (`fs.close_write`, `audio.stop`).

**All service methods are impure (`!`).** Crossing the service boundary is observable by definition. Pure service methods would be a contradiction; the loader rejects them.

## 6 · Error model

Every `@errors <service>` domain MUST include the four universals:

```
@errors <service>
  :unavailable           "Service not available (optional in @needs and platform doesn't provide)"
  :permission_denied     "Caller not in `allow:`, OR @grants denied"
  :service_unavailable   "Provider VM quarantined after repeated panics, OR native impl is in error state"
  :timeout               "Call exceeded max_run_ms"
  ...                    -- service-specific atoms below
```

These cover the four IPC-level failure modes orthogonal to the service's logical errors. Beyond them, each service lists only atoms for distinguishable failure modes the caller might handle differently.

**Discipline rules:**
- No payloads on error atoms (lift structured context into the success type or log it).
- No catch-all `:error`; failures with no meaningful subtype become `panic :internal` instead of `Result :err`.
- A service NEVER exposes its implementation's error domain. If `social.bsky.app/feed.fetch` is implemented with `network.http`, the service does not return `Result Feed http.Error` — it returns `Result Feed feed.Error` and translates via `result.map_err`. This decouples consumers from provider internals.

## 7 · Permission model

Two layers; both must permit a call.

### 7.1 Provider-side `allow:`

Declared in the service's `@service` block (or in the platform's `.deck-os` manifest for native services):

```
@service "social.bsky.app/feed"
  allow: :any
  ...
```

Values:
- `:any` — any installed app may call.
- `:system` — only apps whose `@app.id` starts with `system.`.
- `[app_id, …]` — explicit whitelist.

This is hard policy: failed `allow:` checks return `:err :permission_denied` immediately, with no user prompt. The service author chose this restriction.

### 7.2 Consumer-side `@grants.services.<alias>`

Declared in the consumer's `@grants` block:

```
@grants
  services.bsky:
    reason: "Sync your Bluesky feed."
    prompt: :on_first_use
```

Standard fields:
- `reason: str` — user-facing rationale (required when `prompt ≠ :never`).
- `prompt: atom` — `:at_install` / `:on_first_use` / `:never`. Default per service.
- `persist: bool?` — whether the grant survives uninstall/reinstall.
- Service-specific params (e.g. `paths:` for `storage.fs`, `allowed_hosts:` for `network.http`).

Failure to declare `@grants.services.<alias>` for a non-`:never`-prompt service is `LoadError :permission` at app load.

### 7.3 Resolution order

1. Loader: `@needs.services` lists this service ID → check provider registered + version compatible.
2. Loader: `@grants.services.<alias>` declared with required fields.
3. Bind: provider's `allow:` admits caller's `@app.id`.
4. Runtime: per-call grant check (cached after first prompt).

Failure at any layer returns `:err :permission_denied`. Consumers cannot distinguish *which* layer denied (privacy-preserving).

## 8 · Versioning

Every service declares a single integer `version:` (default `1`).

```
@service "social.bsky.app/feed"
  version: 2
  ...
```

Consumers declare the required range in `@needs.services`:

```
@needs
  services:
    "social.bsky.app/feed": ">= 2"
    "storage.fs":            ">= 1"
```

**What requires a bump:**
- Removing or renaming a method.
- Changing a method signature (params, return type, error domain).
- Adding an error atom (consumers' exhaustive matches break — they must opt in).
- Semantic change to existing method behaviour.
- Schema change to a returned `@type`.

**What does NOT require a bump (additive changes):**
- Adding a new method.
- Adding a new field to the `Config` schema with a default.

**Coexisting versions:** a provider may declare separate `@service` blocks for v1 and v2 of the same ID; the dispatcher routes by the consumer's `@needs.services` range. Useful for migration windows.

## 9 · Lifecycle

The lifecycle phases differ by implementation kind.

### 9.1 Native services

| Phase | When |
|---|---|
| **Resident** (always) | From platform boot until shutdown |
| **Failing** | Internal error state; calls return `:err :service_unavailable`; recovers on next boot |

Native services have no cold-spawn cost. `@on launch` runs once at boot.

### 9.2 Deck-app services

| Phase | Trigger | Effect |
|---|---|---|
| **Cold** | App installed, no recent calls | VM not running; consumes no resources |
| **Spawning** | First IPC call after cold | Runtime spawns VM, runs `@on launch` (with `event.context: :service`), then dispatches the queued call |
| **Resident** | Recent call activity | VM in memory; subsequent calls dispatch immediately |
| **Idle eviction candidate** | No calls for 5 min AND `keep: false` | Runtime may evict; runs `@on terminate` first; persists machine state per §22.4 of `DRAFT.md` |
| **Quarantined** | Provider panicked 3 times within 5 min | VM disabled; further calls return `:err :service_unavailable` until manual relaunch from `system.apps` |

`keep: true` keeps the VM resident indefinitely (subject to OS memory pressure).

### 9.3 Language-integrated services

These are runtime-internal; lifecycle is platform-internal. Apps only see method behaviour.

### 9.4 Foreground vs service VM (Deck-app services only)

Same VM. If the provider app is foreground, service calls dispatch into the same scheduler queue as foreground events; they cooperatively interleave. If foreground is not running, the VM is service-only. Lifecycle hooks see the difference via `event.context: :foreground | :service` on `@on launch`.

### 9.5 Quarantine policy

After 3 panics in 5 min, the runtime quarantines the provider:
- VM stopped.
- Future calls return `:err :service_unavailable`.
- A `:warn` log entry on each call attempt.
- Recovery: manual relaunch via `system.apps.launch(id)`.

This protects consumers from panic-loop services that would otherwise drain battery on cold-spawn retries.

## 10 · Threading and isolation

### 10.1 One thread per service

Each service has one logical scheduler thread. Multiple consumers' calls queue:

- Calls arrive in the service's event queue (default 32; bounded per §14.10 of `DRAFT.md`).
- The scheduler dequeues one at a time, runs to completion, returns the result, then dequeues the next.
- Stream-returning methods occupy the scheduler only when emitting; between emissions the scheduler is free.
- Queue overflow → oldest call dropped; consumer sees `:err :timeout`.

### 10.2 No parallelism within a service

A service author cannot speed up by doing concurrent work across calls. For genuinely parallel work, the service delegates to a lower-tier service that itself parallelises (e.g. `network.http` runs requests in a platform thread pool).

### 10.3 Isolation guarantees

- A service's `@machine` state and `@config` are accessible only inside its own VM. Consumers cannot read or mutate the provider's state directly.
- IPC arguments / return values are deep-copied across the VM boundary (apps perceive value semantics; no shared references).
- A panic in the provider does not propagate to consumers; consumers see `:err :service_unavailable` after quarantine policy applies.

## 11 · Discovery

### 11.1 Static (load-time)

Apps declare dependencies in `@needs.services`. The loader checks:
- Each ID is registered (in the platform's native catalog OR in an installed app's `@app.serves`).
- Each version range is satisfied.
- The consumer's app is in the provider's `allow:`.

Failures are `LoadError :unresolved` or `LoadError :incompatible`.

### 11.2 Dynamic (runtime)

Some apps need to enumerate available services at runtime (Settings, Launcher, IDE, marketplace). The `system.apps` service provides:

```
apps.service (service_id: str) -> ServiceInfo? !
apps.services_provided_by (app_id: str) -> [str] !
apps.services_consumed_by (app_id: str) -> [str] !
```

with `ServiceInfo` carrying provider, version, allow, keep, resident, quarantined.

Static discovery is preferred — runtime discovery exists for tooling, not for production app code.

## 12 · Layered abstraction principle (PREFER HIGH-LEVEL)

Services are organised into **five tiers** by abstraction level. App authors should reach for the **highest tier that fits the task**; lower tiers are escape hatches when no higher tier covers the need.

| Tier | Purpose | Examples | When to reach for it |
|---|---|---|---|
| **1 — Storage primitives** | Files, KV, cache | `storage.fs`, `storage.nvs`, `storage.cache` | Direct file access, dynamic-key persistence, raw caching |
| **2 — Network primitives** | Transport-layer protocols | `network.http`, `network.ws`, `network.wifi`, `network.bluetooth` | Custom protocols, low-level link control |
| **3 — System OS services** | Device + OS state and lifecycle | `system.platform`, `system.apps`, `system.power`, `system.security`, `system.theme`, `system.logs`, `system.scheduler`, `system.events`, `system.intents`, `system.url`, `system.notify`, `system.ota`, `system.time`, `system.locale`, `system.tasks` | Reading device state, managing apps, posting notifications |
| **4 — High-level domain** | Composed abstractions over lower tiers | `api.client`, `media.image`, `media.audio`, `auth.oauth`, `data.cache`, `share.target` | API clients, media handling, auth flows. **Default choice for app authors.** |
| **5 — Sensors** | Physical-world readings | `sensors.imu`, `sensors.environment`, `sensors.light`, etc. | Hardware-dependent; per-platform |

**Rule of thumb:**
- Need to fetch JSON from a REST API? Use `api.client`, not `network.http`.
- Need to display an image? Use `media.image`, not raw `network.http` + `storage.cache`.
- Need to play audio? Use `media.audio`, not `system.audio`.
- Need OAuth? Use `auth.oauth`, not raw `network.http` + `storage.nvs`.

The lower-tier services are still available for cases the higher tier doesn't cover (custom HTTP verbs, exotic auth, raw byte caches). But starting at Tier 4 reduces app complexity dramatically — tier 4 services handle retry, caching, error translation, and protocol details that tier 2 leaves to the consumer.

---

# Part II — System Services Catalog

## 13 · Catalog summary

| # | Service ID | Tier | Impl | DL | Purpose |
|---|---|---|---|---|---|
| 1 | `storage.fs` | 1 | native | 1 | File I/O |
| 2 | `storage.nvs` | 1 | native | 1 | Typed persistent KV (also backs `@config`) |
| 3 | `storage.cache` | 1 | native | 2 | Volatile TTL KV |
| 4 | `network.http` | 2 | native | 2 | HTTP/HTTPS client |
| 5 | `network.ws` | 2 | native | 3 | WebSocket client |
| 6 | `network.wifi` | 2 | native | 2 | Wi-Fi link management |
| 7 | `network.bluetooth` | 2 | native | 3 | BLE GATT |
| 8 | `system.platform` | 3 | native | 1 | Device / runtime metadata |
| 9 | `system.apps` | 3 | native | 2 | App registry + lifecycle + crashes + discovery |
| 10 | `system.power` | 3 | native | 2 | Battery + charging |
| 11 | `system.display` | 3 | native | 2 | Brightness, rotation, lock, sleep |
| 12 | `system.audio` | 3 | native | 3 | Low-level audio I/O + volume |
| 13 | `system.security` | 3 | native | 2 | PIN + permissions storage |
| 14 | `system.time` | 3 | native | 2 | Wall-clock setting (reading is builtin) |
| 15 | `system.locale` | 3 | native | 3 | Language / region |
| 16 | `system.ota` | 3 | native | 2 | Firmware update |
| 17 | `system.url` | 3 | native | 1 | Deep-link dispatch |
| 18 | `system.notify` | 3 | native | 1 | Notifications + toasts |
| 19 | `system.theme` | 3 | native | 2 | OS theme |
| 20 | `system.logs` | 3 | native | 1 | Log sink (also backs `log.*` builtin) |
| 21 | `system.scheduler` | 3 | native | 1 | Timer dispatch (also backs `@on every` / `@on after`) |
| 22 | `system.events` | 3 | native | 1 | OS event bus (also backs `@on os.*`) |
| 23 | `system.intents` | 3 | native | 1 | Intent / `@handles` dispatch |
| 24 | `system.services` | 3 | native | 2 | Service registry (also backs `@service` IPC) |
| 25 | `system.tasks` | 3 | native | 3 | Per-process metrics |
| 26 | `api.client` | 4 | native | 2 | REST client over `network.http` |
| 27 | `media.image` | 4 | native | 2 | Image load + cache + transform |
| 28 | `media.audio` | 4 | native | 3 | High-level audio playback |
| 29 | `auth.oauth` | 4 | native | 2 | OAuth 2.0 helper |
| 30 | `data.cache` | 4 | native | 2 | Schema'd response cache |
| 31 | `share.target` | 4 | deck-app | 2 | Share-to dispatcher (provided by `system.share` app) |
| 32+ | `sensors.<name>` | 5 | native | 3 | Per-platform sensors |

**Total: 31 system services + N sensors.** DL1 minimum (must-have): 11. DL2 reference (CyberDeck target): 25. DL3 extensions: 6 + sensors.

---

## TIER 1 — Storage primitives

### 14 · `storage.fs`

File-level persistent storage. Path semantics are platform-determined (CyberDeck: `/sd/...` for SD card, `/app/...` for internal). Apps write to their sandbox root (`/app/{app_id}/...`) by default; `@grants.storage.fs.paths: [...]` opens additional paths.

**Tier:** 1 · **Impl:** native · **DL:** 1

**State:** stateless.

**Methods:**

```
-- Queries
fs.exists   (path: str) -> bool !
fs.size     (path: str) -> Result Size fs.Error !
fs.modified (path: str) -> Result Timestamp fs.Error !
fs.list     (path: str) -> Result [FsEntry] fs.Error !

-- Mutations
fs.read        (path: str)                 -> Result str fs.Error !
fs.read_bytes  (path: str)                 -> Result Bytes fs.Error !
fs.write       (path: str, content: str)   -> Result unit fs.Error !
fs.write_bytes (path: str, content: Bytes) -> Result unit fs.Error !
fs.append      (path: str, content: str)   -> Result unit fs.Error !
fs.delete      (path: str)                 -> Result unit fs.Error !
fs.rename      (from: str, to: str)        -> Result unit fs.Error !
fs.mkdir       (path: str)                 -> Result unit fs.Error !
fs.rmdir       (path: str)                 -> Result unit fs.Error !

-- Action stream (cancellable; for files large enough to warrant progress)
fs.copy (from: str, to: str) -> Stream CopyProgress !

-- Handle (for chunked writes that don't fit in memory)
fs.open_write  (path: str)                  -> Result FsHandle fs.Error !
fs.write_chunk (h: FsHandle, data: Bytes)   -> Result unit fs.Error !
fs.close_write (h: FsHandle)                -> Result unit fs.Error !

@type FsEntry
  name     : str
  is_dir   : bool
  size     : Size
  modified : Timestamp

@type CopyProgress =
  | :progress (bytes_done: Size, bytes_total: Size)
  | :done
  | :failed   (reason: fs.Error)

@type FsHandle = str

@errors fs
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :not_found
  :exists             "mkdir / rename target already exists"
  :not_empty          "rmdir on non-empty directory"
  :read_only
  :disk_full
  :invalid_path
  :invalid_handle
  :io
```

**Config:** none.
**Grants:** `:at_install`. `paths: [str]?` declares writable paths outside sandbox.
**Denied-grant:** gracefully degrading (returns `:err :permission_denied`).

---

### 15 · `storage.nvs`

Persistent typed key-value store. Small (bytes-scale) values, wear-levelled flash. Use directly when `@config` doesn't fit (dynamic keys, app-internal data not declared as schema'd config).

`@config` is built on top of this service: every `@config field: T` declaration is stored as `app.{id}.config.{field_name}` in NVS, with schema validation done by the runtime (Part III §44).

**Tier:** 1 · **Impl:** native · **DL:** 1

**State:** stateless (KV).

**Methods:**

```
nvs.get_str    (key: str)                 -> Result str   nvs.Error !
nvs.get_int    (key: str)                 -> Result int   nvs.Error !
nvs.get_float  (key: str)                 -> Result float nvs.Error !
nvs.get_bool   (key: str)                 -> Result bool  nvs.Error !
nvs.get_bytes  (key: str)                 -> Result Bytes nvs.Error !
nvs.set_str    (key: str, v: str)         -> Result unit  nvs.Error !
nvs.set_int    (key: str, v: int)         -> Result unit  nvs.Error !
nvs.set_float  (key: str, v: float)       -> Result unit  nvs.Error !
nvs.set_bool   (key: str, v: bool)        -> Result unit  nvs.Error !
nvs.set_bytes  (key: str, v: Bytes)       -> Result unit  nvs.Error !
nvs.delete     (key: str)                 -> Result unit  nvs.Error !
nvs.exists     (key: str)                 -> bool         !
nvs.keys       ()                         -> Result [str] nvs.Error !
nvs.clear      ()                         -> Result unit  nvs.Error !

@errors nvs
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :not_found
  :type_mismatch
  :invalid_key       "Empty, too long (>15 chars), or invalid characters"
  :storage_full
  :io
```

**Config:** none.
**Grants:** `:at_install`. Per-app namespace `app.{id}` — apps cannot read or write other apps' keys.

**Notes:**
- Key length limit is platform-dependent (CyberDeck: 15 chars). Longer → `:err :invalid_key`.
- `nvs.clear` affects only the calling app's namespace.
- Atomicity: each `set_*` is all-or-nothing; concurrent set + crash never produces partial value.

---

### 16 · `storage.cache`

Volatile TTL-backed key-value. Backed by RAM; survives only within the current session.

**Tier:** 1 · **Impl:** native · **DL:** 2

**State:** stateless.

**Methods:**

```
cache.get        (key: str)                          -> str?   !
cache.get_bytes  (key: str)                          -> Bytes? !
cache.set        (key: str, v: str,   ttl: Duration) -> Result unit cache.Error !
cache.set_bytes  (key: str, v: Bytes, ttl: Duration) -> Result unit cache.Error !
cache.delete     (key: str)                          -> unit   !
cache.clear      ()                                  -> unit   !
cache.ttl        (key: str)                          -> Duration? !

@errors cache
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :invalid_key
  :value_too_big
  :cache_full
```

**Config:** `default_ttl: Duration?` (currently unused; reserved).
**Grants:** `:never`.

**Notes:**
- `cache.get` returns `T?` because missing keys are routine.
- Expired keys evict lazily.
- Cross-app isolation: each app has its own namespace.

---

## TIER 2 — Network primitives

### 17 · `network.http`

Raw HTTP / HTTPS client. **For most app needs, prefer `api.client` (Tier 4)** which wraps this with auth, retry, JSON parsing, and base URL config.

**Tier:** 2 · **Impl:** native · **DL:** 2

**State:** stateless per-call.

**Methods:**

```
http.get    (path: str, opts: ReqOpts?)             -> Result Response http.Error !
http.post   (path: str, body: Body, opts: ReqOpts?) -> Result Response http.Error !
http.put    (path: str, body: Body, opts: ReqOpts?) -> Result Response http.Error !
http.patch  (path: str, body: Body, opts: ReqOpts?) -> Result Response http.Error !
http.delete (path: str, opts: ReqOpts?)             -> Result Response http.Error !

http.stream (method: atom, path: str, body: Body?, opts: ReqOpts?) -> Stream HttpChunk !

@type ReqOpts
  query   : {str: str}?
  headers : {str: str}?
  timeout : Duration?
  retry   : int?
  ca      : AssetRef?
  accept  : str?

@type Body =
  | :str       str
  | :bytes     Bytes
  | :json      any
  | :form      {str: str}
  | :multipart [MultipartPart]

@type MultipartPart
  name     : str
  filename : str?
  content  : Body
  mime     : str?

@type Response
  status     : int
  headers    : {str: str}
  body       : str?
  body_bytes : Bytes?

@type HttpChunk =
  | :chunk  Bytes
  | :done   (status: int, headers: {str: str})
  | :failed http.Error

@errors http
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :dns
  :refused
  :tls
  :status_3xx
  :status_4xx
  :status_5xx
  :body_too_large
  :malformed
  :offline
  :aborted
```

**Config:**

```
base_url   : str?     = :none
timeout    : Duration = 30s
retry      : int      = 0
user_agent : str      = "deck-runtime/1.0"
```

**Grants:** `:at_install` with `allowed_hosts: [str]`. Denied-grant: capability unavailable (every call → `:err :permission_denied`).

---

### 18 · `network.ws`

WebSocket client. Used for live update streams.

**Tier:** 2 · **Impl:** native · **DL:** 3

**State:** per-handle connection.

**Methods:**

```
ws.connect    (url: str, opts: WsOpts?) -> Result WsHandle ws.Error !
ws.send_text  (h: WsHandle, msg: str)   -> Result unit ws.Error !
ws.send_bytes (h: WsHandle, msg: Bytes) -> Result unit ws.Error !
ws.close      (h: WsHandle)             -> Result unit ws.Error !
ws.messages   (h: WsHandle)             -> Stream WsMessage !
ws.state      (h: WsHandle)             -> WsState !

@type WsHandle = str

@type WsOpts
  headers   : {str: str}?
  protocols : [str]?
  ping_every: Duration?

@type WsMessage =
  | :text   str
  | :bytes  Bytes
  | :closed (code: int, reason: str)

@type WsState =
  | :connecting
  | :open
  | :closing
  | :closed (code: int, reason: str)

@errors ws
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :dns
  :refused
  :tls
  :upgrade_failed
  :invalid_handle
  :closed
  :send_too_large
```

**Grants:** `:at_install` with `allowed_hosts: [str]`.

---

### 19 · `network.wifi`

Wi-Fi link management.

**Tier:** 2 · **Impl:** native · **DL:** 2

**State:** `WifiStatus`.

**Methods:**

```
wifi.status        () -> WifiStatus !
wifi.status_watch  () -> Stream WifiStatus !
wifi.scan          () -> Stream ScanEvent !
wifi.connect       (ssid: str, password: str?) -> Result unit wifi.Error !
wifi.disconnect    () -> Result unit wifi.Error !
wifi.forget        (ssid: str) -> Result unit wifi.Error !
wifi.set_enabled   (on: bool) -> Result unit wifi.Error !

@type WifiStatus =
  | :off
  | :disconnected
  | :connecting
  | :connected (ssid: str, ip: str, channel: int, rssi: int)
  | :failed    (reason: wifi.Error)

@type WifiNetwork
  ssid     : str
  rssi     : int
  security : atom              -- :open :wpa :wpa2 :wpa3
  channel  : int

@type ScanEvent =
  | :networks [WifiNetwork]
  | :done
  | :failed   wifi.Error

@errors wifi
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :no_network
  :bad_password
  :hardware_off
  :already_connecting
```

**Grants:** `:at_install` (mutations); read is `:never`.

---

### 20 · `network.bluetooth`

BLE GATT.

**Tier:** 2 · **Impl:** native · **DL:** 3

**State:** connection roster.

**Methods:**

```
ble.scan         (opts: ScanOpts?)              -> Stream BleScanEvent !
ble.connect      (addr: str)                    -> Result BleHandle ble.Error !
ble.disconnect   (h: BleHandle)                 -> Result unit ble.Error !
ble.services     (h: BleHandle)                 -> Result [BleService] ble.Error !
ble.read_char    (h: BleHandle, service: str, char: str) -> Result Bytes ble.Error !
ble.write_char   (h: BleHandle, service: str, char: str, value: Bytes) -> Result unit ble.Error !
ble.subscribe    (h: BleHandle, service: str, char: str) -> Stream BleNotification !

@type BleHandle = str

@type ScanOpts
  filter_services : [str]?
  duration        : Duration?

@type BleDevice
  addr     : str
  name     : str?
  rssi     : int
  services : [str]

@type BleScanEvent =
  | :devices [BleDevice]
  | :done
  | :failed  ble.Error

@type BleService
  uuid            : str
  characteristics : [str]

@type BleNotification
  service : str
  char    : str
  value   : Bytes

@errors ble
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :not_found
  :connection_failed
  :auth_failed
  :hardware_off
  :invalid_handle
```

**Grants:** `:at_install`.

---

## TIER 3 — System OS services

### 21 · `system.platform`

Read-only metadata about the device, runtime, and current app.

**Tier:** 3 · **Impl:** native · **DL:** 1

**State:** mostly constant per VM session; heap/cpu/uptime are time-varying.

**Methods:**

```
-- Constants
platform.device_id       () -> str
platform.model           () -> str
platform.os_name         () -> str
platform.os_version      () -> str
platform.runtime_version () -> str
platform.deck_level      () -> int
platform.edition         () -> int
platform.app_id          () -> str
platform.app_version     () -> str
platform.versions        () -> Versions

-- Time-varying
platform.uptime          () -> Duration !
platform.free_heap       () -> Size !
platform.used_heap       () -> Size !
platform.cpu_freq        () -> int  !

-- Watch
platform.heap_watch      () -> Stream HeapSnapshot !

@type Versions
  os                 : str
  runtime            : str
  edition            : int
  deck_level         : int
  max_heap           : Size
  max_stack          : int
  max_source_buffer  : int
  max_dir_entries    : int

@type HeapSnapshot
  free_internal       : Size
  free_psram          : Size
  free_total          : Size
  largest_free_block  : Size
  fragmentation_ratio : float

@errors platform
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
```

**Config:** none.
**Grants:** `:never`.

---

### 22 · `system.apps`

App registry, lifecycle, crashes, badges, service discovery. Backs `@on launch` / `@on resume` / `@on suspend` / `@on terminate` / `@on back` / `@on open_url` lifecycle hooks (Part III §54).

**Tier:** 3 · **Impl:** native · **DL:** 2

**State:** registry of installed + running apps.

**Methods:**

```
-- Queries
apps.info         (id: str) -> AppInfo? !
apps.load_error   (id: str) -> LoadError? !
apps.crashes      (id: str) -> [CrashEntry] !
apps.crashes_since(ts: Timestamp) -> [CrashEntry] !
apps.service      (service_id: str) -> ServiceInfo? !
apps.services_provided_by (app_id: str) -> [str] !
apps.services_consumed_by (app_id: str) -> [str] !

-- Watches
apps.installed_watch    () -> Stream [AppInfo] !
apps.running_watch      () -> Stream [AppInfo] !
apps.notif_counts_watch () -> Stream {str: int} !

-- Mutations
apps.launch  (id: str, data: any?) -> Result unit apps.Error !
apps.kill    (id: str)             -> Result unit apps.Error !

@type AppInfo
  id           : str
  name         : str
  version      : str
  icon         : str
  tags         : [str]
  running      : bool
  pid          : int?
  deck_level   : int
  size_on_disk : Size
  installed_at : Timestamp
  services     : [str]

@type CrashEntry
  app_id        : str
  kind          : atom      -- :bug, :limit, :internal
  message       : str
  trace_summary : str
  ts            : Timestamp

@type ServiceInfo
  id           : str
  provider_id  : str
  version      : int
  impl_kind    : atom      -- :native, :deck-app, :language-integrated
  allow        : ServiceAllow
  keep         : bool
  resident     : bool
  quarantined  : bool

@type ServiceAllow =
  | :any
  | :system
  | :list ([str])

@errors apps
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :unknown_app
  :not_running
  :launch_failed
```

**Config:** none.
**Grants:** `:at_install`. System-only.

---

### 23 · `system.power`

Battery + charging state.

**Tier:** 3 · **Impl:** native · **DL:** 2

**State:** `BatteryState`.

**Methods:**

```
power.state () -> BatteryState !
power.watch () -> Stream BatteryState !

@type BatteryState
  level     : float          -- 0.0..1.0
  charging  : bool
  voltage   : float          -- volts
  remaining : Duration?      -- :none if unknown

@errors power
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
```

**Config:** none.
**Grants:** `:never`.

---

### 24 · `system.display`

Brightness, rotation, lock, sleep, screen timeout. Bridges read these to render the UI per device state.

**Tier:** 3 · **Impl:** native · **DL:** 2

**State:** `DisplayState`.

**Methods:**

```
display.state          () -> DisplayState !
display.watch          () -> Stream DisplayState !
display.brightness     () -> float !
display.rotation       () -> atom  !
display.locked         () -> bool  !
display.screen_timeout () -> Duration !

display.set_brightness     (level: float)  -> Result unit display.Error !
display.set_rotation       (r: atom)       -> Result unit display.Error !
display.set_screen_timeout (d: Duration)   -> Result unit display.Error !
display.lock               ()              -> Result unit display.Error !
display.sleep              ()              -> Result unit display.Error !
display.wake               ()              -> Result unit display.Error !

@type DisplayState
  brightness     : float        -- 0.0..1.0
  rotation       : atom         -- :portrait :landscape
  locked         : bool
  sleeping       : bool
  screen_timeout : Duration

@errors display
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :invalid_value
```

**Config:** none.
**Grants:** Read `:never`; mutations `:at_install` (system-only).

---

### 25 · `system.audio`

Low-level audio playback + recording + volume. **For app authors, prefer `media.audio` (Tier 4)** which adds queue management, fade, codec abstraction.

**Tier:** 3 · **Impl:** native · **DL:** 3

**State:** `{volume, muted, active_handles}`.

**Methods:**

```
audio.volume     () -> float !
audio.muted      () -> bool !
audio.watch      () -> Stream AudioMixerState !

audio.set_volume (v: float) -> Result unit audio.Error !
audio.set_muted  (m: bool)  -> Result unit audio.Error !

audio.play       (src: AssetRef | str)         -> Result AudioHandle audio.Error !
audio.pause      (h: AudioHandle)              -> Result unit audio.Error !
audio.resume     (h: AudioHandle)              -> Result unit audio.Error !
audio.stop       (h: AudioHandle)              -> Result unit audio.Error !
audio.seek       (h: AudioHandle, pos: Duration) -> Result unit audio.Error !
audio.state      (h: AudioHandle)              -> Result AudioState audio.Error !
audio.state_watch(h: AudioHandle)              -> Stream AudioState !

audio.record       (opts: RecordOpts)              -> Stream Bytes !
audio.record_to_file (opts: RecordOpts, path: str) -> Stream RecordEvent !

@type AudioMixerState
  volume : float
  muted  : bool

@type AudioHandle = str

@type AudioState
  position : Duration
  duration : Duration?
  status   : atom              -- :playing :paused :stopped :ended :error

@type RecordOpts
  sample_rate  : int
  channels     : int
  format       : atom          -- :pcm_s16le :wav
  max_duration : Duration?

@type RecordEvent =
  | :progress (bytes_written: Size, elapsed: Duration)
  | :done
  | :failed   audio.Error

@errors audio
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :unsupported_format
  :hardware_unavailable
  :invalid_handle
  :invalid_value
```

**Grants:** playback `:on_first_use`; recording `:at_install`.

---

### 26 · `system.security`

PIN + permission storage. Backs `@needs.caps` / `@grants` (Part III §60).

**Tier:** 3 · **Impl:** native · **DL:** 2

**State:** PIN-set + per-app permission map.

**Methods:**

```
sec.pin_set            () -> bool !
sec.permission_state   (app_id: str, cap: str) -> PermissionState !
sec.grants_for         (app_id: str) -> {str: PermissionState} !
sec.grants_watch       (app_id: str) -> Stream {str: PermissionState} !

sec.set_pin            (pin: str) -> Result unit sec.Error !
sec.clear_pin          () -> Result unit sec.Error !
sec.verify_pin         (pin: str) -> Result bool sec.Error !
sec.permission_set     (app_id: str, cap: str, state: PermissionState) -> Result unit sec.Error !

@type PermissionState =
  | :granted
  | :denied
  | :not_decided

@errors sec
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :invalid_pin       "PIN must be 4-8 digits"
  :unknown_app
  :unknown_cap
```

**Grants:** `:at_install`. System-only.

---

### 27 · `system.time`

Wall-clock setting (NTP sync trigger, manual set). **Reading the clock is a builtin (`time.now`, `time.monotonic`, etc.)** with no service involved.

**Tier:** 3 · **Impl:** native · **DL:** 2

**Methods:**

```
system.time.set      (ts: Timestamp) -> Result unit system.time.Error !
system.time.sync_ntp () -> Result Timestamp system.time.Error !       -- returns the new wall-clock value

@errors system.time
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :out_of_range
  :ntp_failed
```

**Grants:** `:at_install`. System-only.

---

### 28 · `system.locale`

Language / region.

**Tier:** 3 · **Impl:** native · **DL:** 3

**State:** current locale.

**Methods:**

```
locale.current   () -> str   !
locale.available () -> [str] !
locale.watch     () -> Stream str !
locale.set       (code: str) -> Result unit locale.Error !

@errors locale
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :unknown_locale
```

**Grants:** Read `:never`; `set` `:at_install` (system-only).

---

### 29 · `system.ota`

Firmware over-the-air update.

**Tier:** 3 · **Impl:** native · **DL:** 2

**State:** `OtaState`.

**Methods:**

```
ota.state       () -> OtaState !
ota.state_watch () -> Stream OtaState !
ota.check       () -> Stream CheckEvent !
ota.download    () -> Stream DownloadEvent !
ota.apply       () -> Result unit ota.Error !
ota.cancel      () -> Result unit ota.Error !

@type OtaState =
  | :idle
  | :checking
  | :update_available (info: UpdateInfo)
  | :downloading     (progress: float)
  | :ready_to_apply  (info: UpdateInfo)
  | :applying
  | :failed          (reason: ota.Error)

@type UpdateInfo
  version       : str
  size          : Size
  released_at   : Timestamp
  release_notes : str?

@type CheckEvent =
  | :update (info: UpdateInfo)
  | :up_to_date
  | :failed ota.Error

@type DownloadEvent =
  | :progress (pct: float)
  | :done     (info: UpdateInfo)
  | :failed   ota.Error

@errors ota
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :network
  :checksum
  :storage_full
  :busy
  :apply_failed
```

**Config:** `channel: str = "stable"`.
**Grants:** `:at_install`. System-only.

---

### 30 · `system.url`

Deep-link dispatch. Resolves a URL against the registered `@handles` patterns (Part III §58).

**Tier:** 3 · **Impl:** native · **DL:** 1

**Methods:**

```
url.open     (url: str) -> Result unit url.Error !
url.can_open (url: str) -> bool !

@errors url
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :no_handler
  :malformed
```

**Grants:** `:at_install`.

---

### 31 · `system.notify`

Notifications + toasts.

**Tier:** 3 · **Impl:** native · **DL:** 1

**State:** badge + live-notifications per app.

**Methods:**

```
notify.badge     () -> int !
notify.set_badge (count: int) -> Result unit notify.Error !

notify.toast     (message: str, duration: Duration?) -> Result unit notify.Error !
notify.post      (opts: NotifyOpts) -> Result NotificationId notify.Error !
notify.dismiss   (id: NotificationId) -> Result unit notify.Error !
notify.clear_all () -> Result unit notify.Error !

@type NotifyOpts
  title    : str
  body     : str?
  priority : atom?           -- :low :normal :high
  deep_url : str?
  icon     : AssetRef?

@type NotificationId = str

@errors notify
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :rate_limited
  :unknown_id
```

**Grants:** `:on_first_use`. Denied-grant: gracefully degrades (returns `:err :permission_denied`).

---

### 32 · `system.theme`

OS-wide theme selection.

**Tier:** 3 · **Impl:** native · **DL:** 2

**State:** current `Theme`.

**Methods:**

```
theme.current () -> Theme !
theme.all     () -> [Theme] !
theme.watch   () -> Stream Theme !
theme.set     (id: atom) -> Result unit theme.Error !

@type Theme
  id      : atom
  name    : str
  is_dark : bool
  accent  : str           -- 7-char hex #RRGGBB

@errors theme
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :unknown_theme
```

**Grants:** Read `:never`; `set` `:at_install` (system-only).

---

### 33 · `system.logs`

Log sink. **Backs the `log.*` builtin** (Part III §52). Also exposes a query/tail API for system diagnostic apps.

**Tier:** 3 · **Impl:** native · **DL:** 1

**State:** rolling per-app ring buffers + persistent SD writer.

**Methods:**

```
logs.emit       (entry: LogEntry) -> unit !          -- backing for log.* builtins
logs.ring       (app_id: str?, n: int?) -> [LogEntry] !
logs.tail       (app_id: str?, since: Timestamp?) -> Stream LogEntry !
logs.query      (q: LogQuery) -> [LogEntry] !
logs.clear      (app_id: str) -> Result unit logs.Error !
logs.set_level  (app_id: str, level: LogLevel) -> Result unit logs.Error !

@type LogEntry
  ts       : Timestamp
  level    : LogLevel
  app_id   : str
  source   : LogSource
  message  : str
  data     : {str: any}?
  trace_id : str?

@type LogLevel = :trace | :debug | :info | :warn | :error

@type LogSource =
  | :lifecycle (hook: atom)
  | :reactive  (on: atom, source: str)
  | :service   (service_id: str, method: atom)
  | :foreground

@type LogQuery
  app_id   : str?
  level    : LogLevel?
  since    : Timestamp?
  until    : Timestamp?
  contains : str?
  trace_id : str?
  limit    : int?

@errors logs
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :unknown_app
```

**Grants:** `emit` is `:never` (every app logs). Read methods (`ring`, `tail`, `query`, `set_level`) are `:at_install`, system-only.

---

### 34 · `system.scheduler`

Timer dispatch. **Backs `@on every D` and `@on after D`** (Part III §50).

**Tier:** 3 · **Impl:** native · **DL:** 1

**State:** active timers.

**Methods:**

```
scheduler.register_every  (id: str, interval: Duration, keep: bool) -> Result unit scheduler.Error !
scheduler.register_after  (id: str, delay: Duration, keep: bool)    -> Result unit scheduler.Error !
scheduler.cancel          (id: str) -> Result unit scheduler.Error !
scheduler.list            () -> [TimerInfo] !

@type TimerInfo
  id         : str
  app_id     : str
  kind       : atom              -- :every :after
  duration   : Duration
  keep       : bool
  next_fire  : Timestamp

@errors scheduler
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :too_short            "interval < 50ms"
  :unknown_id
  :too_many_timers      "platform per-app cap exceeded"
```

**Grants:** `:never` for app-internal timers (declared via `@on every / after`). System-only for cross-app `register_*` calls.

**Notes:**
- Apps don't typically call `scheduler.*` directly; the runtime manages timer registration based on `@on every / after` declarations.
- Direct calls are for cross-app scheduling (a system app delaying another app's startup, for example).

---

### 35 · `system.events`

OS event bus. **Backs `@on os.<event>`** (Part III §49).

**Tier:** 3 · **Impl:** native · **DL:** 1

**State:** subscriber map.

**Methods:**

```
events.subscribe   (event_name: str) -> Stream OsEvent !
events.subscribe_pattern (pattern: str) -> Stream OsEvent !     -- e.g. "os.wifi.*"
events.publish     (event_name: str, payload: any) -> Result unit events.Error !

@type OsEvent
  name    : str             -- e.g. "os.wifi_changed"
  payload : any             -- shape depends on event
  ts      : Timestamp

@errors events
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :invalid_pattern
```

**Grants:** `subscribe` is `:never` (declared via `@on os.<event>`); `publish` is `:at_install` (system-only).

**Standard OS events:**

```
os.config_changed     (key: atom, old: any, new: any)        -- @config write
os.wifi_changed       (status: WifiStatus)                   -- alias for wifi.status_watch convenience
os.theme_changed      (theme: Theme)                         -- alias for theme.watch
os.locale_changed     (code: str)
os.power_changed      (state: BatteryState)
os.app_installed      (id: str)
os.app_uninstalled    (id: str)
os.app_crashed        (entry: CrashEntry)
os.log_quota_hit      (app_id: str)
os.low_memory         (free: Size)
os.network_change     (online: bool)
```

---

### 36 · `system.intents`

Intent / `@handles` URL pattern dispatch. **Backs `@handles`** (Part III §58).

**Tier:** 3 · **Impl:** native · **DL:** 1

**State:** registered URL patterns per app.

**Methods:**

```
intents.matches    (url: str) -> [Match] !
intents.dispatch   (url: str) -> Result Match intents.Error !

@type Match
  app_id  : str
  pattern : str
  params  : {str: str}        -- extracted path params

@errors intents
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :no_match
  :ambiguous          "multiple non-overlapping matches"
```

**Grants:** read `:never`. Direct `dispatch` is internal — apps use `system.url.open` which calls into intents.

---

### 37 · `system.services`

Service registry. **Backs `@service` declarations and IPC dispatch** (Part III §56).

**Tier:** 3 · **Impl:** native · **DL:** 2

**State:** registered services + per-service metadata.

**Methods:**

```
services.list             () -> [ServiceInfo] !
services.info             (id: str) -> ServiceInfo? !
services.relaunch         (id: str) -> Result unit services.Error !       -- recover a quarantined service

-- Also acts as the IPC dispatcher (apps call services indirectly via @use)

@errors services
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :unknown_service
```

**Grants:** read `:never`; `relaunch` is `:at_install` (system-only).

**Notes:**
- Apps mostly don't call `services.*` directly; the runtime handles IPC routing under `@use`.
- `system.services.relaunch` is the recovery path for quarantined services (per §9.5).

---

### 38 · `system.tasks`

Per-process resource snapshots. Used by Task Manager.

**Tier:** 3 · **Impl:** native · **DL:** 3

**State:** snapshot of running processes.

**Methods:**

```
tasks.processes_watch () -> Stream [ProcessEntry] !

@type ProcessEntry
  app_id      : str
  pid         : int
  cpu_percent : float
  heap_used   : Size
  uptime      : Duration

@errors tasks
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
```

**Config:** `sample_interval: Duration = 1s`.
**Grants:** `:at_install`. System-only.

---

## TIER 4 — High-level domain services (PREFERRED)

### 39 · `api.client`

REST client over `network.http`. Adds: base URL, default headers, auth (bearer / basic), retry with backoff, JSON body codec, response caching with content-type awareness.

**Most app authors fetching JSON should use this, not `network.http` directly.**

**Tier:** 4 · **Impl:** native · **DL:** 2

**State:** per-alias session (auth token, headers).

**Methods:**

```
api.get         (path: str, opts: ApiOpts?)              -> Result T api.Error !
api.post        (path: str, body: any, opts: ApiOpts?)   -> Result T api.Error !
api.put         (path: str, body: any, opts: ApiOpts?)   -> Result T api.Error !
api.patch       (path: str, body: any, opts: ApiOpts?)   -> Result T api.Error !
api.delete      (path: str, opts: ApiOpts?)              -> Result T api.Error !

api.set_token   (token: str) -> unit !
api.clear_token () -> unit !

@type ApiOpts
  query        : {str: any}?
  headers      : {str: str}?
  cache        : Duration?               -- if :some, cache GET responses
  timeout      : Duration?
  expect       : atom?                   -- :json (default), :text, :bytes

@errors api
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :network
  :not_found             -- 404
  :unauthorized          -- 401 (auth token invalid / missing)
  :forbidden             -- 403
  :rate_limited          -- 429
  :server_error          -- 5xx
  :malformed             -- response decode failed
  :offline
```

**Config:**

```
base_url    : str?              = :none
timeout     : Duration          = 30s
retry       : int               = 2
retry_delay : Duration          = 1s
auth        : (atom, str)?      = :none      -- (:bearer, "token") | (:basic, "user:pass")
default_headers : {str: str}?   = :none
```

**Grants:** `:at_install` with `allowed_hosts: [str]` (inherited from `network.http`).

**Notes:**
- The return type `T` is decoded from the response body per `expect:` (default `:json`, parsed via `json.parse`).
- `cache:` opt enables response caching via `data.cache` (Tier 4) keyed on `(method, url, query)`.
- `:err :unauthorized` callers should refresh tokens (via `auth.oauth.refresh`) and retry.

**Example:**

```
@use
  api.client as bsky
    base_url:        "https://bsky.social/xrpc"
    auth:            (:bearer, config.bsky_token)
    default_headers: { "Atproto-Accept-Labelers": "*" }

let timeline = bsky.get("/app.bsky.feed.getTimeline", { query: { limit: 50 } })?
```

---

### 40 · `media.image`

Image loading + caching + transformation. Over `network.http` + `storage.cache` + `storage.fs`.

**Tier:** 4 · **Impl:** native · **DL:** 2

**State:** image cache index.

**Methods:**

```
image.load        (src: str | AssetRef) -> Result ImageRef image.Error !
image.dimensions  (ref: ImageRef) -> Result (width: int, height: int) image.Error !
image.thumbnail   (ref: ImageRef, max_dim: int) -> Result ImageRef image.Error !
image.delete      (ref: ImageRef) -> unit !
image.cache_size  () -> Size !
image.clear_cache () -> Result unit image.Error !

@type ImageRef
  id    : str
  src   : str           -- original source (url or asset name)
  kind  : atom          -- :png :jpeg :webp :gif

@errors image
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :network
  :unsupported_format
  :too_large
  :not_found
  :decode_error
```

**Config:** `max_cache: Size = 16MB`, `default_ttl: Duration = 7d`.
**Grants:** `:at_install` with `allowed_hosts: [str]`.

**Notes:**
- `media.image` returns `ImageRef`, not raw bytes. The bridge resolves `ImageRef` when rendering `media` content (per §15.3 of `DRAFT.md`).
- `image.thumbnail` produces a derived `ImageRef`; original retained until `image.delete`.

---

### 41 · `media.audio`

High-level audio playback (queue, fade, gapless, codec abstraction). Over `system.audio`.

**Tier:** 4 · **Impl:** native · **DL:** 3

**State:** playback queue + position.

**Methods:**

```
media_audio.play_queue    (sources: [AssetRef | str]) -> Result unit media_audio.Error !
media_audio.enqueue       (source: AssetRef | str)    -> Result unit media_audio.Error !
media_audio.skip_next     ()                          -> Result unit media_audio.Error !
media_audio.skip_previous ()                          -> Result unit media_audio.Error !
media_audio.pause         () -> Result unit media_audio.Error !
media_audio.resume        () -> Result unit media_audio.Error !
media_audio.stop          () -> Result unit media_audio.Error !
media_audio.seek          (pos: Duration) -> Result unit media_audio.Error !

media_audio.state         () -> PlayerState !
media_audio.state_watch   () -> Stream PlayerState !

@type PlayerState
  status     : atom            -- :stopped :playing :paused :buffering
  current    : str?            -- source identifier
  position   : Duration
  duration   : Duration?
  queue      : [str]
  queue_pos  : int

@errors media_audio
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :unsupported_format
  :network
  :not_found
  :empty_queue
```

**Config:** `crossfade: Duration = 0ms`.
**Grants:** `:on_first_use`.

---

### 42 · `auth.oauth`

OAuth 2.0 flow helper. Over `network.http` + `storage.nvs` + `system.url`.

**Tier:** 4 · **Impl:** native · **DL:** 2

**State:** per-provider token store (in NVS).

**Methods:**

```
oauth.start_flow   (provider: str, scopes: [str]) -> Result str oauth.Error !       -- returns auth URL to open
oauth.complete     (provider: str, callback_url: str) -> Result Token oauth.Error !  -- called from @on open_url
oauth.token        (provider: str) -> Token? !
oauth.refresh      (provider: str) -> Result Token oauth.Error !
oauth.revoke       (provider: str) -> Result unit oauth.Error !
oauth.providers    () -> [str] !

@type Token
  access_token  : str
  refresh_token : str?
  expires_at    : Timestamp?
  scopes        : [str]

@errors oauth
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :network
  :unknown_provider
  :invalid_callback
  :token_revoked
  :rate_limited
```

**Config:** registry of providers (per-alias):

```
providers: { str: ProviderConfig }

@type ProviderConfig
  client_id      : str
  client_secret  : str?
  authorize_url  : str
  token_url      : str
  redirect_url   : str          -- usually deck://oauth-callback?provider=NAME
```

**Grants:** `:at_install`.

**Notes:**
- `oauth.complete` is called from `@on open_url` when the OAuth provider redirects back to the app (typically `deck://oauth-callback?provider=…&code=…`).
- Token storage uses `storage.nvs` under per-provider key.

---

### 43 · `data.cache`

Schema'd response cache with content-type awareness. Over `storage.cache` + `storage.nvs` (for cache index persistence).

**Tier:** 4 · **Impl:** native · **DL:** 2

**State:** cache index.

**Methods:**

```
data_cache.get    (key: str) -> CacheEntry? !
data_cache.set    (key: str, value: any, opts: CacheOpts) -> Result unit data_cache.Error !
data_cache.delete (key: str) -> unit !
data_cache.invalidate (pattern: str) -> Result int data_cache.Error !

@type CacheEntry
  value         : any
  content_type  : str
  cached_at     : Timestamp
  expires_at    : Timestamp?

@type CacheOpts
  ttl          : Duration
  content_type : str?

@errors data_cache
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :value_too_big
  :cache_full
```

**Config:** `max_size: Size = 8MB`.
**Grants:** `:never`.

---

### 44 · `share.target`

Standard "share-to" interface. Provided by the `system.share` app (Deck-app implementation).

**Tier:** 4 · **Impl:** deck-app · **DL:** 2

```
@service "share.target"
  version: 1
  allow:   :any
  keep:    false

  on :share (content: ShareContent) -> Result unit share.Error !
  on :supported_kinds () -> Result [atom] share.Error !

@type ShareContent
  kind      : atom               -- :url :text :image :file
  title     : str?
  body      : str?
  url       : str?
  image_ref : AssetRef?
  file_path : str?

@errors share
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :no_target            "User cancelled the picker"
  :unsupported_kind
  :payload_too_large
```

The dispatcher presents the user a picker among installed apps that handle the content kind, then forwards the payload to the chosen app.

---

## TIER 5 — Sensors

### 45 · `sensors.<name>` (DL3, platform-defined)

Every sensor capability has the **same shape**:

```
<sensor>.now   ()                 -> Result Sample <sensor>.Error !
<sensor>.watch (opts: SampleOpts?) -> Stream Sample !

@type SampleOpts
  rate_hz : int?              -- :none → platform default

@errors <sensor>
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :hardware_off
```

Standard sensor types (optional per platform):

```
sensors.imu          : Sample = (accel: (x,y,z), gyro: (x,y,z), ts: Timestamp)
sensors.environment  : Sample = (temperature_c: float, humidity_pct: float?, pressure_hpa: float?, ts: Timestamp)
sensors.light        : Sample = (lux: float, ts: Timestamp)
sensors.magnetometer : Sample = (x: float, y: float, z: float, ts: Timestamp)
sensors.gps          : Sample = (lat: float, lng: float, alt_m: float?, accuracy_m: float, ts: Timestamp)
```

CyberDeck reference platform: none shipped by default.

---

# Part III — Language ↔ Service Integration

The Deck language is itself implemented as a **service consumer**. Every language feature that touches state or crosses a boundary translates to a service call. The runtime is the implicit consumer; apps perceive only the language sugar.

## 46 · The integration table

| Language feature | Backed by service | Notes |
|---|---|---|
| `@config field: T = default` | `storage.nvs` | Key `app.{id}.config.{field}`; runtime adds schema validation, atomic writes, change-event emission |
| `@config field: {str: V} = {}` | `storage.nvs` | Single key holding map; dynamic-key pattern via map values |
| `config.set(:k, v)` | `storage.nvs.set_*` + `system.events.publish(:os.config_changed, ...)` | Atomic per §12 of `DRAFT.md` |
| `@assets X: "path" as: :tag` | `storage.fs` (read-only `/app/{id}/assets/`) | Bundled asset; resolved at consumption time |
| `@assets X: download: "url" ttl: 7d` | `network.http` + `storage.cache` | Downloaded with TTL; refreshed on expiry |
| `assets.asset(:name)` | `system.apps` (asset registry) | Returns `AssetRef` |
| `@machine` state persistence | `storage.nvs` | Key `app.{id}.machine.{MachineName}`; serialised at clean suspend |
| `@migrate from N: ...` | `storage.nvs` | Key `app.{id}.migration_version`; migration body uses any service |
| `log.trace/debug/info/warn/error` | `system.logs.emit` | Auto-stamps `app_id`, `trace_id`, `ts`, `source` |
| `log.peek` | `system.logs.emit` | Pass-through with debug log |
| `time.now / monotonic / since / ago` | (builtins; backed by kernel.rtc + monotonic clock) | No service involved; reading is free |
| `time.set_wall_clock` | `system.time.set` | Capability-gated |
| `@on launch / resume / suspend / terminate / back / open_url` | `system.apps` | Lifecycle channel |
| `@on os.<event>` | `system.events.subscribe(event_name)` | Auto-subscribed at install |
| `@on hardware.<event>` | platform-defined kernel events | Low-level hardware event bus |
| `@on every D` | `system.scheduler.register_every` | Auto-registered at install |
| `@on after D` | `system.scheduler.register_after` | One-shot from launch / resume |
| `@on watch <expr>` | (no service; runtime-internal reactive evaluation) | Per §14.10 of `DRAFT.md` |
| `@on source <stream_expr>` | (the source's own service) | Wired by the source expression |
| `@on panic` (system apps only) | `system.events.subscribe("os.app_crashed")` | Crash channel |
| `@on overrun` | runtime-internal | Self-monitoring |
| `@service "id" = ...` | `system.services` (registration) | Provider declares; runtime dispatches IPC |
| `@use service "id" as alias` | `system.services` (lookup) + `system.security` (allow check) | Bind-time resolution |
| `@handles "url-pattern"` | `system.intents` (registration) | Loader registers patterns at install |
| `@needs.caps / @grants` | `system.security` | Install-time validation + runtime permission state |
| `@app.icon / @app.tags` | `system.apps` (registry metadata) | Indexed for search and launcher display |

## 47 · `@config` as `storage.nvs` consumer

`@config` is a **typed schema layer** over `storage.nvs`. The provider behaviour:

```
@config
  sync_every : Duration = 5m
  theme      : atom     = :auto
  pinned     : [str]    = []
```

Translates internally to:

```
-- Read: config.sync_every
nvs.get_int("config.sync_every") |> result.unwrap_or(5m)         -- with type coercion

-- Write: config.set(:sync_every, 10m)
nvs.set_int("config.sync_every", 10m)?
events.publish("os.config_changed", { key: :sync_every, old: 5m, new: 10m })
```

The runtime adds:
1. **Schema validation**: writes against declared type / `min:` / `max:` / `in:` constraints.
2. **Atomicity**: per §12 of `DRAFT.md`.
3. **Change-event emission**: after the body that called `config.set` completes.
4. **Default fallback**: missing keys fall to declared defaults instead of `:err :not_found`.

Apps may *also* `@use storage.nvs` directly for non-schema'd dynamic keys — the two coexist within the same per-app namespace.

## 48 · `@assets` as `storage.fs` + `storage.cache` consumer

```
@assets
  icon      : "assets/icon.png"            as: :icon
  cert      : "assets/api.crt"             as: :tls_cert
  big       : download: "https://cdn/big.json" ttl: 7d
```

Bundled assets (`"assets/..."`) are stored in the read-only fs partition under `/app/{id}/assets/`. `assets.asset(:name)` returns an `AssetRef` resolving to that path.

Downloaded assets (`download: "url" ttl: D`) are fetched on first reference via `network.http.get` and cached via `storage.cache` under key `asset.{app_id}.{name}`. Subsequent references return from cache until TTL expires. `assets.refresh(:name)` forces re-fetch.

## 49 · `@machine` state persistence as `storage.nvs` consumer

At clean suspend (per §22.4 of `DRAFT.md`), the runtime serialises each `@machine` (state atom + payload) and writes to NVS:

```
nvs.set_str("machine.{MachineName}", encoded_state)
```

On resume, the runtime reads back and restores. The encoding is JSON via the `json.encode` builtin; types map to JSON tagged-record form per `DRAFT.md` §15.3.

## 50 · `log.*` as `system.logs.emit` consumer

Every `log.<level>(msg, data?)` call:

```
log.info("connected", { ssid: "Home" })
```

translates to:

```
logs.emit({
  ts:       time.now(),
  level:    :info,
  app_id:   <runtime app_id>,
  source:   <runtime-detected source>,
  message:  "connected",
  data:     { ssid: "Home" },
  trace_id: <runtime-generated trace_id>,
})
```

The runtime auto-stamps `ts`, `app_id`, `source`, and `trace_id` per §16.4 of `DRAFT.md`. Apps cannot bypass this — `log.*` is the only path to `system.logs.emit`.

## 51 · `@on os.<event>` as `system.events.subscribe` consumer

For every `@on os.<event_name>` declared in an app:

```
@on os.wifi_changed (ssid: s, connected: c)
  ->
    if c then log.info("connected to {s}") else log.warn("disconnected")
```

The loader registers a subscription with `system.events.subscribe("os.wifi_changed")` at app install. When the event fires, the runtime dispatches to the app's VM event queue, decoding the payload according to the binding pattern in the `@on` header.

## 52 · `@on every D` / `@on after D` as `system.scheduler` consumer

```
@on every 30m
  keep: true
  ->
    api_refresh.fetch()
```

The loader registers with `system.scheduler.register_every({ id: "every_30m_<hash>", interval: 30m, keep: true })` at install. The scheduler fires monotonic-clock-driven events; the runtime dispatches to the registered handler.

## 53 · `@on source <stream>` as service-stream consumer

```
@on source wifi.status_watch() as Status
  ->
    log.info("wifi: {event}")
```

The expression `wifi.status_watch()` is a `Stream WifiStatus` produced by the `network.wifi` service. The runtime subscribes at handler install; for each emission, dispatches to the body. Cancellation on app termination drops the subscription.

## 54 · `@on launch / resume / suspend / terminate / back / open_url` as `system.apps` consumer

These are special channels delivered by `system.apps` to each app's VM. The runtime's app-spawn / app-suspend logic IS the producer; apps are the consumers.

`@on open_url` handlers receive the URL params extracted by `system.intents` matching against the app's `@handles` patterns.

## 55 · `@service` registration via `system.services`

Every `@service "id"` in an app's source registers with `system.services` at app install:

```
@service "social.bsky.app/feed"
  version: 1
  allow:   :any
  keep:    true
  ...
```

Becomes a `ServiceRegistration` entry in the system services registry. Consumers' `@use service "..."` calls go through the registry to dispatch IPC. The provider's VM is spawned on first call.

## 56 · `@handles` registration via `system.intents`

```
@handles
  "bsky://profile/{handle}"
  "https://bsky.app/profile/{handle}"
```

Registers two URL patterns with `system.intents` at install. `system.url.open(url)` consults the intents registry; on match, dispatches `@on open_url` of the matching app with extracted params.

## 57 · `@needs.caps / @grants` validation via `system.security`

The loader queries `system.security.permission_state(app_id, cap)` for each declared `@needs.caps`. Missing grants prompt the user per `@grants.<cap>.prompt:` timing.

`@grants` declarations are stored in the security service's per-app permission map; user decisions are persisted there.

## 58 · The reflexive insight

The language is implemented over **a small set of services**: `storage.nvs`, `storage.fs`, `network.http`, `storage.cache`, `system.apps`, `system.events`, `system.scheduler`, `system.intents`, `system.services`, `system.security`, `system.logs`. Eleven services back the entire language surface. Adding a twelfth would require a new language feature; removing one would shrink the language.

This is the design's strongest invariant: the language has no special-cased functionality. Everything is service-mediated. The runtime is a particularly trusted consumer with fewer prompts and a reflective view (it can subscribe to events for any app), but the call shape is identical to user-app consumers.

---

# Part IV — Authoring app services

## 59 · Declaring a service

A service is declared in the providing app via a top-level `@service` block. The block belongs to the app's module graph the same way `@machine` and `@on` do.

```
@app
  name:    "Bluesky"
  id:      "social.bsky.app"
  version: "1.0.0"
  edition: 2027
  entry:   App
  serves:
    - "social.bsky.app/feed"
    - "social.bsky.app/notifications"

@service "social.bsky.app/feed"
  version: 1
  allow:   :any
  keep:    true

  on :fetch_latest () -> Result [Post] feed.Error !
    = ...

  on :post (text: str) -> Result PostRef feed.Error !
    = ...

  on :timeline_watch () -> Stream [Post] !
    = ...

@errors feed
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :upstream_unreachable
  :rate_limited
  :auth_required
  :malformed_response
```

**Required fields in `@service`:**

| Field | Type | Default | Notes |
|---|---|---|---|
| `version` | `int` | `1` | |
| `allow` | `ServiceAllow` | `:any` | |
| `keep` | `bool` | `false` | |
| `max_run_ms_default` | `int?` | `5000` | Per-method default; individual methods override with `max_run_ms:` |

The `@service` block is otherwise just a list of `on :name (params) -> ReturnType ! = body` declarations.

## 60 · Stream-returning methods

A service method may return `Stream T`. The provider's body uses the `stream.emit(value)` builtin to push:

```
on :timeline_watch () -> Stream Post !
  =
    while not subscription_cancelled
      let new_posts = poll_for_new()
      list.each_io(new_posts, p -> stream.emit(p))
      time.sleep(30s)
```

Cancellation across IPC: when a consumer drops a stream subscription, the runtime sends a cancel signal; the provider's body terminates after its next yield point.

## 61 · Error translation discipline

A service NEVER exposes its implementation's error domain. The provider translates every internal failure to a consumer-visible atom:

```
on :fetch_latest () -> Result [Post] feed.Error !
  =
    let resp = api.get("/timeline") |> result.map_err(api_to_feed)?
    parse_posts(resp) |> result.map_err(_ -> :malformed_response)

@private fn api_to_feed (e: api.Error) -> feed.Error =
  match e
    | :timeout       -> :upstream_unreachable
    | :network       -> :upstream_unreachable
    | :unauthorized  -> :auth_required
    | :rate_limited  -> :rate_limited
    | _              -> :upstream_unreachable
```

Without translation, consumers couple to provider internals. Refactoring (HTTP today, GraphQL tomorrow) becomes a breaking change. The translation function is the provider's stability boundary.

## 62 · Versioning policy

Per §8 above. Practical guidance:

- Bump version when removing methods, changing signatures, or adding error atoms.
- Don't bump when adding methods.
- Maintain a previous-version `@service` block for at least one release after a bump, calling into the new logic with translation.
- Telemetry: `system.apps.services_consumed_by` (with version range) tells you which consumers still pin which version.

## 63 · State and persistence

A service runs inside the provider's VM. State management:

- **Machine state** — services may dispatch to the provider's `@machine` via `<MachineName>.send(…)` exactly as foreground code.
- **`@config`** — services read/write the provider's `@config` exactly as foreground code.
- **Per-call locals** — gone after the call returns.
- **Multi-call sessions** — use the handle pattern (Part I §5).

## 64 · Examples

### 64.1 Single-method query

```
@service "media.weather.dn/forecast"
  version: 1
  allow:   :any

  on :for_city (city: str) -> Result Forecast forecast.Error !
    = api.get("/forecast", { query: { city: city } }) |> result.map_err(api_to_forecast)

@type Forecast
  high_c       : float
  low_c        : float
  description  : str
  retrieved_at : Timestamp
```

### 64.2 Stream-notifier

```
@service "social.bsky.app/notifications"
  version: 1
  allow:   :any
  keep:    true

  on :unread_count () -> Result int notif.Error !
    = ...

  on :stream () -> Stream Notification !
    = ... (emit each new notification)
```

Consumers tap with `@on source bsky_notif.stream() as Inbox`.

### 64.3 Hub (fan-out)

```
@service "system.health/events"
  version: 1
  allow:   :any
  keep:    true

  on :alerts () -> Stream HealthAlert !
    = ... (provider holds subscriber list; emit broadcasts to all)

@type HealthAlert
  severity : atom              -- :info :warn :critical
  message  : str
  source   : atom              -- :wifi :battery :memory :disk :ota
  ts       : Timestamp
```

---

# Part V — Extension contract

## 65 · Adding a native service

Platform components add native services through `.deck-os` manifest files. Each manifest declares one service following the meta-spec:

```
@service-native storage.fs
  tier:      1
  level:     1                      -- DL minimum
  stateful:  false

@config-schema
  paths: [str]? = :none

@errors fs
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :not_found
  ...

@methods
  exists      (path: str) -> bool !
  read        (path: str) -> Result str fs.Error !
  ...

@grants-schema
  reason: str
  prompt: atom = :at_install
  paths:  [str]?

@events
  (none)
```

The platform-side implementation provides a C/Rust function per method matching the declared signature.

## 66 · Replacing a native service

A platform may ship an alternative implementation under the same service ID. The runtime uses the highest-version implementation; if both versions are equal, the platform's most-recently-installed manifest wins.

To replace `storage.cache` with a smarter LRU implementation: ship a new component with `@service-native storage.cache version: 2` that satisfies the existing v1 signatures plus any new ones.

## 67 · Conformance tests

Every service (native, deck-app, or language-integrated) must pass:

- **Method coverage** — every method invokable; signatures match.
- **Universal errors** — every error domain emits `:unavailable`, `:permission_denied`, `:service_unavailable`, `:timeout` on the right occasions.
- **Watch stream correctness** — first emission ≤ 100 ms after subscription; emissions only on structural change; no emissions after 5 s of idle state.
- **Action stream correctness** — exactly one `:done` / `:failed`; no emissions after terminator; cancellation ≤ 50 ms after subscription drop.
- **Handle lifecycle** — every Handle expires after its explicit close method; post-close calls return `:err :invalid_handle`.
- **Grant behaviour** — denied-grant returns `:err :permission_denied` or binds the alias as `:unavailable`, as declared.
- **Error translation** — for service implementations using lower-tier services, internal `Result :err` from the lower tier never escapes; the service's own error domain is closed.
- **Quarantine recovery** (deck-app services only) — after manual relaunch from `system.apps`, the service responds again.

---

# Part VI — Deferred for future revisions

## 68 · UI bridge — NOT a service (see `BRIDGE.md`)

Earlier drafts of this document reserved a slot for `system.bridge.ui` as "the one deferred service". That framing was wrong and is corrected here.

The UI bridge is **not** a Deck service. Apps do not `@use` the bridge, cannot call any `bridge.*` method, and cannot declare a grant on it. The bridge is a **platform component** — a native driver registered under the SDI slot `DECK_SDI_DRIVER_BRIDGE_UI` (see `BRIDGE.md §4`). Its interfaces are:

- **Content pipeline** (runtime → bridge): DVC snapshots produced by evaluating `content =` blocks.
- **Intent pipeline** (bridge → runtime): user activations fire intents that the runtime routes to machine transitions.
- **UI-service backends**: the bridge hosts the rendering of several Tier-3 system services — `system.notify` (toast), `system.display` (rotation, brightness, lock, sleep/wake), `system.theme` (palette swap), `system.security` (lockscreen), `system.share` (share sheet). An app calling any of those services is consuming a standard service; the bridge's involvement is invisible at the consumer's level.

Apps that need to influence presentation indirectly do so through the services above. Nothing in the catalog exposes "the bridge" as a callable surface.

The full bridge contract — driver vtable, content tree decoding, inference rules, UI services, subsystems, conformance profiles — lives in `BRIDGE.md`. This catalog is complete without it; a platform can implement every service in Tiers 1–5 and still be headless (no bridge at all).

## 69 · Other deferred services

- **`storage.db` (SQLite / embedded DB)** — every observed use case fits `storage.fs` + `@config` (with map-valued fields) + userland iteration. Revisit if relational queries become essential.
- **`network.mqtt`** — zero annex usage; revisit when a real app demands pub/sub.
- **`crypto.aes` / `crypto.hash`** — transport security covered by `network.http` TLS; at-rest encryption out of scope until needed.
- **`camera`** — not supported on CyberDeck hardware.
- **`peripherals.input` (HID over BLE/USB)** — revisit when an app needs Bluetooth keyboard or gamepad input.
- **Cross-device service discovery (mDNS / DNS-SD)** — services are local-VM-only in v1.
- **Service mesh / load balancing** — single provider per ID; multi-instance not supported.
- **Capability tokens (delegation)** — caller always acts as itself; no delegated authority model.
- **Pre-emptive cancellation mid-call** — methods that need cancellability are expressed as streams whose final emission is `:cancelled`.
- **Service-to-service IPC chains** — allowed but mutual-recursion deadlock detection is post-v1 work.

---

**End of services catalog draft. Promote, amend, or discard as a whole.**
