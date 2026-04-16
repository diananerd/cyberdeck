# Deck Runtime Specification
**Version 2.0 — Interpreter Architecture and Execution Model**

---

## 1. Purpose

This document specifies the Deck interpreter internals for implementors. Each component is a distinct module with a clean interface. They can be built, tested, and understood independently. A student can implement a working lexer → parser → evaluator pipeline with zero OS or hardware dependency. Plugging in a mock bridge produces a fully functional test environment.

---

## 2. Architecture

```
  Source files (.deck)
        │
        ▼
  ┌───────────┐
  │   Lexer   │  Source text → Token stream
  └─────┬─────┘
        │
        ▼
  ┌───────────┐
  │   Parser  │  Tokens → AST
  └─────┬─────┘
        │
        ▼
  ┌───────────┐
  │   Loader  │  AST + OS surface → Verified, bound program
  └─────┬─────┘
        │
        ▼
  ┌─────────────────────────────────────────────────┐
  │                   Runtime                       │
  │                                                 │
  │  ┌────────────┐    ┌────────────────────────┐   │
  │  │  Evaluator │    │      Scheduler         │   │
  │  │  (pure AST │    │  Tasks, streams,        │   │
  │  │  reduction)│    │  condition tracking     │   │
  │  └──────┬─────┘    └───────────┬────────────┘   │
  │         └──────────┬───────────┘                │
  │                    │                            │
  │         ┌──────────▼──────────┐                 │
  │         │  Effect Dispatcher  │                 │
  │         └──────────┬──────────┘                 │
  │                    │                            │
  │         ┌──────────▼──────────┐                 │
  │         │  Navigation Manager │                 │
  │         └──────────┬──────────┘                 │
  └────────────────────┼────────────────────────────┘
                       │
                       ▼
             ┌──────────────────┐
             │    OS Bridge     │
             └──────────────────┘
```

---

## 3. Lexer

### 3.1 Responsibility
Converts UTF-8 source text to a flat token stream. Handles indentation, string interpolation, multi-line strings, duration literals, and comments. Produces no structure — that is the parser's job.

### 3.2 Token Types

```
-- Literals
TOK_INT          -- 42, -7
TOK_FLOAT        -- 3.14, -0.5
TOK_BYTE_LIT     -- 0xFF, 0x1A  (hex only; value 0-255; lexer error if > 0xFF)
TOK_BOOL         -- true, false
TOK_UNIT         -- unit
TOK_DURATION     -- 500ms, 5s, 1m, 1h, 1d  → stored as (int64_ms)
TOK_ATOM         -- :ok, :idle

-- Strings
TOK_STR_START    -- opening "  (begins string segment)
TOK_STR_TEXT     -- literal text segment inside string
TOK_INTERP_START -- {  inside an interpolated string
TOK_INTERP_END   -- }  inside an interpolated string
TOK_STR_END      -- closing "
TOK_MULTILINE_STR-- entire """...""" literal (after indentation stripping)

-- Identifiers
TOK_IDENT        -- value_name
TOK_TYPE_IDENT   -- TypeName

-- Keywords
TOK_LET  TOK_FN  TOK_MATCH  TOK_WHEN  TOK_IS  TOK_AS  TOK_WITH
TOK_AND  TOK_OR  TOK_NOT
TOK_FROM  TOK_TO  TOK_ON  TOK_EVERY  TOK_OPTIONAL  TOK_DO
-- TOK_IS parses as infix binary operator (see 01-deck-lang §7.8).
-- It is also used as a sub-keyword in @machine from/to syntax; the parser
-- disambiguates by context (annotation body vs. expression position).

-- Annotations
TOK_AT           -- @
TOK_ANN_NAME     -- identifier immediately after @

-- Operators
TOK_PLUS  TOK_MINUS  TOK_STAR  TOK_SLASH  TOK_PERCENT
TOK_PLUSPLUS   -- ++
TOK_PIPE       -- |>
TOK_PIPE_ERR   -- |>?
TOK_ARROW      -- ->
TOK_FAT_ARROW  -- => (reserved)
TOK_BANG       -- !
TOK_EQ         -- =
TOK_EQEQ       -- ==
TOK_NEQ        -- !=
TOK_LT  TOK_GT  TOK_LEQ  TOK_GEQ

-- Delimiters
TOK_LPAREN  TOK_RPAREN
TOK_LBRACKET  TOK_RBRACKET
TOK_LBRACE  TOK_RBRACE
TOK_COMMA  TOK_COLON  TOK_DOT  TOK_DOTDOT  TOK_DOTDOTDOT

-- Indentation (not emitted inside () [] {})
TOK_INDENT      -- indentation increased by 2
TOK_DEDENT      -- indentation decreased (possibly multiple per line)
TOK_NEWLINE     -- significant line break

-- Special
TOK_PERCENT_SIGN  -- % in battery conditions
TOK_EOF
TOK_ERROR         -- for error recovery
```

