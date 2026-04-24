# Deck 3.0 — Capability Catalog

**Status:** Draft. Companion to `DECK-3.0-DRAFT.md`. Not yet authoritative.

**Edition:** 2027.

This document defines three things:
- **Part I** — the meta-spec: what a capability IS, how any capability must be shaped.
- **Part II** — the v1 catalog: the minimal set of capabilities needed today.
- **Part III** — the extension contract: how a platform adds a capability.

Apps reach the OS only through capabilities (§9 of the draft). Everything observable about the physical device, the app store, the network, or other apps is mediated by one. Pure computation — math, text processing, list manipulation — lives in **builtins** (see `DECK-3.0-BUILTINS.md`, follow-up); builtins require no `@use` and no grants.

---

# Part I — Meta-spec: the shape of a capability

## 0 · Philosophy (Unix for embedded)

1. **One capability, one concern.** No "utility" grab-bags. If two methods belong to different mental models, they belong to different capabilities. Merging them to save a line of import cost is the wrong trade.
2. **Compose via `Stream T` and `Result T E`.** No callbacks, no polling. Any long-running or reactive method returns a stream consumed by `@on source` (§14.3.1). Any fallible method returns `Result`.
3. **The smallest catalog that covers every real app.** The benchmark: can Launcher + Settings + Files + Task Manager + Bluesky (the five representative apps) be written end-to-end using only what Part II lists? If yes and the catalog cannot be shrunk without breaking one, the catalog is correct.
4. **Namespaces are always qualified.** No flat `fs`, `time`, `url`. Everything is `<group>.<name>` (`storage.fs`, `system.url`, `system.info`). Apps alias at `@use` time; call sites remain terse, but the domain structure is visible in imports.
5. **Immediate operations return values; watchable state returns streams.** `battery.state()` is a snapshot; `battery.watch()` is the same typed state over time. These are never mixed in one method.
6. **Apps own their own data model; the OS owns device state.** The OS does not provide OAuth, JSON schemas, database migrations, or any library-level concern. Those belong to userland.
7. **Declared, not derived.** Every capability is declared in a `.deck-os` manifest (§Part III). Apps never guess a method; they never ducktype. The loader verifies every `alias.method(…)` call against the manifest.

## 1 · The seven dimensions of a capability

Every capability, without exception, is fully specified by these seven:

| Dimension | What it says |
|---|---|
| **1 · Identity** | Dotted path `group.name` and conformance level (DL1 / DL2 / DL3). |
| **2 · State** | `@type <Cap>State` record enumerating the capability's observable state — or `unit` if the capability is stateless. |
| **3 · Methods** | Signatures and kinds (query / mutation / action-stream / watch-stream / handle). |
| **4 · Error domain** | `@errors <cap>` listing every atom the capability can return, including the two universals (§3 below). |
| **5 · Configuration** | The keys accepted by `@use cap as alias \n k: v`. Evaluated once at bind (§9.1 of the draft). |
| **6 · Grants** | Permission prompt timing (`:at_install`, `:on_first_use`, `:never`), rationale requirement, and any capability-specific grant parameters (e.g. `paths:` for fs, `allowed_hosts:` for http). |
| **7 · Events** (optional) | `os.<event>` names emitted, reserved for cross-cutting signals that do not belong to a single capability's state (e.g. `os.config_changed`). Per-capability state changes live in `watch()` methods, never in events. |

Nothing else belongs to a capability's definition. Behaviours not covered by one of the seven (random method side-effects, global mutable singletons, exceptions, magic instance variables) are forbidden.

## 2 · Method kinds

Every method falls into exactly one of five kinds. The loader can infer the kind from signature; apps can predict the kind from the name.

| Kind | Return type | Observable effect | Naming convention |
|---|---|---|---|
| **Query** | `T`, `T?`, or `Result T E` (direct) | None beyond reading time-varying state | Noun phrase: `theme.current()`, `battery.state()`, `fs.exists(path)` |
| **Mutation** | `Result unit E` or `Result T' E` | Changes device / app state | Verb or `set_` prefix: `theme.set(t)`, `wifi.connect(ssid, pwd)`, `device.set_brightness(v)` |
| **Action stream** | `Stream <EventT>` | Long-running, terminates on completion/failure | Verb, no `watch` suffix: `fs.copy(src, dst)`, `wifi.scan()`, `ota.download()` |
| **Watch stream** | `Stream T` | Reactive observation of state over time, infinite | Ends in `watch`: `theme.watch()`, `battery.watch()`, `wifi.status_watch()` |
| **Handle-producing** | `Result Handle E` | Opens a long-lived resource; paired with mutation methods on the Handle | Verb: `audio.play(ref)`, `fs.open_write(path)` |

**Discipline rules.**
- A method that changes observable state is **not** a query, regardless of return type.
- A query never causes a side effect the next query would observe.
- An action stream emits its final value as a variant with `:done` / `:failed` / similar; the stream then terminates.
- A watch stream never terminates on its own. It is cancelled only by subscription drop or capability loss.
- Handles are values; they cannot be stored in `@type` fields across suspend (per §22.4 they do not persist). Lifetime = current VM session.

