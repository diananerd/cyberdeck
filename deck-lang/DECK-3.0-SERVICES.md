# Deck 3.0 — Service Catalog

**Status:** Draft. Companion to `DECK-3.0-DRAFT.md`, `DECK-3.0-CAPABILITIES.md`, and `DECK-3.0-BUILTINS.md`. Not yet authoritative.

**Edition:** 2027.

A **service** is a typed IPC interface one app exposes for other apps to call. Services are the third (and final) callable surface in Deck, complementing capabilities (OS-mediated I/O) and builtins (in-VM computation). Together the three categories partition every cross-boundary call an app can make:

| Category | Provider | Mediation | Examples |
|---|---|---|---|
| **Builtin** | Runtime | None — in-VM call | `math.abs`, `text.len`, `list.map` |
| **Capability** | Platform (C/Rust) | OS interposition; granted | `network.http.get`, `storage.fs.read`, `system.apps.launch` |
| **Service** | Another Deck app | App-VM IPC; granted | `social.bsky.app/feed.fetch_latest`, `system.share/target.share` |

This document defines:

- **Part I** — the meta-spec: what a service IS, how any service must be shaped, and how it fits between capabilities and builtins.
- **Part II** — the v1 standard system services catalog (provided by reference platform apps).
- **Part III** — the authoring contract: how an app declares and exposes a service.
- **Part IV** — discovery, registration, conformance.

Services follow exactly the same dimensional discipline as capabilities (`CAPABILITIES.md` §1). The same template applies whether a service is provided by a system app (`system.share/target`) or a third-party app (`social.bsky.app/feed`). There is no privileged "system services" mechanism — system services are simply services declared by apps whose `@app.id` starts with `system.`.

---

# Part I — Meta-spec: the shape of a service

## 0 · Philosophy

The same Unix-philosophy rules as capabilities (§0 of `CAPABILITIES.md`), with three service-specific clarifications:

1. **Apps as APIs.** A service turns an app into a callable interface. Where capabilities expose *the platform*, services expose *another app*. The mechanism is identical from the consumer's perspective — `@use service "id" as alias` produces an alias whose methods are typed and verified at load.
2. **One service ID, one provider.** Service IDs are namespaced under the providing app's `@app.id`. Two apps cannot claim the same service ID (the loader rejects the second install with `LoadError :resource`). Cross-app extensibility for a generic concept (multiple email apps, multiple browsers) is handled by `@handles` URL pattern matching, not by service-ID competition.
3. **Services are stateful by default.** A service runs inside the providing app's VM, sharing its `@machine` state, `@config`, `@assets`, capability bindings, and event loop. There is exactly one VM per app, regardless of how many concurrent service callers exist (§9 below). The provider does not manage threading; the runtime serialises calls into the cooperative scheduler.

## 1 · Services vs capabilities vs builtins

| Dimension | Capability | Builtin | Service |
|---|---|---|---|
| **Provider** | Platform (C/Rust) | Runtime | Deck app |
| **Declared in** | `.deck-os` manifest | `.deck-builtins` manifest | `@service` block in provider app |
| **Imported as** | `@use storage.fs as fs` | (always available) | `@use service "id" as alias` |
| **State** | Provider-defined | None | Provider's app state |
| **Permission** | `@grants.<cap>` | None | `@grants.services.<alias>` + provider's `allow:` field |
| **Lifecycle** | Always available (or `:err :unavailable` when optional) | Always available | Cold-start on first call; eviction by idle policy |
| **Threading** | Provider-defined; usually parallel | In-VM, synchronous | Cooperative, serialised through provider's VM |
| **Cross-call state** | Provider-defined | None | Persistent across calls within VM session |
| **Failure modes** | `Result T E` per capability | `Result T E` per builtin (rare); panic for bugs | `Result T E` per service + universal IPC errors |
| **Versioning** | Platform-edition-pinned | Edition-pinned | Service `version:` integer; consumer declares range in `@needs.services` |

The consumer-side ergonomics are intentionally identical. Whether `feed.fetch_latest()` is implemented by a platform driver or by another running Deck app, the calling code does not change.

