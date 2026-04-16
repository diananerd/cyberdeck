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

`app.deck` is also a `.deck` file for annotation placement purposes. Annotations in the "Any `.deck` file" column are also valid in `app.deck`.

**Module imports**: `@use ./path` entries for local modules are declared **only in `app.deck`**. Individual `.deck` files do not have their own `@use` declarations — the module graph is established once from `app.deck` and all declared modules are in scope project-wide. See `01-deck-lang §10.3`.

`app.deck` is also a `.deck` file for annotation placement purposes. Annotations in the "Any `.deck` file" column are also valid in `app.deck`.

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

**Load-time enforcement**: If a `@use` entry references a capability that has `@requires_permission` in the OS surface, it **must** appear in `@permissions`. The rules:
- Not in `@permissions` AND `@use` entry is **not** `optional` → **load error**
- Not in `@permissions` AND `@use` entry IS `optional` → **load warning** (developer may have intentionally skipped it)
- In `@permissions` → continue normally

**Runtime behavior**: If the user denies a permission at the OS dialog, the capability behaves identically to an `optional` capability that is currently absent. Calls return `:err :permission` as a `Result` value. The app does not crash.

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

  content = ...
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

**`combine_latest` semantics**: does not emit until every source stream has produced at least one value. After that, emits a new tuple each time any source emits, using the most recent value from each source. The type is `Stream (T₁, T₂, ...)` where Tₙ is the element type of the nth source. Up to 8 sources.

**`merge` semantics**: emits each value from any source as it arrives. All sources must have the same element type T. The resulting type is `Stream T`. Values from different sources are interleaved in arrival order.

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
| `resume` | App returns from OS-suspended state (user-initiated) |
| `suspend` | OS is suspending the app — must complete quickly |
| `terminate` | App about to be killed — not guaranteed to run on force kill |

OS-declared events (from `.deck-os`) can also appear in `@on`:
```
@on hardware.button (id: 0, action: :press)
  App.send(:button_pressed)
```

**Background fetch vs. resume**: `@on resume` fires only for user-initiated app returns (e.g., user switches back from another app). Background wakeups from `background_fetch` fire `@on os.background_fetch` — a separate event that can be distinguished:

```
@on os.background_fetch
  match App.state
    | :authenticated _ -> App.send(:background_refresh)
    | _                -> unit
```

**Load-time validation**: Every event name in an `@on` hook must exist in the OS event registry (declared in `.deck-os`). An `@on` referencing an unknown event name is a **load error**.

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

  content =
    view_body
```

### 11.1 @shows and @hides

Evaluated continuously. When the condition changes, the interpreter instructs the OS to show or hide the view. Views with `@shows` appear/disappear automatically — the developer never calls show/hide imperatively.

`@hides when:` is optional and complementary. If both are declared, the view is shown only when `@shows` is true AND `@hides` is false.

Views without `@shows` are only reachable via `@nav` routes.

**Priority when multiple `@shows` are simultaneously true**: At most one view occupies the root position at a time. If two or more views have `@shows` conditions that are simultaneously true, the **last-declared view** (in `app.deck` reading order) takes precedence. The loader emits a warning when two `@shows` conditions can be simultaneously true — this is almost always a design mistake.

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

## 12. View Body

The `content =` of a view is a semantic description of what the user perceives and can do in that context. It does not describe how to render anything — that is the OS's responsibility. The OS uses the semantic structure to apply the appropriate pattern for the form factor (phone, smartwatch, voice, e-ink, terminal).

### 12.1 Structural Primitives

**`list`** — collection of items of the same type.

```
list expr
  empty ->
    content               -- perceived state when expr is []
  more:    bool_expr      -- signal that more items are available
  on more -> action       -- how to load them; the OS exposes this per form factor
  var ->
    content               -- structure of each item
```

**`item`** — standalone entity for detail views (the subject of the view is a single thing).

```
item ->
  content
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

**`flow`** — multi-step flow with dependent steps, not necessarily linear. Combines a state machine (controlling the flow) with content and intents per state. Each `step` defines what the user perceives and can do when the machine is in that state.

```
flow MachineName
  step :state_name ->
    content
  step :state_name var ->   -- when the state has payload, var binds it
    content
```

The OS does not need to know how many steps there are or in what order — the machine controls transitions. The OS only knows it is showing a step of a dependent flow and applies its progress/stepper/conversation patterns accordingly.

Example — login flow with 2FA:

```
@machine LoginFlow
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

flow LoginFlow
  step :credentials ->
    text     :handle    hint: "Handle or email"   on -> send(:set_handle, v: event.value)
    password :password  hint: "Password"           on -> send(:set_password, v: event.value)
    trigger "Sign in" -> auth.login(...)

  step :two_factor s ->
    rich_text "Enter the code sent to {s.email}"
    pin :code  length: 6  on -> send(:verify, code: event.value)

  step :done s ->
    rich_text "Welcome, {s.handle}!"
    navigate "Continue" -> nav.root

  step :failed e ->
    error message: e.message
    trigger "Try again" -> send(:retry)
```

