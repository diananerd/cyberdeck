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
| `@nav` | ✓ | |
| `@migration` | ✓ | |
| `@errors` | | ✓ |
| `@machine` | | ✓ |
| `@stream` | | ✓ |
| `@view` | | ✓ |
| `@task` | | ✓ |
| `@type` | | ✓ |
| `@test` | | ✓ |
| `@doc` | | ✓ |
| `@example` | | ✓ |
| `@private` | | ✓ |

---

## 3. @app — Identity

Defined once, only in `app.deck`.

```
@app
  name:    str     -- display name shown to users
  id:      str     -- reverse-domain unique ID ("mx.lab.monitor")
  version: str     -- semver: "MAJOR.MINOR.PATCH"
  author:  str?
  license: atom?   -- :mit | :apache2 | :gpl3 | :proprietary
```

The `id` is stable identity. If the OS has a previously installed app with the same `id`, it runs `@migration` blocks before starting. If `id` changes, the OS treats it as a new app.

The `@app` block is the app manifest. No separate manifest file exists.

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

### 4.2 Local Modules

```
@use
  ./utils/format
  ./views/timeline
  ./models/post
```

Path is relative to `app.deck`. Module name = last path segment without extension. No qualifiers. Missing paths are a load error.

### 4.3 What @use Does Not Do
Does not install, download, or version-constrain. The OS is monolithic — everything the app can use was compiled into the OS image. `@use` is a declaration of intent, not an acquisition.

---

## 5. @permissions — Authorization

Declares which sensitive capabilities require user/OS authorization and why. Presented to the OS before the app runs.

```
@permissions
  capability.path  reason: "Human-readable explanation"
```

`reason:` is mandatory and shown verbatim in the OS permission dialog — write it for a user, not a developer.

If a permission is denied: the capability behaves as `optional` and absent. Calls return `:err :permission`. The app does not crash.

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

Defines a named state machine type. All stateful behavior in Deck flows through machines.

### 8.1 Scope and Instantiation

**App-level machine** (`@machine` at top level in any file): a single global instance managed by the interpreter. Accessible anywhere as `MachineName.state`, `MachineName.send(...)`, `MachineName is :state`. Created at app load.

**View-local machine** (declared inside a `@view` block): a private instance created when the view appears and destroyed when it disappears. Accessed within the view as `state` (unqualified) and `send(:event)` (unqualified). Cannot be accessed from outside the view.

### 8.2 Declaration Syntax

```
@machine MachineName
  state :name
  state :name (field: Type, field: Type)

  initial :name

  transition :event_atom
    from :state_name
    to   :target_state

  transition :event_atom (param: Type, param: Type)
    from :state_name current
    to   :target (field: expr_using_current_and_params)
    when: guard_bool_expr
```

### 8.3 State Declarations

```
state :idle                                      -- no payload
state :active  (temp: float, max: float)         -- named payload
state :error   (message: str, code: atom)
```

### 8.4 Transition Rules

- Transitions fire when `MachineName.send(:event, param: value)` is called
- If the machine's current state does not match any `from:` for the given event, the send is silently ignored
- If a `when:` guard evaluates to false, the transition is silently ignored
- **Multiple transitions for the same event are allowed** if their `from:` states or `when:` guards are mutually exclusive. Evaluated top-to-bottom; first match fires.
- `from *` matches any state

**Binding current state data:**
```
transition :update (reading: float)
  from :active current
  to   :active (temp: reading, max: math.max(reading, current.max))
```
`current` binds the entire from-state payload as a record.

### 8.5 View-Local Machine Syntax

```
@view MyView
  @machine
    state :idle
    state :editing (text: str)
    initial :idle

    transition :start_edit
      from :idle
      to   :editing (text: "")

    transition :update (text: str)
      from :editing _
      to   :editing (text: text)

    transition :cancel
      from *
      to   :idle

  body = ...
```

The inline `@machine` inside a `@view` does not take a name. It is always accessed as `state` and `send()`.

---

## 9. @stream — Reactive Data Sources

Declares a named, app-wide reactive stream. Streams are active from app load until termination.

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

### 9.1 Source Streams