### 3.3 Indentation Rules

Baseline = 0 spaces. Each level = 2 spaces. Tabs are `TOK_ERROR`. Empty lines and comment-only lines do not affect indentation tracking. Inside `()`, `[]`, `{}`: indentation tokens suppressed. The lexer maintains an indent stack.

### 3.4 String Interpolation Lexing

For `"Hello, {name}!"`:
```
TOK_STR_START
TOK_STR_TEXT    "Hello, "
TOK_INTERP_START
TOK_IDENT       name
TOK_INTERP_END
TOK_STR_TEXT    "!"
TOK_STR_END
```

Nested expressions inside `{}` are fully lexed (including nested strings). The lexer tracks brace depth to find the correct closing `}`.

`\{` in a string is emitted as `TOK_STR_TEXT` containing a literal `{`. 

### 3.5 Multi-line String Processing

The lexer:
1. Collects all content between `"""` delimiters
2. Strips the first newline (immediately after opening `"""`)
3. Finds the minimum leading whitespace across all non-empty lines
4. Strips that prefix from every line
5. Produces a single `TOK_MULTILINE_STR` with the processed content

Interpolation applies identically.

### 3.6 Error Recovery

On unrecognized input, the lexer emits `TOK_ERROR` and advances one character. It does not abort — it collects all errors so the parser can find more. After lexing, if any `TOK_ERROR` exists, the loader stage does not run.

---

## 4. Parser

### 4.1 Responsibility
Converts token stream to a typed AST. Grammar is LL(1) — at most one token of lookahead, no backtracking. All nodes carry source location.

### 4.2 AST Node Types

