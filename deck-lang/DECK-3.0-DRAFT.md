# Deck 3.0 — Draft Consolidated Spec

**Status:** Draft. Captures all design decisions agreed across the redesign conversation, plus a minimalism pass for implementation and a full consistency review. Not yet authoritative. Does not replace `01-deck-lang.md` through `16-deck-levels.md` until promoted.

**Edition:** 2027.

---

## 0 · Philosophy

Deck is a domain-specific, interpreted, purely functional language for **embedded application authoring**. Not general-purpose. Every feature is justified by an embedded-application need, *and* by being the simplest construct that expresses it.

**Invariants:**

1. **Apps declare semantic intent, never presentation.** The bridge infers layout, widgets, colors, spacing, gestures, animations from the declared semantics plus device context. The same `.deck` runs against different bridges (LVGL, e-ink, voice, terminal) and each makes distinct presentation decisions.
2. **One way to express each concept.** If two forms exist for the same concept, one is wrong.
3. **Indentation is the block structure.** Annotations, machines, content bodies, matches, lambdas — all nest by indent. Curly, square, and round delimiters are only for **data** (map / record / list / tuple / fn params / fn application).
4. **Declarative first.** Before writing imperative code, check whether the OS offers the concept as a capability, service, stream, or config field. Imperative code exists only where the OS genuinely cannot help.
5. **Pure by default; impurity is marked.** A single-bit `!` marks functions that may produce observable side effects.
6. **Errors are values or terminations.** Three levels: `Result` (recoverable, propagated via `?`), `panic` (terminal), `LoadError` (prevented).
7. **Embedded-first.** Memory, time, concurrency, persistence are spec concerns, not implementation details.
8. **Load-time > runtime.** Any error that can be caught before `@on launch` is a load-time error with source location.
9. **No null, no exceptions, no mutation of shared state, no inheritance, no classes, no hidden control flow.**
10. **Implementation minimalism is a spec concern.** If a feature requires a complex compiler pass or a non-obvious runtime mechanism, and the same use case can be expressed with existing simpler machinery, the simpler path wins.

---

## 1 · Lexical structure

### 1.1 Encoding

UTF-8. File and directory names: lowercase with underscores (`sensor_view.deck`).

### 1.2 Comments

```
-- single-line, to end of line
```

Only `--`. Multi-line comments do not exist; use multiple `--` lines or a triple-quoted docstring. Comments are whitespace at the token layer.

### 1.3 Indentation

Two-space indent. Tabs are a lex error.

A block opens on one of:
- a line ending with `=` or `:`,
- an annotation header (`@name …`),
- a `state` declaration header,
- a `content =` header,
- a `match` or handler header.

A block closes when indentation returns to the enclosing level. Inside **data delimiters** (`()`, `[]`, `{}`), indentation is suspended.

### 1.4 Identifiers

```
value_name     -- lowercase snake_case: values, fns, fields, params
TypeName       -- UpperCamelCase: @type, @machine
:atom          -- colon-prefixed symbolic constant
```

Grammar: values `[a-z][a-zA-Z0-9_]*`, types `[A-Z][a-zA-Z0-9]*`, atoms `:[a-z][a-zA-Z0-9_]*`.

### 1.5 Literals

```
42              -- int (i64)
-7              -- int
3.14            -- float (f64)
true  false     -- bool
unit            -- unit value
"hello"         -- str (UTF-8, immutable)
0xFF            -- byte (0..255)

500ms 1s 5m 1h 1d       -- Duration (ms-backed int, §2.1)
64KB 512KB 2MB 1GB      -- Size (byte-backed int, §2.1)
```

**Duration suffixes:** `ms`, `s`, `m`, `h`, `d`. Integer only; internally milliseconds.
**Size suffixes:** `KB`, `MB`, `GB`. Integer only; internally bytes. Multiples are 1024-based.

### 1.6 String interpolation

```
"Hello, {name}"
"Temp: {temp}°C"
"{a + b}"
```

`{expr}` evaluates and calls `str()`. Literal `{` via `\{`. Interpolation applies to both `"…"` and `"""…"""`.

### 1.7 Triple-quoted strings

```
let sql = """
  SELECT *
  FROM readings
"""
```

Strips common leading indent + leading newline. Interpolation works.

### 1.8 Keywords

**Global keywords** (reserved in all positions):

```
let fn match when as and or not
on every after watch from to previous current
true false unit if then else panic
```

**Contextual keywords** (reserved only in the positions documented in their section):

| Keyword | Reserved in | Section |
|---|---|---|
| `for` | inside `content =` blocks | §15.7 |
| `source` | after `@on` | §14.1 |
| `service` | inside `@use` bodies | §9.2 |
| `empty`, `more`, `submit`, `change`, `complete`, `link`, `image`, `cursor`, `selection`, `enter`, `leave` | after `on` in content handlers and state hooks | §13.1, §15.4 |

Outside their reserved positions these are ordinary identifiers.

### 1.9 The `->` operator

Used in five distinct positions, disambiguated by surrounding tokens:

| Position | Example |
|---|---|
| Lambda | `x -> x * 2`, `(a, b) -> a + b` |
| Match arm body | `\| :some v -> v * 2` |
| Handler body | `on change -> Machine.send(:evt)` |
| Action intent body | `trigger label: "Open" -> Machine.send(:open)` |
| `@on` body separator | `@on source <expr> -> <body>` |

All five introduce a body or value on the right. The spec admits no other use of `->`.

### 1.10 The `?` postfix operator

Applied to a `Result T E` expression inside an `!` fn:

```
let body = http.get("…")?
```

If the `Result` is `:ok v`, unwraps to `v`. If `:err e`, immediately returns `:err e` from the enclosing fn. Desugars mechanically to a `match`; no AST rewrite pass required.

Only valid in `!` fns whose return type is `Result T' E` compatible with the error domain of the expression. Load-verified.

---

## 2 · Type system

Dynamically typed at runtime; structurally validated at load. Annotations are **optional** in code positions (`fn` params/return, `let`) and **required** in schema positions (`@type` fields, `@config` entries, `state` payloads, capability signatures).

### 2.1 Primitives

| Type | Values | Notes |
|---|---|---|
| `int` | 64-bit signed | overflow → `panic :bug` |
| `float` | 64-bit IEEE 754 | NaN surfaces as atom `:nan` |
| `bool` | `true` / `false` | |
| `str` | UTF-8, immutable | |
| `byte` | 0..255 | binary data |
| `unit` | single value `unit` | side-effect return |
| `Timestamp` | alias of `int` | epoch milliseconds |
| `Duration` | alias of `int` | milliseconds |
| `Size` | alias of `int` | bytes |

