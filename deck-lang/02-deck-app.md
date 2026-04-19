# Deck App Model Specification
**Version 2.0 — Application DSL**

---

## 1. What the App Model Is

The language spec (`01-deck-lang`) defines how to compute with values. This document defines how to describe an application: its identity, capabilities, state, UI, background work, and lifecycle.

The app model is expressed entirely through `@` annotations. These are not decorators — they are declarations. The interpreter reads them before executing any code, verifies them as a complete structural description, and rejects with clear errors anything that is inconsistent or missing.

---

## 2. The @ Annotation System

### 2.1 What Annotations Are
An annotation is `@keyword` followed by an indented body. Annotations are not callable, not passable as values, not composable. They declare what something is.

### 2.2 Annotation Placement

| Annotation | Only in `app.deck` | Any `.deck` file |
|---|---|---|
| `@app` | ✓ | |
| `@use` | ✓ | |
| `@permissions` | ✓ | |
| `@config` | ✓ | |
| `@on` | ✓ | |
| `@migration` | ✓ | |
| `@errors` | | ✓ |
| `@machine` | | ✓ |
| `@flow` | | ✓ |
| `@stream` | | ✓ |
| `@task` | | ✓ |
| `@type` | | ✓ |
| `@test` | | ✓ |
| `@doc` | | ✓ |
| `@example` | | ✓ |
| `@private` | | ✓ |

`app.deck` is also a `.deck` file for annotation placement purposes. Annotations in the "Any `.deck` file" column are also valid in `app.deck`.

**Module imports**: `@use ./path` entries for local modules are declared **only in `app.deck`**. Individual `.deck` files do not have their own `@use` declarations — the module graph is established once from `app.deck` and all declared modules are in scope project-wide. See `01-deck-lang §10.3`.

---

## 3. @app — Identity

Defined once, only in `app.deck`.

```
@app
  name:        str     -- display name shown to users
  id:          str     -- reverse-domain unique ID ("mx.lab.monitor")
  version:     str     -- semver: "MAJOR.MINOR.PATCH"
  edition:     int     -- Deck language edition (calendar year, e.g. 2026); MANDATORY
  entry:       Name    -- root @machine or @flow that is the app entry point
  icon:        str?    -- short textual or asset reference identifying the app; bridge resolves.
  -- `icon:` is a string; if it matches an entry in `@assets as :icon` it is resolved to that asset
  -- path (typically a PNG), otherwise it is used verbatim (e.g. a 2–3 char glyph the bridge may
  -- render in a bitmap font). The bridge picks final rendering per device — never coordinates or
  -- pixel sizes from the app.
  tags:        [str]?  -- optional discovery tags for launcher search and registry indexing.
  author:      str?
  license:     atom?   -- :mit | :apache2 | :gpl3 | :proprietary
  orientation: atom?   -- :portrait | :landscape | :any (default)
  -- If :portrait or :landscape, the bridge refuses the rotation that would switch to
  -- the other mode while this app is foreground. Settings button is dimmed in that mode.
  -- :any (default) allows both orientations and receives os.display_rotated normally.
```

The `id` is stable identity. If the OS has a previously installed app with the same `id`, it runs `@migration` blocks before starting. If `id` changes, the OS treats it as a new app.

`entry:` names the root `@machine` or `@flow`. The OS starts the app at its `initial` state. The root machine/flow defines the top-level navigation topology.

`edition:` is the Deck language edition the app targets. **Mandatory.** It pins the syntax and semantics: an edition-2026 app uses `@machine`/`@flow`/no `if/then/else`; an edition-2027 app may differ. The runtime declares which editions it supports via `system.info.versions().editions_supported`. An app whose declared edition is outside that set fails to load with `:incompatible_edition` (see `15-deck-versioning §9`). If you don't know which edition to use, pick the current calendar year — the latest spec.

The `@app` block is the app manifest. No separate manifest file exists.

For OS surface, runtime, and capability version requirements, see the companion `@requires` annotation (§4A) and the full versioning model in `15-deck-versioning`.

---

## 4. @use — Capabilities and Local Modules

Brings OS capabilities and local `.deck` files into scope.

```
@use
  capability.path  as alias
  capability.path  as alias   optional
  capability.path  as alias   when: condition_expr
  ./relative/path
```

### 4.1 OS Capabilities

Capability paths match entries in the OS surface definition (see `03-deck-os`). Every capability must have an `as alias`. The alias is how code refers to the capability.

**`optional`**: The capability may be absent. Functions using it return `:err :unavailable` automatically when absent. Code does not need to check before calling.

**`when: condition`**: The capability is active only when the condition is true. Re-evaluated continuously by the runtime. When it becomes false, new calls return `:err :unavailable`.

Valid `when:` terms:
```
network is :connected
network is :offline
battery > N%
battery < N%
alias is :available
alias is :unavailable
App is :machine_state
```

**Catalog snapshot.** Common capability paths and their typical aliasing pattern. The full surface lives in `03-deck-os §4`.

```
@use
  -- Sensors (§4.1)
  sensors.temperature  as temp
  sensors.humidity     as humidity   optional
  sensors.pressure     as pressure   optional
  sensors.accelerometer as accel     optional
  sensors.gyroscope    as gyro       optional
  sensors.gps          as gps        optional
  sensors.light        as light      optional

  -- Storage (§4.2)
  storage.local        as store
  nvs                  as nvs
  fs                   as fs
  db                   as db

  -- Network (§4.3)
  network.http         as net
  network.wifi         as wifi
  network.socket       as sock       optional   -- raw TCP/UDP for protocol bring-up
  api_client           as api
  cache                as cache
  mqtt                 as mqtt       optional
  background_fetch     as bg

  -- Display & Notifications (§4.4)
  display.notify       as notify     -- transient toast (different from `notifications`)
  display.screen       as screen
  display.theme        as theme
  notifications        as notif

  -- BLE & external radios (§4.5)
  ble                  as ble
  bt_classic           as bt         -- external UART1 BT module on this board

  -- System (§4.6)
  system.info          as sysinfo
  system.locale        as locale
  system.time          as systime
  system.audio         as audio      optional
  system.battery       as battery
  system.security      as security   -- system.* apps only

  -- Hardware I/O (§4.7) — direct port access for advanced use
  hardware.uart        as uart       optional

  -- OTA & crypto (§4.8, §4.9)
  ota                  as ota
  crypto.aes           as aes
```

The `optional` keyword is the cheapest hedge against capability fragmentation across boards: declare it once, and any function call returns `:err :unavailable` automatically when the underlying hardware is missing — no branching required.

### 4.2 Local Modules

```
@use
  ./utils/format
  ./views/timeline
  ./models/post
```

Path is relative to `app.deck`. Module name = last path segment without extension. No qualifiers. Missing paths are a load error.

### 4.3 What @use Does Not Do
Does not install, download, or version-constrain. The OS is monolithic — everything the app can use was compiled into the OS image. `@use` is a declaration of intent, not an acquisition. **Version constraints belong in `@requires` (§4A), not in `@use`.**

---

## 4A. @requires — Compatibility Contract

Declares the OS surface, runtime, and capability versions the app needs to run. Top-level annotation in `app.deck`. Optional but **strongly recommended** for any app published outside its author's own device.

```
@requires
  deck_level: 2                     -- Minimum conformance profile: 1 (Core), 2 (Standard), 3 (Full)
                                    -- See 16-deck-levels for the full feature matrix.
  deck_os: ">= 2"                   -- OS surface API level range; see 15-deck-versioning §5
  runtime: ">= 1.0"                 -- Interpreter version range; see 15-deck-versioning §6
  capabilities:
    network.http:        ">= 2"     -- capability path : version range
    storage.local:       "any"      -- "any" matches any version
    notifications:       ">= 1"
    sensors.accelerometer: optional -- declares as optional; absence is acceptable
    ext.cyberdeck.battery_curve: ">= 1"   -- ext.* extension; vendor-specific
```

### 4A.1 Range syntax

Supported operators:

```
">= N"       greater than or equal
"> N"        strictly greater
"<= N"       less than or equal
"< N"        strictly less
"== N"       exactly N (rarely correct; prefer ranges)
"any"        any version
optional     declared optional; runtime treats it like any with absence allowed
```

Versions are integers for `deck_os`, semver strings for `runtime`, integers for capability versions. Range expressions cannot mix ANDs/ORs in v1; if a more complex range is needed, declare the most restrictive form.

### 4A.2 Defaults when omitted

If `@requires` is absent or partially specified:

- `deck_level`: inferred as the lowest level that covers every language feature and capability the app uses. Implementations should fail closed — if inference is ambiguous, reject the load with `E_LEVEL_INCONSISTENT` and instruct the developer to declare the level explicitly. See `16-deck-levels.md §5` and `§10` for the error catalog.
- `deck_os`: inferred as the lowest API level that contains every capability the app uses (computed from `@use` declarations + capability surface metadata).
- `runtime`: defaults to `>= 1.0`.
- `capabilities`: any capability appearing in `@use` is implicitly required at version `>= 1`. Optional `@use` entries become optional capability requirements.

Explicit declarations always override inferred ones.

### 4A.3 What the loader does with `@requires`

At app load time, the loader runs the compatibility check sequence in `15-deck-versioning §9.1`:

1. Validates `edition` from `@app` against the runtime's supported editions.
2. Validates `requires.deck_os` against the runtime's surface API level.
3. Validates `requires.runtime` against the runtime's own version.
4. For each `requires.capabilities` entry: looks up the registered driver, verifies it satisfies the version range (and any required `capability_flags`), and confirms permissions.

Any failure produces a structured `LoadError` per `15-deck-versioning §9.2` with a `code`, `actor`, `message`, `fix`, and structured `detail` so the OS shell can render an actionable message to the right person.