```
-- Top-level declarations
AppDecl         { name, id, version, author?, license? }
UseDecl         { entries: [UseEntry] }
UseEntry        = CapEntry { path, alias, optional, when? }
                | LocalEntry { path }
PermissionsDecl { entries: [(path, reason)] }
ConfigDecl      { entries: [ConfigEntry] }
ConfigEntry     { name, type, default, range?, options?, unit? }
TypeDecl        { name: TypeIdent, fields: [(name, type_ann)] }
ErrorsDecl      { domain, variants: [(atom, str)] }
MachineDecl     { name: TypeIdent?, states, initial, transitions }
StateDecl       { name: atom, fields: [(name, type_ann)] }
TransDecl       { event, params, from_state, from_binding?, to_state,
                  to_fields: [(str, Expr)], guard? }
StreamDecl      { name, source?, from?, operators: [StreamOp] }
OnHookDecl      { event: str, body: Expr }
ViewDecl        { name, shows?, hides?, params, machine?, listens,
                  hooks: [(atom, Expr)], body: ComponentNode }
NavDecl         { root, stack: [NavEntry], modal: [NavEntry],
                  tab: [TabEntry] }
NavEntry        { route: atom, view: str, params: [(str, type_ann)] }
TabEntry        { route: atom, view: str, icon: atom, label: str }
TaskDecl        { name, every?, when_conds: [Expr], priority, battery, body: Expr }
MigrationDecl   { from_version: str, body: Expr }
TestDecl        { description: str, assertions: [Assertion] }
DocDecl         { text: str }
ExampleDecl     { expr: Expr, expected: Expr }
PrivateMarker   {}

-- Code definitions
FnDef           { name, params: [(str, type_ann)], return_type, effects: [str],
                  doc?, examples: [ExampleDecl], body: Expr }
LetBinding      { name, type_ann?, value: Expr }

-- Expressions
MatchExpr       { subject: Expr, arms: [MatchArm] }
MatchArm        { pattern: Pattern, guard?: Expr, body: Expr }
IfExpr          { cond: Expr, then_: Expr, else_: Expr }
DoExpr          { stmts: [DoStmt] }   -- DoStmt = LetBinding | Expr
PipeExpr        { left: Expr, right: Expr, propagate: bool }
CallExpr        { fn_: Expr, args: [Expr], named: [(str, Expr)] }
FieldExpr       { object: Expr, field: str }
WithExpr        { record: Expr, updates: [(str, Expr)] }
RecordExpr      { type_: TypeIdent, fields: [(str, Expr)] }
SendExpr        { machine?: Expr, event: atom, args: [(str, Expr)] }
LambdaExpr      { params: [(str, type_ann?)], body: Expr }
BlockExpr       { bindings: [LetBinding], result: Expr, where_: [LetBinding] }
VarExpr         { name: str }
TypeVarExpr     { name: TypeIdent }
AtomExpr        { name: atom }
AtomVariantExpr { name: atom, positional?: Expr, fields?: [(str, Expr)] }
InterpStrExpr   { parts: [StrPart] }  -- StrPart = Literal str | Interpolated Expr
LitInt  LitFloat  LitBool  LitStr  LitUnit
LitList  LitTuple  LitMap
NavPushExpr     { route: atom, params: [(str, Expr)] }
NavBackExpr     {}
NavRootExpr     {}
NavReplaceExpr  { route: atom, params: [(str, Expr)] }

-- Patterns
WildPat  LitPat  AtomPat  AtomVarPat  AtomFieldsPat
TypePat         { type_: TypeIdent, fields: [(str, str)] }
TuplePat  ListPat  SomePat  NonePat  OkPat  ErrPat
GuardPat        { base: Pattern, guard: Expr }

-- Component nodes
ComponentNode   { type_: atom, props: [(str, PropValue)], children: [ComponentNode],
                  action?: Action, on_change?: Expr, condition?: Expr,
                  loop?: LoopSpec, confirm?: ConfirmSpec,
                  accessibility?: Expr, component_id: uint64 }
```

### 4.3 Operator Precedence (lowest to highest)

1. `or`
2. `and`
3. `not`
4. `== != < > <= >= is`
5. `++`
6. `+ -`
7. `* / %`
8. unary `-`
9. `|> |>?` (pipe — left-associative; binds tighter than arithmetic so chains read naturally)
10. field access `.`, `with { }`, function application `()`

---

## 5. Loader

### 5.1 Responsibility
Takes the parsed AST and produces a fully verified, bound program. The most complex phase. Executes in order; if a stage produces errors, later dependent stages are skipped to avoid cascading false errors.

### 5.2 Loading Sequence