## 2 · Service identity

A service ID has the shape `<app-id>/<service-name>`:

- `<app-id>` is the providing app's `@app.id` (lowercase reverse-DNS-style: `social.bsky.app`, `system.launcher`, `media.notes.dn`).
- `<service-name>` matches `[a-z][a-z0-9_-]*` (no slashes, no dots, no uppercase).

Examples:
- `social.bsky.app/feed`
- `social.bsky.app/notifications`
- `system.share/target`
- `media.notes.dn/inbox`

**One provider per ID.** The loader scans every installed app's `@app.serves` and rejects any second app whose `serves` list overlaps with an already-installed app. The conflict is resolved at install time, not at call time.

**Multiple services per app.** A single app may declare any number of `@service` blocks, one per ID. An app's `@app.serves` enumerates them.

**System services** are services whose provider `@app.id` starts with `system.`. They follow the same shape as any other service; the only effective privilege is that some `system.*` providers may declare `allow: :system` to restrict callers to other system apps.

## 3 · The seven dimensions of a service

Every service, system or app-defined, is fully specified by these seven (the capability meta-spec's seven dimensions, adjusted for IPC semantics):

| Dimension | What it says |
|---|---|
| **1 · Identity** | Service ID (`<app-id>/<service-name>`) and `version:` integer. |
| **2 · State** | The service runs inside the provider's app VM and may read/write the provider's `@machine`, `@config`, capabilities. From the consumer's perspective, every call is "stateless" — no handle is needed to identify the service across calls. Stateful conversations across calls use service-internal identifiers (e.g. session tokens) carried in arguments. |
| **3 · Methods** | Each method declared as `on :name (params) -> Result T E ! = body`. All service methods are impure — IPC is observable. |
| **4 · Error domain** | `@errors <alias>` declared at the service block; includes universals (§5 below) plus service-specific atoms. |
| **5 · Configuration** | `@use service "id" as alias \n k: v` — same shape as capability config. The provider declares the schema in its `@service` block. |
| **6 · Grants** | Two-layer: provider's `allow:` (hard policy: `:any`, `:system`, `[app_id, …]`) plus consumer's `@grants.services.<alias>` (user-facing rationale, prompt timing). |
| **7 · Lifecycle modifiers** | `keep:` (whether the provider VM stays resident after idle); per-method `max_run_ms:`. |

Nothing else belongs in a service definition. Services have no events of their own (cross-cutting notifications go through `os.*` events; per-service push goes through stream-returning methods).

## 4 · Method kinds

Every service method falls into the same five kinds as capability methods (`CAPABILITIES.md` §2). The naming conventions are identical:

| Kind | Return type | Naming convention |
|---|---|---|
| **Query** | `Result T E` (or `T?` when absence is routine) | Noun phrase: `feed.timeline`, `inbox.message` |
| **Mutation** | `Result unit E` or `Result T' E` | Verb or `set_<noun>`: `inbox.mark_read`, `feed.post` |
| **Action stream** | `Stream <EventT>` (terminator variant) | Verb: `feed.fetch_history`, `share.send` |
| **Watch stream** | `Stream T` (infinite, reactive) | `<state>_watch` suffix: `inbox.unread_watch`, `feed.timeline_watch` |
| **Handle-producing** | `Result Handle E` | Verb returning `Result Handle E`; paired with handle methods |

The runtime's IPC layer carries every kind across the VM boundary. From the consumer side, `@on source feed.timeline_watch() as Timeline` works exactly as it does for `battery.watch()`.

**No pure service methods.** A service call always crosses the VM boundary; the call itself is observable. Every method is `!`. Marking a service method without `!` → `LoadError :type`.

**Stream cancellation across IPC.** When a consumer drops a stream subscription (e.g. the `@on source` handler is destroyed), the runtime sends a cancel signal to the provider; the provider's stream-emitting body terminates after its next yield point. Apps writing stream-returning methods must yield cooperatively (loop body short, or `time.monotonic` checks) so cancellation is responsive.

## 5 · Error model

Every service `@errors <alias>` domain MUST include the four universals:

```
:unavailable           "Provider not installed (or marked optional and absent)"
:permission_denied     "Caller not in `allow:`, or @grants.services denied"
:service_unavailable   "Provider VM quarantined after repeated panics (§11.2 of draft)"
:timeout               "IPC budget exceeded (per-method default 5s; configurable)"
```

These are the four IPC-level failure modes orthogonal to the service's logical errors.

Beyond these, each service domain lists only atoms representing distinguishable failure modes the caller might handle differently. Same discipline as capabilities (§3 of `CAPABILITIES.md`):

- No payloads on error atoms (lift structured context into the success type or log it).
- No catch-all `:error`.
- No re-export of capability errors (`fs.Error`, `http.Error`) — the provider maps capability failures to its own service-domain atoms.

This last rule deserves emphasis: **a service never exposes its implementation's error domain.** If the provider's `feed.fetch` is implemented by `http.get` + `json.parse`, the service does not return `Result Feed http.Error`. It returns `Result Feed feed.Error`, where `feed.Error` enumerates the *consumer-visible* failure modes (`:upstream_unreachable`, `:rate_limited`, `:malformed_response`, etc.). The provider uses `result.map_err` to translate. This decouples the consumer from the provider's implementation.

## 6 · Permission model

Two layers, both must permit a call:

### 6.1 Provider-side `allow:`

Declared in the `@service` block:

```
@service "social.bsky.app/feed"
  allow: :any
  ...
```

Values:
- `:any` — any installed app may call.
- `:system` — only apps whose `@app.id` starts with `system.`.
- `[app_id, …]` — explicit whitelist.

Calls failing the `allow:` check return `:err :permission_denied` immediately, without entering the method body. Not user-prompted; this is a hard policy choice by the service author.

### 6.2 Consumer-side `@grants`

Declared in the consumer's `@grants` block under `services.<alias>`:

```
@grants
  services.bsky:
    reason: "Sync your Bluesky feed."
    prompt: :on_first_use
```

Standard fields (`reason`, `prompt`) match `CAPABILITIES.md` §7. The user can deny; denied calls return `:err :permission_denied` (graceful degradation).

A consumer that fails to declare `@grants.services.<alias>` for a non-`:never`-prompt service is `LoadError :permission` at app load.

### 6.3 Resolution order

1. Loader: consumer's `@needs.services` lists this service ID → check provider exists / version compatible.
2. Loader: consumer's `@grants` covers the service → otherwise `LoadError :permission`.
3. Bind: provider's `allow:` admits caller's app ID.
4. Runtime: user grant check (per `@grants.prompt:` timing); store user's decision.
5. Per-call: every call re-checks the grant decision (cached).

If any check fails, the call returns `:err :permission_denied`. The consumer cannot distinguish *which* layer denied it (intentional — privacy-preserving).

## 7 · Versioning

Every service declares an integer `version:` (default `1`):

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
```

**Version compatibility rules** (provider-side discipline):

- **Same version, additive changes** — adding a new method, adding a new error atom (consumers' exhaustive matches break and become `LoadError :exhaustive` at next install). No bump needed for *adding methods*; **adding error atoms requires bump**.
- **Bump version** — removing a method, changing a method signature, removing an error atom, semantic change to existing method behaviour, schema change to a returned `@type`.
- **No minor/patch versions** — services use a single integer. The simplification is intentional: services are app-internal contracts, and two-axis versioning (semver) historically masks breaking changes via "minor" bumps.

**Multiple major versions in one provider** — a provider may declare separate `@service` blocks for `version: 1` and `version: 2` of the same ID, supporting both consumer cohorts during migration:

```
@service "social.bsky.app/feed"
  version: 1
  allow: :any
  on :fetch ... = (call legacy)

@service "social.bsky.app/feed"
  version: 2
  allow: :any
  on :fetch ... = (call new)
```

The IPC dispatcher routes by the consumer's declared `@needs.services` range.

## 8 · Lifecycle

Per `DECK-3.0-DRAFT.md` §18.2, refined here:

| Phase | Trigger | Effect |
|---|---|---|
| **Cold** | App installed, no recent calls | Provider VM not running; consumes no resources |
| **Spawning** | First IPC call after cold | Runtime spawns provider VM, runs `@on launch` (with `service: true` flag in event), then dispatches the queued call |
| **Resident** | Recent call activity | Provider VM stays in memory; subsequent calls dispatch immediately |
| **Idle eviction candidate** | No calls for 5 min AND `keep: false` | Runtime may evict; runs provider's `@on terminate` first, persists machine state per §22.4 of draft |
| **Quarantined** | Provider panicked 3 times within 5 min | Provider VM disabled; further calls return `:err :service_unavailable` until manual relaunch from system.apps |

**`keep: true`** keeps the provider VM resident indefinitely (subject to OS memory pressure). Use for services where cold-spawn latency would be user-visible (e.g. notifications inbox).

**Foreground vs service VM** — same VM. If the provider app is currently in foreground, service calls dispatch into the same scheduler queue as foreground events; they cooperatively interleave (§14.10 of draft). If the provider is not foreground, the VM is service-only. Lifecycle hooks see the difference via `event.context: :foreground | :service` on `@on launch`.

## 9 · Threading and isolation

Each service has **one logical thread** — the provider's VM scheduler. Multiple consumers' calls queue:

- Calls arrive in the provider's event queue (default 32; bounded per §14.10).
- The scheduler dequeues one at a time, runs the method body to completion, returns the result, then dequeues the next.
- Stream-returning methods occupy the scheduler only when emitting; between emissions the scheduler is free.
- Queue overflow → oldest call dropped; consumer sees `:err :timeout` (or `:service_unavailable` if quarantined).

**No parallelism inside a service.** A service author cannot speed up by doing work concurrently across calls; the model is cooperative. For genuinely parallel work, the service may delegate to a capability that itself parallelises (e.g. `network.http` runs HTTP requests in a platform thread pool).

**Isolation guarantees:**

- A service's `@machine` state and `@config` are accessible only inside its own VM — no consumer can read or mutate the provider's state directly.
- IPC arguments / return values are deep-copied across the VM boundary at the runtime level (apps perceive value semantics; no shared references).
- A panic in the provider's body does not propagate to consumers; consumers see `:err :service_unavailable` after quarantine policy applies.

## 10 · Discovery

Apps discover services via the `system.apps` capability (declared in `CAPABILITIES.md` §16):

```
@use system.apps as apps

apps.info(id: str) -> AppInfo?
  -- AppInfo.services : [str]    -- list of service IDs the app provides
```

For per-service metadata (version, allow status, current resident state), the catalog adds **one method** to `system.apps`:

```
apps.service (service_id: str) -> ServiceInfo?

@type ServiceInfo
  id           : str
  provider_id  : str             -- @app.id of provider
  version      : int
  allow        : ServiceAllow
  keep         : bool
  resident     : bool             -- VM currently spawned
  quarantined  : bool             -- :service_unavailable until manual relaunch

@type ServiceAllow =
  | :any
  | :system
  | :list ([str])                 -- whitelisted app IDs
```

Add to `CAPABILITIES.md` §16 method list (catalog-side amendment captured in this document for now; merge on next consolidation pass).

Static discovery is via `@needs.services` declarations — load-verified at install. Dynamic discovery (`apps.service(id)`) is for system apps (Settings, Launcher) that survey the running system.

---

# Part II — v1 Standard System Services

The reference CyberDeck platform ships system apps providing the following services. Third-party platforms may omit them (apps that depend on them declare `optional` in `@needs.services`) or substitute their own provider apps under the same service ID, **provided the provider is a `system.*` app**.

| Service ID | Provider | Purpose |
|---|---|---|
| `system.share/target` | `system.share` (built-in app) | Receive shared content from any app |
| `system.launcher/badges` | `system.launcher` | (Deprecated path; prefer `display.notify.set_badge`) |

The catalog is intentionally tiny in v1. Most cross-app communication is mediated by capabilities (`display.notify`, `system.url`, `system.apps.launch`); services are reserved for cases where a structured typed call from one app to another carries clear advantages over capability-mediated indirection.

## 11 · `system.share/target`

Standard "share-to" interface. Any app can share a structured payload to the user's chosen share target, mediated by the OS picker UI.

```
@service "system.share/target"
  version: 1
  allow:   :any
  keep:    false

  on :share (content: ShareContent) -> Result unit share.Error !
    = ...

  on :supported_kinds () -> Result [atom] share.Error !
    = ...

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
  :no_target            "User cancelled the picker; no target selected"
  :unsupported_kind     "Provider doesn't accept this content kind"
  :payload_too_large
```

**Notes:**
- `system.share/target` is conceptually a **dispatcher**: it presents the user a picker among installed apps that handle the content kind, then forwards the payload to the chosen app.
- Apps wishing to *receive* shared content register a separate service per app (`mynotes.app/share-receiver`, etc.) AND declare `@handles "share://..."` URL patterns. The dispatcher matches by content kind.
- This is the canonical example of a system service whose value over capabilities is the structured `ShareContent` payload; encoding the same data through `system.url.open("share://...")` would lose typing.

## 12 · `system.launcher/badges` (deprecated path)

Documented for completeness; **new apps should use `display.notify.set_badge(count)` (capability)** instead. The system.launcher service exists in case third-party launchers want to expose alternative badge aggregation; it forwards to the same `system.apps.notif_counts_watch` stream that the capability writes to.

```
@service "system.launcher/badges"
  version: 1
  allow:   :any

  on :set (count: int) -> Result unit badges.Error !
    = ...
  on :clear () -> Result unit badges.Error !
    = ...

@errors badges
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :invalid_count        "count < 0"
```

This is the only spot in the v1 catalogs where two paths exist for the same operation. Future revisions remove `system.launcher/badges` once the deprecation window closes.

---

# Part III — Authoring app services

## 13 · Declaring a service

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
    = (Bluesky business logic; see Part IV examples)

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

| Field | Type | Notes |
|---|---|---|
| `version` | `int` | Default `1` |
| `allow` | `ServiceAllow` | Default `:any` |
| `keep` | `bool` | Default `false` |
| (optional) `max_run_ms_default` | `int` | Per-method default; individual methods may override with `max_run_ms:` |

The `@service` block is otherwise just a list of `on :name (params) -> ReturnType ! = body` declarations.

## 14 · Stream-returning methods

A service method may return `Stream T`. The provider's body for such a method runs in a stream-context — instead of a single return, it `emit`s values:

```
@service "social.bsky.app/feed"
  version: 1
  allow: :any

  on :timeline_watch () -> Stream Post !
    = ...
```

Inside the body, the provider uses the `stream.emit(value)` builtin to push:

```
on :timeline_watch () -> Stream Post !
  =
    -- pseudo-shape; actual idiom uses inner @on source / loop
    while not subscription_cancelled
      let new_posts = poll_for_new()
      list.each_io(new_posts, p -> stream.emit(p))
      time.sleep(30s)
```

(Note: `stream.emit` is added to `BUILTINS.md` §12 in the next consolidation.)

The IPC layer carries each emission across the VM boundary to the consumer's `@on source` handler.

## 15 · Service consumer pattern

Consumers import services exactly like capabilities:

```
@app
  ...

@needs
  services:
    "social.bsky.app/feed": ">= 1"

@grants
  services.bsky:
    reason: "Show the user's Bluesky feed."
    prompt: :on_first_use

@use
  service "social.bsky.app/feed" as bsky

@on launch
  match bsky.fetch_latest()
    | :ok posts  -> Timeline.send(:loaded, posts: posts)
    | :err e     -> log.warn("feed fetch failed: {e}")

@on source bsky.timeline_watch() as Timeline
  ->
    NotificationsState.send(:new_post, post: event)
```

The call site is identical to a capability call. The only consumer-facing difference is that `bsky.*` may return `:err :service_unavailable` (the provider's quarantine state) — a class capabilities don't have.

## 16 · Service state and persistence

A service runs inside the provider's VM. State management:

- **Machine state** — services may dispatch to the provider's `@machine` via `<MachineName>.send(…)` exactly as foreground code does. Machine state persists across suspend (§22.4 of draft).
- **`@config`** — services read/write the provider's `@config` exactly as foreground code does. Persistence and atomicity per §12 of draft.
- **Per-call locals** — `let` bindings in the method body are gone after the call returns.

For multi-call sessions (e.g. a chat session: open, send N messages, close), the service exposes a handle:

```
@service "media.notes.dn/inbox"
  version: 1
  allow: [:system.share]

  on :open_session () -> Result SessionHandle inbox.Error !
    = ...

  on :write (h: SessionHandle, content: str) -> Result unit inbox.Error !
    = ...

  on :close_session (h: SessionHandle) -> Result unit inbox.Error !
    = ...
```

The handle is a string identifier; the provider stores per-handle state in its machine state (e.g. as a map keyed by handle). Handles are not persisted across consumer-VM suspend (per `CAPABILITIES.md` §5).

## 17 · Error domain authoring

The provider maps every internal failure to a *consumer-visible* error atom. Translation happens at every method's exit:

```
on :fetch_latest () -> Result [Post] feed.Error !
  =
    let resp = http.get("/timeline")
              |> result.map_err(http_to_feed)?
    parse_posts(resp.body)
              |> result.map_err(_ -> :malformed_response)

@private fn http_to_feed (e: http.Error) -> feed.Error =
  match e
    | :timeout            -> :upstream_unreachable
    | :dns                -> :upstream_unreachable
    | :refused            -> :upstream_unreachable
    | :status_5xx         -> :upstream_unreachable
    | :status_4xx         -> :auth_required          -- simplified for example
    | :tls                -> :upstream_unreachable
    | _                   -> :upstream_unreachable
```

**Why translate.** Without translation, consumers couple to the provider's implementation (HTTP today, gRPC tomorrow, WebSocket the day after). Every refactor inside the provider would propagate as breaking changes to every consumer. The translation function is the provider's stability boundary.

## 18 · Versioning policy

Per §7 above. In practice:

- Bump the version when removing or changing the shape of any method or returned type.
- Bump when adding error atoms (consumers' exhaustive matches break).
- Don't bump when adding new methods (consumers ignore unknown methods).
- Maintain a previous-version `@service` block for at least one release after a bump, calling into the new logic with translation.

The provider deletes the old `@service` block when telemetry shows zero consumer apps still pinning the old version. (Telemetry mechanism: `system.apps.consumers_of(service_id, version)` — added to `system.apps` capability in the same amendment as `apps.service` above.)

## 19 · Examples

### 19.1 Single-method query service

```
@service "media.weather.dn/forecast"
  version: 1
  allow:   :any
  keep:    false

  on :for_city (city: str) -> Result Forecast forecast.Error !
    = api.fetch(city) |> result.map_err(http_to_forecast)

@type Forecast
  high_c        : float
  low_c         : float
  description   : str
  retrieved_at  : Timestamp

@errors forecast
  :unavailable
  :permission_denied
  :service_unavailable
  :timeout
  :unknown_city
  :upstream_unreachable
```

### 19.2 Stream-notifier service

```
@service "social.bsky.app/notifications"
  version: 1
  allow:   :any
  keep:    true

  on :unread_count () -> Result int notif.Error !
    = ...

  on :stream () -> Stream Notification !
    = (loop pulling new notifications, emit each)

@type Notification
  id        : str
  kind      : atom               -- :reply :like :follow :mention
  from      : str
  excerpt   : str?
  ts        : Timestamp
```

Consumers tap with `@on source bsky_notif.stream() as Inbox`.

### 19.3 Hub (many subscribers, fan-out)

When multiple apps need to react to the same event stream, the provider's stream-returning method fan-outs internally:

```
@service "system.health/events"
  version: 1
  allow:   :any
  keep:    true

  on :alerts () -> Stream HealthAlert !
    = ...

@type HealthAlert
  severity : atom               -- :info :warn :critical
  message  : str
  source   : atom               -- :wifi :battery :memory :disk :ota
  ts       : Timestamp
```

The provider holds an internal subscriber list (each `:alerts ()` call adds to it). Emit broadcasts to all. Consumers cancel by dropping the `@on source` subscription; the provider removes them from the list.

This pattern requires care around backpressure (slow subscribers shouldn't block fast ones) — the runtime's per-stream queue (§14.10 of draft) handles per-subscriber buffering, but the provider must not accumulate unbounded state per subscriber. Document each hub service's scaling characteristics.

---

# Part IV — Extension contract

## 20 · Service discovery

When the OS scans installed apps:

1. Each app's `@app.serves` is read.
2. Each ID is added to a global service registry (provider, version, allow, keep).
3. Conflicts (two apps claiming the same ID) → second install is rejected with `LoadError :resource`.
4. Consumer apps' `@needs.services` are validated against the registry at install + at every load.

The registry is queryable via `system.apps.service(id)` (Part I §10).

## 21 · Service registration manifest

Internally, the registry stores per-service metadata. This is *not* an authored manifest — the loader builds it from each app's `@service` blocks. But the schema is documented for tooling:

```
@type ServiceRegistration
  id              : str
  provider_app_id : str
  version         : int
  allow           : ServiceAllow
  keep            : bool
  methods         : [ServiceMethodSig]
  errors          : [atom]
  registered_at   : Timestamp

@type ServiceMethodSig
  name        : atom
  params      : [(name: atom, type: str)]
  return_type : str               -- spec-rendered signature
  is_stream   : bool
  max_run_ms  : int
```

Tools (Settings, IDE, marketplace) display `ServiceInfo` derived from this registration. Apps can introspect a service before declaring `@needs.services` on it (in interactive contexts; load-time apps still must `@needs`).

## 22 · Conformance tests

Every service author SHOULD pass these tests for their service implementation. Platforms providing system services MUST pass them:

- **Method coverage** — every method in `@app.serves` is callable; types match declarations.
- **Universal errors** — every error domain emits the four universals on the right occasions; `:permission_denied` when `allow:` excludes the caller; `:service_unavailable` after 3 panics in 5 min; `:timeout` after `max_run_ms` exceeded.
- **Error translation** — internal `Result :err <other_domain>` never escape; the provider's error domain is closed.
- **Stream cancellation** — dropping subscription terminates the provider's stream body within 1 second; no orphaned subscribers in provider state.
- **Version isolation** — if multiple `@service` versions exist for the same ID, calls route by consumer's `@needs.services` range; no cross-version contamination of state.
- **Quarantine recovery** — after manual relaunch from `system.apps`, the service responds to calls again.
- **Eviction safety** — for `keep: false` services, the OS may evict at any 5-min idle boundary; the provider's `@on terminate` runs and persistent state survives; next call cold-spawns cleanly.

---

# Part V — Deferred for future revisions

- **Cross-device service discovery (mDNS / DNS-SD)** — services are local-VM-only in v1. Cross-network IPC is out of scope.
- **Service mesh / load balancing** — single provider per ID. Multi-instance is not supported.
- **Capability tokens / OAuth-style delegation** — the consumer always calls as itself; there is no delegated authority model. Apps that need to act on behalf of another do so via direct payload (caller passes credentials; provider trusts the protocol).
- **Pre-emptive scheduling for long-running calls** — every call runs to completion in the provider VM. If a consumer needs to cancel, it must drop the subscription (for streams) or wait. A `:cancel` mid-call signal is not provided; methods that need cancellability are expressed as streams whose final emission is `:cancelled`.
- **Service-to-service IPC chains** — service A calling service B is allowed (A's VM dispatches to B's VM), but mutual recursion across services without a clear quiescence point can deadlock the cooperative scheduler. The runtime will detect cycles in v1.1; for now, authors are responsible.

---

**End of services catalog draft. Promote, amend, or discard as a whole.**