`Timestamp`, `Duration`, and `Size` are documentation aliases of `int`. `5m == 300000`, `64KB == 65536`. They exist solely to carry intent; builtins (`time.*`) both produce and consume them.

### 2.2 Composites

```
[T]              -- List
{K: V}           -- Map
(T, U)           -- Tuple (arity ≥ 2)
T?               -- Optional, sugar for :some T | :none
```

Lists and maps immutable with structural sharing.

### 2.3 `@type` declaration — unified record + variant

```
@type Post                          -- record (one implicit constructor :Post)
  uri        : str
  author     : Author
  created_at : Timestamp

@type Pair (A, B)                   -- parametric record
  fst : A
  snd : B

@type Status =                      -- closed variant
  | :idle
  | :active (temp: float, max: float)
  | :error (reason: str)
```

A `@type` declares either a record or a closed variant.

- **Record:** a single implicit constructor atom derived from the type name (`Post` → `:Post`). Construction `Post { … }` and pattern `Post { … }` both key on this implicit constructor. Internally, a record is a variant with exactly one constructor.
- **Closed variant:** one or more constructors, each optionally with a payload (no payload, positional payload, or named-field payload). Match over a value of the type is load-time exhaustive-verified.

Recursion is allowed only through `[T]` or `T?`. Direct recursive fields are `LoadError :type`.

### 2.4 `@errors` — shorthand for atom-only variants

```
@errors api
  :unauthorized  "401 or token invalid"
  :timeout       "Request timed out"
  :rate_limited  "Quota exceeded"
```

Equivalent to:

```
@type api.Error =
  | :unauthorized
  | :timeout
  | :rate_limited
```

Descriptions are documentation, not part of the type. `@errors` always produces `{domain}.Error` named type.

### 2.5 `any`

Escape hatch for dynamic values from external sources. Invalid in `@type` fields, `@config` entries, `@on source` type parameters. Match on `any` requires wildcard coverage.

### 2.6 Purity (`!`)

```
fn fetch_profile (did: str) -> Result Profile api.Error ! =     -- impure
  …

fn format_handle (h: str) -> str =                              -- pure
  …
```

Single bit. **Pure fns** may only call other pure fns and pure builtins. **Impure fns** may call anything. The loader derives which capabilities an impure fn touches from its body; no alias list. Purity and `Result` are orthogonal.

**Content fns** (§4.5) forbid `!` — marking a content fn impure is `LoadError :type`.

### 2.7 Stream type

```
Stream T
```

Continuous sequence, possibly infinite. Produced by capability methods and long-running operations (§14.3.1). Consumed via `@on source` (§14). Not a list; no length; not iterable.

### 2.8 Fragment type

```
fragment
```

Zero or more view content nodes. Returned by content fns (§4.5). Inferred when the body produces content nodes and declares no `!`. Not storable in data structures; not passable to non-content fns.

---

## 3 · Bindings

### 3.1 `let`

```
let x = 42
let name : str = "deck"
```

Immutable. Lexically scoped. Shadowing allowed. Annotation optional.

### 3.2 Destructuring

```
let (p, q) = some_tuple
let [h, t]  = fixed_list    -- length mismatch: runtime pattern error
```

Chained `let`s inside a body replace Deck 2.0's `where`:

```
fn process (raw: float) -> str =
  let offset   = 2.5
  let adjusted = math.round(raw - offset, 1)
  "{adjusted}°C"
```

Later `let`s may reference earlier ones. No topological sort. No `where` keyword.

---

## 4 · Functions

```
fn name (a, b) =                                -- minimal; pure
  body

fn name (a: Type, b: Type) -> Type =            -- annotated pure
  body

fn name (a: Type) -> Type ! =                   -- annotated impure
  body
```

### 4.1 Application

