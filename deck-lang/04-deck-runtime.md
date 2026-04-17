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
StateDecl       { name: atom, fields: [(name, type_ann)],
                  content?: Expr,           -- view content body for this state
                  on_enter?: Expr,          -- hook evaluated on state entry
                  on_exit?: Expr }          -- hook evaluated on state exit
TransDecl       { event, params, from_state, from_binding?, to_state,
                  to_fields: [(str, Expr)], guard?,
                  watch?: Expr }            -- boolean condition for reactive firing
FlowDecl        { name: TypeIdent?,        -- equivalent to MachineDecl with step blocks
                  steps: [StepDecl],        -- ordered step declarations
                  initial: atom }
StepDecl        { name: atom,              -- step state name
                  content: Expr,            -- view content body for this step
                  on_enter?: Expr,
                  on_exit?: Expr }
StreamDecl      { name, source?, from?, operators: [StreamOp] }
OnHookDecl      { event: str, body: Expr }
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
-- Note: NavPushExpr / NavBackExpr / NavRootExpr / NavReplaceExpr are removed.
-- Navigation is driven entirely by machine state transitions (SendExpr).
-- `to history` in a TransDecl signals the OS bridge to perform back navigation.

-- Patterns
WildPat  LitPat  AtomPat  AtomVarPat  AtomFieldsPat
TypePat         { type_: TypeIdent, fields: [(str, str)] }
TuplePat  ListPat  SomePat  NonePat  OkPat  ErrPat
GuardPat        { base: Pattern, guard: Expr }

-- View content nodes (produced by evaluating a machine state or @flow step content= body)
-- The evaluator produces a [ViewContentNode] from the content= expression.
-- Regular language constructs (match, let, for) are standard Expr nodes;
-- the evaluator resolves them to [ViewContentNode] sequences at runtime.
ViewContentNode
  -- Structural primitives
  = VCList        { source: Expr, empty?: [ViewContentNode], more?: Expr,
                    on_more?: Expr, binding: str, body: [ViewContentNode] }
  | VCGroup       { label: str, body: [ViewContentNode] }
  | VCForm        { on_submit: Expr, body: [ViewContentNode] }
  | VCFlow        { machine: TypeIdent, steps: [FlowStep] }
  -- View state
  | VCLoading     {}
  | VCError       { message: Expr }
  -- Data content
  | VCData        { expr: Expr }               -- bare data expression or fragment call in content position
  | VCMedia       { expr: Expr, alt: Expr, hint?: atom }
  | VCRichText    { expr: Expr }
  | VCStatus      { expr: Expr, label: Expr }
  | VCChart       { expr: Expr, label?: Expr, x_label?: Expr, y_label?: Expr }
  | VCProgress    { value: Expr, label?: Expr }
  -- Intents
  | VCToggle      { name: atom, state: Expr, on_: Expr }
  | VCRange       { name: atom, value: Expr, min: float, max: float, step?: float, on_: Expr }
  | VCChoice      { name: atom, value: Expr, options: Expr, on_: Expr }
  | VCMultiselect { name: atom, value: Expr, options: Expr, on_: Expr }
  | VCText        { name: atom, value?: Expr, hint?: Expr, max_length?: int, format?: atom, on_: Expr }
  | VCPassword    { name: atom, value?: Expr, hint?: Expr, on_: Expr }
  | VCPin         { name: atom, length: int, on_: Expr }
  | VCDate        { name: atom, value?: Expr, hint?: Expr, on_: Expr }
  | VCSearch      { name: atom, value?: Expr, hint?: Expr, on_: Expr }
  | VCNavigate    { label: Expr, target: NavTarget }
  | VCTrigger     { label: Expr, action: Expr }
  | VCConfirm     { label: Expr, message: Expr, action: Expr }
  | VCCreate      { label: Expr, target: NavTarget }
  | VCShare       { expr: Expr, label?: Expr }

FlowStep  { state: atom, binding?: str, body: [ViewContentNode] }

-- NavTarget: destination of a navigate or create intent (rendered as VCNavigate / VCCreate)
-- NTRoute maps to a Machine.send() that transitions to the named state.
-- NTBack signals the OS bridge to perform back navigation (equivalent to to history).
-- NTRoot signals the OS bridge to return to the app root state.
NavTarget = NTRoute   { route: atom, params: [(str, Expr)] }
          | NTBack
          | NTRoot