### 4A.4 Required flags

For capabilities with optional features expressed as `capability_flags` (e.g. `network.http` may or may not support `:http2`), the app can require specific flags:

```
@requires
  capabilities:
    network.http: ">= 1"  flags: [:http2, :stream_body]
```

A driver missing any required flag yields `:incompatible_capability`. For graceful degradation, omit the flags from `@requires` and probe at runtime via `cap.has_flag(:flag_name)`.

### 4A.5 Lock file (optional)

A `deck.lock` companion file inside the bundle records the exact version envelope used at build / test time. The loader does not enforce that the lock matches at launch — that would defeat the purpose of compatibility ranges. Tools (`deck check`, `deck info`) use the lock for diagnostics. See `15-deck-versioning §15`.

---

## 5. @permissions — Authorization

Declares which sensitive capabilities require user/OS authorization and why. Presented to the OS before the app runs.

```
@permissions
  capability.path  reason: "Human-readable explanation"
```

`reason:` is mandatory and shown verbatim in the OS permission dialog — write it for a user, not a developer.

**Load-time enforcement**: If a `@use` entry references a capability that has `@requires_permission` in the OS surface, it **must** appear in `@permissions`. The rules:
- Not in `@permissions` AND `@use` entry is **not** `optional` → **load error**
- Not in `@permissions` AND `@use` entry IS `optional` → **load warning** (developer may have intentionally skipped it)
- In `@permissions` → continue normally

**Runtime behavior**: If the user denies a permission at the OS dialog, the capability behaves identically to an `optional` capability that is currently absent. Calls return `:err :permission` as a `Result` value. The app does not crash.

**Permissions that require `@permissions`** (non-exhaustive):

| Capability path | What it gates |
|---|---|
| `notifications` | Registrar fuentes, post_local, recibir `@on os.notification`, y que el badge aparezca en el Launcher |
| `network.http` | Cualquier llamada de red saliente (`net.get()`, `api_client`) |
| `network.wifi` | Scan, connect, disconnect, forget — gestión de redes WiFi |
| `storage.local` | Acceso a storage persistente del app |
| `sensors.*` | Cualquier sensor de hardware |
| `ble` | Bluetooth LE scan y conexiones (nativo ESP32-S3) |
| `bt_classic` | Módulo BT Classic externo por UART1 |
| `hardware.uart` | Acceso directo a puertos UART |
| `system.time` | Forzar sync NTP, cambiar timezone |

Ejemplo — app con notificaciones:

```
@use
  notifications as notif   -- requires_permission
  network.http  as net
  storage.local as store

@permissions
  notifications reason: "To alert you when new posts arrive from people you follow"
  network.http  reason: "To fetch your timeline from bsky.social"
  storage.local reason: "To cache your timeline for offline reading"
```

---

## 6. @config — Typed Persistent Configuration

Declares named configuration values with types, defaults, and constraints. Different from `let` bindings in three ways: **persisted** across restarts, **accessible everywhere** as `config.name` without import, and **OS-exposable** as a native settings screen.

```
@config
  name : type = default
  name : type = default  range: min..max
  name : type = default  range: min..max  unit: "label"
  name : type = default  options: [:opt1, :opt2, :opt3]
```

Supported types: `int`, `float`, `bool`, `str`, `atom` (with `options:`).

`range:` applies to `int` and `float`. `options:` applies to `atom`. The runtime enforces constraints — assigning a value outside the range is rejected.

Config values are read-only from Deck code. Only the OS settings UI can change them. Changes are reflected immediately on next read.

**Settings app integration:** The Settings app calls `system.apps.config_schema(app.id)` to discover config fields and render a native editor. When the user saves a change, the OS fires `os.config_change (field: str, value: any)` to the running app (see `03-deck-os §5`). Apps that need to react immediately should handle this event:

```deck
@on os.config_change
  -- config.field_name already has the new value when this fires
  log.info("config changed: {event.field} = {event.value}")
```

The `ConfigFieldInfo` type returned by `config_schema()` is derived automatically from `@config` — developers do not write it manually. See `03-deck-os §4.11`.

---

## 7. @errors — Error Vocabulary

Defines named error variants for a domain. The `E` in `Result T E` return types.

```
@errors domain_name
  :atom  "Human description"
  :atom  "Human description"
```

Creates the type `domain_name.Error`. Variants are atoms. Defined in any `.deck` file. OS capabilities also declare their errors in `.deck-os` (see `03-deck-os`).

---

## 8. @machine — State Machines

Defines a named state machine. All stateful behavior in Deck flows through machines. `@machine` is the core primitive; `@flow` (§9) is ergonomic sugar over it.

### 8.1 Scope

| Scope | Declared in | Lifecycle | Access |
|---|---|---|---|
| **App-level** | Top level of any `.deck` file | Entire app lifetime | `MachineName.state`, `MachineName.send(...)`, `MachineName is :state` anywhere |
| **Flow-scoped** | Inside a `@flow` block | Activated when the flow enters the stack; OS decides freeze vs. destroy on exit | Same qualifiers, visible only within the flow |
| **State-scoped** | Inside a `state` block within a `@flow` | Created on state entry, destroyed on state exit | Same qualifiers, visible only within the state |

App-level machines are created at app load. Scoped machines follow their enclosing scope's lifecycle — the developer does not write creation or restoration code.

### 8.2 Declaration Syntax

```
@machine MachineName
  state :name
  state :name (field: Type, field: Type)
  state :name machine: OtherMachine   -- delegates content to another machine
  state :name flow: OtherFlow         -- delegates content to a @flow

  initial :name

  transition :event_atom
    from :state_name
    to   :target_state

  transition :event_atom (param: Type, param: Type)
    from :state_name current
    to   :target (field: expr_using_current_and_params)
    when: guard_bool_expr
    before -> expr   -- effect before this transition executes
    after  -> expr   -- effect after this transition executes

  -- Reactive transition: fires automatically when condition becomes true
  transition :event_atom
    from :state_name
    to   :target_state
    watch: bool_expr

  -- Return to the previous state (one level of history)
  transition :back
    from :overlay _
    to   history
```

States with `content =` define what the user perceives in that state (see §11):

```
state :active
  on enter -> expr   -- effect on any transition that activates this state
  on leave -> expr   -- effect on any transition that deactivates this state
  content =
    view_body_nodes
```

### 8.3 State Declarations

```
state :idle                                      -- no payload
state :active  (temp: float, max: float)         -- named payload
state :error   (message: str, code: atom)
state :editing machine: DraftMachine             -- delegates to another machine
state :thread  flow: ThreadFlow                  -- delegates to a @flow
```

`machine:` and `flow:` references are validated at load time — the named declaration must exist. A state with `machine:` or `flow:` may not also have `content =`.

### 8.4 Transition Rules

- Transitions fire when `MachineName.send(:event, param: value)` is called
- If the machine's current state does not match any `from:` for the given event, the send is silently ignored
- If a `when:` guard evaluates to false, the transition is silently ignored
- **Multiple transitions for the same event** are allowed if `from:` states or `when:` guards are mutually exclusive. Evaluated top-to-bottom; first match fires.
- `from *` matches any state

**Binding current state data:**
```
transition :update (reading: float)
  from :active current
  to   :active (temp: reading, max: math.max(reading, current.max))
```
`current` binds the entire from-state payload as a record.

**`to history`**: transitions to the state that was active immediately before the current one. One level only. Useful for modal overlays that should restore their parent's state on dismiss.

### 8.5 Hooks and Execution Order

Three hook kinds exist, each with a distinct scope:

| Hook | Scope | Declared as |
|---|---|---|
| `state … on enter` / `on leave` | State-scoped — fires for **every** transition that activates / deactivates this state | Inside the `state` block |
| `transition :name before -> …` / `after -> …` | Transition-scoped — fires only for **that** transition | Inside the `transition` block |
| `@machine.before:` / `@machine.after:` | Machine-scoped — fires around **every** transition of this machine | Top-level annotation in the same `.deck` file as `@machine` |

Execution order for a transition — from source state `S` to destination state `D` via transition `T`:

```
1. @machine.before         -- machine-scoped, before any state change
2. transition T    before  -- transition-scoped, before any state change
3. state S         on leave -- source state
4. [state changes: S → D, payload bound]
5. state D         on enter -- destination state
6. transition T    after   -- transition-scoped, after enter
7. @machine.after          -- machine-scoped, after enter and transition.after
```

**Entering the initial state** is treated as a transition from the pseudo-state `:__init` to the initial state: only hooks 4 (state changes), 5 (initial.on enter), and 7 (`@machine.after`) fire. `@machine.before` and `transition.before` do **not** fire on initial entry — the app did not send an event; initialization happened as part of `@on launch`.

**Terminating state** (a state with no transitions) fires `state on leave` when the machine reaches end-of-life (e.g. app suspend / unload); `@machine.after` does not fire on termination.

If a hook returns an error, subsequent hooks in the same transition are **not** run and the transition is rolled back: the machine remains in state `S`, the state payload is unchanged, and the error propagates up through `send()`.

### 8.6 Reactive Transitions (`watch:`)

A `watch:` transition fires automatically whenever its condition changes from false to true — no `send()` needed. The runtime evaluates `watch:` reactively on every change to any value it references (machine state, stream value, network state).

```
transition :require_login
  from :feed
  to   :login
  watch: not (AuthState is :authenticated)

transition :auto_login
  from :login
  to   :feed
  watch: AuthState is :authenticated
```

When multiple `watch:` transitions could apply simultaneously, evaluation is top-to-bottom and the first applicable one fires. Overlapping `watch:` conditions on the same machine are almost always a modeling mistake; the loader emits a warning.

### 8.7 Implicit Reactivity