```
Stage 1: MODULE GRAPH RESOLUTION
  Walk @use local entries (./path)
  Parse each, add to module graph
  Detect circular imports → load error for each cycle
  Build complete module namespace map
  Resolve forward references within each module (declaration order irrelevant within a file)

Stage 2: OS SURFACE LOADING
  Load .deck-os from configured location
  Parse all @builtin, @capability, @event, @type declarations
  Build: capability registry, event registry, type registry

Stage 3: TYPE CHECKING
  For each @type declaration:
    Validate field types reference known types
    Detect illegal direct recursion (non-optional, non-list self-reference)
  For each RecordExpr construction:
    All declared fields present
    No extra fields
    Field value types match declared types
  For each FieldExpr:
    Field exists on the @type

Stage 4: CAPABILITY VERIFICATION
  For each @use capability entry:
    Exists in OS registry?
      NO, not optional → load error
      NO, optional     → mark statically unavailable
      YES              → bind alias
    Has @requires_permission?
      YES, not in @permissions, and @use entry is NOT optional → load error
      YES, not in @permissions, and @use entry IS optional     → warning
      YES, in @permissions                                     → continue

Stage 5: PERMISSION NEGOTIATION
  Call deck_bridge_request_permissions()
  Mark denied capabilities as runtime-unavailable

Stage 6: EFFECT VERIFICATION
  For each FnDef with effects: [alias1, alias2, ...]:
    Each alias must be in @use
  For each @on hook body:
    All !effects used are in @use
  For each @task run body:
    All !effects used are in @use
  For each component-returning function body:
    No !effect calls (component functions are pure)

Stage 7: NAV VALIDATION
  All route targets reference existing @view declarations
  All with: param types match the view's param declarations
  All -> :route navigation actions reference declared routes
  No route appears in more than one nav section

Stage 8: MACHINE VALIDATION
  All states in transitions exist in the state declarations
  The initial state is declared
  All to: field expressions have correct types
  Guards are pure expressions (no !effect calls, no send() calls)
  Multiple transitions for same event: from-states or guards must be
    statically mutually exclusive (warning if cannot be verified)

Stage 9: STREAM VALIDATION
  All source: capabilities exist, return Stream T
  All from: references resolve to declared @stream
  No circular stream derivation
  @listens references resolve to declared @stream
  combine_latest and merge: type compatibility

Stage 10: EXHAUSTIVENESS
  For each match expression:
    All possible top-level shapes of the matched type are covered
    Unreachable arms → warning
    Uncovered variants of known error types → error

Stage 11: CONFIG VALIDATION
  Default values satisfy range: and options: constraints
  Types are consistent

Stage 12: BIND
  Replace all VarExpr nodes with direct references to their definitions
  Replace all TypeVarExpr nodes with @type definitions
  Assign component_id to every ComponentNode:
    hash = FNV-1a 64-bit of "<file_path>:<line>:<col>"
    This is stable across hot reloads as long as the component's source position
    does not move. The bridge uses component_id for input routing and diff reconciliation.
  Instantiate app-level @machine instances in initial state
  Register @stream subscriptions with scheduler
  Register @task conditions with scheduler
  Register @on hooks with event dispatcher
  Ready → begin execution
```

### 5.3 Load Error Format

```
Load error [stage]: [file]:[line]:[col]
  [Problem in plain language]
  Hint: [Actionable suggestion]
  See:  [Related declaration, if helpful]
```

Multiple errors collected and reported together. The loader makes minimal assumptions to continue checking after errors (unresolvable names get synthetic `any` type for later stage validation).

---

## 6. Evaluator

### 6.1 Responsibility
Pure recursive AST evaluator. Does not mutate state, does not perform I/O, does not call the OS. All effects go through the Effect Dispatcher via continuation-passing. This separation makes the evaluator independently testable.

### 6.2 Value Representation

```
type Value =
  | VInt     of int64
  | VFloat   of float64
  | VBool    of bool
  | VStr     of string
  | VByte    of uint8
  | VUnit
  | VAtom    of string
  | VVariant of string * Value        -- :atom payload
  | VRecord  of string * (string * Value) list
    -- type_name, field list (preserves declaration order)
  | VList    of Value list
  | VTuple   of Value array
  | VMap     of (string * Value) list
  | VFn      of Closure
  | VResult  of [ Ok of Value | Err of Value ]
  | VOpt     of Value option
  | VStream  of stream_handle
  | VConn    of opaque_handle        -- WebSocket, Socket, BLE connections
  | VMachine of machine_instance
  | VDuration of int64               -- milliseconds
  | VTimestamp of int64              -- ms since epoch
  | VComponent of component_tree     -- component-returning fn result
  | VOpaque  of void*               -- bridge-defined types
```

### 6.3 Environment

```
type Frame = { bindings: HashMap<string, Value> }
type Env   = Frame list   -- head is innermost scope
```

Lookup: innermost to outermost. Not found: impossible after the Loader Bind stage (all names are resolved). Extend: prepend a new frame for each new scope.

### 6.4 Evaluation Rules