---

### 12.2 View State

Semantic markers for the perceived state of the view. Used inside `match` arms over machine states. The OS maps them to its loading/error patterns for the device.

```
loading                     -- the system is working; the user is waiting
error message: str_expr     -- something went wrong; the message is shown to the user
```

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
  hint: atom?         -- usage context: :avatar :cover :thumbnail :inline
```

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

**`navigate`** — the user wants to go to a related context.
```
navigate label: str  -> :route [with: param = expr]
navigate label: str  -> nav.back
navigate label: str  -> nav.root
```

---

**`trigger`** — the user initiates an action without navigation or confirmation.
```
trigger label: str  -> action
```
Uses: refresh, retry, sync, mark as read, play/pause.

---

**`confirm`** — the user initiates a significant or irreversible action. The OS interposes a confirmation; how depends on the form factor (dialog, voice confirmation, double-press).
```
confirm label: str  message: str_expr  -> action
```
Uses: delete, block, sign out, reset.

---

**`create`** — the user initiates creation of a new entity. Semantically distinct from `navigate`: the destination is a blank creation context.
```
create label: str  -> :route [with: param = expr]
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

- **Context-level** — direct child of `content =` (not inside a `list` item or `group`): applies to the whole view. The OS places it as a primary action (FAB, voice command, toolbar, physical key).
- **Item-level** — inside the body of a `list` item: applies to that specific item. The OS exposes it through item affordances (tap, swipe, long-press, contextual voice command).
- **Group-level** — inside a `group`: scoped to that group's semantic context. The OS may collapse group intents behind a secondary action (overflow menu, "more options", voice "options for [label]").

---

### 12.6 Language Integration

`match`, `let`, and `for` work naturally inside content:

```
content =
  match Timeline.state
    | :loading   -> loading
    | :error e   -> error message: e.message
    | :loaded s  ->
        list s.posts
          more:    s.cursor is :some
          on more -> Timeline.send(:paginate)
          p ->
            group "author"
              media p.author.avatar  alt: p.author.handle  hint: :avatar
              p.author.display_name
              p.author.handle
            rich_text p.text
            p.created_at
            group "reactions"
              toggle :liked    state: p.liked_by_me    on -> interaction.toggle_like(p)
              toggle :reposted state: p.reposted_by_me on -> interaction.toggle_repost(p)
              navigate "Reply" -> :compose_reply with: post = p
            group "options"
              confirm "Report"     message: "Report this post?"         -> moderation.report_post(p)
              confirm "Block user" message: "Block @{p.author.handle}?" -> moderation.block(p.author)
    | _ -> unit

  create "Compose" -> :compose     -- context-level; always available
```

`for` iterates over any `[T]` and is equivalent to `list` without `more:` and `empty ->` semantics. Use `list` when the OS should treat the collection as a navigable, potentially paginated data set; use `for` for repeated inline content inside a larger structure.

---

### 12.7 `event` Binding

Inside `on ->` handlers of input intents, `event` is an implicit binding:

| Intent | `event.value` |
|---|---|
| `toggle` | `bool` |
| `range` | `float` |
| `choice` | `any` |
| `multiselect` | `[any]` |
| `text` | `str` |
| `password` | `str` |
| `pin` | `str` (digits; invoked on completion) |
| `date` | `Timestamp` |
| `search` | `str` |

`navigate`, `trigger`, `confirm`, `create`, and `share` do not bind `event`.

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

**Tab `icon:` atoms** — the following atoms are guaranteed to map to a native icon on all compliant OS implementations. The OS author may add platform-specific atoms; unknown atoms render as a generic `:menu` icon with a load-time warning.

| Atom | Meaning |
|---|---|
| `:home` | Home / dashboard |
| `:search` | Search |
| `:notifications` | Notifications / bell |
| `:profile` | User profile / person |
| `:settings` | Settings / gear |
| `:compose` | Compose / new item |
| `:feed` | Feed / list |
| `:favorites` | Favorites / star |
| `:history` | History / clock |
| `:menu` | Hamburger menu (generic fallback) |

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

**Ordering and overlap**: When updating, the OS identifies all `@migration` blocks whose `from:` range matches the installed version. They are sorted by specificity (most specific first: `"1.2.3"` before `"1.2.x"` before `"1.x"` before `"<2.0"`). If two blocks have equal specificity, they run in declaration order. Multiple blocks may match and all run. The OS tracks which blocks have run by a hash of `(app.id, from_range_string)` — a block never runs twice on the same device.

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