`source:` must be a capability method returning `Stream T`. If the capability is `optional` and unavailable, the stream produces no values.

### 9.2 Derived Streams

`from:` references another declared `@stream`. Operators chain in declaration order. All operator lambdas are pure. Circular stream references are a load error.

`distinct: true` — skip consecutive duplicate values.
`throttle: 5s` — emit at most one value per 5 seconds.
`debounce: 2s` — emit only after the value has been stable for 2 seconds.
`buffer_n: 10` — collect 10 values then emit `[T]` (type becomes `Stream [T]`).
`window_n: 5` — sliding window of 5, emit `[T]` on each new value.
`take_while: x -> ...` — stop emitting when predicate becomes false.
`skip: n` — discard first n values.

### 9.3 Stream Access

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

---

## 10. @on — App Lifecycle Hooks

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
| `resume` | App returns from OS-suspended state |
| `suspend` | OS is suspending the app — must complete quickly |
| `terminate` | App about to be killed — not guaranteed to run on force kill |

OS-declared events (from `.deck-os`) can also appear in `@on`:
```
@on hardware.button (id: 0, action: :press)
  App.send(:button_pressed)
```

`@on` hooks are the bridge between OS events and the app state machine. The app machine itself does not observe OS events directly — `@on` hooks translate them into `send()` calls.

---

## 11. @view — User Interface

```
@view ViewName
  @shows when: condition
  @hides when: condition
  param field : Type
  @machine
    ...   -- optional inline local machine
  @listens StreamName
  @listens OtherStream

  on appear    -> expr_or_do_block
  on disappear -> expr_or_do_block
  on suspend   -> expr_or_do_block
  on resume    -> expr_or_do_block

  body =
    component_tree
```

### 11.1 @shows and @hides

Evaluated continuously. When the condition changes, the interpreter instructs the OS to show or hide the view. Views with `@shows` appear/disappear automatically — the developer never calls show/hide imperatively.

`@hides when:` is optional and complementary. If both are declared, the view is shown only when `@shows` is true AND `@hides` is false.

Views without `@shows` are only reachable via `@nav` routes.

### 11.2 Params

```
param user_id : str
param post    : Post
```

Passed when navigating to the view. Available as plain identifiers in the view body and hooks.

### 11.3 @listens

Views with `@listens StreamName` re-render reactively when the stream produces a new value. Without `@listens`, a view reads stream data once (on appear) and does not update.

### 11.4 View Lifecycle Hooks

`on appear` — view is shown (navigation push or `@shows` becoming true).
`on disappear` — view is hidden (navigation pop or `@shows` becoming false).
`on suspend` — app suspends while this view is visible.
`on resume` — app resumes while this view is visible.

These hooks may use `!effect` capabilities declared in `@use`.

---

## 12. View Body DSL

The body is a tree of components. Components are provided and rendered natively by the OS. Deck describes structure, data, and intent — the OS handles layout, fonts, colors, and animation. This means all apps look consistent on a given OS and require no rendering code.

### 12.1 Root and Layout

```
screen                        -- required root container

column                        -- children stacked vertically
row                           -- children stacked horizontally
center                        -- single child, centered in available space
spacer                        -- flexible empty space
divider                       -- horizontal rule

scroll                        -- scrollable container
  direction: :vertical | :horizontal     -- default :vertical
  on refresh -> expr          -- enables pull-to-refresh; omit to disable
  component_tree

card                          -- visually grouped container (elevation/border)
  component_tree
```

### 12.2 Text and Display

```
text str_expr
  style: :heading | :subheading | :body | :caption | :code
         :large | :small | :muted | :warning | :danger | :success
         -- multiple styles: style: :large :muted

reading                       -- labeled data value (sensor/metric display)
  label:     str_expr
  value:     any_expr
  unit:      str_expr
  alert:     when condition   -- OS highlights/pulses when true
  available: when condition   -- hides component when false
  accessibility: str_expr?    -- overrides default VoiceOver/TalkBack label
```

### 12.3 Inputs

All inputs dispatch `event.value` (the new value) to their `on change` handler. The current value is read from state — inputs are controlled components.