## 3 · Error model

Every `@errors <cap>` domain **must** include the two universals:

```
:unavailable         "Capability is marked optional in @needs and this platform does not provide it"
:permission_denied   "User / platform denied this call (cap is present but not grantable)"
```

These cover the **capability-level** failure cases, orthogonal to the capability's specific errors.

`:unavailable` is returned by every call to a capability that was declared `optional` in `@needs.caps` and is not available on this platform. Apps use this to gracefully degrade.

`:permission_denied` is returned when the user (via `@grants` flow) or the OS (via policy) refuses a specific call. Some methods are per-call permission-gated (e.g. `notify.post` when user denied notifications); others are capability-wide (`wifi.connect` when the `network.wifi` grant was rejected).

Beyond these two, each capability lists only atoms that represent distinguishable failure modes the caller might handle differently. "Catch-all" atoms like `:error` are forbidden — failures that have no meaningful subtype become `panic :internal` in the runtime, not Result errors.

**No payloads on error atoms.** For the rare case of structured error context (HTTP status code, line number), lift it into the `@type` of the successful path (e.g. `Response.status`) or log it with `log.error`. Error atoms are branching tags only.

## 4 · Stream model

Two stream flavours, both values of `Stream T` consumed via `@on source` (§14.3.1 of the draft).

### 4.1 Action streams

Finite. Emit a sequence of progress / partial-result values, then a terminator. The event type is almost always a variant with explicit phases:

```
@type CopyProgress =
  | :progress (bytes_done: Size, bytes_total: Size)
  | :done
  | :failed   (reason: fs.Error)
```

Apps consume:

```
@on source fs.copy(src, dst)
  ->
    match event
      | :progress s  -> ui_progress(s.bytes_done, s.bytes_total)
      | :done        -> Downloads.send(:finished)
      | :failed (reason: e) -> log.warn("copy failed: {e}")
```

**Invariants.**
- Exactly one `:done` or `:failed` emission per action stream.
- After the terminator, the stream closes; no further emissions.
- Cancelling (subscription drop) before the terminator aborts the underlying operation.

### 4.2 Watch streams

Infinite. Emit the capability's current state on subscription, then subsequent values on every change (equality-distinct — duplicates are suppressed).

```
@on source battery.watch() as Battery
  ->
    if event.level < 0.1 then notify.toast("Low battery") else unit
```

**Invariants.**
- First emission is **synchronous** with subscription — the handler body runs once before any further events.
- Subsequent emissions occur only when the observable state changes (by structural equality of the emitted value).
- Watch streams never emit a terminator.
- Cancelling drops the subscription; no teardown effect is visible to other observers.

## 5 · Handle model

Some resources outlive a single method call: an audio playback, a file being written in chunks, an open database cursor. These are represented by **Handles** — opaque typed references.

```
@type AudioHandle = str                 -- opaque; shape is platform-private

audio.play   (src: AssetRef | str) -> Result AudioHandle audio.Error !
audio.pause  (h: AudioHandle)      -> Result unit audio.Error !
audio.resume (h: AudioHandle)      -> Result unit audio.Error !
audio.stop   (h: AudioHandle)      -> Result unit audio.Error !
audio.state  (h: AudioHandle)      -> Result AudioState audio.Error !
audio.watch  (h: AudioHandle)      -> Stream AudioState !
```

**Invariants.**
- A Handle is a plain value (usually a `str`); it may be stored in machine state payloads and passed as a fn arg.
- A Handle is **not persisted** across suspend (§22.4). On resume, all handles are invalid; apps must re-open.
- After the terminating method (`stop`, `close_write`, etc.) or after resource expiration, further calls on the Handle return `:err :invalid_handle`.
- A Handle value leaking across apps has no meaning — each app's Handle namespace is isolated.

Capabilities MUST expose an explicit close / stop method for every Handle they produce. The runtime does best-effort auto-cleanup on VM termination but apps should close proactively.

## 6 · Configuration model

A capability's alias-level configuration is declared as a record type in the `.deck-os` manifest; apps set values at `@use` time:

```
@use
  network.http as http
    base_url:    "https://api.example.com"
    timeout:     15s
    retry:       2
    user_agent:  "MyApp/1.0"
```

**Rules (from §9.1 of the draft).**
- Evaluated **once at bind time**. Runtime mutations to `config.*` do not re-apply to already-bound capabilities.
- Values are literals or pure expressions.
- Unknown keys / type mismatches → `LoadError :type` at app load.
- Two aliases of the same capability with different configs are **independent bindings**. They share no state.
- Capabilities with no configurable parameters simply take no config block — `@use storage.fs as fs` is complete.

If an app needs **runtime-mutable** capability parameters (e.g. rotating API tokens), the capability exposes a `cap.set_<param>(v) -> Result unit cap.Error` mutation method — it does **not** re-read `config.*` automatically.

## 7 · Grant model

Each capability defines its permission semantics:

```
@grants
  network.http:
    reason:         "Sync with the Bluesky API."
    prompt:         :at_install
    allowed_hosts:  ["bsky.social", "*.bsky.app"]
  display.notify:
    reason:         "Let you know when a reply arrives."
    prompt:         :on_first_use
```

**Standard grant fields** (documented per-capability in Part II):

| Field | Type | Purpose |
|---|---|---|
| `reason` | `str` | User-facing rationale. Required when `prompt` ≠ `:never`. |
| `prompt` | atom | `:at_install` / `:on_first_use` / `:never`. Default per capability. |
| `persist` | bool | Whether the grant persists across uninstall/reinstall (used by logging). |
| `<param>` | capability-specific | e.g. `paths` for `storage.fs`, `allowed_hosts` for `network.http`. |

**Denied-grant behaviour** is documented per-capability. Two models:
- **Gracefully degrading**: every method returns `:err :permission_denied` and the app continues. Preferred.
- **Capability unavailable**: the alias binds to `:unavailable` status; every call returns `:err :unavailable`. Used when partial functionality would be misleading (e.g. denied `network.wifi` makes `wifi.scan` useless).

Which model applies is declared in the capability's manifest.

## 8 · OS-event vs watch-stream

Both channels exist because they serve different purposes:

| Mechanism | Use for | Shape |
|---|---|---|
| `<cap>.watch()` / `<cap>.<state>_watch()` | **State of a specific capability** — observe `battery.level`, `theme.current`, `wifi.status`. | `Stream <T>` |
| `@on os.<event>` | **Cross-cutting system signals** that do not belong to any single capability's observable state — `os.config_changed`, `os.app_crashed`, `os.low_memory`. | `@on` with named / pattern binders |

If a value can be expressed as "the current state of capability X", it uses `watch`. If it's a signal independent of any capability's state ("something-happened"), it's an `os.*` event.

In Part II, no capability declares a `os.*` event for its own state — that would be a double-exposure. Every state transition is observable through the capability's watch.

---

# Part II — v1 Catalog

## 9 · Summary

| # | Capability | Level | Purpose |
|---|---|---|---|
| 1 | `storage.fs` | DL1 | File I/O (blobs, data files, logs) |
| 2 | `storage.cache` | DL2 | Volatile TTL-backed KV |
| 3 | `network.http` | DL2 | HTTP/HTTPS client |
| 4 | `network.wifi` | DL2 | Wi-Fi link management |
| 5 | `network.bluetooth` | DL3 | BLE GATT |
| 6 | `system.info` | DL1 | Device / app / runtime metadata |
| 7 | `system.apps` | DL2 | App lifecycle + registry + crashes + notif counts |
| 8 | `system.security` | DL2 | PIN + permissions |
| 9 | `system.ota` | DL2 | Firmware update |
| 10 | `system.device` | DL2 | Brightness, rotation, lock, sleep, timeout |
| 11 | `system.battery` | DL2 | Battery state |
| 12 | `system.time` | DL1 | Wall-clock setting (reading is a builtin) |
| 13 | `system.tasks` | DL3 | Per-process CPU / memory |
| 14 | `system.locale` | DL3 | Language / region |
| 15 | `system.url` | DL1 | Deep-link dispatch |
| 16 | `display.theme` | DL2 | OS theme |
| 17 | `display.notify` | DL1 | Notifications + toasts |
| 18 | `media.audio` | DL3 | Audio playback + recording + volume |
| 19 | `sensors.*` | DL3 | Platform-defined physical sensors |

Total: **19 capabilities** (one fewer than the earlier draft, because `storage.nvs` was subsumed by `@config` with map-valued fields; dynamic-keyed persistence now uses `@config key: {str: V} = {}`).

**DL1 minimum viable**: 6 (fs, info, time, notify, url + one of the system caps needed for launch).
**DL2 reference (CyberDeck target)**: 16.
**DL3 extensions**: 5.

Time reading (`time.now`, `time.monotonic`, `time.ago`, `time.format`, etc.) is a **builtin module**, not a capability — no grant is needed to read a clock. Only wall-clock **setting** is capability-gated (`system.time.set`).

Likewise, `log.*`, `math.*`, `text.*`, `list.*`, `map.*`, `stream.*`, `bytes.*`, `record.*`, `type_of` are all builtins. See `DECK-3.0-BUILTINS.md`.

---

## 10 · `storage.fs`

File-level persistent storage. Path semantics are platform-determined (CyberDeck: `/sd/...` for SD card, `/app/...` for internal). Apps write to their sandbox root by default; `@grants.storage.fs.paths: [...]` opens additional paths.

**State:** stateless (each method is a standalone operation).

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

-- Action stream (cancellable; for files big enough to warrant progress)
fs.copy (from: str, to: str) -> Stream CopyProgress !