If a `content =` body reads from a stream (e.g., `Timeline.last()`), the runtime detects that dependency and re-evaluates the content automatically when the stream emits. No `@listens` opt-in is needed — reactivity is implicit and derived from data dependencies.

### 8.8 Scoped Declarations Inside a Machine or Flow

The following declarations may appear inside a `@flow` body or inside a `state` block. Their lifecycle is tied to the enclosing scope:

```
@flow ComposeFlow
  @machine Draft
    state :empty
    state :editing (text: str)
    initial :empty
    ...

  @stream LiveUpdates
    source: ws.messages()

  @task AutoSave
    every: 30s
    run = Draft.save()

  @on hardware.headphone_removed
    send(:pause)

  fn render_empty () -> fragment =
    rich_text "Nothing here yet."

  initial :writing
  step :writing -> ...
```

State-scoped (created on state entry, destroyed on exit):

```
state :results
  @machine Filters
    state :collapsed
    state :expanded
    initial :collapsed
    ...
  @stream LiveFeed
    source: ws.feed()
  @task PollFallback
    every: 30s
    run = Timeline.send(:refresh)
```

Within a `@flow`, all scoped declarations (machines, streams, tasks, `fn`, `@on`) are visible to each other without qualification — they form a private module. The `fn` scope is visibility-only (functions are pure and have no lifecycle).

### 8.9 Flow Entry Context

When a `@flow` is referenced by a state with a payload, that payload is automatically in scope inside the flow without explicit declaration:

```
state :thread (post: Post)  flow: ThreadFlow
-- ThreadFlow has `post` in scope — it is the payload of the activating state

@flow ThreadFlow
  @stream ThreadUpdates
    source: ws.subscribe(thread_id: post.uri)  -- post in scope
```

A flow may only be referenced from states with compatible (or absent) payloads. Incompatible types across reference sites is a load error.

---

## 9. @flow — Flow Sugar

`@flow` is syntactic sugar over `@machine`. It adds `step :name ->` blocks that combine state content in a compact form. It does not add new semantics — a `@flow` desugars completely to a `@machine` with `content =` blocks per state.

### 9.1 Syntax

```
@flow FlowName
  state :state_a
  state :state_b (field: Type)
  state :state_c
  initial :state_a

  transition :event (param: Type)
    from :state_a
    to   :state_b (field: param)

  transition :back
    from :state_b _
    to   :state_a

  step :state_a ->
    content_nodes

  step :state_b s ->    -- s binds the state payload
    content_nodes using s.field
```

`step :name ->` desugars to `state :name { content = ... }`. States that have `machine:` or `flow:` references do not need a `step` — their content is provided by the referenced declaration.

Scoped declarations (§8.8) work identically inside `@flow`.

### 9.2 When to Use Each

| | `@machine` | `@flow` |
|---|---|---|
| Has `content =` per state | Optional | Always (via `step`) |
| Typical use | Background state, no direct UI | All navigable, user-facing flows |
| Examples | `AuthState`, loader, connection state | `App`, `FeedFlow`, `ComposeFlow` |

In practice, almost everything the user sees is a `@flow`. Pure `@machine` stays for background state.

### 9.3 Sequential Step Sugar (auto-transition)

When a `@flow` declares **only** `step :name -> …` blocks (no explicit `transition` declarations and no `state :name` with composition), the flow desugars to a linear state machine where each step's `on enter` body is the declared content and each step auto-transitions to the next in declaration order. The last step has no transition and the machine terminates in it.

```
@flow onboard
  step :welcome ->
    trigger "Continue" -> App.send(:start)
  step :collect ->
    form
      on submit -> App.send(:submitted)
      text :handle  hint: "Enter handle"  on -> …
  step :done ->
    rich_text "All set."
```

Desugars to:

```
@machine onboard
  initial :welcome
  state :welcome
    on enter -> [content_of :welcome]
    transition :__auto_1  from :welcome  to :collect
  state :collect
    on enter -> [content_of :collect]
    transition :__auto_2  from :collect  to :done
  state :done
    on enter -> [content_of :done]
```

Auto-transitions are synthesized at parse time with internal event names (`:__auto_n`) and fire immediately after the source step's `on enter` body completes. Apps **cannot** intercept auto-transitions; they are intended for deterministic linear sequences (onboarding, wizards, boot flows).

As soon as the `@flow` body contains a single explicit `transition` or `state` declaration, auto-transition is disabled for that flow — the flow is treated as full `@machine` equivalence (per §8) and every transition must be declared explicitly.

### 9.4 Navigation Topology

The root flow (named in `@app entry:`) expresses the app's navigation topology. States with `flow:` references compose it hierarchically. The OS infers navigation affordances from the structure:

| Structure | Phone | Tablet | Voice |
|---|---|---|---|
| Peer states with `from *` bidirectional | Tab bar | Sidebar | "Switch to X" |
| Nested states (depth) | Push/back | Split view | "More detail" |
| `from * → history` | Modal sheet | Popover | Interruption |

Deck does not declare tab bars, stacks, or modals explicitly. The topology is the semantics.

```
@flow App
  state :feed    flow: FeedFlow
  state :search  flow: SearchFlow
  state :profile flow: ProfileFlow
  state :compose flow: ComposeFlow

  initial :feed

  transition :go_feed    from * to :feed
  transition :go_search  from * to :search
  transition :go_profile from * to :profile

  transition :open_compose
    from *
    to   :compose

  transition :close_compose
    from :compose
    to   history
```

---

## 10. @stream — Reactive Data Sources

Declares a named reactive stream. App-level streams are active from app load until termination.

```
@stream Name
  source: capability_alias.method(args)
```

```
@stream DerivedName
  from:      SourceStreamName
  filter:    x -> bool_expr
  map:       x -> transform_expr
  distinct:  bool
  throttle:  Duration
  debounce:  Duration
  buffer_n:  int
  window_n:  int
  take_while: x -> bool_expr
  skip:      int
```

### 10.1 Source Streams

`source:` must be a capability method returning `Stream T`. If the capability is `optional` and unavailable, the stream produces no values.

### 10.2 Derived Streams

`from:` references another declared `@stream`. Operators chain in declaration order. All operator lambdas are pure. Circular stream references are a load error.

`distinct: true` — skip consecutive duplicate values.
`throttle: 5s` — emit at most one value per 5 seconds.
`debounce: 2s` — emit only after the value has been stable for 2 seconds.
`buffer_n: 10` — collect 10 values then emit `[T]` (type becomes `Stream [T]`).
`window_n: 5` — sliding window of 5, emit `[T]` on each new value.
`take_while: x -> ...` — stop emitting when predicate becomes false.
`skip: n` — discard first n values.

### 10.3 Stream Access

```
StreamName.last()        -- T?   (most recent value, or :none if none yet)
StreamName.recent(n)     -- [T]  (last n values, oldest first)
```

`combine_latest` and `merge` for multiple streams:
```
@stream Combined
  combine_latest
    TemperatureStream
    HumidityStream
  -- Type: Stream (float, float)

@stream AllAlerts
  merge
    TempAlerts
    PressureAlerts
  -- Type: Stream str (both must be same type)
```

**`combine_latest` semantics**: does not emit until every source stream has produced at least one value. After that, emits a new tuple each time any source emits, using the most recent value from each source. The type is `Stream (T₁, T₂, ...)` where Tₙ is the element type of the nth source. Up to 8 sources.

**`merge` semantics**: emits each value from any source as it arrives. All sources must have the same element type T. The resulting type is `Stream T`. Values from different sources are interleaved in arrival order.

---

## 11. @on — App Lifecycle Hooks

```
@on launch
  expr_or_do_block

@on resume
  expr_or_do_block

@on suspend
  expr_or_do_block

@on terminate
  expr_or_do_block
```

| Event | When |
|---|---|
| `launch` | App starts for the first time or after full restart |
| `resume` | App returns from OS-suspended state (user-initiated) |
| `suspend` | OS is suspending the app — must complete quickly |
| `terminate` | App about to be killed — not guaranteed to run on force kill |
| `back` | User pressed back while the app's root flow is at its initial state (no history to pop) |
| `open_url` | OS routed a deep link URL matching one of the app's `@handles` patterns |
| `crash_report` | The runtime caught an unrecoverable error in any app VM (system.crash_reporter only) |

**`@on back` — root back interception:**

Called only when the OS would otherwise suspend the app (i.e., the flow has no history left to pop and the bridge has already checked for dirty forms). The hook must return one of:

```
:handled                           -- app consumed the event; OS does nothing
:unhandled                         -- delegate to OS; app is suspended
:confirm                           -- OS shows a native confirm dialog (not the app — avoids
  prompt:  str_expr                   orphaned dialogs). confirm/cancel carry the atom to return
  confirm: label -> :handled | :unhandled
  cancel:  label -> :handled | :unhandled
```

The `prompt:` field is semantically parallel to `confirm`'s `prompt:` in content bodies (§12.4): the **question posed to the user**. Example — guard against leaving unsaved work:

```
@on back
  match unsaved_changes()
    | true  ->
        :confirm
          prompt:  "Discard changes?"
          confirm: "Leave"   -> :unhandled
          cancel:  "Stay"    -> :handled
    | false -> :unhandled
```

The bridge evaluates `@on back` **after** the flow history check and **after** the dirty-form check (§6.2 of `10-deck-bridge-ui.md`). If neither intercepted the back event, `@on back` runs. If the hook returns `:unhandled` (or is absent), the OS suspends the app.

OS-declared events (from `.deck-os`) can also appear in `@on`. Three binding styles are available, matching the patterns used elsewhere in Deck:

| Style | Example | Semantic |
|---|---|---|
| **No params** | `@on os.locked` | Handler fires for every event; access payload via the implicit `event` binding — e.g. `event.field`, `event.value` — where the field names come from the `@event` declaration in the `.deck-os` file. |
| **Named binders** | `@on os.wifi_changed (ssid: s, connected: c)` | `s` and `c` are regular value bindings for the whole handler body. Use `_` for fields whose value you do not need (`(orientation: _)`). |
| **Value patterns** | `@on hardware.button (id: 0, action: :press)` | Pattern-matches the event against the given values. The handler fires **only** when `id == 0` and `action == :press`; other events of the same type are dropped at dispatch time. Combine with binders for partial matches: `(id: n, action: :press)`. |

The binding style is chosen per handler; a single app may use all three for different events. The runtime picks the most specific matching `@on` handler; equal-specificity handlers declared for the same event are a load error.

```
@on hardware.button (id: 0, action: :press)
  App.send(:button_pressed)

@on os.display_rotated (orientation: _)
  -- Fired before ui_activity_recreate_all(). Use to reset any orientation-dependent state.
  -- Most apps don't need this — the bridge rebuilds screens automatically.
  App.send(:orientation_changed)

@on os.theme_changed (theme: _)
  -- Fired when the user switches UI theme. Bridge rebuilds screens automatically.
  -- Only needed if the app caches theme-derived values outside the bridge.
  App.send(:theme_changed)

@on os.wifi_changed (ssid: s, connected: c)
  match c
    | true  -> App.send(:wifi_up (ssid: s))
    | false -> App.send(:wifi_down)

@on os.locked
  -- App is still running but the lockscreen is now in front.
  App.send(:screen_locked)

@on os.storage_changed (mounted: m)
  -- SD card inserted or removed. Apps using fs/db must handle false gracefully.
  match m
    | false -> App.send(:storage_lost)
    | true  -> App.send(:storage_ready)
```

**Background fetch vs. resume**: `@on resume` fires only for user-initiated app returns (e.g., user switches back from another app). Background wakeups from `background_fetch` fire `@on os.background_fetch` — a separate event that can be distinguished:

```
@on os.background_fetch
  match App.state
    | :authenticated _ -> App.send(:background_refresh)
    | _                -> unit
```

**Notification events**: Apps that declare `notifications` in `@permissions` and register sources via `notif.register_source()` receive OS events when new notifications arrive:

```
@on os.notification (entry: NotifEntry)
  -- Called whether the app is foreground, suspended, or freshly relaunched.
  -- entry: { id, app_id, source_id, title, body, url?, received_at, read }
  App.send(:new_notification (entry: entry))
```

The hook fires even if the app is suspended — `svc_notifications` stores the entry in SQLite and the bridge delivers the event as a `MSG_OS_EVENT` to the VM. If the app is suspended, the bridge temporarily restores it for hook execution (same mechanism as `background: true` tasks — see §14.4). If the VM is destroyed (app terminated), the event is stored but not dispatched until the app re-launches and calls `notif.list()`.

**`@on open_url` — deep link entry point:**

Fired when the OS routes a URL to this app. The URL must match one of the patterns declared in the app's `@handles` block (see §20). Pattern parameters are extracted by the bridge and passed in `params`.

```
@on open_url (url: str, params: {str: str})
  -- url:    the full URL that was matched (e.g. "myapp://profile/abc123")
  -- params: named segments extracted from the matched pattern
  --         (e.g. { "id": "abc123" } for pattern "myapp://profile/{id}")
  match params["id"]
    | :some id -> App.send(:open_profile (id: id))
    | :none    -> App.send(:open_root)
```

If the app is suspended when the URL arrives, the OS resumes it (same path as `system.apps.bring_to_front`) and then fires `@on open_url`. If the app is terminated, it is launched first; `@on launch` runs to completion before `@on open_url` is dispatched.

If multiple installed apps register a `@handles` pattern matching the same URL, the OS shows a Choice Overlay (§5.5 of `10-deck-bridge-ui.md`) for the user to pick. There is no priority/ordering between apps — first install wins is **not** the rule; the user always disambiguates explicitly.

**`@on crash_report` — system-only crash sink:**

Restricted to the app whose `app.id` is `"system.crash_reporter"`. The OS fires this hook every time the runtime catches an unrecoverable error in any app VM. The Crash Reporter persists `info` via the `system.crashes` capability (§4.11 of `03-deck-os.md`).

```
@on crash_report (info: CrashInfo)
  -- info: { app_id, message, stack, occurred }
  -- The OS guarantees this hook runs even if the crashed app is the foreground app —
  -- the bridge temporarily resumes the Crash Reporter VM to deliver the event.
  CrashLog.send(:append (info: info))
```

A Loader error is raised at install time if any app other than `system.crash_reporter` declares `@on crash_report`.

**Load-time validation**: Every event name in an `@on` hook must exist in the OS event registry (declared in `.deck-os`). An `@on` referencing an unknown event name is a **load error**.

`@on` hooks are the bridge between OS events and the app state machine. The app machine itself does not observe OS events directly — `@on` hooks translate them into `send()` calls.

---

## 12. Content Bodies

The `content =` block inside any `@machine` state or `@flow` step is a semantic description of what the user perceives and can do in that context. It does not describe rendering — the OS decides how to present each element for the form factor (phone, smartwatch, voice, e-ink, terminal).

### 12.1 Structural Primitives

**`list`** — collection of items of the same type.

```
list expr
  empty ->
    content               -- perceived state when expr is []
  has_more: bool_expr     -- whether more items exist beyond the current `expr`
  on more -> action       -- how to load them; the OS exposes this per form factor
  var ->
    content               -- structure of each item
```

**`group`** — named semantic grouping. The label communicates the group's purpose to the OS; together with the types and intents inside, the OS decides how to present it.

```
group "label"
  content
```

Groups may nest. Depth communicates hierarchy.

**`form`** — cohesive set of inputs that are filled and submitted together. Tells the OS that the input intents contained belong to a single interaction flow. The OS may present them as a flow, sequential conversation, or traditional form depending on form factor.

```
form
  on submit -> action
  content               -- intents of type text, choice, range, password, pin, date...
```

Example — login flow with 2FA expressed as `@flow`:

```
@flow LoginFlow
  state :credentials
  state :two_factor (email: str)
  state :done       (handle: str)
  state :failed     (message: str)
  initial :credentials

  transition :submit (handle: str, password: str)
    from :credentials
    to   :two_factor (email: auth.email_for(handle))

  transition :verify (code: str)
    from :two_factor _
    to   :done (handle: auth.handle())

  transition :error (message: str)
    from *
    to   :failed (message: message)

  transition :retry
    from :failed _
    to   :credentials

  step :credentials ->
    text     :handle    hint: "Handle or email"   on -> LoginFlow.send(:set_handle, v: event.value)
    password :password  hint: "Password"           on -> LoginFlow.send(:set_password, v: event.value)
    trigger "Sign in" -> auth.login(...)

  step :two_factor s ->
    rich_text "Enter the code sent to {s.email}"
    pin :code  length: 6  on -> LoginFlow.send(:verify, code: event.value)

  step :done s ->
    rich_text "Welcome, {s.handle}!"
    trigger "Continue" -> App.send(:authenticated)

  step :failed e ->
    error reason: e.message
    trigger "Try again" -> LoginFlow.send(:retry)
```

---

### 12.2 State

Semantic markers for the perceived state of the view. Used inside `match` arms over machine states. The OS maps them to its loading/error patterns for the device.

```
loading                     -- the system is working; the user is waiting
error reason: str_expr      -- something went wrong; the app declares WHY so the OS can present it
```

The rename from `message:` to `reason:` is deliberate: the app is not authoring a "message to display" (that would be a presentation concern); it is declaring the **reason** the error exists. A voice bridge may speak it; a screen bridge may dim-wrap it; a logger bridge may pipe it to telemetry. All three consume the same semantic reason.

Example — a fetch flow with a loading view during the request and an error view on failure:

```
step :fetching ->
  loading

step :loaded s ->
  s.title
  rich_text s.body

step :failed e ->
  error reason: e.why
  trigger "Try again" -> Fetch.send(:retry)
```

Neither `loading` nor `error` carries any presentation field. `loading` has no fields at all — the bridge picks the affordance (animated cursor on this device, spoken "One moment…" on a voice device, a single underscore on a terminal). `error` has the single semantic `reason:` and nothing else; colour, icon, tone, dismissal timing are all bridge decisions.

---

### 12.3 Content

Data expressions are placed directly in the body. The OS uses the Deck type to decide how to present each value.

| Type | Presentation (examples) |
|---|---|
| `str` | Text |
| `int`, `float` | Numeric value |
| `bool` | Indicator / yes/no |
| `Timestamp` | Formatted date/time, relative age |
| `Duration` | Human-readable duration |
| `@type` record | Structured presentation derived from field names and types |

**Semantic wrappers** — when the type alone is not enough:

```
media expr
  alt:  str_expr      -- required; accessibility label
  role: atom?         -- semantic role: :avatar :cover :thumbnail :inline
```

The field was `hint:` in earlier drafts. Renamed to `role:` because the atom declares the media's **role** in the UI, not a suggestion — it is authoritative semantic input the bridge uses to decide size, framing, and placement.

`expr` is a URL (`str`) or binary (`[byte]`). The OS infers the medium (image, video, audio) from the content type. Images from URLs are cached by the OS renderer; no `!http` is needed in the view.

```
rich_text expr
```

`expr` is a `str` that may contain mentions, hashtags, links, or markup. The OS parses and renders according to the device (active links on screen, spoken annotations on voice).

```
status expr  label: str_expr
```

`expr` is `bool` or an atom representing a state the user must perceive: connected, active, paused, online, loading. `label` names the observed thing. The OS renders as a semantic indicator.

