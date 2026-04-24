# Deck — Capabilities (Consumer Protocol)

Companion to `LANG.md` (language), `SERVICES.md` (foundation + catalog), `BUILTINS.md` (in-VM modules), `BRIDGE.md` (UI bridge).

**Edition:** 2027.

## What this document is

A **capability** is the consumer-side handle for a service. This document defines the protocol an app uses to import, configure, call, and handle errors from any service — system or app-provided, native or Deck-implemented. It does not enumerate services; for the catalog, see `SERVICES.md`.

The two documents are deliberately split:

- **SERVICES.md** describes what *exists* (singleton processes, registered IDs, lifecycle, error domains, the full catalog).
- **CAPABILITIES.md** describes how an app *consumes* what exists (`@use`, `@grants`, `@needs`, configuration, alias semantics, error idioms, optionality, best practices).

The split mirrors a clean provider/consumer separation: an author writing a new service reads SERVICES; an author writing an app that uses services reads CAPABILITIES.

## What a capability is NOT

- Not a separate kind of OS surface. Every capability binds to exactly one service.
- Not a runtime entity in its own right. The runtime knows about services; capabilities are a parser-and-loader concern that produces typed bindings.
- Not the same as a builtin. Builtins are always-available, in-VM, no `@use`. Capabilities require `@use` and may require grants.

---

## Part I — The consumer view

### 0 · Philosophy

Capabilities are pure consumer ergonomics. The rules below exist so that:

1. **Every cross-boundary call is visible at the file's top.** Reading the `@use` block tells you what the file touches outside its own VM.
2. **Versioned dependencies are declared at install.** The loader rejects an app that requires services the platform cannot satisfy. Failures happen before the app runs, not mid-flight.
3. **Permission rationale is co-located with the dependency.** `@grants.services.<alias>` lives next to the `@use` it justifies; review tools can audit one without reading the entire app.
4. **The call site looks the same regardless of provider.** `bsky.fetch_latest()` reads the same whether `bsky` aliases a native HTTP-backed system service, an app-provided IPC, or a stub during local testing. Consumer code does not encode implementation details.

### 1 · Importing services

A `@use` block declares every service the app consumes:

```
@use
  storage.fs        as fs
  storage.cache     as cache
  network.http      as http
    base_url:    "https://api.example.com"
    timeout:     15s
  api.client        as bsky
    base_url:    "https://bsky.social/xrpc"
    auth:        (:bearer, config.bsky_token)
  service "social.bsky.app/feed" as feed_svc

  ./utils/format
  ./api/auth
```

**Two import forms:**

| Form | When to use | Example |
|---|---|---|
| `<service-id> as <alias>` | System services (no slash in ID) | `network.http as http` |
| `service "<service-id>" as <alias>` | App-provided services (slash in ID) | `service "social.bsky.app/feed" as feed_svc` |

The `service` keyword is required only when the ID contains a `/` (which app-provided IDs always do). System service IDs never have a slash; the `service` keyword on them is rejected (`LoadError :parse`).

**Local modules** (no service): a relative path imports a Deck source file. Module name = last path segment. Local modules are not services.

### 2 · Aliases

Every service binding requires `as <alias>`. The alias becomes the value-namespace prefix at call sites:

```
@use storage.fs as fs

let exists = fs.exists("/data/foo.txt")
let body   = fs.read("/data/foo.txt")?
```

Alias names are arbitrary identifiers (`[a-z][a-zA-Z0-9_]*`). Convention: short, name-the-domain (`fs` not `filesystem`, `http` not `http_client`).

**Multiple aliases of the same service** are independent bindings:

```
@use
  network.http as gh
    base_url: "https://api.github.com"
  network.http as cf
    base_url: "https://api.cloudflare.com"
```

The two aliases share no state. Each maintains its own configuration, its own per-call retry counters, its own auth tokens (if any). This is the standard pattern for talking to multiple endpoints with the same protocol.

### 3 · Configuration

A capability alias may carry configuration lines indented under it:

```
@use
  network.http as http
    base_url:   "https://api.example.com"
    timeout:    15s
    retry:      2
    user_agent: "MyApp/1.0"
```

**Rules:**