User fns: **positional only**. Named args are not supported for user-defined fns.
Capabilities and builtins: **positional or named** (as declared in the capability's `.deck-os` signature). Never mixed in a single call.

```
my_fn(arg1, arg2)                          -- positional (user fn)
http.get(url: "…", timeout: 5s)            -- named (capability)
time.now()                                 -- positional, no args (builtin)
```

**Named args live in the argument list, separated by commas (no braces).** They are not map literals; they do not use `{ … }`.

### 4.2 Data construction — `{ … }` and `[ … ]`

All `{ … }` and `[ … ]` constructs are **data**, not code blocks. Inside them, indentation is suspended.

**Map literal** — string keys, quoted:
```
let m = { "host": "a.example", "port": 8080 }
```

**Record construction** — field names, unquoted, prefixed by the type name:
```
let p = Post { uri: "…", author: a, created_at: time.now() }
```

**Variant construction** — atom + optional payload:
```
let s  = :active (temp: 25.0, max: 30.0)
let ok = :ok 200
let e  = :err :timeout
let n  = :none
```

**List literal:**
```
let xs = [1, 2, 3]
```

**Tuple literal:**
```
let pt = (x, y)
let xs = ("name", 5, :ok)
```

The parser disambiguates map vs record by the presence of a preceding `TypeName`.

### 4.3 Record update

A builtin, not a keyword:
```
let updated = record.update(post, { likes: post.likes + 1 })
```

Takes a record and a map of overrides; returns a new record. No `with` keyword.

### 4.4 Lambdas

```
x -> x * 2
(a, b) -> a + b
(p: Post) -> p.likes > 100
```

Capture lexical scope. Impure lambdas (calling `!` fns) inherit `!` implicitly.

### 4.5 Content fns

```
fn post_card (p: Post) =
  group "post"
    media p.author.avatar alt: p.author.handle role: :avatar
    p.author.display_name
    rich_text p.text
    p.created_at
```

Return `fragment` (inferred when the body produces content nodes). **Forbidden to carry `!`** — `LoadError :type`. Nodes splice inline at call sites.

### 4.6 Recursion

Direct and mutual supported. Tail-call optimization via trampolining. Non-tail recursion is bounded by VM stack (default 512 frames; configurable per platform). Exceeding → `panic :limit`.

---

## 5 · Expressions

Every construct is an expression. No statements.

### 5.1 Arithmetic, comparison, logical

`+ - * / %`, `== != < > <= >=`, `and or not`. Integer division truncates toward zero; modulo sign follows dividend. Short-circuit `and` / `or`.

### 5.2 String concat

```
"Hello, " ++ name
```

Valid only on `str`. For complex assembly, prefer interpolation.

### 5.3 Pipe

```
v |> f              -- f(v)
v |> f(extra)       -- f(v, extra)
```

No `|>?` variant. Error propagation uses postfix `?` (§1.10).

### 5.4 Bodies are implicit blocks

Any multi-line body position (fn body, match arm body, `@on` body, content handler body, state hook body) is an implicit sequence. Each non-`let` line must produce `unit` or `Result unit E`. Values of other types escaping the body are `LoadError :type`.

There is no `do` keyword. Semicolons do not exist. Newline + same indent separates statements.

### 5.5 `if / then / else`

```
if cond then a else b
```

Sugar for `match cond | true -> a | false -> b`. `cond` must be `bool`; both branches same type. No `else if` grammar — nest or use `match`.

### 5.6 `match`

```
match expr
  | :idle                        -> …
  | :active s when s.temp > 30.0 -> …
  | :error (reason: r)           -> log.warn(r)
  | [h, t]                       -> …
  | Post { likes: n }            -> …
  | (a, b)                       -> …
  | _                            -> …
```

Exhaustiveness is load-verified for closed variants, tuples of known arity, record patterns of known type, and fixed-length list patterns. For `any`, wildcard is mandatory.

**Record patterns use the same shape as record construction.** The parser disambiguates by position: a `Post { … }` on the LHS of `->` in a match arm is a pattern; elsewhere it is construction.

Arm bodies are implicit sequences.

### 5.7 No `is` operator

Machine state test uses `==`:
```
Machine.state == :authed
```

Type test uses a builtin:
```
type_of(v) == :Post
```

Two forms; zero dedicated parser rules; no `is` keyword.

### 5.8 `panic`

```
panic "expected positive, got {t}"
```

Terminal. Only valid in `!` fns (pure fns cannot panic — `LoadError :type`). Produces Level-2 error (§11.2).

---

## 6 · Annotations — the app model

All annotation bodies are **indent-structured declarations**, not map literals. Though visually similar, `@name\n  key: value` (indented) and `{ "key": value }` (data) are distinct shapes with distinct parsers. `@app { name: "x" }` is a syntax error.

Data *inside* an annotation (e.g. a list field, a record field) uses the data delimiters.

**Final set: 14 annotations.**

| Annotation | Purpose | Where | Header shape |
|---|---|---|---|
| `@app` | Identity | `app.deck` | `@app` |
| `@needs` | Requirements contract | `app.deck` | `@needs` |
| `@use` | Import capabilities / modules + per-alias config | `app.deck` | `@use` |
| `@grants` | Permission rationale + opt-ins | `app.deck` | `@grants` |
| `@config` | Typed persistent configuration | `app.deck` | `@config` |
| `@on` | Reactive / lifecycle / event dispatch | any `.deck` | `@on <source>` |
| `@machine` | State machine | any `.deck` | `@machine Name` |
| `@service` | IPC-callable functions | any `.deck` | `@service "id"` |
| `@handles` | Deep link URL patterns | `app.deck` | `@handles` |
| `@assets` | Bundled static resources | `app.deck` | `@assets` |
| `@migrate` | Data schema evolution | `app.deck` | `@migrate` |
| `@type` | Record or variant type | any `.deck` | `@type Name` or `@type Name (A, B)` |
| `@errors` | Error-domain shorthand | any `.deck` | `@errors domain` |
| `@private` | Module-private marker | any `.deck` | prefix modifier, §21 |

All annotations are **top-level within their file**. Nesting annotations inside another annotation body is forbidden — scopes are flat per file. Composition happens only through the `machine:` field on `state` (§13.1) for machines and through module `@use` (§9) for code organization.

---

## 7 · `@app` — identity

```
@app
  name:        "Launcher"
  id:          "system.launcher"
  version:     "1.0.0"
  edition:     2027
  entry:       App
  icon:        "LN"
  tags:        ["system", "home"]
  author:      "diana"
  license:     :mit
  orientation: :any
  log_level:   :info
  serves:      ["system.launcher/badges"]
```

Required: `name`, `id`, `version`, `edition`, `entry`. Others optional.

- `entry:` names the root `@machine`.
- `edition:` pins syntax + semantics.
- `orientation:` is a bridge constraint (`:portrait` / `:landscape` / `:any`).
- `log_level:` default minimum log level.
- `serves:` lists service IDs exposed by this app (§18).

---

## 8 · `@needs` — requirements contract

```
@needs
  deck_level: 2
  deck_os:    ">= 2"
  runtime:    ">= 1.0"
  max_heap:   128KB
  caps:
    network.http:  ">= 2"
    nvs:           ">= 1"
    notifications: optional
  services:
    "social.bsky.app/feed": ">= 1"
```

Evaluated at load. Missing or incompatible entries → `LoadError :incompatible`. `optional` caps / services degrade to `:err :unavailable` at call sites.

`max_heap:` declares the app's heap budget (§22.1). Default 64 KB. Accepts any `Size` literal.

---

## 9 · `@use` — capabilities and modules

```
@use
  network.http as http
    base_url:   "https://api.example.com"
    timeout:    15s
    retry:      2
    user_agent: "Bluesky-Deck/1.0"

  nvs            as nvs
  fs             as fs
  display.notify as notify

  service "social.bsky.app/feed" as bsky

  ./utils/format
  ./api/auth
```

- `as alias` mandatory for capabilities and services.
- Local modules: relative path; module name = last segment.
- No `optional` / `when` here — those live in `@needs.caps`.

### 9.1 Instance configuration

A capability alias may carry config lines indented under it:

```
network.http as http
  base_url: "https://api.example.com"
  timeout:  15s
```

- Optional block. Capabilities with no settable parameters omit it.
- Schema of accepted keys + types is declared in the capability's `.deck-os` entry. Unknown keys / type mismatches / out-of-range → `LoadError :type`.
- Values are literals or pure expressions. **Evaluated once at bind time.** Runtime mutations via `config.set` do **not** re-apply to already-bound capabilities. If an app needs runtime-mutable capability parameters, the capability itself must read `config.*` on each call (declared in its `.deck-os` contract).
- Two aliases of the same capability with different configs are independent bindings.
- Credentials and runtime-mutable state live in `@config` (§12).

### 9.2 Service aliasing

```
service "social.bsky.app/feed" as bsky
```

`service` is a contextual keyword valid only at the start of a `@use` entry. The string-literal is the service ID (provider-id + `/` + name). Resolved by the OS service registry.

---

## 10 · `@grants` — permission rationale

```
@grants
  location:
    reason: "Show weather for your current location."
    prompt: :on_first_use
  notifications:
    reason: "Alert you to new messages."
    prompt: :at_install
  logging:
    persist: true
```

Each entry is `name:` (ending with colon, opening a nested block) + indented `key: value` pairs. Same shape as `@needs.caps:` / `@needs.services:`.

Used by the OS to render install-time dialog and Settings → App Detail. Distinct from `@needs.caps`: `@needs` declares *machine requirements*; `@grants` declares *user-facing rationale and opt-ins*.

---

## 11 · Errors — three-level model

### 11.1 Level 1 — `Result T E` (recoverable)

```
@errors api
  :unauthorized  "401"
  :timeout       "Timeout"
  :server        "5xx"
```

All capability error domains (`fs.Error`, `nvs.Error`, `http.Error`) are closed variants of atoms (via `@errors`). Never `str`, never records. Exhaustive match is load-verified.

**Propagation** uses the postfix `?` operator:

```
fn fetch_profile (did: str) -> Result Profile http.Error ! =
  let r = http.get("/actor/{did}")?       -- :err → early return; :ok → unwrap to r
  parse_profile(r.body)
```

To consume in place:

```
fn fetch_or_empty (did: str) -> Profile ! =
  match http.get("/actor/{did}")
    | :ok r  -> parse_profile(r.body)
    | :err _ -> Profile { … }
```

`?` is only valid in `!` fns whose return type is `Result T' E` compatible with the expression's error domain. Load-verified. Pure fns cannot produce capability `Result`s, so `?` is not needed (and not accepted) there.

### 11.2 Level 2 — `panic` (terminal)

```
panic "unexpected {state}"
```

Terminal. VM suspended. Not catchable by the panicking app. Three kinds:

| Kind | Cause |
|---|---|
| `:bug` | `panic` keyword, divide-by-zero, bounds, pattern-failed, int overflow |
| `:limit` | Stack overflow, heap exceeded, body timeout |
| `:internal` | Runtime defect |

The keyword emits `:bug`; the runtime emits the others when applicable. Automatic log entry (§16.7) + `os.app_crashed` event dispatched to `system.*` crash-receiver apps.

**Restart policy:**
- Foreground VM: no auto-restart.
- Service VM: exponential backoff (1 s / 4 s / 16 s), max 3 attempts in 5 min, then disabled until manual launch. Calls to a disabled service → `:err :service_unavailable`.

### 11.3 Level 3 — `LoadError`

```
@type LoadError
  kind    : LoadErrorKind
  where   : SourceSpan
  message : str
  context : {str: str}

@type SourceSpan
  file : str
  line : int
  col  : int

@type LoadErrorKind =
  | :lex
  | :parse
  | :type
  | :unresolved
  | :incompatible
  | :exhaustive
  | :permission
  | :resource
  | :internal
```

Nine kinds; structured `context`. Stored in the OS app registry and exposed via `system.apps.load_error(app_id) -> LoadError?`. A Deck app composes the explanation in pure Deck.

---

## 12 · `@config` — persistent state

```
@config
  sync_every  : Duration  = 5m
  theme       : atom      = :auto
  max_items   : int       = 100   min: 10  max: 500
  api_token   : str?      = :none
  bsky_host   : str       = "https://bsky.social"
```

Every entry is typed with a default. Stored in NVS namespace `app.{id}`. Range validation uses `min:` / `max:` fields (no `range:` keyword, no `..` literal).

### 12.1 Read

```
config.sync_every      -- typed per schema
config.api_token       -- Optional type per schema
```

The bridge re-renders views dependent on `config.*` on change.

### 12.2 Write

```
config.set(:sync_every, 10m)              -- Result unit config.Error
config.set(:api_token, :some token)
```

The atom argument (`:sync_every`) names a field declared in `@config`. If the atom does not match any declared field, `:err :unknown_key` at runtime (or `LoadError :unresolved` if the literal is statically known).

- Transactional: persisted before returning.
- Fires `@on os.config_changed (key: atom, old: any, new: any)` **after** the body that called `config.set` completes (queued, single-threaded — §14.10).
- Within the same body, reads after a `config.set` see the new value.
- Returns `:err :unknown_key` / `:type_mismatch` / `:out_of_range`.
- Load-time: literal-arg mismatches in `config.set(:literal_atom, literal_value)` are `LoadError :type`.

### 12.3 Scope

Per-install, shared across instances of `app.id` on this device. Per-user profile partitioning is out of scope for v1.

---

## 13 · `@machine` — state machines

```
@machine LauncherState
  state :grid
  state :search (query: str)
  state :empty

  initial :grid

  on :open_search
    from :grid
    to :search (query: "")

  on :update_query (q: str)
    from :search _
    to :search (query: q)

  on :close
    from :search _
    to previous

  on :timeout
    from *
    to :grid
    when battery < 10%
```

### 13.1 States

```
state :name                              -- no payload
state :name (field: Type, …)             -- typed payload
state :name machine: OtherMachine        -- composition

state :name
  on enter -> effect_expr                -- fires on entry
  on leave -> effect_expr                -- fires on exit
  content = …                             -- §15
```

A state with `machine:` delegates its lifecycle to the referenced machine and may not have `content =`, `on enter`, or `on leave`. The referenced machine must be declared elsewhere in the module graph; self-reference is `LoadError :unresolved`.

### 13.2 `initial`

```
initial :state_name
```

Mandatory top-level declaration naming the entry state. More than one `initial` in the same `@machine` → `LoadError :parse`.

### 13.3 Transitions

```
on :event_atom (param: Type, …)?
  from :source [_ | current | (field: binder, …)]?    -- or `from *`
  to :dest (field: expr)?                              -- or `to previous`
  when expr?                                           -- guard, no colon
```

**Source binding forms** (all paralleling match patterns):

- `from :state` — no binding.
- `from :state _` — explicit ignore (same as bare `from :state`, stylistic for transitions that deliberately don't read the payload).
- `from :state current` — binds the full payload record as `current` for use in `to:` and `when` expressions.
- `from :state (field: binder, …)` — destructures specific fields.
- `from *` — matches any source state (payload inaccessible).

**Rules:**

- Multiple transitions per event are allowed when `from:` or `when` disambiguate. First match fires (top-to-bottom order).
- `to previous` returns to the state immediately prior (one level of navigation history).
- Transitions fire only via `Machine.send(…)`. Reactive triggers use `@on watch expr` that calls `Machine.send(…)` (§14.1). There is no `watch:` attribute on transitions.
- `when expr` must be pure. Impure guard → `LoadError :type`.
- **Transitions have no body.** Pre/post effects go in the destination's `on enter` or the source's `on leave`. If an effect is specific to a single transition (not a state), declare a distinct event per case.

### 13.4 Dispatch

```
Machine.send(:event)
Machine.send(:event, q: "hello")
Machine.state                     -- current state atom
Machine.state == :grid            -- state test
```

If no transition matches current state × event, `send` is silently ignored. `.send` accepts **named args only** — the parameter names are declared by the transition's `(param: Type, …)` clause.

### 13.5 Hook execution order

Transition S → D:

```
1. state S on leave
2. [state change: payload bound on D]
3. state D on enter
```

Initial entry (`:__init` pseudo-transition, one-time at machine birth): only step 3 fires, on the `initial` state.

If any hook returns `Result :err` that isn't consumed (no `?`, no `match`), the transition rolls back: machine remains in S with original payload. Automatic `:warn` log entry with the unhandled error.

### 13.6 `content =`

```
state :grid
  content =
    list installed_apps
      app ->
        trigger label: app.name -> apps.launch(app.id)
```

See §15.

---

## 14 · `@on` — unified reactive / lifecycle

Single primitive for every "when X, do Y" pattern. Replaces Deck 2.0's `@on`, `@stream`, `@task`, and `@machine watch:`.

### 14.1 Sources

An `@on` binds to exactly one source:

| Form | Semantic | Availability |
|---|---|---|
| `@on launch` | Lifecycle: app started | any app |
| `@on resume` | Lifecycle: app returned from suspend | any app |
| `@on suspend` | Lifecycle: app being suspended | any app |
| `@on terminate` | Lifecycle: app being killed | any app |
| `@on back` | Lifecycle: root back (§14.8) | any app |
| `@on open_url (params)` | Deep link routed to this app (§19) | apps declaring `@handles` |
| `@on panic` | Other-app panic event | `system.*` apps with `system.crashes` |
| `@on overrun` | *This* app's own handler exceeded budget | any app |
| `@on os.<event> (params)?` | OS event bus | any app with the relevant capability |
| `@on hardware.<event> (params)?` | Hardware event bus | any app with the relevant capability |
| `@on every <duration>` | Periodic timer | any app |
| `@on after <duration>` | One-shot timer (from launch or resume) | any app |
| `@on watch <pure_bool_expr>` | Reactive: fires on false→true edge | any app |
| `@on source <stream_expr>` | Subscription to `Stream T` | any app |

All 14 forms produce the same AST node `On { source, params?, modifiers?, body }`. The lexer recognizes the short-form names (`launch`, `resume`, `every`, etc.) as contextual keywords *after* `@on`; a single parser rule handles all cases.

**`watch` purity:** the expression in `@on watch <expr>` must be **pure** — no `!` calls, no capability reads. It may read `Machine.state`, named sources (§14.4), and `config.*`. An impure `watch` expression is `LoadError :type`.

### 14.2 Parameter binding

OS, hardware, and deep-link sources carry payloads. Three binding styles:

```
@on os.locked
  log.info("locked, user: {event.user}")

@on os.wifi_changed (ssid: s, connected: c)            -- named binders
  match c
    | true  -> App.send(:wifi_up, ssid: s)
    | false -> App.send(:wifi_down)

@on hardware.button (id: 0, action: :press)            -- value-pattern filter
  App.send(:button_pressed)
```

The implicit `event` binding is always available and contains the full payload as a record.

### 14.3 Stream pipelines

Stream source expressions use ordinary pipe. A pipe chain may span multiple lines; the runtime folds lines whose first non-whitespace token is `|>` onto the previous expression:

```
@on source sensors.temperature.watch()
           |> stream.filter(t -> t > 30.0)
           |> stream.throttle(5s)
           |> stream.distinct
  ->
    alerts.send(:hot, temp: event.value)
```

`stream.*` is a pure builtin module (`filter`, `map`, `throttle`, `debounce`, `distinct`, `skip`, `take_while`, `buffer`, `window`, `merge`, `combine`). No special parser support for operators — they are ordinary fn calls piped with `|>`.

### 14.3.1 Long-running operations as streams

Capabilities expose long-running operations as `Stream T`:

```
-- fs.copy(src, dst) -> Stream CopyProgress

@on source fs.copy(s.src, s.dst)
  ->
    FilesState.send(:progress, pct: event.pct)
```

The capability handles chunking, yielding, memory. Cancellation on subscription drop.

**Rule:** file copy, large download, bulk compute, etc. must be `Stream T` — not callback-based. Short I/O remains `Result T E`.

### 14.4 Naming a source

```
@on source ws.messages() as Messages
  ->
    App.notify(event)

-- elsewhere:
Messages.last()          -- T?
Messages.recent(10)      -- [T]
```

`as Name` gives the source identity and exposes read-only accessors `.last()` / `.recent(n)`. Without `as`, the `@on` is a fire-and-forget subscriber.

### 14.5 Body introduction

`@on` bodies are introduced by a bare `->` whenever *any* modifier line (`as`, `keep:`, `max_run_ms:`, or a stream operator chain) appears between the source header and the body. If no modifiers appear, the body is the direct indented block after the source line.

```
-- direct body (no modifiers):
@on os.wifi_changed (ssid: s)
  App.send(:wifi_up, ssid: s)

-- with modifier line → -> separates:
@on every 30m
  keep: true
  ->
    api.refresh_token()

-- with pipe chain → -> separates:
@on source ws.messages() as M
  ->
    App.notify(event)
```

### 14.6 Foreground vs background

By default, every `@on` except lifecycle hooks is **paused** when app is not foreground. To keep a handler active during suspension, declare `keep: true`:

```
@on every 30m
  keep: true
  ->
    api.refresh_token()
```

The OS enforces a per-app background budget. Exceeding fires `@on overrun` and cancels the current run.

### 14.7 Lifecycle contracts

| Hook | Timeout | I/O | On timeout |
|---|---|---|---|
| `launch` | 2000 ms | yes | load aborted |
| `resume` | 500 ms | yes | warn, app runs |
| `suspend` | 200 ms | brief save | OS suspends anyway |
| `terminate` | 100 ms | no heavy I/O | OS kills anyway |
| `back` | 50 ms | no | default back behavior |

### 14.8 `@on back` return value

```
@on back
  match unsaved_changes()
    | true  -> :confirm (
                 prompt:  "Discard changes?",
                 confirm: ("Leave", :unhandled),
                 cancel:  ("Stay",  :handled)
               )
    | false -> :unhandled
```

Returns one of:
- `:handled` — app consumed the back.
- `:unhandled` — delegate to OS (suspends the app).
- `:confirm (…)` — a variant payload with named fields `prompt: str`, `confirm: (str, atom)`, `cancel: (str, atom)`. The bridge renders a confirmation UI; the second element of each tuple is the return atom when that option is chosen.

`:confirm` is a regular atom-variant value (§4.2). No special syntax.

### 14.9 `@on panic` — system crash receiver

Only in `system.*` apps that declare `system.crashes` capability (§11.2). Receives panic events from **other** apps; an app never receives its own panic.

```
@on panic
  -- event: { app_id, kind, message, trace, ts }
  log.error("Crash in {event.app_id}", event)
  notify.post("App {event.app_id} crashed")
```

### 14.10 Cooperative scheduling

Each VM has **one logical thread**. `@on` handler bodies run to completion before the next starts. Events arriving while a body runs queue (bounded; default 32, OS-configurable per app). When the queue overflows, the oldest event is dropped and a `:warn` entry logged.

`@on watch` edges are evaluated after every `Machine.send` / `config.set` / source emission. A watch that rapidly toggles its own condition (>100 fires/sec on the same watch) is capped by the runtime and logs `:warn`.

### 14.11 `@on overrun`

Fires when one of *this app's own* handlers exceeded its `max_run_ms`.

```
@on overrun
  -- event: { handler: atom, elapsed_ms: int }
  log.warn("{event.handler} overran by {event.elapsed_ms}ms")
```

`max_run_ms:` is a per-handler metadata field (§22.2), alongside `keep:`. Default 200 ms.

---

## 15 · Content bodies

Inside a state's `content =` block and content fns. The bridge renders; the app declares intent only.

Every content node is represented internally as `Content { kind: atom, fields, children, handler? }`. The parser and walker dispatch on `kind`. The spec below groups kinds by purpose.

### 15.1 Structural primitives

```
list expr
  on empty -> content_nodes?
  has_more: bool_expr?
  on more -> action?
  binder ->
    content_nodes

group "label"
  content_nodes

form
  on submit -> action
  content_nodes             -- input intents aggregated into event.values
```

### 15.2 State markers

```
loading                         -- no fields; bridge picks affordance
error reason: str_expr          -- only field; app declares WHY
```

### 15.3 Data wrappers

Bare typed expressions presented by type:
```
p.author.display_name           -- str
n                               -- int / float
p.created_at                   -- Timestamp
```

Semantic wrappers when type alone is insufficient:

```
media expr
  alt:  str_expr
  role: (:avatar | :cover | :thumbnail | :inline)?

rich_text expr

status expr
  label: str_expr

chart expr
  label:   str_expr?
  x_label: str_expr?
  y_label: str_expr?

progress
  value: float_expr           -- 0.0..1.0
  label: str_expr?

markdown content_expr
  purpose:  (:reading | :reference | :fragment)?
  on link  -> expr?
  on image -> expr?
  focus:    str?
  describe: str?

markdown_editor
  value: str_expr
  on change    -> expr
  on cursor    -> expr?
  on selection -> expr?
  placeholder:    str?
  controlled_by:  MdEditorState?
  describe:       str?
```

### 15.4 Intents (13)

Intents split into two categories by how their action is wired:

**Input intents** — the user produces a value; the handler takes it. Shape: `<kind> <fields> on <event> -> action`.

```
toggle
  name: :lights
  state: s
  on change -> Machine.send(:toggle_lights, event.value)

range
  name: :volume
  value: v
  min: 0
  max: 100
  step: 1
  on change -> Machine.send(:set_vol, event.value)

choice
  name: :theme
  value: current
  options: [(label: "Dark", value: :dark), (label: "Light", value: :light)]
  on change -> config.set(:theme, event.value)

multiselect
  name: :filters
  value: selected
  options: […]
  on change -> Machine.send(:set_filters, event.value)

text
  name: :query
  value: q
  hint: "Search"
  max_length: 200
  on change -> Machine.send(:set_query, event.value)

password
  name: :pass
  value: p
  hint: "Password"
  on change -> Machine.send(:set_pass, event.value)

pin
  name: :code
  length: 6
  on complete -> Machine.send(:verify, event.value)

date
  name: :when
  value: d
  hint: "Pick a date"
  on change -> Machine.send(:set_date, event.value)

search
  name: :q
  value: s
  hint: "Filter"
  on change -> Machine.send(:search, event.value)
```

**Action intents** — the user triggers; there is no editable value. Shape: `<kind> <fields> -> action` (direct `->`, no handler name).

```
navigate
  label: "Settings"
  badge: unread_count
  -> App.send(:open_settings)

trigger
  label: "Refresh"
  badge: 0
  -> Timeline.send(:refresh)

confirm
  label: "Delete"
  prompt: "Permanently delete {file}?"
  -> File.send(:delete, file: file)

create
  label: "New post"
  -> Compose.send(:open_new)
```

**Passive intent** — declarative only, no handler:
```
share expr
  label: "Share this post"
```

### 15.5 Event bindings

| Handler | Event |
|---|---|
| `on change` (toggle / range / choice / multiselect / text / password / date / search / markdown_editor) | `event.value : T` (typed per intent) |
| `on complete` (pin) | `event.value : str` |
| `on submit` (form) | `event.values : {str: any}` keyed by each child's `name:` |
| `on more` (list) | `event.page : int` |
| `on empty` (list) | (no payload) |
| `on link` (markdown) | `event.url : str`, `event.text : str` |
| `on image` (markdown) | `event.url : str`, `event.alt : str` |
| `on cursor` (markdown_editor) | `event.cursor : int`, `event.formats : [atom]` |
| `on selection` (markdown_editor) | `event.selection : MdRange`, `event.text : str` |
| Action intents (`navigate` / `trigger` / `confirm` / `create`) | (no event binding — direct action on `->`) |
| `on enter` / `on leave` (state hooks, §13.1) | (no event payload) |

### 15.6 Content fns

See §4.5. Splice inline; may call pure business fns; may use `when` / `for`.

### 15.7 `when` / `for` in content

```
when cond
  content_node_or_nodes

for x in list_expr
  content_node_or_nodes
```

Conditional inclusion and iteration of content nodes. `when` and `for` are **not general expressions** — they are reserved in content bodies only; outside content they are ordinary identifiers. Produces zero-or-more content nodes.

---

## 16 · Logging

### 16.1 API

```
log.trace (msg: str, data: {str: any}?) -> unit
log.debug (msg: str, data: {str: any}?) -> unit
log.info  (msg: str, data: {str: any}?) -> unit
log.warn  (msg: str, data: {str: any}?) -> unit
log.error (msg: str, data: {str: any}?) -> unit
log.peek  (label: str, value: any)      -> any     -- passthrough; logs :debug
```

Five levels + `peek` for pipeline debugging. If correlation is needed, the author passes `trace_id:` as part of `data`:

```
log.info("linked", {trace_id: parent_trace, step: :fetch})
```

Runtime stamps `ts`, `app_id`, `source`, and an auto-generated `trace_id` per entry point (§16.4).

### 16.2 Log entry shape

```
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
```

### 16.3 Data cap

`data` capped at **1 KB serialized** per entry. Exceeds → truncated with `data.truncated: true` added.

### 16.4 Trace correlation

Runtime generates a fresh `trace_id` at each entry point:
- Each `@on` dispatch.
- Each `@service` method invocation.
- Each foreground render frame.

All `log.*` within that scope inherit it automatically. Override by passing `trace_id:` in `data`.

### 16.5 Sinks

| Sink | Retention | Controlled by |
|---|---|---|
| Console (UART) | stream only | OS-global level filter |
| Ring buffer (RAM) | 1024 entries per app | always; per-app quota |
| Persistent (SD) | 5 × 1 MB rotating files at `/deck/logs/{app_id}.{n}.log` | `@grants logging persist: true` |
| Remote shipping | streamed | DL3, opt-in |

**Crash guarantee:** `:error` level and panic records are flushed to SD immediately (not batched). Other levels batch every 5 s or 256 entries.

### 16.6 Reading (DL3, `system.*` only)

```
@use
  system.logs as logs

logs.ring      (app_id: str?, n: int?) -> [LogEntry]
logs.tail      (app_id: str?, since: Timestamp?) -> Stream LogEntry
logs.query     (q: LogQuery) -> [LogEntry]
logs.clear     (app_id: str) -> Result unit logs.Error
logs.set_level (app_id: str, level: LogLevel) -> Result unit logs.Error
```

```
@type LogQuery
  app_id   : str?
  level    : LogLevel?
  since    : Timestamp?
  until    : Timestamp?
  contains : str?
  trace_id : str?
  limit    : int?
```

### 16.7 Integration with errors

Automatic `:warn` / `:error` entries for the following:

- **`panic`** → `:error` entry with `data = {panic_kind, trace, machine_state}`. Immediate SD flush.
- **Unhandled `Result :err` escaping an `@on` body** → `:warn`.
- **Unhandled `Result :err` escaping a state hook (`on enter` / `on leave`)** → `:warn` + transition rollback (§13.5).
- **Unhandled `Result :err` escaping a `@service` method body** → returned as the method's `:err` to the caller; no automatic log (the caller decides).
- **Unhandled `Result :err` escaping a content fn body** → `:warn` + content element replaced with `error reason: …` marker.
- **`LoadError`** → `:error` entry under the system loader's `app_id`.

### 16.8 OS events

```
os.log_emitted    (entry: LogEntry)       -- Stream
os.log_quota_hit  (app_id: str)           -- app exceeded ring quota
```

---

## 17 · `@migrate`

```
@migrate
  from 0:
    nvs.set("schema_version", 1)

  from 1:
    let old = nvs.get("username")
    match old
      | :some v -> nvs.set("handle", v)
      | :none   -> unit
    nvs.delete("username")

  from 2:
    db.run("ALTER TABLE posts ADD COLUMN reply_ref TEXT")
```

Integer versions (not semver). The `from N:` header opens an indented block body.

- OS stores `last_migration_run` per app.
- At load, all blocks with `N >= last_migration_run` run in ascending order.
- If a block panics, `last_migration_run` does not advance; retried on next fix.

---

## 18 · `@service`

```
@service "social.bsky.app/feed"
  allow: :any
  keep:  false

  on :fetch_latest ()
    -> Result [Post] api.Error !
    = …

  on :post (text: str)
    -> Result PostRef api.Error !
    = …
```

- Service ID is the string literal after `@service`.
- `allow:` — `:any` / `:system` / `[app_id, …]` whitelist.
- `keep:` — if `true`, OS retains the VM resident; default `false`.

Each `on :name (params) -> ReturnType ! = body` declares a callable method. The shape is the same as `@machine` transitions and `@on` events — unified `on :atom (params)` pattern applies wherever Deck names an invocable with a signature.

Multiple `@service` blocks per app allowed (one per ID).

### 18.1 Invocation

```
@use
  service "social.bsky.app/feed" as bsky

fn refresh () ! =
  match bsky.fetch_latest()
    | :ok posts -> feed.update(posts)
    | :err e    -> log.warn("fetch fail: {e}")
```

IPC is typed; caller stubs are derived from declared service signatures. Mismatched signatures at bind → `LoadError :incompatible`.

### 18.2 Lifecycle

```
installed         → registered in OS service table (VM cold)
first call        → OS spawns VM, runs @on launch, methods callable
idle > 5 min and not keep:true
                  → OS may evict (runs @on terminate first)
call after evict  → cold respawn
```

Service VM and foreground VM for the same app are the **same VM**. Foreground UI and service methods run cooperatively in one thread (§14.10).

### 18.3 Registration

Every declared service ID must appear in `@app.serves: […]`. Consumers declare dependency in `@needs.services:`.

### 18.4 Permissions

OS validates caller identity at the IPC boundary against `allow:`. Unauthorized → `:err :unauthorized`.

### 18.5 Disabled service

If the service VM is quarantined after repeated panics (§11.2), further calls return `:err :service_unavailable` until manual relaunch or reinstall.

---

## 19 · `@handles`

```
@handles
  "bsky://profile/{handle}"
  "bsky://post/{post_id}"
  "https://bsky.app/profile/{handle}"

@on open_url (handle: h)
  App.send(:open_profile, handle: h)
```

Params extracted by the OS and passed to `@on open_url` as named binders.

Resolution order: most-specific first (more literal segments win). Equally specific overlapping patterns → `LoadError :resource`.

An app initiates a deep link via the `url` capability (`.deck-os`):

```
@use
  url as url

fn share_profile (handle: str) ! =
  match url.open("bsky://profile/{handle}")
    | :ok _   -> unit
    | :err e  -> log.warn("open url failed: {e}")
```

---

## 20 · `@assets`

```
@assets
  icon   : "assets/icon.png"            as: :icon
  avatar : "assets/default_avatar.png"
  cert   : "assets/api.crt"             as: :tls_cert  for_domain: "api.bsky.app"
  sound  : "assets/alert.wav"
  big    : download: "https://cdn.example/big.json"   ttl: 7d
```

Each entry: `name : <source> (as: :atom)? (for_domain: "…")? (ttl: Duration)?`. Source is either a quoted path (bundled) or `download: "url"` (fetched on first use, cached under `/deck/assets/{app_id}/{name}`, refreshed when TTL expires).

Referenced via `assets.asset(:name) -> AssetRef`. Bridge resolves paths at consumption time (media, TLS, audio). Raw bytes never enter the Deck heap.

---

## 21 · `@private`

`@private` is a **prefix modifier** applied to a single top-level declaration. It is not a bodied annotation; it takes no indented body of its own.

```
@private fn helper (x) = …

@private @type Internal
  field : int

@private @errors internal
  :bad_state  "…"
```

Symbols so marked are module-private. Accessing from outside the declaring file → `LoadError :unresolved`.

---

## 22 · Runtime envelope

### 22.1 Memory

- Declared in `@needs.max_heap` (e.g. `64KB`). Default 64 KB.
- Exceeded → `panic :limit`.
- Intern table is per-app (not global).

### 22.2 Time per handler

- Each `@on` body has `max_run_ms` (default 200 ms). Override as a metadata field alongside `keep:`:
  ```
  @on every 30m
    keep: true
    max_run_ms: 500
    ->
      …
  ```
- Exceeded → cancel, fire `@on overrun`, `:warn` log.
- Runtime yields between statements cooperatively.

### 22.3 Background budget

- Foreground: no budget (user-driven).
- Background (handlers with `keep: true`): N ms per M seconds (OS policy; default 500 ms / 30 s).
- `@service` handling: counts toward caller's context.

### 22.4 Suspend / resume persistence

- Local bindings and stack frames are not persisted.
- `@config` persists (NVS).
- Machine state (`.state` + current payload) is persisted to the app's service partition at a clean suspend.
- On resume: machine restores to last-saved state, `@on resume` runs, views re-render from current `config` + machine state.
- Unclean termination (panic, OS kill) may lose the last machine state write; `@on launch` runs on next spawn as if fresh.

### 22.5 Stack

- Default 512 frames; configurable per platform.
- Exceeded → `panic :limit`.
- TCO guaranteed for self-recursive and mutual-tail via trampolining.

### 22.6 Concurrency

One logical thread per VM. Event queue (default 32; OS-configurable per app). Oldest dropped on overflow with `:warn` log.

---

## 23 · Summary of changes from Deck 2.0

### 23.1 Annotations

- **Removed:** `@flow`, `@stream`, `@task`, `@permissions`, `@effects`, `@doc`, `@example`, `@test`.
- **Added:** `@needs` (replaces `@requires`), `@grants` (replaces `@permissions`), `@service`, `@migrate` (renamed from `@migration`).
- **14 total.**

### 23.2 Keywords

- **Removed:** `where`, `with`, `do`, `history`, `is`, `via`, effect aliases after `!`.
- **Added:** `panic`, `previous`, `current`.
- **Contextual (new):** `service` (in `@use`), handler-name tokens (`empty`, `more`, `submit`, `change`, `complete`, `link`, `image`, `cursor`, `selection`, `enter`, `leave`).

### 23.3 Simplifications

- **`where` → chained `let`.** No topological sort.
- **`with` → `record.update(r, {…})` builtin.** No keyword.
- **`do` eliminated.** Bodies are always implicit sequences.
- **`is` eliminated.** `== :atom` + `type_of(v) == :Name` replace.
- **Implicit `Result` propagation → postfix `?`.** No AST rewrite pass.
- **Stream operators as `stream.*` builtins piped with `|>`.** No `@on` operator syntax beyond `as`.
- **Transition `before` / `after` hooks removed.** Only state-scoped `on enter` / `on leave`.
- **`to <match_expr>` removed.** Use one event per target.
- **Nested `@machine` / `@on` / `@type` forbidden.** Annotations are top-level per file.
- **Intents unified.** Two shapes (input intents with `on <name> ->`; action intents with direct `->`). All 13 share one AST node type.
- **Content primitives unified.** Every content node is one AST node type with a `kind` atom.
- **`@on` sources unified.** 14 source forms share one AST node; the lexer handles short-name ergonomics.
- **Capability config evaluated once at bind.** No dynamic re-application.
- **Multi-line comments removed.** Use `--` blocks or triple-quoted docstrings.
- **Range literal `1..10` removed.** `min:` / `max:` fields in `@config`; guards use explicit `>=` / `<=`.

### 23.4 Specified for the first time

- Three-level error model with postfix `?` propagation.
- Structured log system with automatic trace stamping.
- Cooperative single-threaded VM with event queue.
- Foreground / background / service lifecycle with explicit budgets.
- `@service` IPC contract.
- Memory, time, stack budgets per app; `Size` literal type.
- Suspend / resume persistence model.
- Long-running operations as capability-exposed `Stream T`.
- `on :name (params)` as the unified shape for every named-invocable with a signature (`@machine` transitions, `@on` events, `@service` methods).

---

## 24 · Implementation size notes (non-normative)

**Parser:**
- 1 rule per annotation × 14 annotations (of which 7 share a "key-value indent block" shape: `@app`, `@needs`, `@grants`, `@config`, `@assets`, `@handles`, `@migrate`).
- 1 rule for `@on` with 14 short-form contextual keywords. All forms produce the same AST node.
- 1 rule for content primitives across 3 structural + 2 markers + 7 wrappers + 13 intents = 25 kinds, all one AST node.
- 1 rule for transitions, shared with `@service` methods and named `@on` events (unified `on :atom (params)` shape).
- 1 rule for type declarations covering both records and variants.

Total: ~20 parser functions.

**Walker:**
- 1 dispatch table for content kinds (25 entries).
- 1 dispatch table for `@on` sources (14 entries).
- 1 dispatch for transitions.

**Loader passes:**
- Lex / parse.
- Bind capability aliases + read `.deck-os` config schemas.
- Verify `@type` and `@errors` (no direct recursion, no duplicate constructors, fields match parametric arity).
- Verify match exhaustiveness against closed variants.
- Verify `?` placement (in `!` fns, compatible error domain).
- Verify `on <name>` handler names match the content primitive's declared set.
- Verify transitions reference declared states; no duplicate `initial`.
- Verify `@on watch` expressions are pure.
- Bind `@use cap as alias { config }` values (pure-evaluated once).
- Verify `@needs.caps` / `@needs.services` against platform.
- Verify `@grants` covers declared permissions.
- Verify content fns have no `!`.
- Verify no nested annotations.

No AST rewrite passes. No implicit type inference beyond content-fn fragment. No `watch:` dependency analysis at load (reactive tracking is runtime-only).

**Runtime:**
- One VM per app.
- One cooperative scheduler per VM.
- One event queue per VM.
- One capability binding table per VM.
- One config store (NVS-backed) per app.
- One log ring buffer + SD writer.
- One service registry (shared across VMs).

No coroutines, fibers, async / await. No green threads. No cycle collector — app-local arena with reference counting plus the discipline of `@config` being the only mutable-across-calls storage.

---

## 25 · Open for future decisions

- Full DL1 / DL2 / DL3 feature matrix re-cast in 3.0.
- Concrete `@on watch expr` dependency tracking algorithm.
- Builtin catalog audit (reduce ~187 to target ~80; naming consistency).
- Capability namespace normalization (bare `nvs` / `fs` vs qualified `network.http`).
- `@component` extension point for content primitives beyond §15.
- Effect inference mode (whether `!` can be derived).
- Hot reload contract across `@config` / `@migrate` / VM state.
- Per-user profile partitioning of `@config`.

---

**End of draft. Promote, amend, or discard as a whole.**