```
chart expr
  label:   str_expr?
  x_label: str_expr?
  y_label: str_expr?
```

`expr` must be `[float]` or `[(Timestamp, float)]`. The OS decides the appropriate visualization for the device (line, bars, ASCII sparkline, spoken trend summary).

```
progress value: float_expr  label: str_expr?
```

`value` is `0.0..1.0`. An in-progress operation with measurable progress.

**Markdown** — first-class. Two nodes, both purely semantic. The bridge infers density, fonts, code-block chrome, ToC visibility, image sizing, editor toolbar layout, preview placement, and every other presentation choice from the declared purpose, the content itself, and the device.

```
markdown content_expr
  -- content_expr: str | MdDocument | MdPatch?
  -- (types in 03-deck-os §3; pre-parse with md.parse() to control parser options
  --  or share a parse across multiple views)

  purpose:       :reading | :reference | :fragment   -- default :reading
  -- Semantic intent for this content:
  --   :reading   long-form prose meant to be read end-to-end (articles,
  --              docs, AI responses, e-reader pages)
  --   :reference lookup-style content the user scans (cookbook, manual,
  --              README inside a card)
  --   :fragment  small snippet inline within other content (chat bubble,
  --              list-row body, status panel)
  -- The bridge maps purpose to density, max-width, internal scroll behavior,
  -- ToC inclusion, and code-block affordances. Apps never specify any of those.

  on link  ->    expr        -- presence makes links tappable; event.url, event.text
  on image ->    expr        -- presence makes images tappable; event.url, event.alt

  focus:         str?        -- semantic: the id of the heading the user should be focused on;
                             -- reactive — the bridge brings it into view when the value changes.
                             -- (Renamed from `scroll_to:` — the app is not requesting a scroll
                             --  action, it is declaring the point of user attention; a non-scroll
                             --  bridge like a voice reader reads that heading aloud instead.)
  describe:      str?        -- accessible description of the region (renamed from `accessibility:`
                             -- for consistency: the app *describes* the semantic of the region;
                             -- the bridge decides how to surface that description — ARIA label,
                             -- spoken preamble, Braille annotation, etc.)
```

```
markdown_editor
  value:         str_expr           -- current content (from machine state)
  on change ->   expr               -- event.value: str
  on cursor ->   expr?              -- presence = app reacts to cursor
                                    --   event.cursor: int, event.formats: [atom]
  on selection -> expr?             -- presence = app reacts to selection
                                    --   event.selection: MdRange, event.text: str

  placeholder:   str?               -- semantic content shown when empty
  controlled_by: MdEditorState?     -- if set, the editor is externally controlled:
                                    --   the app owns the canonical editor state (via
                                    --   the markdown.editor_* capability in 03-deck-os §4.4)
                                    --   and the bridge mirrors it. (Renamed from
                                    --   `editor_state:` — the field declares a *relationship*
                                    --   ("this editor is controlled by X"), not a data name.)
  describe:      str?               -- accessible description of the region (same semantic as
                                    --   on `markdown`; renamed from `accessibility:`).
```

Both nodes accept their content (`content_expr` / `value`) as raw `str` or pre-parsed `MdDocument`. The `markdown` node also accepts `MdPatch?` from `markdown.stream_parse` for incremental rendering of streamed content (AI chat responses, live document sync).

**What the bridge decides** (every choice the app does NOT make):

| Concern | Bridge inference rule on this device |
|---|---|
| Density / spacing | Derived from `purpose:` — `:reading` gets generous, `:fragment` gets tight |
| Max width / readable measure | `:reading` gets a comfortable measure for the screen; `:reference` and `:fragment` fill their container |
| Internal scroll vs container scroll | `:reading` may scroll internally if it exceeds the screen; `:fragment` always fills naturally |
| Inline ToC | Auto-shown when `purpose: :reading` AND the document has ≥ 3 headings |
| Code block "copy" affordance | Auto-shown when the device has a clipboard and the document has code blocks |
| Code block syntax highlighting theme | Follows the global theme (`display.theme`); never per-node |
| Image sizing | Bridge sizes images to the available width with a sensible cap; no app-side px |
| Image fallback | Alt text shown when the asset is missing |
| Editor toolbar visibility / layout | Bridge decides per device — touch-only with small screen may use long-press menus instead of a toolbar; tablet-class shows full toolbar |
| Editor preview | Bridge picks `:none`/`:toggle`/`:side` based on screen size and orientation |
| Editor line numbers | Off on touch devices; configurable in Settings for keyboard-driven devices |
| Text selection | Enabled when the device supports it; never an app concern |
| Virtual rendering | Engaged automatically when content size exceeds the device's render budget |

The capability `markdown` (`03-deck-os §4.4`) provides programmatic editor-state operations (`editor_format`, `editor_undo`, `editor_select`, etc.) for apps that want explicit control of the editor. The view node still owns no presentation.

**Tagged literals.** Markdown source can be authored inline using the `md"""..."""` tagged multi-line string (see `01-deck-lang §2.7`). Semantically identical to `"""..."""`; the tag is a hint to editors and language servers for syntax highlighting only.

---

### 12.4 Intents

Intents declare what the user can do. Each has a semantic interaction type — not a widget type. The OS maps each type to the appropriate affordance for the form factor.

---

**`toggle`** — the user activates/deactivates a boolean state.
```
toggle name: atom  state: bool_expr  on -> action
```
`event.value: bool`. Uses: like, follow, mute, subscribe, enable setting.

---

**`range`** — the user controls a continuous numeric value within a range.
```
range name: atom  value: float_expr  min: float  max: float
  step: float?
  on -> action
```
`event.value: float`. Uses: volume, brightness, seek position, threshold.

---

**`choice`** — the user selects one option from a discrete set.
```
choice name: atom  value: expr  options: [(label: str, value: any)]
  on -> action
```
`event.value: any`. Uses: theme, language, sort order, quality.

---

**`multiselect`** — the user selects one or more options from a set.
```
multiselect name: atom  value: [expr]  options: [(label: str, value: any)]
  on -> action
```
`event.value: [any]`. Uses: filters, batch operations, multiple selection.

---

**`text`** — the user enters free text.
```
text name: atom  value: str?
  hint:       str_expr?
  max_length: int?
  format:     atom?     -- :email :phone :numeric :url (hint to the OS, not strict validation)
  on -> action
```
`event.value: str`. Uses: compose, edit name, enter URL.

---

**`password`** — the user enters text the OS must hide. Semantically distinct from `text`: the OS uses secure input, hides characters, may disable screenshots.
```
password name: atom  value: str?
  hint: str_expr?
  on -> action
```
`event.value: str`.

---

**`pin`** — the user enters a fixed-length numeric code. The OS may present a dedicated numpad, progress dots, voice entry with confirmation, or physical buttons.
```
pin name: atom  length: int
  on -> action
```
`event.value: str` (digits as string). The handler is invoked when the user completes the `length` digits; not invoked on each digit.

---

**`date`** — the user selects a date/time. Produces a `Timestamp`, not a string. The OS decides the affordance: calendar, wheels, voice "pick a date", etc.
```
date name: atom  value: Timestamp?
  hint: str_expr?
  on -> action
```
`event.value: Timestamp`.

---

**`search`** — the user filters or queries content. Semantically distinct from `text`: the OS may offer autocomplete, history, voice search.
```
search name: atom  value: str?
  hint: str_expr?
  on -> action
```
`event.value: str`.

---

**`navigate`** — the user wants to go to a related context. The `->` action sends to a machine or flow to trigger a state transition. The OS treats `navigate` semantically — it knows the user intends movement to a new context, not a pure action.
```
navigate label: str  badge: int?  -> action_expr
```
`action_expr` is typically a `Machine.send(...)` call that triggers a navigation transition in the app's root flow. `badge: int?` (optional) mirrors the one on `trigger` — a count the bridge may surface on the navigation affordance. The OS may render navigate as a disclosure affordance, transition animation hint, or navigation gesture.

---

**`trigger`** — the user initiates an action without navigation or confirmation.
```
trigger label: str  badge: int?  -> action
```
`badge: int?` (optional) — a count the app wants associated with the trigger (e.g. pending actions, unread items). The bridge decides how/whether to render it; voice bridges may verbalize "3 new" before the label. Uses: refresh, retry, sync, mark as read, play/pause.

---

**`confirm`** — the user initiates a significant or irreversible action. The OS interposes a confirmation; how depends on the form factor (dialog, voice confirmation, double-press).
```
confirm label: str  prompt: str_expr  -> action
```
`prompt:` is the semantic explanation of what the user is being asked to confirm (e.g. `"Permanently delete {file}?"`). Earlier drafts called this `message:`; renamed to `prompt:` because the app is declaring the **question posed to the user**, not a generic body-text message — a voice bridge will phrase it as a question, a screen bridge will put it in the dialog body, a logger bridge will record it as the `prompt` field of the audit event. Uses: delete, block, sign out, reset.

---

**`create`** — the user initiates creation of a new entity. Semantically distinct from `navigate`: the destination is a blank creation context. The OS may render this with a distinct affordance (FAB, "+" button, voice "create new").
```
create label: str  -> action_expr
```
Uses: compose post, add contact, new document.

---

**`share`** — the user shares content via the OS share mechanism.
```
share expr  label: str?
```

---

### 12.5 Scope

The position of a declaration in the content determines its scope:

- **Context-level** — direct child of `content =` (not inside a `list` item or `group`): applies to the whole state context. The OS places it as a primary action (FAB, voice command, toolbar, physical key).
- **Item-level** — inside the body of a `list` item: applies to that specific item. The OS exposes it through item affordances (tap, swipe, long-press, contextual voice command).
- **Group-level** — inside a `group`: scoped to that group's semantic context. The OS may collapse group intents behind a secondary action (overflow menu, "more options", voice "options for [label]").