-- StreamOp: operators in a derived @stream declaration
StreamOp  = FilterOp      { pred: Expr }               -- filter: x -> bool
          | MapOp          { fn_: Expr }               -- map: x -> y
          | DistinctOp     {}                          -- distinct: true
          | ThrottleOp     { duration: Expr }          -- throttle: 5s
          | DebounceOp     { duration: Expr }          -- debounce: 2s
          | BufferNOp      { n: int }                  -- buffer_n: 10
          | WindowNOp      { n: int }                  -- window_n: 5
          | TakeWhileOp    { pred: Expr }              -- take_while: x -> bool
          | SkipOp         { n: int }                  -- skip: 3
          | CombineLatestOp { sources: [str] }         -- combine_latest (up to 8)
          | MergeOp         { sources: [str] }         -- merge (all same type)
```

### 4.3 DeckViewContent C API

`DeckViewContent` is the opaque type passed to `deck_bridge_render()` (see `03-deck-os §7`). Bridge implementors traverse it using the accessors below.

```c
/* Opaque types */
typedef struct DeckViewContent DeckViewContent;
typedef struct DeckViewNode    DeckViewNode;

/* Node type tag — mirrors ViewContentNode variants */
typedef enum {
  DVC_LIST, DVC_GROUP, DVC_FORM, DVC_FLOW,
  DVC_LOADING, DVC_ERROR,
  DVC_DATA, DVC_MEDIA, DVC_RICH_TEXT, DVC_STATUS, DVC_CHART, DVC_PROGRESS,
  DVC_TOGGLE, DVC_RANGE, DVC_CHOICE, DVC_MULTISELECT,
  DVC_TEXT, DVC_PASSWORD, DVC_PIN, DVC_DATE, DVC_SEARCH,
  DVC_NAVIGATE, DVC_TRIGGER, DVC_CONFIRM, DVC_CREATE, DVC_SHARE
} DvcNodeType;

/* Root — ordered list of top-level nodes */
int           deck_content_count (DeckViewContent* c);
DeckViewNode* deck_content_node  (DeckViewContent* c, int i);

/* Every node */
DvcNodeType   deck_node_type     (DeckViewNode* n);

/* Children (VCList body, VCItem, VCGroup, VCForm, VCFlow active step) */
int           deck_node_child_count  (DeckViewNode* n);
DeckViewNode* deck_node_child        (DeckViewNode* n, int i);

/* VCList: empty subtree, more flag */
int           deck_node_empty_count  (DeckViewNode* n);
DeckViewNode* deck_node_empty_child  (DeckViewNode* n, int i);
bool          deck_node_more         (DeckViewNode* n);  /* evaluated more: value */

/* VCGroup / VCStatus: label string */
const char*   deck_node_label        (DeckViewNode* n);

/* VCError: message string */
const char*   deck_node_message      (DeckViewNode* n);

/* Data content: evaluated value (VCData, VCMedia, VCRichText, VCStatus,
   VCChart, VCProgress) */
DeckValue*    deck_node_value        (DeckViewNode* n);

/* VCMedia */
const char*   deck_node_alt          (DeckViewNode* n);
const char*   deck_node_hint         (DeckViewNode* n);  /* atom str or NULL */

/* VCChart / VCProgress: optional label strings */
const char*   deck_node_x_label      (DeckViewNode* n);  /* NULL if absent */
const char*   deck_node_y_label      (DeckViewNode* n);  /* NULL if absent */

/* Intents: name atom and current value */
const char*   deck_node_name         (DeckViewNode* n);  /* intent name: atom */
DeckValue*    deck_node_intent_value (DeckViewNode* n);  /* current value; NULL if none */

/* VCRange */
double        deck_node_min          (DeckViewNode* n);
double        deck_node_max          (DeckViewNode* n);
bool          deck_node_has_step     (DeckViewNode* n);
double        deck_node_step         (DeckViewNode* n);

/* VCPin */
int           deck_node_pin_length   (DeckViewNode* n);

/* VCText / VCPassword / VCSearch / VCDate: hint string */
const char*   deck_node_hint_text    (DeckViewNode* n);  /* NULL if absent */

/* VCChoice / VCMultiselect: options */
int           deck_node_option_count (DeckViewNode* n);
const char*   deck_node_option_label (DeckViewNode* n, int i);
DeckValue*    deck_node_option_value (DeckViewNode* n, int i);

/* VCNavigate / VCTrigger / VCConfirm / VCCreate: label */
const char*   deck_node_text         (DeckViewNode* n);

/* VCConfirm: confirmation message */
const char*   deck_node_confirm_msg  (DeckViewNode* n);

/* VCNavigate / VCCreate: route atom ("back" and "root" are special) */
const char*   deck_node_route        (DeckViewNode* n);