```
input                         -- single-line text input
  value:       str_expr
  placeholder: str_expr
  style:       :default | :search | :password | :email | :url | :numeric
  max_length:  int?
  enabled:     bool_expr      -- default true
  on change -> expr_using_event_value

textarea                      -- multi-line text input
  value:       str_expr
  placeholder: str_expr
  max_length:  int?
  lines:       int            -- initial height hint (default: 3)
  on change -> expr_using_event_value

toggle                        -- boolean switch
  value:  bool_expr
  label:  str_expr?
  on change -> expr_using_event_value  -- event.value: bool

slider                        -- continuous numeric input
  value: float_expr
  min:   float
  max:   float
  step:  float?               -- default: continuous
  on change -> expr_using_event_value  -- event.value: float

picker                        -- selection from discrete options
  value:   any_expr
  options: [(label: str, value: any)]
  on change -> expr_using_event_value  -- event.value: selected value
```

**`event` binding**: Inside `on change ->`, `on press ->`, and similar handlers, `event` is an implicit binding with fields:
- `event.value` — new value (for input, toggle, slider, picker)
- `event.index` — zero-based index of selected item (for list item handlers)

### 12.4 Actions

```
button str_expr
  -> :nav_route                               -- navigate
  -> :nav_route with: param = expr            -- navigate with params
  -> nav.back                                 -- go back
  -> nav.root                                 -- go to root
  -> expr                                     -- expression (send, call, do)
  style:         :primary | :secondary | :danger | :ghost | :link
  enabled:       bool_expr                    -- default true
  confirm:       str_expr                     -- shows native confirmation dialog
  confirm_label: str_expr                     -- default "Confirm"
  cancel_label:  str_expr                     -- default "Cancel"
  on confirm -> expr                          -- runs if user confirms
  accessibility: str_expr?

actions                       -- horizontal button group (OS decides layout)
  button ...
  button ...
```

When `confirm:` is present: the OS intercepts the press, shows a native dialog with the given message, and only executes the action (or `on confirm`) if the user confirms. Without `on confirm`, the original `->` action runs on confirmation.

### 12.5 Lists and Collections

```
list source_expression
  item variable_name ->
    component_tree_using_variable
  on refresh -> expr          -- pull-to-refresh on the list (alternative to scroll)
  empty_state ->              -- shown when source_expression is []
    component_tree
```

`source_expression` must be `[T]`.

### 12.6 Media

```
image
  src:      str_expr          -- URL (http/https) or local fs path
  alt:      str_expr          -- required; used for accessibility
  style:    :avatar | :thumbnail | :cover | :inline | :full_width
  cache:    bool              -- default true; OS caches fetched images
  fallback ->                 -- shown while loading or on error
    component_tree
```

Images are fetched and cached by the OS renderer transparently. The app declares intent; the OS handles networking and caching. No `!http` effect is needed on the view — the OS renderer operates outside the app's effect scope.

### 12.7 Data Visualization

```
chart data_expression
  style: :line | :bar | :area | :dot | :pie
  label: str_expr?
  x_label: str_expr?
  y_label: str_expr?
```

`data_expression` must be `[float]` or `[(Timestamp, float)]`.

### 12.8 Status and Feedback

```
spinner                       -- indefinite loading indicator

banner str_expr
  style: :info | :warning | :danger | :success

status
  icon:    :ok | :warning | :error | :loading | :info
  message: str_expr
  detail:  str_expr?

alert
  level:   :low | :medium | :high | :critical
  title:   str_expr
  message: str_expr?
  value:   any_expr?
  unit:    str_expr?
```

### 12.9 Conditional and Iterative Rendering

```
when condition
  component_tree

when condition
  component_tree_a
else
  component_tree_b

for item_name in list_expression
  component_tree_using_item_name
```

These are rendering constructs, not control flow. The interpreter re-evaluates them reactively. `for` over an empty list renders nothing.

### 12.10 Accessibility

All components accept an optional `accessibility:` prop that overrides the default VoiceOver/TalkBack label:
```
reading
  label: "Heart Rate"
  value: hr
  unit: "bpm"
  accessibility: "Heart rate: {hr} beats per minute"
```