- Optional. Capabilities with no configurable parameters take no config block (`@use storage.fs as fs` is complete).
- Each key matches the service's `@config-schema` declared in its `.deck-os` manifest (or `@service` block for app-provided). Unknown keys / mistyped values → `LoadError :type`.
- Values are literals or pure expressions. **Evaluated once at bind time.** Runtime mutations to `config.*` (the app's `@config` keys) do **not** re-apply to already-bound services.
- Two aliases of the same service with different configs are independent (§2 above).

**Runtime-mutable parameters** (auth tokens that rotate, base URLs that change per environment): the service must expose a mutation method (e.g. `api.set_token(t)`). The runtime does not re-bind services on `config.set` change — apps explicitly invoke the mutation method when needed.

```
-- token rotation pattern
@on every 50m
  let t = oauth.refresh("bsky")?
  bsky.set_token(t.access_token)
```

### 4 · Calling

Every method call uses the alias prefix:

```
let resp = http.get("/users/me")?
```

**Argument styles:**

| Service kind | Allowed arg style |
|---|---|
| System service | Positional or named, as declared in the service's `@methods` schema |
| App-provided service | Positional or named, as declared in the `@service`'s `on :method` signatures |

Mixing positional and named in a single call is forbidden (per `DRAFT.md` §4.1).

**Examples:**

```
http.get("/users/me", { timeout: 5s })                       -- positional path, named opts
http.post(path: "/posts", body: :json data, opts: opts)      -- all named
fs.read("/sd/data.txt")                                      -- positional
```

### 5 · Method-kind ergonomics

Per `SERVICES.md` §5, every method falls into one of five kinds. Consumer-side patterns:

#### 5.1 Query

Synchronous; pattern-match the result:

```
match fs.read("/data/notes.txt")
  | :ok content -> render(content)
  | :err :not_found -> show_empty_state()
  | :err e          -> log.warn("read failed: {e}")
```

Or unwrap with `?` in an `!` body:

```
let content = fs.read("/data/notes.txt")?
```

#### 5.2 Mutation

Same pattern as queries; usually `Result unit E`:

```
fs.write("/data/notes.txt", content)?
```

#### 5.3 Action stream

Consumed via `@on source`:

```
@on source fs.copy("/sd/big.bin", "/spiffs/big.bin")
  ->
    match event
      | :progress p   -> ui_progress(p.bytes_done, p.bytes_total)
      | :done         -> Saved.send(:complete)
      | :failed (reason: e) -> log.warn("copy failed: {e}")
```

#### 5.4 Watch stream

Consumed via `@on source`. Optionally named:

```
@on source battery.watch() as Battery
  ->
    if event.level < 0.1 then notify.toast("Low battery") else unit
```

Named sources expose `Battery.last() / .recent(n) / .count()` accessors elsewhere in the app (per `DRAFT.md` §14.4).

#### 5.5 Handle-producing

The handle threads through subsequent calls; explicit close required:

```
let h = audio.play(asset_ref)?
audio.pause(h)?
audio.stop(h)?
```

Handles do not persist across suspend (§22.4 of `DRAFT.md`). Apps re-open after resume.

### 6 · Error handling

Every service call returns `Result T E`, `T?`, or `Stream T`. Idiomatic patterns:

#### 6.1 Propagate within a single error domain

If the enclosing fn / `@on` body shares the same error domain, use `?`:

```
fn refresh () -> Result unit fs.Error ! =
  let body = fs.read("/data/in.txt")?
  fs.write("/data/out.txt", body)?
```

#### 6.2 Bridge across error domains with `result.map_err`

Different services have different error domains. Bridge with the builtin:

```
fn fetch_and_save (url: str) -> Result unit app.Error ! =
  let body = http.get(url) |> result.map_err(_ -> :upstream)?
  fs.write("/data/cache.bin", body) |> result.map_err(_ -> :storage)?
```

The `?` operator requires identical error domains (per `DRAFT.md` §1.10). `result.map_err` is the only sound bridge.

#### 6.3 Match in place

When the caller wants different behaviour per error atom:

```
match wifi.connect(ssid, password)
  | :ok _                  -> Wifi.send(:connected)
  | :err :bad_password     -> show_password_error()
  | :err :no_network       -> show_no_network_error()
  | :err :timeout          -> retry_with_backoff()
  | :err _                 -> log.warn("wifi connect failed")
```

#### 6.4 Distinguish service-level from logic-level failures

The four universal error atoms (`:unavailable`, `:permission_denied`, `:service_unavailable`, `:timeout`) are *infrastructure* failures, often handled the same way regardless of which service produced them. Consider centralising:

```
fn handle_infra_err (e: any) -> unit ! =
  match e
    | :unavailable          -> log.warn("service unavailable")
    | :permission_denied    -> notify.toast("Permission required")
    | :service_unavailable  -> log.error("service crashed; will retry")
    | :timeout              -> log.warn("timeout")
    | _                     -> log.warn("unknown error: {e}")
```

---

## Part II — Permission flow

### 7 · `@needs.services` declarations

Required for every service the app consumes:

```
@needs
  services:
    "storage.fs":             ">= 1"
    "network.http":           ">= 2"
    "social.bsky.app/feed":   ">= 1"
    "system.audio":           optional
```

**Format per entry:**

| Form | Meaning |
|---|---|
| `">= N"` | Provider's `version:` must be ≥ N |
| `"= N"` | Exactly version N |
| `optional` | If provider is missing, the alias binds to `:unavailable` and every call returns `:err :unavailable` instead of `LoadError :unresolved` |

The loader checks every `@needs.services` entry against the platform's service registry. Missing or version-mismatched (non-optional) → `LoadError :incompatible`.

### 8 · `@grants.services` rationale

Required for every service whose grant policy is not `:never`:

```
@grants
  services.bsky:
    reason: "Sync your Bluesky feed."
    prompt: :on_first_use
  services.fs:
    reason: "Save drafts to your SD card."
    prompt: :at_install
    paths:  ["/sd/notes/"]
```

Standard fields per service (also documented in each service's section in `SERVICES.md`):

| Field | Type | Notes |
|---|---|---|
| `reason` | `str` | User-facing rationale; required when `prompt ≠ :never` |
| `prompt` | atom | `:at_install` / `:on_first_use` / `:never`. Default per service |
| `persist` | bool? | Whether the grant survives uninstall/reinstall (used by `system.logs` and a few others) |
| service-specific | varies | e.g. `paths:` for `storage.fs`, `allowed_hosts:` for `network.http`, `providers:` for `auth.oauth` |

Missing required fields → `LoadError :permission`.

### 9 · Resolution order

Per `SERVICES.md` §7.3, every consumer call passes through:

1. **Loader (install-time)**: `@needs.services` resolves; `@grants.services` declared with required fields.
2. **Bind (app-launch)**: provider's `allow:` admits caller's `@app.id`.
3. **Runtime (first call, then cached)**: user grant decision per `@grants.prompt:` timing.
4. **Per-call**: cached grant rechecked.

Failures at any layer → `:err :permission_denied`. Consumers cannot distinguish *which* layer denied (privacy-preserving).

---

## Part III — Optional bindings

### 10 · `optional` in `@needs.services`

Use when a service is nice-to-have but not required:

```
@needs
  services:
    "storage.fs":      ">= 1"
    "system.battery":  optional       -- battery-less platforms degrade gracefully
    "media.audio":     optional       -- audio-less platforms degrade gracefully
```

When optional and not available, the alias is bound but every call returns `:err :unavailable`. The app continues to load.

### 11 · Detecting `:unavailable`

Apps that need to behave differently on missing-capability platforms inspect:

```
@on launch
  match battery.state()
    | :ok s             -> show_battery_widget(s)
    | :err :unavailable -> hide_battery_widget()
    | :err e            -> log.warn("battery state failed: {e}")
```

For ergonomic checks without forcing every method call, the runtime exposes (via `system.apps`):

```
apps.service("system.battery").quarantined        -- false if available, true if quarantined
apps.service("system.battery") == :none           -- :none if not registered at all
```

But the most idiomatic pattern is to call and match `:err :unavailable` per-method. Less code, no ambient global state.

### 12 · Graceful degradation patterns

Standard shapes when a capability may be unavailable or denied:

#### 12.1 Skip silently

```
let _ = notify.toast("Saved")    -- ignore result; if denied, no toast
```

#### 12.2 Fall back to alternative

```
let img_data = match image.load(url)
  | :ok ref           -> :some ref
  | :err :unavailable -> :none           -- platform doesn't support media.image
  | :err _            -> :none
```

#### 12.3 Surface to UI

```
match wifi.connect(ssid, pwd)
  | :ok _                 -> proceed()
  | :err :permission_denied -> show_permission_request_dialog()
  | :err e                  -> show_error("Connection failed: {e}")
```

---

## Part IV — Lifecycle from the consumer

### 13 · First-call latency (deck-app services)

Calls into a `:deck-app` service that hasn't been spawned cold-starts the provider's VM (per `SERVICES.md` §9). The first call may take several hundred milliseconds (parsing the provider's source, running `@on launch`, then dispatching). Subsequent calls within the resident window are fast.

**Implication:** the first call to a service after app suspend / cold boot may exceed `max_run_ms` for tight handlers. Use `@on launch` for warm-up of services you'll need synchronously:

```
@on launch
  -- warm the service so subsequent calls in @on resume are fast
  let _ = bsky.fetch_latest()
```

For services declared with `keep: true`, this is unnecessary.

### 14 · Stream cancellation

Dropping an `@on source` subscription cancels the upstream stream:

```
state :reading
  on enter -> ...           -- subscription created
  on leave -> ...           -- subscription dropped on state exit; provider notified

@on source feed.timeline_watch() as Timeline   -- subscription tied to handler lifetime
```

Cancellation propagates within ~1s for cooperative providers. For action streams (e.g. `fs.copy`), the operation is genuinely aborted — partial work is left in place (the copy doesn't auto-rollback).

### 15 · Handle ownership

Handles (`AudioHandle`, `FsHandle`, `WsHandle`, etc.) are values:

- Stored in machine state payloads, passed across fns.
- **Not persisted across suspend** — handles are valid only within the current VM session.
- After the closing method (`audio.stop(h)` / `fs.close_write(h)`) or VM termination, the handle becomes invalid; further calls return `:err :invalid_handle`.

Apps that survive suspend must re-open handles in `@on resume`:

```
@on resume
  let h = audio.play(current_track_ref)?
  AudioState.send(:resumed, handle: h, position: saved_position)
```

---

## Part V — Best practices

### 16 · Choose the highest-tier service

Per `SERVICES.md` §12, prefer Tier 4 (high-level) over lower tiers when the higher-tier service does the job. Concrete examples:

| Don't | Do |
|---|---|
| `network.http.get(...)` for REST APIs | `api.client.get(...)` |
| `network.http.get(...)` to download an image | `media.image.load(url)` |
| `system.audio.play(...)` for music playback | `media.audio.play_queue(...)` |
| `network.http` + `storage.nvs` for OAuth | `auth.oauth.start_flow(...)` |
| `storage.cache` directly for response caching | `data.cache.set(key, value, ...)` |

Lower tiers remain available for edge cases (custom HTTP verbs, raw audio, exotic auth flows). Defaulting to Tier 4 reduces app code dramatically — Tier 4 services handle retry, caching, error translation, and protocol details that Tier 2/3 leave to the consumer.

### 17 · Wrap third-party services in your own typed adapters

Apps that consume a third-party service (e.g. `social.bsky.app/feed`) should wrap it in a private fn module:

```
@use service "social.bsky.app/feed" as bsky_svc

@private fn timeline () -> Result [Post] app.Error ! =
  bsky_svc.fetch_latest() |> result.map_err(svc_to_app)
```

Then the rest of the app uses `timeline()` rather than `bsky_svc.fetch_latest()` directly. Benefits:

- Error domain translation in one place.
- Shielded from version bumps in the upstream service (the wrapper absorbs the change).
- Easier to swap providers (`media.notes.dn/feed` instead of `social.bsky.app/feed`) without touching the rest of the app.

### 18 · Don't over-pin version ranges

```
-- Don't:
@needs
  services:
    "social.bsky.app/feed": "= 1"        -- breaks on every version bump

-- Do:
@needs
  services:
    "social.bsky.app/feed": ">= 1"       -- accepts any compatible version
```

Pin only when you depend on behaviour that's known to change in a future version. Otherwise, accept the broader range — consumers that pin too narrowly cause unnecessary install failures when the platform updates.

### 19 · Co-locate `@use`, `@needs`, `@grants`

The three blocks together describe the app's outside-world surface. Reading them is the fastest way to understand what an app touches. Convention: place them in this order at the top of `app.deck`, immediately after `@app`:

```
@app
  ...

@needs
  services:
    "storage.fs":      ">= 1"
    "api.client":      ">= 1"
    "system.battery":  optional
  caps:
    notifications: optional
  max_heap: 128KB

@grants
  services.fs:
    reason: "Save your drafts."
    prompt: :at_install
    paths:  ["/sd/notes/"]
  services.bsky_api:
    reason: "Fetch posts."
    prompt: :at_install

@use
  storage.fs    as fs
  api.client    as bsky_api
    base_url: "https://bsky.social"
  system.battery as battery     -- optional per @needs

@on launch
  ...
```

A reviewer reading the top of the file knows: this app touches the filesystem (with rationale), talks to bsky.social, optionally reads the battery, and nothing else.

### 20 · Builtin vs capability decision

Builtins (no `@use`) and capabilities (with `@use`) overlap conceptually for some operations. Rules of thumb:

| Operation | Use |
|---|---|
| Read current time | `time.now()` (builtin) |
| Set wall clock | `system.time.set(ts)` (capability) |
| Log a message | `log.info(msg)` (builtin) |
| Query log history | `system.logs.tail(...)` (capability — system-only) |
| Compute on data (math, text, list) | builtins |
| Touch device state, network, persistence | capability |

The principle: **pure computation is a builtin; observable side effects are a capability**. The single exceptions (`time.now`, `log.*`, `rand.*`) are documented in `BUILTINS.md` §0.