-- Handle (for chunked writes that don't fit in memory)
fs.open_write  (path: str)                  -> Result FsHandle fs.Error !
fs.write_chunk (h: FsHandle, data: Bytes)   -> Result unit fs.Error !
fs.close_write (h: FsHandle)                -> Result unit fs.Error !
```

**Types:**

```
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
```

**Errors:**

```
@errors fs
  :unavailable
  :permission_denied
  :not_found
  :exists             "Target already exists (mkdir, rename)"
  :not_empty          "rmdir called on non-empty directory"
  :read_only
  :disk_full
  :invalid_path
  :invalid_handle     "FsHandle not from a live open_write(), or already closed"
  :io
```

**Config:** none.

**Grants:** `:at_install`. `paths: [str]?` declares writable paths outside the sandbox root.

**Denied-grant model:** gracefully degrading — each method returns `:err :permission_denied`.

**Notes:**
- `fs.copy` is a stream because large copies take seconds.
- `fs.list` returns a list (not a stream) — directory listings are expected to fit in memory. Platform cap in `system.info.versions().max_dir_entries`.

---

## 11 · `storage.cache`

Volatile TTL-backed key-value. Backed by RAM; survives only within the current VM session.

**State:** stateless (values are keyed by string).

**Methods:**

```
cache.get        (key: str)                          -> str? !
cache.get_bytes  (key: str)                          -> Bytes? !
cache.set        (key: str, v: str,   ttl: Duration) -> Result unit cache.Error !
cache.set_bytes  (key: str, v: Bytes, ttl: Duration) -> Result unit cache.Error !
cache.delete     (key: str)                          -> unit !
cache.clear      ()                                  -> unit !
cache.ttl        (key: str)                          -> Duration? !       -- remaining TTL, :none if missing
```

**Errors:**

```
@errors cache
  :unavailable
  :permission_denied
  :invalid_key
  :value_too_big
  :cache_full
```

**Config:** `default_ttl: Duration = 5m` (applied when `set` is called without an explicit ttl — but `set` signature requires `ttl`, so this is currently unused; reserved for future).

**Grants:** `:never` (no user concern; cache is local and non-persistent).

**Notes:**
- `cache.get` returns `T?` because missing keys are the routine case (§1.3 of the draft, §3 of this catalog).
- Expired keys are lazily evicted: next access returns `:none`.
- Cross-app cache isolation: each app sees only its own namespace.

---

## 12 · `network.http`

HTTP / HTTPS client. Used by every app speaking REST / JSON / GraphQL.

**State:** stateless per-call (aliases are configured, not session-stateful).

**Methods:**

```
http.get    (path: str, opts: ReqOpts?)                -> Result Response http.Error !
http.post   (path: str, body: Body, opts: ReqOpts?)    -> Result Response http.Error !
http.put    (path: str, body: Body, opts: ReqOpts?)    -> Result Response http.Error !
http.patch  (path: str, body: Body, opts: ReqOpts?)    -> Result Response http.Error !
http.delete (path: str, opts: ReqOpts?)                -> Result Response http.Error !

-- Action stream for large downloads
http.stream (method: atom, path: str, body: Body?, opts: ReqOpts?) -> Stream HttpChunk !
```

**Types:**

```
@type ReqOpts
  query   : {str: str}?
  headers : {str: str}?
  timeout : Duration?             -- overrides alias-level timeout
  retry   : int?                  -- overrides alias-level retry
  ca      : AssetRef?             -- pinned TLS cert
  accept  : str?                  -- Accept-header shortcut

@type Body =
  | :str       str
  | :bytes     Bytes
  | :json      any                -- serialised by the runtime (records → tagged maps)
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
  body       : str?               -- :none if body was binary; use http.stream for blobs
  body_bytes : Bytes?

@type HttpChunk =
  | :chunk  Bytes
  | :done   (status: int, headers: {str: str})
  | :failed http.Error
```

**Errors:**

```
@errors http
  :unavailable
  :permission_denied
  :timeout
  :dns
  :refused
  :tls
  :status_3xx                     -- unhandled redirect
  :status_4xx
  :status_5xx
  :body_too_large                 -- only for non-stream methods
  :malformed
  :offline
  :aborted                        -- subscription dropped mid-stream
```

**Config:**

```
base_url    : str?       = :none
timeout     : Duration   = 30s
retry       : int        = 0
user_agent  : str        = "deck-runtime/1.0"
```

**Grants:** `:at_install` with `allowed_hosts: [str]`. Denied-grant: capability unavailable (every call → `:err :permission_denied`).

---

## 13 · `network.wifi`

Wi-Fi link management.

**State:** `WifiStatus` (see below).

**Methods:**

```
-- Queries / watches
wifi.status        () -> WifiStatus !
wifi.status_watch  () -> Stream WifiStatus !

-- Mutations
wifi.connect       (ssid: str, password: str?) -> Result unit wifi.Error !
wifi.disconnect    () -> Result unit wifi.Error !
wifi.forget        (ssid: str) -> Result unit wifi.Error !
wifi.set_enabled   (on: bool)  -> Result unit wifi.Error !

-- Action stream
wifi.scan () -> Stream ScanEvent !
```

**Types:**

```
@type WifiStatus =
  | :off
  | :disconnected
  | :connecting
  | :connected (ssid: str, ip: str, channel: int, rssi: int)
  | :failed    (reason: wifi.Error)

@type WifiNetwork
  ssid     : str
  rssi     : int              -- dBm
  security : atom             -- :open :wpa :wpa2 :wpa3
  channel  : int

@type ScanEvent =
  | :networks [WifiNetwork]
  | :done
  | :failed   wifi.Error
```

**Errors:**

```
@errors wifi
  :unavailable
  :permission_denied
  :no_network
  :bad_password
  :timeout
  :hardware_off
  :already_connecting
```

**Grants:** `:at_install`. Read (`status`, `status_watch`) is `:never`; mutations require the grant.

**Notes:** since WiFi connection state is exposed through `status_watch()`, no `os.wifi_changed` event exists (per §8).

---

## 14 · `network.bluetooth` (DL3)

BLE GATT. Classic BT is not supported on ESP32-S3.

**State:** connection roster (list of `BleHandle`).

**Methods:**

```
ble.scan         (opts: ScanOpts?) -> Stream BleScanEvent !
ble.connect      (addr: str)                    -> Result BleHandle ble.Error !
ble.disconnect   (h: BleHandle)                 -> Result unit ble.Error !
ble.services     (h: BleHandle)                 -> Result [BleService] ble.Error !
ble.read_char    (h: BleHandle, service: str, char: str) -> Result Bytes ble.Error !
ble.write_char   (h: BleHandle, service: str, char: str, value: Bytes) -> Result unit ble.Error !
ble.subscribe    (h: BleHandle, service: str, char: str) -> Stream BleNotification !
```

**Types:**

```
@type ScanOpts
  filter_services : [str]?
  duration        : Duration?

@type BleHandle = str

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
```

**Errors:**

```
@errors ble
  :unavailable
  :permission_denied
  :not_found
  :connection_failed
  :auth_failed
  :timeout
  :hardware_off
  :invalid_handle
```

**Grants:** `:at_install`.

---

## 15 · `system.info`

Read-only metadata.

**State:** mostly constant per VM session; heap/cpu/uptime are time-varying.

**Methods:**

```
-- Constants (no `!`; resolved at first call, cached)
info.device_id       () -> str
info.model           () -> str
info.os_name         () -> str
info.os_version      () -> str
info.runtime_version () -> str
info.deck_level      () -> int
info.edition         () -> int
info.app_id          () -> str
info.app_version     () -> str
info.versions        () -> Versions

-- Time-varying (`!`)
info.uptime          () -> Duration !
info.free_heap       () -> Size !
info.used_heap       () -> Size !
info.cpu_freq        () -> int  !

-- Watch
info.heap_watch      () -> Stream HeapSnapshot !
```

**Types:**

```
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
  free_internal        : Size
  free_psram           : Size
  free_total           : Size
  largest_free_block   : Size
  fragmentation_ratio  : float
```

**Errors:** none beyond the universals; all methods are infallible reads.

```
@errors info
  :unavailable
  :permission_denied
```

**Config:** none.
**Grants:** `:never`.

---

## 16 · `system.apps`

App lifecycle, registry, crash history, notification-count aggregation. Used by system.* apps (Launcher, Settings, Task Manager).

**State:** `{installed: [AppInfo], running: [AppInfo], notif_counts: {str: int}}` — surfaced separately per watch.

**Methods:**

```
-- Queries
apps.info         (id: str) -> AppInfo?  !
apps.load_error   (id: str) -> LoadError? !
apps.crashes      (id: str) -> [CrashEntry] !
apps.crashes_since(ts: Timestamp) -> [CrashEntry] !

-- Watches
apps.installed_watch    () -> Stream [AppInfo] !
apps.running_watch      () -> Stream [AppInfo] !
apps.notif_counts_watch () -> Stream {str: int} !

-- Mutations
apps.launch  (id: str, data: any?) -> Result unit apps.Error !
apps.kill    (id: str)             -> Result unit apps.Error !
```

**Types:**

```
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
  kind          : atom            -- :bug, :limit, :internal
  message       : str
  trace_summary : str
  ts            : Timestamp
```

**Errors:**

```
@errors apps
  :unavailable
  :permission_denied
  :unknown_app
  :not_running          "kill() on a non-running app"
  :launch_failed        "Load / spawn failed — see load_error(id) for structured reason"
```

**Config:** none.
**Grants:** `:at_install`. System-only.

---

## 17 · `system.security`

PIN + permissions state. Used by Settings (permission UI) and the lockscreen flow.

**State:** `{pin_set: bool, permissions: {str: {str: PermissionState}}}`.

**Methods:**

```
-- Queries / watches
sec.pin_set           () -> bool !
sec.permission_state  (app_id: str, cap: str) -> PermissionState !
sec.grants_for        (app_id: str) -> {str: PermissionState} !
sec.grants_watch      (app_id: str) -> Stream {str: PermissionState} !

-- Mutations
sec.set_pin          (pin: str)     -> Result unit sec.Error !
sec.clear_pin        ()             -> Result unit sec.Error !
sec.verify_pin       (pin: str)     -> Result bool sec.Error !
sec.permission_set   (app_id: str, cap: str, state: PermissionState) -> Result unit sec.Error !
```

**Types:**

```
@type PermissionState =
  | :granted
  | :denied
  | :not_decided
```

**Errors:**

```
@errors sec
  :unavailable
  :permission_denied
  :invalid_pin          "PIN must be 4-8 digits"
  :unknown_app
  :unknown_cap
```

**Config:** none.
**Grants:** `:at_install`. System-only.

---

## 18 · `system.ota`

Firmware over-the-air update.

**State:** `OtaState`.

**Methods:**

```
-- Queries / watches
ota.state       () -> OtaState !
ota.state_watch () -> Stream OtaState !

-- Action streams
ota.check       () -> Stream CheckEvent !
ota.download    () -> Stream DownloadEvent !

-- Mutations
ota.apply       () -> Result unit ota.Error !    -- triggers reboot on success
ota.cancel      () -> Result unit ota.Error !    -- cancels in-flight download
```

**Types:**

```
@type OtaState =
  | :idle
  | :checking
  | :update_available (info: UpdateInfo)
  | :downloading     (progress: float)      -- 0.0..1.0
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
```

**Errors:**

```
@errors ota
  :unavailable
  :permission_denied
  :network
  :checksum
  :storage_full
  :busy                 "download() while another is in flight"
  :apply_failed
```

**Config:** `channel: str = "stable"`.
**Grants:** `:at_install`. System-only.

---

## 19 · `system.device`

Brightness, rotation, lock, sleep, screen timeout. Mutable device state user-adjustable from Settings.

**State:** `DeviceState`.

**Methods:**

```
-- Queries / watches
device.state          () -> DeviceState !
device.watch          () -> Stream DeviceState !

-- Individual-field accessors (convenience; same data as state().*)
device.brightness     () -> float !
device.rotation       () -> atom  !
device.locked         () -> bool  !
device.screen_timeout () -> Duration !

-- Mutations
device.set_brightness     (level: float)  -> Result unit device.Error !
device.set_rotation       (r: atom)       -> Result unit device.Error !
device.set_screen_timeout (d: Duration)   -> Result unit device.Error !
device.lock               ()              -> Result unit device.Error !
device.sleep              ()              -> Result unit device.Error !
device.wake               ()              -> Result unit device.Error !
```

**Types:**

```
@type DeviceState
  brightness     : float                 -- 0.0..1.0
  rotation       : atom                  -- :portrait :landscape
  locked         : bool
  sleeping       : bool
  screen_timeout : Duration
```

**Errors:**

```
@errors device
  :unavailable
  :permission_denied
  :invalid_value
```

**Config:** none.
**Grants:** Read is `:never`; mutations `:at_install` (system-only).

---

## 20 · `system.battery`

Battery level + charging state. Optional per-platform; battery-less boards declare absence.

**State:** `BatteryState`.

**Methods:**

```
battery.state () -> BatteryState !
battery.watch () -> Stream BatteryState !
```

**Types:**

```
@type BatteryState
  level     : float              -- 0.0..1.0
  charging  : bool
  voltage   : float              -- volts
  remaining : Duration?          -- :none if unknown
```

**Errors:**

```
@errors battery
  :unavailable
  :permission_denied
```

**Config:** none.
**Grants:** `:never`.

**Notes:** on battery-less platforms, `@needs.caps.system.battery: optional` makes every call return `:err :unavailable`.

---

## 21 · `system.time`

Wall-clock **setting** only. Clock **reading** is a builtin (`time.*`) with no grant requirement.

**Methods:**

```
system.time.set (ts: Timestamp) -> Result unit system.time.Error !
```

**Errors:**

```
@errors system.time
  :unavailable
  :permission_denied
  :out_of_range          "Timestamp exceeds representable wall-clock range"
```

**Config:** none.
**Grants:** `:at_install`. System-only (Settings sets the clock after NTP sync).

---

## 22 · `system.tasks` (DL3)

Per-process resource monitoring. Used by Task Manager.

**State:** snapshot of running processes.

**Methods:**

```
tasks.processes_watch () -> Stream [ProcessEntry] !
```

**Types:**

```
@type ProcessEntry
  app_id      : str
  pid         : int
  cpu_percent : float
  heap_used   : Size
  uptime      : Duration
```

**Errors:**

```
@errors tasks
  :unavailable
  :permission_denied
```

**Config:** `sample_interval: Duration = 1s`.
**Grants:** `:at_install`. System-only.

---

## 23 · `system.locale` (DL3)

Language / region setting.

**State:** `locale` (str, e.g. `"en-US"`).

**Methods:**

```
locale.current   () -> str   !
locale.available () -> [str] !
locale.watch     () -> Stream str !

locale.set       (code: str) -> Result unit locale.Error !
```

**Errors:**

```
@errors locale
  :unavailable
  :permission_denied
  :unknown_locale
```

**Config:** none.
**Grants:** Read `:never`; `set` `:at_install` (system-only).

---

## 24 · `system.url`

Deep-link dispatch (§19 of the draft). Resolved via `@handles` pattern matching; `http`/`https` URLs fall back to a built-in browser if one is installed.

**Methods:**

```
url.open     (url: str) -> Result unit url.Error !
url.can_open (url: str) -> bool !
```

**Errors:**

```
@errors url
  :unavailable
  :permission_denied
  :no_handler
  :malformed
```

**Config:** none.
**Grants:** `:at_install`.

---

## 25 · `display.theme`

OS-wide theme selection. Settings mutates; bridges re-render reactively.

**State:** current `Theme`.

**Methods:**

```
theme.current () -> Theme !
theme.all     () -> [Theme] !
theme.watch   () -> Stream Theme !

theme.set     (id: atom) -> Result unit theme.Error !
```

**Types:**

```
@type Theme
  id       : atom              -- e.g. :matrix :amber :neon
  name     : str
  is_dark  : bool
  accent   : str               -- 7-char hex #RRGGBB
```

**Errors:**

```
@errors theme
  :unavailable
  :permission_denied
  :unknown_theme
```

**Config:** none.
**Grants:** Read `:never`; `set` `:at_install` (system-only).

---

## 26 · `display.notify`

Notifications + toasts. Every app can surface messages.

**State:** per-app badge count + list of live notifications.

**Methods:**

```
-- Queries
notify.badge () -> int !

-- Mutations
notify.toast     (message: str, duration: Duration?) -> Result unit notify.Error !
notify.post      (opts: NotifyOpts) -> Result NotificationId notify.Error !
notify.dismiss   (id: NotificationId) -> Result unit notify.Error !
notify.clear_all () -> Result unit notify.Error !
notify.set_badge (count: int) -> Result unit notify.Error !
```

**Types:**

```
@type NotifyOpts
  title    : str
  body     : str?
  priority : atom?             -- :low :normal :high; default :normal
  deep_url : str?              -- tap → open_url delivered to this app
  icon     : AssetRef?

@type NotificationId = str
```

**Errors:**

```
@errors notify
  :unavailable
  :permission_denied
  :rate_limited
  :unknown_id
```

**Config:** none.
**Grants:** `:on_first_use`. Denied-grant: gracefully degrades (every `post`/`toast` returns `:err :permission_denied`).

---

## 27 · `media.audio` (DL3)

Audio playback + recording + volume.

**State:** `{volume: float, muted: bool, active_handles: [AudioHandle]}`.

**Methods:**

```
-- Queries / watches
audio.volume     () -> float !
audio.muted      () -> bool !
audio.watch      () -> Stream AudioMixerState !

-- Volume mutations
audio.set_volume (v: float) -> Result unit audio.Error !
audio.set_muted  (m: bool)  -> Result unit audio.Error !

-- Playback (handle-producing)
audio.play       (src: AssetRef | str)            -> Result AudioHandle audio.Error !
audio.pause      (h: AudioHandle)                 -> Result unit audio.Error !
audio.resume     (h: AudioHandle)                 -> Result unit audio.Error !
audio.stop       (h: AudioHandle)                 -> Result unit audio.Error !
audio.seek       (h: AudioHandle, pos: Duration)  -> Result unit audio.Error !
audio.state      (h: AudioHandle)                 -> Result AudioState audio.Error !
audio.state_watch(h: AudioHandle)                 -> Stream AudioState !

-- Recording (action stream)
audio.record       (opts: RecordOpts)              -> Stream Bytes !
audio.record_to_file (opts: RecordOpts, path: str) -> Stream RecordEvent !
```

**Types:**

```
@type AudioMixerState
  volume : float
  muted  : bool

@type AudioHandle = str

@type AudioState
  position : Duration
  duration : Duration?
  status   : atom              -- :playing :paused :stopped :ended :error

@type RecordOpts
  sample_rate  : int           -- e.g. 16000
  channels     : int           -- 1 or 2
  format       : atom          -- :pcm_s16le :wav
  max_duration : Duration?

@type RecordEvent =
  | :progress (bytes_written: Size, elapsed: Duration)
  | :done
  | :failed   audio.Error
```

**Errors:**

```
@errors audio
  :unavailable
  :permission_denied
  :unsupported_format
  :hardware_unavailable
  :invalid_handle
  :invalid_value
```

**Config:** none.
**Grants:** playback `:on_first_use`; recording `:at_install`.

---

## 28 · `sensors.*` (DL3, platform-defined)

Physical-world sensors. **Every concrete sensor is its own capability** (one sensor, one concern, per §0.1). A platform declares which it supports in its `.deck-os` bundle; apps gate with `optional` when a sensor may or may not be present.

### 28.1 Common sensor shape

Every `sensors.<name>` capability has **exactly this shape**, parametrised by the sample type:

```
<cap>.now   ()      -> Result Sample <cap>.Error !
<cap>.watch (opts: SampleOpts?) -> Stream Sample !
```

Where `Sample` is capability-specific (defined per sensor below) and `SampleOpts` is a named tuple declaring sample rate / decimation, with defaults.

```
@type SampleOpts
  rate_hz : int?               -- samples per second; :none → platform default
```

Every `<cap>.Error` domain has exactly `:unavailable`, `:permission_denied`, `:hardware_off`.

This uniformity is the meta-spec's strongest claim about sensors: they all look the same from the app's perspective. A platform adding a new sensor only declares its `Sample` shape and its `<cap>.Error` atoms — the method list and grant model are fixed.

### 28.2 Standard sensor types (optional; reference platform may ship some subset)

```
sensors.imu         : Sample = (accel: (x,y,z), gyro: (x,y,z), ts: Timestamp)
sensors.environment : Sample = (temperature_c: float, humidity_pct: float?, pressure_hpa: float?, ts: Timestamp)
sensors.light       : Sample = (lux: float, ts: Timestamp)
sensors.magnetometer: Sample = (x: float, y: float, z: float, ts: Timestamp)
sensors.gps         : Sample = (lat: float, lng: float, alt_m: float?, accuracy_m: float, ts: Timestamp)
```

CyberDeck reference platform: none shipped by default.

---

# Part III — Extension contract

## 29 · Platform `.deck-os` manifest

Every capability the platform provides is declared in a manifest file named `<group>.<name>.deck-os`, located in the platform's component bundle. The loader aggregates all manifests at app load and verifies every `@use cap as alias` against the aggregate.

### 29.1 Manifest shape

```
@capability storage.fs               -- identity
  level:     1
  stateful:  false

@config_schema                       -- what `@use storage.fs as fs \n k: v` accepts
  paths: [str]? = :none

@errors fs                           -- the error domain (reuses @errors syntax)
  :unavailable
  :permission_denied
  :not_found
  :exists
  ...

@methods                             -- signatures as they appear in app code
  exists   (path: str) -> bool !
  read     (path: str) -> Result str fs.Error !
  ...

@grants_schema                       -- fields accepted under @grants.storage.fs
  reason: str
  prompt: atom = :at_install
  paths:  [str]?

@events                              -- os.* events this capability emits (usually none)
  (none)
```

### 29.2 Loader verification

At app load, the loader:

1. Reads every `.deck-os` manifest the platform provides.
2. For each `@use cap as alias …` in the app:
   - Matches `cap` against a manifest; missing → `LoadError :unresolved` (or `:err :unavailable` at call time if the `@needs` entry was `optional`).
   - Validates the config block against `@config_schema`; unknown / mistyped keys → `LoadError :type`.
3. For each `alias.method(…)` call:
   - Locates the method in `@methods`; missing → `LoadError :unresolved`.
   - Verifies argument arity and types against the signature.
   - Verifies named-arg names exist.
4. For each `@grants.cap` entry:
   - Validates against `@grants_schema`; missing required `reason` → `LoadError :permission`.

### 29.3 Adding a new capability

A native extension bundled with the platform may add capabilities beyond this catalog:

1. Author a `.deck-os` manifest following §29.1.
2. Implement the C/Rust side of each method (runtime-platform contract, outside this spec — see `12-deck-service-drivers.md` follow-up).
3. Register the manifest with the platform bundle.

The new capability must follow every rule of Part I — the same naming conventions, the same error-domain universals, the same stream model. The meta-spec is enforced; custom capabilities that diverge break the "one-mental-model-for-every-capability" contract and will surface as usability bugs even if the loader accepts them.

### 29.4 Conformance tests

Every capability declared in a platform manifest must pass:

- **Method coverage** — every method invokable with documented signatures; results typed correctly; `Result` variants returned per domain.
- **Watch stream correctness** — first emission ≤ 100 ms after subscription; emissions only on structural change; no emissions after 5 s of idle state.
- **Action stream correctness** — exactly one `:done` / `:failed`; no emissions after terminator; cancellation ≤ 50 ms after subscription drop.
- **Handle lifecycle** — every Handle expires after its explicit close method; post-close calls return `:err :invalid_handle`.
- **Grant behaviour** — denied-grant returns `:err :permission_denied` or binds the alias as `:unavailable`, as declared.
- **Error-domain universality** — every error domain emits `:unavailable` (when the cap is optional and absent) and `:permission_denied` (when a grant is missing); other atoms only when the distinguishing condition occurs.

Platforms run the conformance suite before publishing a release.

---

# Part IV — Deferred / out of scope for v1

- **`storage.db` (SQLite / embedded DB)** — removed; every proposed use case fits `storage.fs` + `@config` (with map-valued fields) + userland iteration. Revisit when an app needs relational queries over thousands of rows on-device.
- **`network.mqtt`** — zero annex usage; revisit when a real app demands pub/sub.
- **`crypto.aes` / `crypto.hash`** — transport security is covered by `network.http` TLS. At-rest encryption is out of scope until a real app needs it.
- **`camera`** — not supported on CyberDeck hardware; revisit per-platform.
- **`input` (USB HID keyboard / mouse)** — consumed by the platform layer, not apps.
- **`system.power`** — split from `system.device` when sleep-mode granularity demands it (not yet).
- **`@component` — extension point for content primitives beyond §15 of the draft.** Same meta-spec-dimensions discipline, applied to content nodes. Open.
- **`ipc` beyond `@service`** — `@service` (§18 of the draft) + `system.url` cover all observed IPC patterns.

---

**End of capability catalog draft. Promote, amend, or discard as a whole.**