/* VCFlow: machine name, currently active state atom, active step children */
const char*   deck_node_machine      (DeckViewNode* n);
const char*   deck_node_flow_state   (DeckViewNode* n);
```

All `DeckViewContent*` and `DeckViewNode*` pointers are valid only for the duration of the `deck_bridge_render()` call. The bridge must copy any data it needs to retain.

---

### 4.4 Operator Precedence (lowest to highest)

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
Stage 0: ANNOTATION PLACEMENT VALIDATION
  Verify that @app, @use, @permissions, @config, @on, @migration
    only appear in app.deck → load error for each violation in other files

Stage 0.5: MIGRATION EXECUTION (if prior version exists)
  Query OS for previously installed app version
  If found:
    Collect all @migration blocks whose from: range matches the installed version
    Sort by specificity (most specific first: "1.2.3" > "1.2.x" > "1.x" > "<2.0")
    For each, check runtime's migration-completion log (keyed by hash of app.id + from_range)
    Execute only those not yet run, in order
    On success: log completion hash
    On failure (returns :err or panics): abort — do not proceed to Stage 1;
      report failure with migration's from: range and the error
  Migration bodies run in a restricted context (store, db, nvs, config.set only — see 02-deck-app §15)

Stage 1: MODULE GRAPH RESOLUTION
  Walk @use local entries (./path) from app.deck only
  Parse each, add to module graph
  Detect circular imports → load error for each cycle
  Build complete module namespace map (all modules available project-wide)
  Resolve forward references within each module (declaration order irrelevant within a file)

Stage 2: OS SURFACE LOADING
  Determine .deck-os path:
    1. os_surface_path config field if non-NULL
    2. Else: $DECK_OS_SURFACE environment variable if set
    3. Else: /etc/deck/system.deck-os if it exists
    4. Else: compiled-in default (fatal error if none)
  Parse top-level @os block
  Process @include directives depth-first (path relative to including file;
    circular includes → fatal parse error)
  Parse all @builtin, @capability, @event, @type, @opaque declarations
  Build: capability registry, event registry, type registry, opaque-type registry

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
  For each CallExpr:
    If both positional args and named args are non-empty → load error
    (positional and named args cannot be mixed in one call)
  For each FnDef containing @where bindings:
    Build dependency graph among where bindings
    Detect cycles → load error listing the full cycle
    Topological-sort and store pre-sorted binding list
  For each DoExpr:
    Non-let statements that produce a type other than unit or Result unit E → load error

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
  For each capability call:
    If the method is marked @pure in the OS surface → exempt from !effect requirement
    Otherwise → the calling function must declare !alias for the capability's alias
  For each @on hook body:
    All !effects used are in @use
  For each @task run body:
    All !effects used are in @use
  For each view content function body (inferred or declared -> fragment):
    No !effect calls allowed (view content evaluation is always pure) → load error
  For each @on hook event name:
    Must exist in the OS event registry (from Stage 2) → load error if not found

Stage 7: FLOW VALIDATION
  All flow: and machine: references in states resolve to declared @flow or @machine declarations
  All watch: conditions are boolean expressions (no !effect calls, no send() calls)
  to history is only used from states reachable via a transition, not from initial
  Entry context types are compatible: if a @flow is referenced from multiple states
    with payloads, the payload types must be compatible across all call sites

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
  CombineLatestOp: each source name resolves to a declared @stream;
    source count > 8 → load error; all sources accessible
  MergeOp: each source name resolves to a declared @stream;
    all sources must have the same element type T → load error if types differ
  combine_latest resulting type: Stream (T1, T2, ...) matching source types
  merge resulting type: Stream T (same as all sources)

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
  | VFragment  of ViewContentNode list  -- view content function result; spliced inline at call site
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

**Float NaN**: After any float arithmetic (`+`, `-`, `*`, `/`, `%`) or builtin math function that would produce an IEEE NaN value, the evaluator returns `VAtom(":nan")` instead. The `:nan` atom propagates through pure arithmetic (`:nan + 5` → `:nan`). `math.is_nan(v)` tests for it; `match` on a float value should cover `:nan` if the value may be invalid.

**FieldExpr** (`.field`):
- On `VRecord(_, fields)`: find field in fields list
- On `VMap`: equivalent to `map.get`
- On module namespace: look up exported definition
- On machine instance: read field from current state payload
- On `config` namespace (the special config binding): look up the field name in the app's `@config` declarations and return the current persisted value. `config` is always in scope; no `@use` or `!effect` annotation required for reading config values.
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
6. Emit internal `MACHINE_STATE_CHANGED(machine_name, new_state)` event to the Scheduler's Condition Tracker. The Condition Tracker re-evaluates all `when:` conditions that reference this machine (via `is` operator), which may update `@task` eligibility and `@use when:` capability availability. The Navigation Manager observes the results from the Condition Tracker — it does not subscribe to `MACHINE_STATE_CHANGED` directly.
7. Return new state value

**Recursion**: the evaluator uses an explicit call stack (not host language call stack). Direct and mutual recursion supported. Tail calls in `|>` chains and final match arms are trampolined. Stack depth limit configurable; default 512.

**View content evaluation**: the `content =` body of a machine state or `@flow` step is evaluated in a restricted context that reads machine state, config, and stream data but cannot perform effects. The result is a `[ViewContentNode]` list that the runtime serializes into a `DeckViewContent*` and passes to `deck_bridge_render()`. `match`, `let`, and `for` within content are evaluated normally; their results must be `ViewContentNode` values or lists thereof. When a view content function (one whose body produces `ViewContentNode` values and carries no `!` effects) is called inside a content body, its result is a `VFragment` — a `[ViewContentNode]` list that is spliced inline into the parent node list at the call site.

**Intent handler evaluation**: when the bridge delivers an intent event via `deck_bridge_handle_intent(view_name, intent_name, payload)`, the evaluator locates the `on ->` handler of the intent whose `name:` atom matches `intent_name` and invokes it with the environment extended by an `event` binding:
```
event.value  -- the new value; type depends on intent (see 02-deck-app §12.7)
```
`event` is in scope only inside `on ->` handlers of input intents (`toggle`, `range`, `choice`, `multiselect`, `text`, `password`, `pin`, `date`, `search`). Non-input intents (`navigate`, `trigger`, `confirm`, `create`, `share`) do not bind `event`. Referencing `event` outside a handler is a load error.

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

Maintains a table of `(condition_expr → current_bool)` for all `@use when:` and `@task when:` conditions.

Re-evaluates affected conditions on:
- OS events (`os.network_change`, `system.battery.watch()` stream updates, `os.permission_change`, etc.)
- Internal `MACHINE_STATE_CHANGED(machine_name, new_state)` events emitted by the Evaluator after each successful `send()`

For each condition whose value changed:
- `@use when:` → update Effect Dispatcher availability map
- `@task when:` → may trigger task (false→true transition fires the task if no `every:`)

Condition expressions are evaluated in a pure read-only context. They may read `@config`, call `@pure`-marked capability methods, use the `is` operator on machines and atoms, and access stream `.last()`. They may **not** contain `!effect` calls or `send()`.

### 8.4 Stream Buffers

```
StreamBuffer {
  capacity:  int            -- max values, ring. Default from OS surface.
  items:     Value array    -- ring buffer
  head:      int
  count:     int
}
```

On new value:
1. Apply `filter:` lambda if present; discard if false
2. Apply `map:` lambda if present; transform value
3. Apply additional operators (distinct, throttle, debounce, etc.)
4. Push to buffer (overwrite oldest if full)
5. Push to all derived stream buffers that `from:` this stream

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

combine_latest (sources: [StreamName]):
  Maintain a "last value" slot for each source (initially :none).
  On every new value from any source: update that source's slot.
  Emit a tuple of all current values only when ALL slots are :some (non-empty).
  After first full emission, emit on every subsequent update from any source.
  Result type: Stream (T1, T2, ...) matching the element types of each source.
  Up to 8 sources (load error if exceeded).

merge (sources: [StreamName]):
  All sources must have the same element type T.
  Emit each value from any source as it arrives, in arrival order.
  Result type: Stream T.
  No buffering: values from different sources are interleaved transparently.
```