**Literals**: produce corresponding Value directly.

**VarExpr**: dereference bound pointer (post-bind: O(1) lookup).

**LetInExpr / BlockExpr**: evaluate bindings in sequence, each extending the env for subsequent bindings, evaluate result expression in final env.

**DoExpr**: evaluate each statement in sequence; each extends the env for subsequent statements. Final value: `VUnit`.

**LambdaExpr**: capture current env, produce `VFn(Closure { params, body, env })`.

**InterpStrExpr**: evaluate each interpolated part via `str()`, concatenate literal and interpolated segments.

**RecordExpr** (`Post { field: expr ... }`):
1. Evaluate all field expressions
2. Look up the `@type` definition
3. Verify all fields present and typed correctly (runtime check as defense-in-depth)
4. Produce `VRecord("Post", [(field, value), ...])`

**FieldExpr** (`.field`):
- On `VRecord(_, fields)`: find field in fields list
- On `VMap`: equivalent to `map.get`
- On module namespace: look up exported definition
- On machine instance: read field from current state payload
- Field not found: runtime error with message

**WithExpr** (`record with { field: expr }`):
1. Evaluate record
2. Evaluate each update expression
3. Produce new `VRecord` with updated fields, all other fields unchanged

**TypePat matching** (`Post { likes: n, text: t }`):
1. Value must be `VRecord("Post", _)`
2. For each named field in pattern: find in record fields, bind to pattern variable
3. Unmentioned fields ignored (not required to cover all fields)

**MatchExpr**: walk arms top to bottom; first structural match + true guard wins.

**Pattern algorithm**: see `01-deck-lang §8.2`. Identical semantics, now extended with `TypePat`.

**PipeExpr** (`a |> f`): evaluate a, evaluate f (must be VFn or call-expr), call f with a as first arg.

**PipeExpr error** (`a |>? f`):
- `VResult(Ok v)` → pipe v into f
- `VResult(Err e)` → short-circuit, return `VResult(Err e)`
- `VOpt(Some v)` → pipe v into f
- `VOpt(None)` → short-circuit, return `VOpt(None)`
- Other → runtime error

**CallExpr with effects** (`!effect` function):
1. Evaluate function and arguments
2. Produce `EffectRequest { alias, method, args, continuation }`
3. Suspend evaluation
4. Effect Dispatcher handles the request and resumes continuation with result

**SendExpr** (`Machine.send(:event, args)`):
1. Evaluate the machine reference
2. Find matching transition(s) for current state + event
3. Evaluate guard if present; if false, return current state unchanged
4. Evaluate `to:` field expressions in env extended with from-binding and params
5. Update live machine instance in interpreter state
6. Emit internal `MACHINE_STATE_CHANGED(machine_name, new_state)` event to the Scheduler's Condition Tracker. The Condition Tracker re-evaluates all `when:` conditions that reference this machine (via `is` operator), which may update `@task` eligibility, `@use when:` capability availability, and `@shows`/`@hides` visibility. The Navigation Manager observes the results from the Condition Tracker — it does not subscribe to `MACHINE_STATE_CHANGED` directly.
7. Return new state value

**IfExpr**: evaluate cond, branch to then or else.

**Recursion**: the evaluator uses an explicit call stack (not host language call stack). Direct and mutual recursion supported. Tail calls in `|>` chains and final match arms are trampolined. Stack depth limit configurable; default 512.

**Component evaluation**: component expressions (screen, column, text, button, etc.) are evaluated in a restricted context that reads machine state, config, and stream data but cannot perform effects. The result is a `VComponent(ComponentTree)`.

---

## 7. Effect Dispatcher

### 7.1 Responsibility
Routes all capability calls from the evaluator to the OS bridge. Maintains capability availability state. Handles stream subscriptions.

### 7.2 Effect Request

```
type EffectRequest = {
  alias:        string,
  method:       string,
  args:         Value list,
  named_args:   (string * Value) list,
  continuation: Value -> unit
}
```

The evaluator suspends and hands this to the dispatcher. The dispatcher invokes the bridge and resumes the continuation with the result.