---

### 12.6 Language Integration

`match`, `let`, `when`, and `for` work naturally inside content:

```
content =
  match Timeline.state
    | :loading   -> loading
    | :error e   -> error reason: e.message
    | :loaded s  ->
        list s.posts
          has_more: s.cursor is :some
          on more -> Timeline.send(:paginate)
          p ->
            group "author"
              media p.author.avatar  alt: p.author.handle  role: :avatar
              p.author.display_name
              p.author.handle
            rich_text p.text
            p.created_at
            group "reactions"
              toggle :liked    state: p.liked_by_me    on -> interaction.toggle_like(p)
              toggle :reposted state: p.reposted_by_me on -> interaction.toggle_repost(p)
              navigate "Reply" -> App.send(:open_compose, reply_to: p)
            group "options"
              confirm "Report"     prompt: "Report this post?"         -> moderation.report_post(p)
              confirm "Block user" prompt: "Block @{p.author.handle}?" -> moderation.block(p.author)
    | _ -> unit

  create "Compose" -> App.send(:open_compose)     -- context-level; always available
```

`when bool_expr content*` — conditional content inclusion. Sugar for `match bool_expr | true -> (nodes) | false -> unit`. Produces the body nodes only when the condition is true; produces nothing otherwise. Valid only inside content bodies (see `01-deck-lang §7.6`).

`for binding in list_expr content*` — inline content iteration. Evaluates the body for each element, producing a flat sequence of nodes. `for` is equivalent to `list` without `more:` and `empty ->` semantics. Use `list` when the OS should treat the collection as a navigable, potentially paginated data set; use `for` for repeated inline content inside a larger structure.

View content functions (§6.3 of `01-deck-lang`) may be called freely inside content bodies. Their nodes are spliced inline at the call site. Business logic (`let`, `match`, pure functions) may appear alongside content nodes anywhere in the body — the evaluator resolves the full body to a `[ViewContentNode]` sequence.

---

### 12.7 `event` Binding

Inside an `on … ->` handler, `event` is an implicit binding carrying the payload specific to that handler's triggering condition. The binding shape is handler-specific — not a single `event.value` scalar for every case.

**Input intents (§12.4)** — all carry a single `value`:

| Intent | Binding |
|---|---|
| `toggle` | `event.value: bool` |
| `range` | `event.value: float` |
| `choice` | `event.value: any` (the selected option's `value:` field) |
| `multiselect` | `event.value: [any]` |
| `text` | `event.value: str` |
| `password` | `event.value: str` |
| `pin` | `event.value: str` (digits; handler invoked on completion) |
| `date` | `event.value: Timestamp` |
| `search` | `event.value: str` |

`navigate`, `trigger`, `confirm`, `create`, `share` do **not** bind `event` — they are pure actions without user-supplied data.

**Structural handlers**:

| Handler | Binding |
|---|---|
| `form on submit ->` | `event.values: {str: any}` — map of input `name: atom` → submitted value. Keys are the atom names declared on each intent inside the form (e.g. `:handle`, `:password`). Values follow each intent's `event.value` shape above. |
| `list on more ->` | `event.page: int` (0-based; 0 is the initial page, incremented each time the user requests more). The handler is expected to extend the list's source expression with the next batch; the bridge waits on any effects before rendering further. |

**Content / annotation handlers (§12.3)**:

| Handler | Binding |
|---|---|
| `markdown on link ->` | `event.url: str`, `event.text: str` (the visible anchor text) |
| `markdown on image ->` | `event.url: str`, `event.alt: str` |
| `markdown_editor on change ->` | `event.value: str` (new full content) |
| `markdown_editor on cursor ->` | `event.cursor: int` (char offset), `event.formats: [atom]` (active formatting at cursor) |
| `markdown_editor on selection ->` | `event.selection: MdRange`, `event.text: str` (selected text) |

**Stream handlers** (`on StreamName var ->`):

| Handler | Binding |
|---|---|
| `on StreamName v ->` | `v` is the emitted value; there is **no** implicit `event` binding inside this handler. Use the named binder `v` directly. |

Handlers not listed here do not bind `event`.

---

## 13. Navigation Topology

Navigation topology is expressed through the structure of the root `@flow` (or `@machine`) named in `@app entry:`. The OS infers affordances from the topology — Deck never declares "use a tab bar" or "use a stack." The structure is the semantics. See §9.3 for the inference rules and examples.

---

## 14. @task — Background Work

```
@task TaskName
  every:      Duration
  when:       condition_expr
  priority:   :high | :normal | :low | :background
  battery:    :normal | :efficient
  background: bool     -- default false; true = runs independently of main task

  run =
    expr_or_do_block
```

**`background: true`** — the task runs in its own execution context, independent of the main task. It continues running even when the main task is suspended. Effects (`!http`, `!db`, etc.) and `Machine.send()` calls work normally. Without `background: true`, the task runs in the main task context and suspends with it.

**Background → main communication**: A background task may call `Machine.send()` on any app-level machine. If the main task is suspended, the machine state updates immediately; the UI re-renders when the main task resumes. The developer does not write restoration code.

### 14.1 Scheduling

**`every:`** only: runs unconditionally at each interval.
**`when:`** only: runs once per false→true transition of the condition. If the condition becomes false and then true again, the task runs again. If you need "run exactly once ever", track completion in NVS.
**Both**: runs at each interval only when all `when:` conditions are currently true.
**Multiple `when:` clauses**: all must be true (logical AND).

**Behavior at app launch**: `when:`-only tasks are evaluated once at launch time. If the condition is already `true` when the app starts, the task fires immediately — the initial evaluation is treated as a false→true transition from the "not yet evaluated" state. This means a task like `when: App is :authenticated` will fire on first launch if the app launches directly into the authenticated state.

### 14.2 Priority and Battery

`:high` — run as soon as conditions are met.
`:normal` — default.
`:low` — can be deferred if system is busy.
`:background` — only when app is backgrounded and OS permits.

`:efficient` battery hint — OS may batch with other background work to conserve power.

### 14.3 Task Body

May use any `!effect` capability declared in `@use`. Runs in its own effect context. Unhandled errors are logged; the task is eligible to run again on the next trigger. Tasks do not crash the app.

### 14.4 Task Lifecycle and App State

Tasks are **app-level constructs**, not flow-level. Their lifecycle is tied to the app VM, not to which `@flow` state is currently active.

**App in foreground**: all tasks (main and background) are eligible to run when their conditions are met.

**App suspended** (OS backgrounds the app):
- `background: false` (default — "main task"): the timer keeps ticking, but when it fires the runtime holds the pending run until the app is next foregrounded. The task doesn't execute while the app is suspended.
- `background: true`: the task executes even while the app is suspended. The runtime temporarily restores the VM, runs the task body (including `!effect` calls and `Machine.send()`), then re-suspends. The UI does not re-render until the app returns to the foreground — but machine state is already up to date when it does, so no restoration code is needed.

**Navigation is orthogonal to task scheduling.** Moving between flow states — navigating to `:compose`, going back via `to history`, entering a sub-flow — does not pause or affect any `@task`. The only thing navigation changes in the scheduler is that `when:` conditions referencing machine state are re-evaluated after each `Machine.send()` (see §8.3 of 04-deck-runtime.md).

**App terminated**: all tasks stop immediately. Timers are cancelled. A background task in the middle of a run gets 500ms (same as `@on terminate`) before the VM is force-killed.

---

## 15. @migration — Data Evolution

Runs when the app is updated before the new version starts. Migrations are indexed by integer schema version, tracked per-app by the OS. Each `from N:` block runs exactly once per device, in ascending key order, the first time the app is loaded with a schema version higher than `N`.

```
@migration
  from 0:
    store.set("new_key", unwrap_or(store.get("old_key"), ""))
    store.delete("old_key")
  from 1:
    db.exec("ALTER TABLE posts ADD COLUMN score REAL DEFAULT 0.0")
  from 2:
    db.exec("CREATE INDEX IF NOT EXISTS idx_posts_score ON posts(score)")
```

Migration versions are plain integers — not the `@app.version` semver. The OS stores the highest `N` run for each app; on next load, every `from K >= stored` block runs in ascending order, then stores `max(K) + 1`. Integer versioning keeps the model minimal: no range parser, no wildcard matching, one authoritative counter per app. The loader refuses to load the app if any `from N:` body returns an error — migration is all-or-nothing.

**Migration execution context**: Migrations run before `@on launch`, before the app's capability aliases are in scope. The following capabilities are always available inside a `@migration` body under their canonical names, regardless of what aliases are declared in `@use`:

| Name in migration | Capability |
|---|---|
| `store` | `storage.local` |
| `db` | `db` (SQLite) |
| `nvs` | `nvs` |
| `config` | app `@config` (write via `config.set` only) |

Available operations inside `@migration`:
- `store.*` — `storage.local` operations (canonical name `store`, not the alias from `@use`)
- `db.*` — SQLite operations (schema changes, data transforms)
- `nvs.*` — NVS operations
- `config.set(field: str, value: any)` — set a config field to a new default value

**Ordering**: On each app load the OS reads the stored schema version `S` (default `0` for a newly installed app), then runs every `from N:` block where `N >= S` in ascending `N` order. After all bodies succeed, the OS stores `max(N) + 1` as the new schema version. If any body errors, the stored version is left at `S` — the migration can be retried on the next load after the bug is fixed, and no half-migrated state is committed.

If a migration fails (returns `:err` or panics), the app does not start. The OS reports the failure with the migration's `from:` range and the error. The migration is not re-attempted on subsequent launches; the failed state is reported to the user until a new app version is installed.

---

## 16. @test — Inline Tests

```
@test "description"
  assertion
  assertion

assert expr
assert expr == expected
assert expr is :atom
assert expr is :ok
assert expr is :err
assert not expr
```

Test helpers:
```
MachineName.simulate(:state_name)
MachineName.simulate(:state_name (field: value))
  -- creates a machine instance frozen in the given state

let ctx = simulate
  battery: 15%
  network: :offline
  App:     :active (temp: 25.0, max: 25.0)

TaskName.would_run(ctx)   -- bool: would this task fire under this context?
```

In test mode (`deck test`), `random.seed(42)` is called automatically unless overridden.

---

## 17. @doc and @example

```
@doc "Module description."      -- at file level

fn celsius (t: float) -> str
  @doc "Format temperature with Celsius unit symbol."
  @example celsius(100.0) == "100.0°C"
  @example celsius(-10.0) == "-10.0°C"
  = "{math.round(t, 1)}°C"
```

`@doc` and `@example` appear between the function signature and `=` body. `@example` assertions are run automatically in `deck test` mode.

---

## 18. Complete Example

```deck
-- app.deck

@app
  name:    "Ambient Monitor"
  id:      "mx.lab.ambient"
  version: "2.0.0"
  entry:   App

@use
  sensors.temperature as temp
  sensors.humidity    as humidity  optional
  storage.local       as store
  network.http        as http      when: network is :connected
  display.notify      as notify
  db                  as db
  ./machines/sensor
  ./flows/app
  ./flows/main
  ./flows/history

@permissions
  sensors.temperature reason: "Read ambient temperature from sensor"
  storage.local       reason: "Store reading history locally"
  network.http        reason: "Sync data to your server"
  db                  reason: "Maintain local history database"

@config
  alert_threshold : float = 75.0  range: 40.0..95.0  unit: "°C"
  sync_interval   : int   = 30    range: 5..300       unit: "s"
  notifications   : bool  = true

@errors sensor
  :unavailable  "Sensor not responding"
  :out_of_range "Reading outside calibrated range"

@stream TempStream
  source: temp.watch(hz: 1)

@stream HumidityStream
  source: humidity.watch(hz: 1)

@on launch
  do
    db.schema("""
      CREATE TABLE IF NOT EXISTS readings (
        id    INTEGER PRIMARY KEY AUTOINCREMENT,
        value REAL    NOT NULL,
        ts    TEXT    NOT NULL
      )
    """)
    match temp.read()
      | :ok v  -> Sensor.send(:ready, first: v)
      | :err _ -> Sensor.send(:sensor_lost)

@on suspend
  store.set("last_reading", str(unwrap_or(TempStream.last(), 0.0)))


-- machines/sensor.deck

@machine Sensor
  state :booting
  state :active   (temp: float, max: float)
  state :alert    (temp: float, max: float)
  state :no_sensor

  initial :booting

  transition :ready (first: float)
    from :booting
    to   :active (temp: first, max: first)

  transition :update (t: float)
    from :active s
    to   :active (temp: t, max: math.max(t, s.max))
    when: t <= config.alert_threshold

  transition :update (t: float)
    from :active s
    to   :alert (temp: t, max: math.max(t, s.max))
    when: t > config.alert_threshold

  transition :update (t: float)
    from :alert s
    to   :alert (temp: t, max: math.max(t, s.max))

  transition :acknowledge
    from :alert s
    to   :active (temp: s.temp, max: s.max)

  transition :sensor_lost
    from *
    to   :no_sensor

  transition :sensor_found (first: float)
    from :no_sensor
    to   :active (temp: first, max: first)


-- flows/app.deck

@flow App
  state :main    flow: MainFlow
  state :history flow: HistoryFlow

  initial :main

  transition :go_history
    from :main
    to   :history

  transition :go_main
    from :history
    to   :main


-- flows/main.deck

@flow MainFlow
  @task PollSensor
    every: 1s
    run = do
      match temp.read()
        | :ok v  -> Sensor.send(:update, t: v)
        | :err _ -> Sensor.send(:sensor_lost)

  state :dashboard
  initial :dashboard

  step :dashboard ->
    match Sensor.state
      | :booting   -> loading
      | :no_sensor -> error reason: "Sensor not responding"
      | :alert s   ->
          status true  label: "Temperature alert"
          "{s.temp}°C (max recorded: {s.max}°C)"
          match HumidityStream.last()
            | :some h -> "Humidity: {h}%"
            | :none   -> unit
          chart TempStream.recent(60)
            label: "Last minute"
            y_label: "°C"
          confirm "Dismiss alert" prompt: "Clear the temperature alert?" ->
            Sensor.send(:acknowledge)
          navigate "History" -> App.send(:go_history)
      | :active s  ->
          "{s.temp}°C"
          "Max: {s.max}°C"
          match HumidityStream.last()
            | :some h -> "Humidity: {h}%"
            | :none   -> unit
          chart TempStream.recent(60)
            label: "Last minute"
            y_label: "°C"
          navigate "History" -> App.send(:go_history)


-- flows/history.deck

@flow HistoryFlow
  state :list
  initial :list

  step :list ->
    match db.query("SELECT value, ts FROM readings ORDER BY ts DESC LIMIT 100")
      | :loading -> loading
      | :ok rows ->
          list rows
            empty ->
              "No readings recorded yet."
            r ->
              "{r.value}°C"
              r.ts
          navigate "Back" -> App.send(:go_main)
```

**What the topology expresses:** `App` is a 2-state peer flow (tab bar or sidebar on phone/tablet). `MainFlow` reads reactively from `Sensor` and both streams — no `@listens` needed. `PollSensor` is flow-scoped: active only while `MainFlow` is on the stack. `HistoryFlow` queries the DB on demand. The OS infers all affordances from the structure.

---

## 19. @assets — Bundle y Recursos Estáticos

### 19.1 Estructura del Bundle

Una app en SD card es un directorio. El Loader lo trata como una unidad:

```
/apps/myapp/
  app.deck               ← entrada obligatoria
  flows/
    *.deck
  assets/
    icon.png             ← 64×64 PNG mínimo; usado en launcher y task switcher
    icon@2x.png          ← 128×128 para alta densidad (opcional)
    splash.png           ← mostrado durante @on launch (opcional)
    certs/
      api.pem            ← CA certificate para conexiones TLS
      client.pem         ← client cert para mutual TLS (si se necesita)
    fonts/
      custom.ttf         ← fuente personalizada (si el bridge la soporta)
    audio/
      beep.wav           ← recurso de audio (si el device tiene audio)
    data/
      seed.db            ← SQLite pre-poblado; se copia al app storage en first launch
```

`assets/` es el único directorio accesible desde Deck code. El Loader rechaza cualquier path fuera de este directorio.

### 19.2 La Anotación @assets

Declarada en `app.deck`. Enumera todos los recursos estáticos que la app necesita:

```deck
@assets
  required:
    icon:         assets/icon.png                  as :icon
    bsky_ca:      assets/certs/bsky_ca.pem         as :cert_bsky        for_domain: "bsky.social"
    bsky_client:  assets/certs/bsky_client.pem     as :client_cert_bsky for_domain: "bsky.social"
    plc_ca:       assets/certs/plc_ca.pem          as :cert_plc         for_domain: "plc.directory"

  optional:
    splash:       assets/splash.png                as :splash
    custom_font:  assets/fonts/custom.ttf          as :custom_font
    beep:         assets/audio/beep.wav            as :beep

  data:
    seed_db: assets/data/seed.db  as :seed_db
             copy_to: "database.db"
             on: :first_launch
```

- **`required:`** — deben existir al cargar. Si falta cualquiera, el Loader falla en Stage 0 listando todos los archivos ausentes. La app no se lanza.
- **`optional:`** — pueden estar ausentes. En runtime, `asset(:name)` retorna `:err :not_found` si el archivo no existe. La app maneja esto normalmente.
- **`data:`** — archivos copiados al app storage antes de que corra `@on launch`. `copy_to:` es relativo al directorio de storage de la app. `on: :first_launch` significa "copiar solo si el destino no existe todavía" — no sobreescribe datos del usuario en actualizaciones.
- **`for_domain: "hostname"`** — asocia el cert al hostname dado. El OS construye un TLS trust map al cargar la app; `api_client` y `net.get()` lo usan automáticamente. Ver §19.7.

### 19.3 Tipos de Asset

| Átomo convencional | Contenido | Cómo lo usa el bridge |
|---|---|---|
| `:icon` | PNG RGBA, 64×64 mínimo | Launcher grid, task switcher |
| `:splash` | PNG, cualquier tamaño | Pantalla durante @on launch |
| `:image_*` | PNG / JPEG | Nodos `VCMedia` |
| `:cert_*` | PEM (CA certificate) | TLS trust map automático si tiene `for_domain:`; o pasado manualmente en `net.get()` |
| `:client_cert_*` | PEM (client certificate) | Mutual TLS — igual que `:cert_*` con `for_domain:`, o pasado manualmente |
| `:font_*` | TTF / OTF | Fuente personalizada, registrada en el bridge renderer |
| `:audio_*` | WAV / MP3 | `system.audio.play()` capability |
| `:seed_db` | SQLite 3 | Copiado a app storage en first launch |

El nombre del átomo (`as :icon`, `as :api_cert`) es libre — lo define el developer. La convención de la tabla no es forzada por el Loader. La única excepción es `:icon`: el OS busca exactamente ese átomo para mostrar el ícono en el launcher.

### 19.4 Referenciando Assets en Deck Code

Los tres builtins del módulo `assets` están siempre en scope cuando la app tiene un bloque `@assets` — sin `@use`, sin `!effect`. Son errores de compilación llamarlos desde una app sin `@assets`. Declaración formal en `03-deck-os §3`:

| Builtin | Firma | Notas |
|---|---|---|
| `asset` | `(name: atom) -> AssetRef` | Pure. Falla en compilación si el átomo no está declarado en `@assets`. |
| `asset_bytes` | `(name: atom) -> Result [byte] :not_found` | Carga el archivo completo en heap de Deck. Solo para assets pequeños. |
| `asset_from_bytes` | `(data: [byte]) -> AssetRef` | Envuelve bytes en memoria como AssetRef efímero. El bridge copia los bytes. |

`AssetRef` es un tipo opaco (`@opaque AssetRef` en `03-deck-os §3`). El Deck heap solo guarda el handle; el bridge resuelve el path real cuando la capability lo necesita. El heap nunca contiene bytes crudos de imágenes, certs, ni audio salvo llamada explícita a `asset_bytes`.

```deck
-- En un content= body (contexto puro)
media
  source: asset(:splash)
  alt:    "App logo"

-- En un @on hook o @task (con efectos)
-- Si el dominio tiene for_domain: en @assets, los certs se aplican automáticamente:
net.get("https://bsky.social/xrpc/app.bsky.feed.getTimeline")

-- Para dominios sin for_domain:, o para override puntual:
net.get(url, HttpOptions { tls_ca_cert: asset(:cert_bsky) })
net.get(url, HttpOptions { tls_ca_cert: asset(:cert_bsky), tls_client_cert: asset(:client_cert_bsky) })
```

### 19.5 Assets Descargables

Los assets que no van en el bundle se gestionan con `net` y `fs` — no son `@assets`. El patrón habitual es fetch-and-cache:

```deck
fn ensure_avatar (did: str) -> Result AssetRef net.Error !net !fs =
  let path = "avatars/{did}.png"
  match fs.exists(path)
    | true  -> :ok (asset_from_bytes(fs.read_bytes(path) |> unwrap_or([])))
    | false ->
        match net.get("https://cdn.bsky.app/img/avatar/{did}")
          | :ok r ->
              do
                fs.write_bytes(path, r.body_bytes)
              :ok (asset_from_bytes(r.body_bytes))
          | :err e -> :err e
```

Para archivos grandes (firmware, bases de datos, video), usar `net.download(url, dest_path)` en lugar de `net.get()` — escribe directamente al filesystem sin pasar por el heap de Deck.

### 19.6 Verificación del Loader (Stage 0)

```
Stage 0: ASSET VERIFICATION (antes de todo lo demás)
  Para cada required: entry:
    Verificar que assets/<path> exista en el directorio de la app
    Verificar que sea legible
    Si falta o no es legible → load error (listar TODOS los ausentes de una vez)
  Para cada optional: entry:
    Verificar existencia; si falta → marcar como :not_found en el asset registry
  Para cada data: entry:
    Verificar que el source exista (se aplican las mismas reglas required/optional)
    Si copy_to + on: :first_launch:
      Verificar si el destino existe en app storage
      Si no existe → encolar copia (el bridge la ejecuta antes de @on launch)
  Para cada entry con for_domain::
    Verificar que el átomo sea :cert_* o :client_cert_*
    Si no → load error "for_domain: only valid on cert assets"
    Añadir al TLS trust map del app: domain → {ca?, client?}
    El trust map persiste en la instancia de capability (net, api_client)
    hasta que el app termina — no se reconstruye en suspend/resume
```

### 19.7 TLS Trust Map — Certs por Dominio

Cuando una entrada de `@assets` declara `for_domain: "hostname"`, el OS construye un **TLS trust map** durante el Stage 0 y lo pasa a las capabilities `net` y `api_client` al inicializar la instancia de esa app. Desde ese momento:

- Toda conexión TLS a ese hostname usa automáticamente el CA cert asociado (verificación de servidor)
- Si hay un `:client_cert_*` con el mismo `for_domain:`, se usa también para mutual TLS sin que el código Deck lo especifique

```
TLS trust map (por instancia de app):
  "bsky.social"    → { ca: AssetRef(:cert_bsky),  client: AssetRef(:client_cert_bsky) }
  "plc.directory"  → { ca: AssetRef(:cert_plc),   client: nil }
```

**Matching de dominio:**
- Exacto: `"bsky.social"` aplica solo a `bsky.social`, no a `cdn.bsky.social`
- Wildcard: `"*.bsky.social"` aplica a cualquier subdominio directo (`cdn.bsky.social`, `api.bsky.social`), no al root ni a subdominios de segundo nivel

**Override manual:** pasar `tls_ca_cert: asset(:name)` en `HttpOptions` en `net.get()` / `net.post()` tiene precedencia sobre el trust map para esa llamada específica.

**El trust map no persiste entre lanzamientos.** Es un artefacto de la instancia de capability en memoria — se destruye con la VM en `@on terminate`. No hay estado en NVS ni en storage. Cada launch reconstruye el trust map desde `@assets`.

La verificación de assets ocurre antes del module graph (Stage 1), antes del OS surface (Stage 2), y antes de cualquier type checking. Una app que no puede cargar sus certificados no vale la pena parsear.

### 19.8 Icono — Contrato Específico

El ícono (declarado `as: :icon`) tiene un contrato especial porque el OS lo usa cuando la app no está corriendo:

- **Formato**: PNG, RGBA o RGB, sin animación
- **Tamaño mínimo**: 64×64 px. El bridge escala según necesite.
- **Tamaño recomendado**: proveer también `assets/icon@2x.png` (128×128); el bridge elige el mejor
- **Alpha**: soportado; el launcher lo pone sobre el color de fondo del sistema
- **Átomo exacto**: debe declararse `as: :icon`. Cualquier otro nombre lo convierte en imagen ordinaria

Apps sin `:icon` reciben el placeholder del sistema (el carácter `?` del bridge renderer del launcher).

---

## 20. @handles — Deep Link Patterns

`@handles` declares the URL patterns that route to this app via `@on open_url` (§11). It is a **top-level annotation** in `app.deck` (not nested inside `@app`) and not allowed in any other `.deck` file in the bundle.

```deck
@handles
  "myapp://profile/{id}"
  "myapp://post/{uri}"
  "https://myapp.example.com/profile/{id}"
```

### 20.1 Pattern Syntax

A handler entry is a string literal with the following grammar:

```
pattern   ::= scheme "://" host? path?
scheme    ::= identifier ("+" identifier)*
host      ::= literal | "{" name "}"
path      ::= ("/" segment)*
segment   ::= literal | "{" name "}" | "{" name ":" kind "}"
kind      ::= "rest"           -- captures the remainder of the path including slashes
literal   ::= [a-zA-Z0-9._~%-]+
name      ::= identifier
```

- `{name}` matches one path segment (no slashes) and binds it under `name`.
- `{name:rest}` matches everything from that point (including slashes) and is allowed only as the last segment.
- A pattern with no `{...}` placeholders matches exactly one URL.
- The query string is never matched against the pattern. Query parameters are collected into `params` keyed by name (last-write-wins on duplicates).
- Fragments (`#…`) are stripped before matching and not exposed.

### 20.2 Resolution Order

When a URL arrives:

1. The OS scans the patterns of every installed app.
2. Apps whose pattern matches the URL form the **candidate set**.
3. **Candidate set size:**
   - **0 candidates** → the OS shows a Toast `"NO APP HANDLES THIS URL"` (1500 ms) and discards the URL.
   - **1 candidate** → the OS launches/resumes the app and fires `@on open_url`.
   - **>1 candidate** → the OS shows a Choice Overlay (§5.5 of `10-deck-bridge-ui.md`) listing the candidate apps; the user picks one. There is no implicit priority based on install order, app id, or pattern specificity.

Within a single app, if multiple patterns of the same app match, the **most specific** one wins (literal segments are scored higher than `{name}` segments; `{name:rest}` ranks lowest). If two patterns of the same app are equally specific the loader rejects the bundle at install time.

### 20.3 Parameter Extraction

The bridge builds a `{str: str}` map from the matched pattern:

```deck
@handles
  "bsky://profile/{handle}/post/{rkey}"

@on open_url (url: str, params: {str: str})
  -- For url = "bsky://profile/alice.bsky.social/post/3kabc"
  -- params = { "handle": "alice.bsky.social", "rkey": "3kabc" }
  let handle = params["handle"] |> unwrap
  let rkey   = params["rkey"]   |> unwrap
  App.send(:open_thread (handle: handle, rkey: rkey))
```

Values are URL-decoded once before being placed into the map. Apps that need the raw URL inspect `url`.

### 20.4 Validation Rules

The Loader rejects an app at install time if:

- A pattern fails to parse against the grammar above.
- Two patterns within the same app are equally specific (ambiguous within the app).
- A pattern references the `https`/`http` scheme without a host (`https:///foo` is invalid — the host segment is required for web schemes).
- A pattern reserves a system scheme (`system://`, `deck://`, `os://`).
- The app declares `@handles` but no `@on open_url` hook (or vice versa). Both must be present together.

### 20.5 Privacy and Trust

Deep links are an **untrusted entry point.** A URL can come from any app, an NFC tag, a QR scan, or a notification. The receiving app must validate `params` before performing destructive or sensitive actions. The OS does not authenticate the source of the URL.

For `https://` patterns specifically, the OS does not verify that the receiving app actually owns the domain — there is no Universal Links / App Links equivalent on this platform. Apps that handle web URLs should treat them as informational links and never auto-execute privileged operations on receipt.

### 20.6 Triggering a Deep Link from Another App

Use `system.apps.launch_url(id, url)` from a privileged app (Launcher, Settings) to route a URL to a specific app, bypassing the candidate selection. From a regular app, `system.shell` is unavailable; a non-system app can only emit URLs by inviting the user to share/open them through the system Share Sheet (§5.12 of `10-deck-bridge-ui.md`).