---

## 9. Navigation Manager

### 9.1 Navigation State

The Navigation Manager tracks the set of active machine instances and the currently rendered content. There is no push/pop route stack in the runtime — the OS bridge maintains any platform-level back history.

```
NavState {
  machines:      HashMap<string, MachineInstance>  -- all live @machine instances
  active_content: DeckViewContent*?                -- last rendered content
}
```

The active content is always derived from the `content =` body of the currently active state in the root `@machine` or `@flow`. When a machine state changes, the Navigation Manager re-evaluates that body and calls `deck_bridge_render()` with the new `DeckViewContent*`.

### 9.2 Reactive Transitions (watch:)

The runtime tracks `watch:` conditions declared on `@machine` transitions. Each `watch:` condition is a boolean expression evaluated in the same pure read-only context as `@task when:` expressions — it may read machine state, config, stream `.last()`, and call `@pure` capability methods, but may not contain `!effect` calls or `send()`.

Re-evaluation pipeline:
```
machine state change | stream value | network state
  → Condition Tracker re-evaluates affected watch: conditions
  → if a watch: condition changed to true and the machine is currently
    in the matching from: state → transition fires automatically
  → send() is called internally with the watch: event
  → MACHINE_STATE_CHANGED emitted → further reactive evaluation
```

**Ordering rule**: When multiple `watch:` transitions apply to the same machine and the machine is in a state where more than one could fire, they are evaluated top-to-bottom in declaration order. The first watch: condition that is currently true fires; remaining are not evaluated for that cycle.