### 7.3 Dispatch Algorithm

```
dispatch(request):
  1. Resolve alias → capability path in registry
  2. Check availability map:
       UNAVAILABLE → resume continuation with VResult(Err(VAtom(":unavailable")))
  2b. Check @singleton flag for this capability.method:
       IN PROGRESS → enqueue request; return without calling bridge.
       When the in-progress call completes, dequeue and dispatch next.
  3. Marshal Deck values → DeckValue C structs (overload selected by argc,
     then first-arg DeckType if arity ties; see 03-deck-os §7.1)
  4. Call deck_bridge_call(path, method, args, ...)
  5. Unmarshal result → Deck Value
  6. Resume continuation
```

### 7.4 Stream Subscription

For methods returning `Stream T`:
```
subscribe(request):
  1. Resolve and check availability
  2. Allocate stream buffer
  3. Call deck_bridge_subscribe(path, method, args, on_value_callback, ...)
  4. on_value_callback: marshal incoming value, push to stream buffer,
                        notify scheduler of new value
  5. Return stream handle to continuation
```

### 7.5 Availability Map

Updated by:
- Permission negotiation results (Loader stage 5)
- `when:` condition re-evaluations from Scheduler
- `os.permission_change` OS events

A capability is available iff: exists in OS registry AND permission granted AND `when:` condition is currently true (if declared).

---

## 8. Scheduler

### 8.1 Responsibility
Manages task triggering, stream buffering, condition state tracking, and reactive re-render triggers.

### 8.2 Task Registry

```
TaskEntry {
  name:         string
  every?:       Duration
  when_exprs:   Expr list
  run_body:     Expr
  priority:     atom
  battery:      atom
  last_run?:    Timestamp
  timer?:       OsTimerHandle
  cond_state:   bool    -- last evaluated value of all when: ANDed
}
```

Task execution: timer fires → evaluate all `when:` exprs → all true → enqueue with priority.

Tasks with `when:` only (no `every:`): triggered when condition state transitions false→true.

Task bodies run via the evaluator's do-block mechanism. Errors in task bodies: logged, task not re-triggered early, normal schedule resumes.

### 8.3 Condition Tracker

Maintains a table of `(condition_expr → current_bool)` for all `@use when:`, `@task when:`, `@shows when:`, `@hides when:` conditions.

Re-evaluates affected conditions on:
- OS events (`os.network_change`, `system.battery.watch()` stream updates, `os.permission_change`, etc.)
- Internal `MACHINE_STATE_CHANGED(machine_name, new_state)` events emitted by the Evaluator after each successful `send()`

For each condition whose value changed:
- `@use when:` → update Effect Dispatcher availability map
- `@task when:` → may trigger task (false→true transition fires the task if no `every:`)
- `@shows`/`@hides` → pass new desired-visibility state to Navigation Manager

Condition expressions are evaluated in a pure read-only context. They may read `@config`, call `@pure`-marked capability methods, use the `is` operator on machines and atoms, and access stream `.last()`. They may **not** contain `!effect` calls or `send()`.

### 8.4 Stream Buffers

```
StreamBuffer {
  capacity:  int            -- max values, ring. Default from OS surface.
  items:     Value array    -- ring buffer
  head:      int
  count:     int
  listeners: ViewRef list   -- views with @listens
}
```

On new value:
1. Apply `filter:` lambda if present; discard if false
2. Apply `map:` lambda if present; transform value
3. Apply additional operators (distinct, throttle, debounce, etc.)
4. Push to buffer (overwrite oldest if full)
5. Push to all derived stream buffers that `from:` this stream
6. Signal all `listeners` to re-render

`StreamName.last()` → `buffer.items[(head - 1 + capacity) % capacity]` as `VOpt`.
`StreamName.recent(n)` → last `min(n, count)` items as `VList`, oldest first.

### 8.5 Stream Operator Implementation

