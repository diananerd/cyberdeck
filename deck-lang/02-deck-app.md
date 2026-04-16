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

`app.deck` is also a `.deck` file for annotation placement purposes. Annotations in the "Any `.deck` file" column are also valid in `app.deck`.

---

## 3. @app — Identity

Defined once, only in `app.deck`.

```
@app
  name:    str     -- display name shown to users
  id:      str     -- reverse-domain unique ID ("mx.lab.monitor")
  version: str     -- semver: "MAJOR.MINOR.PATCH"
  entry:   Name    -- root @machine or @flow that is the app entry point
  author:  str?
  license: atom?   -- :mit | :apache2 | :gpl3 | :proprietary
```

The `id` is stable identity. If the OS has a previously installed app with the same `id`, it runs `@migration` blocks before starting. If `id` changes, the OS treats it as a new app.

`entry:` names the root `@machine` or `@flow`. The OS starts the app at its `initial` state. The root machine/flow defines the top-level navigation topology.

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

`on enter` and `on leave` are sugar: they apply to **every** transition that activates or deactivates a state. `before` / `after` on a specific transition apply only to **that** transition.

Execution order for a transition:

```
1. transition before    -- hook of the specific transition
2. state    on leave    -- hook of the from-state
3. [state changes]
4. state    on enter    -- hook of the to-state
5. transition after     -- hook of the specific transition
```

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

### 9.3 Navigation Topology

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

## 12. Content Bodies

The `content =` block inside any `@machine` state or `@flow` step is a semantic description of what the user perceives and can do in that context. It does not describe rendering — the OS decides how to present each element for the form factor (phone, smartwatch, voice, e-ink, terminal).

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
    error message: e.message
    trigger "Try again" -> LoginFlow.send(:retry)
```

---

### 12.2 State

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

**`navigate`** — the user wants to go to a related context. The `->` action sends to a machine or flow to trigger a state transition. The OS treats `navigate` semantically — it knows the user intends movement to a new context, not a pure action.
```
navigate label: str  -> action_expr
```
`action_expr` is typically a `Machine.send(...)` call that triggers a navigation transition in the app's root flow. The OS may render this as a disclosure affordance, transition animation hint, or navigation gesture.

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
              navigate "Reply" -> App.send(:open_compose, reply_to: p)
            group "options"
              confirm "Report"     message: "Report this post?"         -> moderation.report_post(p)
              confirm "Block user" message: "Block @{p.author.handle}?" -> moderation.block(p.author)
    | _ -> unit

  create "Compose" -> App.send(:open_compose)     -- context-level; always available
```

`for` iterates over any `[T]` and is equivalent to `list` without `more:` and `empty ->` semantics. Use `list` when the OS should treat the collection as a navigable, potentially paginated data set; use `for` for repeated inline content inside a larger structure.

View content functions (§6.3 of `01-deck-lang`) may be called freely inside content bodies. Their nodes are spliced inline at the call site. Business logic (`let`, `match`, pure functions) may appear alongside content nodes anywhere in the body — the evaluator resolves the full body to a `[ViewContentNode]` sequence.

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
  store.set("last_reading", str(unwrap_opt_or(TempStream.last(), 0.0)))


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
      | :no_sensor -> error message: "Sensor not responding"
      | :alert s   ->
          status true  label: "Temperature alert"
          "{s.temp}°C (max recorded: {s.max}°C)"
          match HumidityStream.last()
            | :some h -> "Humidity: {h}%"
            | :none   -> unit
          chart TempStream.recent(60)
            label: "Last minute"
            y_label: "°C"
          confirm "Dismiss alert" message: "Clear the temperature alert?" ->
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