### 9.3 Machine State Transitions as Navigation

Navigation in Deck is not a push/pop stack operation — the flow structure defined by `@machine` and `@flow` declarations IS the navigation. State transitions drive what content is displayed.

**Machine.send() is the only navigation primitive.** When a transition fires (via an explicit `send()` call in an `on ->` handler, a `VCTrigger` action, or a `watch:` condition), the evaluator updates the machine instance and emits `MACHINE_STATE_CHANGED`.

The OS bridge receives `MACHINE_STATE_CHANGED` events from the Navigation Manager and re-renders the `content =` of the newly active state:
```
send(:event, args)
  → transition fires → machine instance updated
  → MACHINE_STATE_CHANGED emitted
  → Navigation Manager evaluates content= of new active state
  → deck_bridge_render(content) called with new DeckViewContent*
  → bridge updates display
```

**Back navigation via `to history`**: When the runtime evaluates a transition whose `to:` target is `history`, it signals the OS bridge to restore the previous display context. The OS bridge (not the runtime) maintains the back stack — the runtime emits a `FLOW_BACK` event and the bridge handles the actual navigation. This keeps the runtime agnostic of platform navigation conventions.

**No stack management in the runtime.** There is no push/pop/replace API in the Navigation Manager. The `@machine` and `@flow` state graphs replace the stack; `to history` is the only escape hatch for platform-level back navigation.

### 9.4 View Re-render Trigger

Triggered by:
- Navigation (view appears or changes)
- Machine state change in any `@machine` that is referenced by the active state's `content =` body
- View `on resume` lifecycle

For each trigger:
1. Evaluate view `content =` body in current context (machine state, config, streams, params)
2. Serialize result to `DeckViewContent*`
3. Call `deck_bridge_render(view_name, content)` — full evaluated semantic content;
   diffing against previous state is the bridge's responsibility

---

## 10. Memory Management

### 10.1 Immutability and Sharing

All values are immutable. Structural sharing: list/map operations produce new values that share unchanged subtrees with originals. Machine state transitions replace the state value; old state is freed when ref count drops to zero.

### 10.2 Reference Counting

The interpreter uses reference counting. Reasons: predictable pause times (critical for embedded), no separate GC thread, memory pressure immediately visible via `system.info.free_heap()`.

**Cycle prevention**: Deck's value model cannot produce cycles. Closures capture immutable environments. Machines hold state values, not closures that reference the machine. No cycle detection needed.

### 10.3 String Interning

Atom strings are interned (one heap allocation per unique atom string). String comparisons for atoms are pointer equality. This is important for performance because pattern matching on atoms is the dominant branching operation.

### 10.4 View Content

A `DeckViewContent*` is allocated fresh on each render trigger by evaluating the view's `content =` body. It is serialized into the bridge-facing structure and passed to `deck_bridge_render()`. The previous content is freed after the call returns. The bridge owns no reference beyond the duration of the call — it must copy any data it needs to retain (see §4.3).

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

All other changes (`fn`, `let`, `@flow` step body or `@machine` state `content =`, `@task` body, `@on` hooks, `@config` defaults, derived `@stream` operators) support hot reload.

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
  Input:  MachineDecl[], FlowDecl[], BridgeInterface*
  Output: deck_bridge_render() calls with DeckViewContent*,
          deck_bridge_handle_intent() routing to on -> handlers,
          FLOW_BACK events to OS bridge for to history transitions
```

**Dependency summary:**
- `lexer`, `parser`, `evaluator`: no bridge dependency
- `loader`: depends on OS surface structure, not bridge
- `effect_dispatcher`, `scheduler`, `nav_manager`: depend on bridge

A complete, testable pure-language implementation needs only lexer + parser + evaluator + a mock value environment. Add loader + mock OS surface for load-time verification. Add bridge for hardware execution.

**`random` initialization**: `deck_runtime_create()` seeds the `random` builtin from OS hardware entropy before any app code runs. If the platform has no hardware RNG, the bridge implementor must provide a software PRNG seeded from an alternative entropy source (boot timestamp, ADC noise, chip ID, etc.). In `deck test` mode, `random.seed(42)` is called automatically before each test unless the test body overrides it.