```
distinct:   track last emitted value; discard if equal to new value
throttle d: track last emit timestamp; discard if time.since(last) < d
debounce d: reset timer on each new value; emit only when timer expires without new value
buffer_n n: accumulate n values into a list, then emit the list, reset
window_n n: maintain a sliding window list; emit the window on each new value
take_while: evaluate predicate; when first false, permanently deactivate stream
skip n:     discard first n values; then pass through
```

---

## 9. Navigation Manager

### 9.1 Navigation State

```
NavState {
  stack:      ViewInstance list   -- index 0 = root
  modals:     ViewInstance list
  active_tab: atom?
}

ViewInstance {
  route:    atom
  view_def: ViewDecl
  params:   HashMap<string, Value>
  machine?: MachineInstance
}
```

### 9.2 @shows/@hides Evaluation

After every `send()` (machine state change), the navigation manager re-evaluates all `@shows when:` / `@hides when:` conditions:
1. Determine desired visibility for every declared `@shows` view
2. Compare to current display state
3. For each change: call `deck_bridge_render` with new view or nil

Multiple views with `@shows` conditions: at most one is visible at the root position at a time. The last-declared view whose condition is true wins. Overlapping conditions are a design problem, not a language error, but the loader emits a warning if two `@shows` conditions could be simultaneously true.

### 9.3 Explicit Navigation

```
NavPushExpr:
  1. Evaluate params
  2. Instantiate ViewInstance
  3. Initialize view-local machine if declared (initial state)
  4. Call current view's on disappear (if any)
  5. Push to stack (or modal stack for :modal routes; switch tab for :tab)
  6. Render new view
  7. Call new view's on appear

nav.back:
  1. Call current view's on disappear
  2. If modal stack non-empty: pop modal
  3. Else: pop main stack
  4. Destroy popped view's machine instance if any
  5. Call previous view's on resume
  6. Re-render previous view

nav.root:
  1. Unwind stack calling on disappear for each
  2. Re-render root view (on appear)
```

### 9.4 View Re-render Trigger

Triggered by:
- Navigation (view appears or changes)
- Any `send()` to any machine (state may affect `match state` in body)
- New value in any `@listens` stream
- `@shows`/`@hides` condition change
- View `on resume` lifecycle

For each trigger:
1. Evaluate view body in current context (machine state, config, streams, params)
2. Structural diff against previous component tree
3. Call `deck_bridge_render(view_name, delta)` — only changed subtrees

---

## 10. Memory Management

### 10.1 Immutability and Sharing

All values are immutable. Structural sharing: list/map operations produce new values that share unchanged subtrees with originals. Machine state transitions replace the state value; old state is freed when ref count drops to zero.

### 10.2 Reference Counting

The interpreter uses reference counting. Reasons: predictable pause times (critical for embedded), no separate GC thread, memory pressure immediately visible via `system.info.free_heap()`.

**Cycle prevention**: Deck's value model cannot produce cycles. Closures capture immutable environments. Machines hold state values, not closures that reference the machine. No cycle detection needed.

### 10.3 String Interning

Atom strings are interned (one heap allocation per unique atom string). String comparisons for atoms are pointer equality. This is important for performance because pattern matching on atoms is the dominant branching operation.

### 10.4 Component Trees

Component trees are allocated fresh on each render call, diffed against the previous tree, and the previous tree freed. The bridge receives only the delta. Previous tree retained until the bridge confirms rendering.

### 10.5 Stack

The evaluator uses an explicit `Value stack` (not the host call stack). Default depth: 512. Configurable in the OS bridge initialization. Overflow is a runtime error with a clear stack trace of machine states and function names.

---

## 11. Developer Tools

### 11.1 Run Modes

```
deck run app.deck              -- normal execution
deck test [file|--all]         -- run @test blocks and @example assertions
deck check app.deck            -- full load pipeline, no execution; reports all errors
deck fmt app.deck              -- reformat source in place (canonical style)
deck fmt --check app.deck      -- exit non-zero if not formatted (for CI)
deck watch app.deck            -- hot reload on file change (if OS supports)
deck repl                      -- interactive evaluator for expressions
```

### 11.2 Test Mode