For `image`, `alt:` is mandatory and serves as the accessibility label.

For `button`, the label text is the default accessibility label. Override when the visual label is insufficient:
```
button "❤️"
  accessibility: "Like this post"
  -> toggle_like(p)
```

---

## 13. @nav — Navigation Topology

Declares the complete navigation structure. Views exist at exactly one position in this hierarchy.

```
@nav
  root: ViewName

  stack
    :route -> ViewName
    :route -> ViewName  with: param:Type  other_param:Type

  modal
    :route -> ViewName
    :route -> ViewName  with: param:Type

  tab
    :route -> ViewName  icon: :icon_atom  label: str
    :route -> ViewName  icon: :icon_atom  label: str
```

**`root`**: Initial view. Shown after `@on launch` completes.
**`stack`**: Views pushed onto a navigation stack. OS provides back gesture/button.
**`modal`**: Views presented over current content. Dismissed with `nav.back`.
**`tab`**: Views in a persistent tab bar. Switching tabs is stateless.

A route may not appear in more than one section. The nav topology is validated at load time:
- All referenced views must exist
- All `with:` params must match the view's `param` declarations
- All `-> :route` navigation actions in view bodies must reference declared routes

### 13.1 Navigation Actions

```
-> :route_name
-> :route_name with: param = expr  other_param = expr
nav.back
nav.root
nav.replace(:route)              -- replace current in stack (no back entry)
```

---

## 14. @task — Background Work

```
@task TaskName
  every:    Duration
  when:     condition_expr
  priority: :high | :normal | :low | :background
  battery:  :normal | :efficient

  run =
    expr_or_do_block
```

### 14.1 Scheduling

**`every:`** only: runs unconditionally at each interval.
**`when:`** only: runs once when conditions first become true.
**Both**: runs at each interval if all conditions are met.
**Multiple `when:` clauses**: all must be true (logical AND).

### 14.2 Priority and Battery

`:high` — run as soon as conditions are met.
`:normal` — default.
`:low` — can be deferred if system is busy.
`:background` — only when app is backgrounded and OS permits.

`:efficient` battery hint — OS may batch with other background work to conserve power.

### 14.3 Task Body

May use any `!effect` capability declared in `@use`. Runs in its own effect context. Unhandled errors are logged; the task is eligible to run again on the next trigger. Tasks do not crash the app.

---

## 15. @migration — Data Evolution

Runs when the app is updated before the new version starts. Each `@migration` runs only once per device (tracked by the OS).

```
@migration from: "semver_range"
  do
    store.set("new_key", unwrap_opt_or(store.get("old_key"), ""))
    store.delete("old_key")
    db.exec("ALTER TABLE posts ADD COLUMN score REAL DEFAULT 0.0")
```

`from:` accepts semver ranges: `"1.x"`, `"1.2.x"`, `"<2.0"`.

Available operations inside `@migration`:
- `store.*` — `storage.local` operations
- `db.*` — SQLite operations (schema changes)
- `nvs.*` — NVS operations
- `config.set(field: str, value: any)` — set config to new default

If a migration fails, the app does not start. The OS reports the failure.

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

@use
  sensors.temperature as temp
  sensors.humidity    as humidity  optional
  storage.local       as store
  network.http        as http      when: network is :connected
  display.notify      as notify
  db                  as db
  ./types
  ./views/main
  ./views/alert
  ./views/history
  ./tasks/sync
  ./tasks/persist

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

@machine App
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

@stream Readings
  source: temp.watch(hz: 1)

@stream HumidityReadings
  source: humidity.watch(hz: 1)

@on launch
  db.exec("""
    CREATE TABLE IF NOT EXISTS readings (
      id    INTEGER PRIMARY KEY AUTOINCREMENT,
      value REAL    NOT NULL,
      ts    TEXT    NOT NULL
    )
  """)
  match temp.read()
    | :ok v      -> App.send(:ready, first: v)
    | :err _     -> App.send(:sensor_lost)

@on suspend
  store.set("last_reading", str(unwrap_opt_or(Readings.last(), 0.0)))

@nav
  root: MainView
  stack
    :history -> HistoryView
  modal
    :settings -> SettingsView
```