```
deck test
  PASS  "sync does not run offline"
  PASS  "celsius formats correctly"
  FAIL  "alert fires at threshold"
        assert m.send(:update, 80.0) is :alert
        Got:    :active (temp: 80.0, max: 80.0)
        At:     tasks/monitor.test.deck:12

  3 passed, 1 failed
```

`@example` assertions from function docs are included automatically. Test files named `*.test.deck` in any directory under the project root are discovered automatically. `random.seed(42)` is called before each test unless overridden in the test body.

### 11.3 REPL

The REPL operates in a stateless expression-evaluation mode. No `@app`, no capabilities (unless the OS bridge is initialized). Useful for learning the language and testing pure expressions.

```
deck repl
> 2 + 2
4
> let xs = [1, 2, 3, 4, 5]
> filter(xs, x -> x > 2)
[3, 4, 5]
> Post { uri: "a", text: "b", author: Author { did: "d", handle: "h", name: "h" }, likes: 0, reposts: 0, created_at: time.now() }
Post { uri: "a", text: "b", ... }
```

### 11.4 Formatter (deck fmt)

Canonical formatting rules:
- 2-space indentation
- No trailing whitespace
- Blank line between top-level definitions
- `@` annotations before their definitions with no blank line between them
- Function signatures on one line if under 80 characters; otherwise wrapped
- `match` arms: one per line, aligned `|` and `->`
- `do` blocks: one statement per line

### 11.5 Debugger (DAP Protocol)

The interpreter implements the Debug Adapter Protocol (DAP) for IDE integration. Supports:
- Breakpoints on source lines
- Step over, step into, step out
- Variable inspection (local env frame contents)
- Machine state inspection (`Machine.state` displayed as structured data)
- Stream buffer inspection
- Conditional breakpoints (any Deck expression)

DAP server is activated with `deck run --dap app.deck` on a configurable port.

### 11.6 Hot Reload

When `deck watch` is running and a `.deck` file changes:
1. Re-lex and re-parse the changed file
2. Re-run Loader stages 3–12 (module graph already resolved)
3. If successful: hot-swap the affected function/view/task definitions
4. Re-render all visible views
5. Preserve current machine state and stream subscriptions (state is not reset)
6. If load errors: report them without crashing the running app

Hot reload does not work for changes to:
- `@app` — identity change requires restart
- `@use` — capability set change requires restart
- `@permissions` — permission set change requires restart
- `@machine` blocks (any part: state declarations, transitions, guards, initial state) — live machine instances cannot be migrated to a new state schema
- `@type` field declarations — live record values cannot be migrated
- `@stream source:` declarations — subscriptions must be re-established

All other changes (`fn`, `let`, `@view` body, `@task` body, `@on` hooks, `@config` defaults, derived `@stream` operators) support hot reload.

---

## 12. Interpreter Module Boundaries

For modular implementation:

```
lexer.{h,c}
  Input:  source text (UTF-8 string + length)
  Output: Token array + error list

parser.{h,c}
  Input:  Token array
  Output: AstNode* root + error list

loader.{h,c}
  Input:  AstNode* root, DeckOsSurface*, BridgeInterface*
  Output: BoundProgram* + error list

evaluator.{h,c}
  Input:  Expr, Env*
  Output: Value (pure evaluation)
         | EffectRequest (for effect calls — suspends evaluation)

effect_dispatcher.{h,c}
  Input:  EffectRequest, CapabilityMap*, BridgeInterface*
  Output: resumes evaluator continuations

scheduler.{h,c}
  Input:  TaskRegistry, ConditionTracker, EventQueue
  Output: task execution requests to evaluator

nav_manager.{h,c}
  Input:  NavDecl, ViewDecl[], BridgeInterface*
  Output: deck_bridge_render() calls, input routing
```

**Dependency summary:**
- `lexer`, `parser`, `evaluator`: no bridge dependency
- `loader`: depends on OS surface structure, not bridge
- `effect_dispatcher`, `scheduler`, `nav_manager`: depend on bridge

A complete, testable pure-language implementation needs only lexer + parser + evaluator + a mock value environment. Add loader + mock OS surface for load-time verification. Add bridge for hardware execution.
