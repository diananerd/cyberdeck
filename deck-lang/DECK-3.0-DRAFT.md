# Deck 3.0 — Draft Consolidated Spec

**Status:** Draft. Captures all design decisions agreed across the redesign conversation, plus a minimalism pass for implementation and a full consistency review. Not yet authoritative. Does not replace `01-deck-lang.md` through `16-deck-levels.md` until promoted.

**Edition:** 2027.

---

## 0 · Philosophy

Deck is a domain-specific, interpreted, purely functional language for **embedded application authoring**. Not general-purpose. Every feature is justified by an embedded-application need, *and* by being the simplest construct that expresses it.

**Invariants:**

1. **Apps declare semantic intent, never presentation.** The bridge infers layout, widgets, colors, spacing, gestures, animations from the declared semantics plus device context. The same `.deck` runs against different bridges (LVGL, e-ink, voice, terminal) and each makes distinct presentation decisions.
2. **One way to express each concept.** If two forms exist for the same concept, one is wrong.
3. **Indentation is the block structure.** Annotations, machines, content bodies, matches, lambdas — all nest by indent. Curly, square, and round delimiters are reserved for **non-body** positions: data (map / record / list / tuple / named tuple), fn params, fn application, and expression grouping. Inside any delimiter pair, indentation is suspended.
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

**Dotted type paths.** A type reference may be prefixed by a lowercase dotted namespace — `api.Error`, `fs.Error`, `network.http.Error`. This form is reserved for types produced by `@errors` (§2.4) and capability namespaces (§9). No other dotted type forms exist.

**Dotted value paths.** A value reference may be prefixed by a capability alias (`http.get`), a named source (`Messages.last`), a config namespace (`config.theme`), or a machine name (`Launcher.send`). These are builtin name resolutions, not part of the identifier grammar.

### 1.5 Literals

```
42              -- int (i64)
-7              -- int
0xFF            -- int (hex notation; 0..2^63−1)
3.14            -- float (f64)
1.5e-3          -- float (scientific)
true  false     -- bool
unit            -- unit value
"hello"         -- str (UTF-8, immutable)

500ms 1s 5m 1h 1d       -- Duration (ms-backed int, §2.1)
64KB 512KB 2MB 1GB      -- Size (byte-backed int, §2.1)
```

Hex is a lexical convenience for `int`; there is no separate `byte` scalar (see §2.1).

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
let fn match when as with and or not
on every after watch from to previous current
true false unit if then else panic
```

**Contextual keywords** (reserved only in the positions documented in their section):

| Keyword | Reserved in | Section |
|---|---|---|
| `state`, `initial` | inside `@machine` bodies | §13 |
| `content` | at statement start inside a state body | §13.6 |
| `for` | at statement start inside `content =` blocks and content fns | §15.7 |
| `source` | after `@on` | §14.1 |
| `service` | at the start of a `@use` entry | §9.2 |
| `empty`, `more`, `submit`, `change`, `complete`, `link`, `image`, `cursor`, `selection`, `enter`, `leave` | after `on` in content handlers and state hooks | §13.1, §15.4 |
| content primitives: `list`, `group`, `form`, `loading`, `error`, `media`, `rich_text`, `status`, `chart`, `progress`, `markdown`, `markdown_editor` | at statement start inside `content =` blocks and content fns | §15.1–§15.3 |
| intent primitives: `toggle`, `range`, `choice`, `multiselect`, `text`, `password`, `pin`, `date`, `search`, `navigate`, `trigger`, `confirm`, `create`, `share` | at statement start inside `content =` blocks and content fns | §15.4 |

Outside their reserved positions these are ordinary identifiers. The content- and intent-primitive names are reserved only **at statement start** — they remain available as fn names, parameter names, and module members elsewhere (`text.length`, `let status = …`, `fn chart (…)`).

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

Applied to a `Result T E` expression inside any impure body:

```
let body = http.get("…")?
```

If the `Result` is `:ok v`, unwraps to `v`. If `:err e`, short-circuits the enclosing body:

| Enclosing context | `:err e` short-circuit behaviour |
|---|---|
| `!` fn returning `Result T' E` | Immediately returns `:err e`. The error domain `E` must be **identical** to the fn's `E` (load-verified). |
| `@service` method body returning `Result T' E` | Same as `!` fn. |
| `@on` body | The body ends; the unhandled error logs at `:warn` per §16.7; no further expressions in that body run. |
| `state :s on enter` / `on leave` body | The body ends; the transition rolls back per §13.5; `:warn` entry per §16.7. |

**Domain compatibility.** Deck has no error-union types. To propagate a `Result T E1` through a fn returning `Result T' E2`, map the error explicitly with the pure builtin:

```
fn map_err (r: Result T E1, f: (E1) -> E2) -> Result T E2
```

Example bridging two error domains:

```
fn fetch_and_save (url: str) -> Result unit app.Error ! =
  let body = http.get(url) |> map_err(_ -> :upstream)?
  fs.write("/data", body) |> map_err(_ -> :storage)?
```

Mismatched domains without an explicit `map_err` → `LoadError :type` at the `?` site.

Desugars mechanically to a `match`; no AST rewrite pass is required.

`?` is **not valid** in pure fn bodies (pure fns cannot produce capability `Result` values, so `?` has nothing to short-circuit). Writing `?` in a pure fn → `LoadError :type`.

---

## 2 · Type system

Dynamically typed at runtime; structurally validated at load. Annotations are **optional** in code positions (`fn` params/return, `let`) and **required** in schema positions (`@type` fields, `@config` entries, `state` payloads, capability signatures).

### 2.1 Primitives

| Type | Values | Notes |
|---|---|---|
| `int` | 64-bit signed | overflow → `panic :bug` |
| `float` | 64-bit IEEE 754, finite values only | NaN, ±Inf, denormals-from-overflow → `panic :bug` (no in-band sentinel value); `0.0 / 0.0`, `1.0 / 0.0`, `sqrt(-1.0)`, `log(-1.0)`, `inf - inf` all panic. The language admits no representable NaN or Inf. |
| `bool` | `true` / `false` | |
| `str` | UTF-8, immutable | |
| `Bytes` | opaque immutable byte sequence | produced by capabilities (e.g. `fs.read_bytes`); inspected via `bytes.*` builtins; never constructed from a literal |
| `unit` | single value `unit` | side-effect return |
| `Timestamp` | alias of `int` | epoch milliseconds |
| `Duration` | alias of `int` | milliseconds |
| `Size` | alias of `int` | bytes |

`Timestamp`, `Duration`, and `Size` are documentation aliases of `int`. `5m == 300000`, `64KB == 65536`. They exist solely to carry intent; builtins (`time.*`) both produce and consume them.

### 2.2 Composites

```
[T]                  -- List
{K: V}               -- Map (K ∈ comparable primitive; immutable)
(T, U)               -- Tuple, positional (arity ≥ 2)
(f1: T1, f2: T2)     -- Named tuple (arity ≥ 2; field names unique)
T?                   -- Optional, sugar for :some T | :none
```

**Map key types.** `K` must be a primitive with structural equality and total ordering: `int`, `float`, `bool`, `str`, `atom`, `Timestamp`, `Duration`, `Size`. Composite keys (tuples, records, lists, named tuples) are not allowed in v1 — they would require a structural hash function whose stability across runtime versions is non-trivial (open for future revisions).

Lists, maps, tuples, and named tuples are immutable with structural sharing.

**Named tuples** are anonymous records: they have field names but no declared type name. They are distinct from positional tuples — `(1, 2)` and `(x: 1, y: 2)` are different values and types.

Named tuples appear in three positions in the spec:
- Variant payloads (§2.3): `:active (temp: float, max: float)`.
- Content intent option lists (§15.4): `(label: "Dark", value: :dark)`.
- `@on back` confirm return (§14.8): `(prompt: str, confirm: (str, atom), …)`.

They are pattern-matched by field name (`(temp: t, max: m) -> …`) or by shorthand (`(temp, max) -> …` binds fields to identically-named variables). When a record's shape would be used in exactly one place, reach for a named tuple; when the shape is reused, declare a `@type`.

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

**Parametric types.** `@type Pair (A, B)` declares two type parameters. Construction `Pair { fst: 1, snd: "x" }` infers `A := int`, `B := str` from the field values. There is no syntax for explicit type-application — the loader resolves type parameters from the literal. If two values disagree (`Pair { fst: 1, snd: "x" }` followed by code that destructures it expecting `A := str`), the second use is `LoadError :type`. Ambiguity (no values constrain a parameter) → `LoadError :type` with `:unresolved_param` in `context`.

**Construction and pattern rules:**
- All declared fields are required at construction. Missing a field is `LoadError :type`.
- Extra fields not in the declaration are `LoadError :type`.
- The order of fields in a literal does not matter; they are keyed by name.
- A record with zero fields is constructed as `Empty {}` (empty braces required); the same shape is used as a pattern.
- `Post { uri: u, author: a, created_at: t }` is a full pattern; match `Post { likes: n }` matches any `Post` value and binds only the named field(s) — partial patterns are exhaustive when the constructor is covered.

**Field-type domain:** every field declared in a `@type` must be a concrete type (`int`, `str`, `bool`, `float`, `unit`, `Bytes`, `Timestamp`, `Duration`, `Size`, `T?`, `[T]`, `{K: V}` with `K ∈ {str, atom}`, a tuple / named tuple, another `@type`, or a parametric instantiation `Pair int str`). `any` and `Stream T` are forbidden in field positions (§2.5, §2.7).

**Recursion (well-foundedness):** the loader builds a directed graph where nodes are `@type` declarations and edges are field-of relationships. For every strongly connected component (SCC) in that graph, at least one edge in every cycle must pass through a constructor that admits a finite base case:

- `[T]` (the empty list `[]` is a finite base),
- `T?` (`:none` is a finite base),
- `{K: V}` (the empty map `{}` is a finite base),
- A variant whose constructor set includes at least one option without a payload that recurses into the SCC.

If every cycle has such an edge, the type is **inhabited and finite-constructible** — any value of the type has a finite representation. Otherwise → `LoadError :type` with the offending cycle in `context`.

This rule admits binary trees (`@type Tree = | :leaf | :node (left: Tree, right: Tree, value: int)` — the `:leaf` constructor is the base), arbitrary ASTs, linked lists via `[T]`, and rejects only genuinely unbounded shapes (`@type Bad \n field: Bad`).

### 2.4 `@errors` — shorthand for atom-only variants

```
@errors api
  :unauthorized  "401 or token invalid"
  :timeout       "Request timed out"
  :rate_limited  "Quota exceeded"
```

Equivalent to:

```
@type Error =            -- in the `api` namespace
  | :unauthorized
  | :timeout
  | :rate_limited
```

- Each `@errors <domain>` block produces a closed variant type named `Error` in the `<domain>` namespace. `<domain>` is a lowercase dotted identifier (same lexical class as a capability path: `[a-z][a-zA-Z0-9_]*` optionally dotted).
- External references use the **dotted type path** `<domain>.Error` (the only form of dotted type reference allowed by the grammar). Examples: `api.Error`, `network.http.Error`, `fs.Error`.
- Descriptions are documentation, not part of the type.
- Capability error domains (produced by the platform when an app writes `@use network.http as http`) follow the same rule: the alias introduces a namespace, and its error type is `http.Error`.
- Variants cannot carry payloads in `@errors`; for payload-bearing error types declare a `@type Name = | :variant (field: T) …` directly.

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

**Function types** carry the purity bit. A fn-typed value has signature `(T₁, T₂, …) -> R` (pure) or `(T₁, T₂, …) -> R !` (impure). Pure fns may not invoke impure-typed callables. This makes higher-order functions (§4.4) statically sound:

- A pure HOF (`fn map (xs: [T], f: (T) -> U) -> [U]`) requires its callable arg to be pure.
- An impure HOF (`fn map_io (xs: [T], f: (T) -> U !) -> [U] !`) accepts any callable.

The standard library exposes both flavours for any iteration / fold / traversal builtin (`map` / `map_io`, `filter` / `filter_io`, `reduce` / `reduce_io`). The trade-off — a small naming duplication in the stdlib — is taken in exchange for full purity soundness without effect polymorphism (which would add type-system complexity inconsistent with §0.10).

**Content fns** (§4.5) forbid `!` — marking a content fn impure is `LoadError :type`.

### 2.7 Stream type

```
Stream T
```

Continuous sequence, possibly infinite. Produced by capability methods and long-running operations (§14.3.1). Consumed via `@on source` (§14). Not a list; no length; not iterable.

**Substructural restriction.** A `Stream T` value is **linear**: it can appear only as the source expression of an `@on source` form (or as the input of a `stream.*` builtin in a pipe chain inside that source expression). It cannot:
- be bound by `let`,
- appear as a field of a `@type`,
- be passed as a fn argument,
- be returned from a fn (other than capability constructors documented to return `Stream T`),
- be stored in a list, map, tuple, or named tuple.

Any of the above → `LoadError :type`.

The named-source binding `as Name` (§14.4) yields a *handle* that exposes `Name.last()` / `Name.recent(n)` / `Name.count()` — those handles are ordinary values and may flow freely. The `Stream T` itself never escapes the `@on source` form.

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

**Scope rules:**
- A `let` binds from the next top-level expression in the enclosing body to the end of that body.
- Bodies of `if then` / `if else` branches, `match` arms, lambdas, and content nested blocks are each their own scope. A `let` in one does not escape.
- Shadowing an outer binding is allowed; inside the inner scope, only the inner binding is visible. The outer is restored when the inner scope ends.
- Within data delimiters (`( … )`, `[ … ]`, `{ … }`), `let` is not admitted (these positions are expression-only).

### 3.2 Destructuring

`let` accepts every pattern form admitted by `match` (§5.6):

```
let (p, q)         = some_tuple              -- positional tuple
let (x: a, y: b)   = some_point              -- named tuple, explicit
let (x, y)         = some_point              -- named-tuple shorthand (binds fields to same-name vars)
let [h, t]         = fixed_list              -- fixed-length list; mismatch → runtime pattern error
let [head, ...rest] = any_list                -- cons pattern; head: T, rest: [T]
let Post { author, created_at } = post       -- record field-shorthand pattern
let :ok v          = maybe_result            -- variant; only safe if exhaustive at load
```

A destructuring `let` that is not statically exhaustive (variant not covered, fixed-length mismatch) → runtime pattern error (`panic :bug`). Use `match` when the value may legitimately not match.

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
fn name (a, b) =                                -- minimal; allowed only as nested or inline lambda binding
  body

fn name (a: Type, b: Type) -> Type =            -- annotated pure (top-level requires this form)
  body

fn name (a: Type) -> Type ! =                   -- annotated impure (top-level requires this form)
  body
```

**Annotation requirement.** Every **top-level** `fn` (declared at module scope, including those reachable via `@use`) must annotate every parameter and the return type. Missing annotations on top-level fns → `LoadError :type`. Inline lambdas (`x -> x * 2`, `(a, b) -> a + b`) and `fn` values bound by `let` may omit annotations; the loader infers their signature from immediate use.

Rationale: top-level fns are reachable across module boundaries and recursive call sites; without annotations, type errors that should be load-time would surface only at runtime when a particular path is exercised — violating the "load-time > runtime" invariant (§0.8).

### 4.1 Application

User fns: **positional only**. Named args are not supported for user-defined fns.
Capabilities and builtins: **positional or named** (as declared in the capability's `.deck-os` signature). Never mixed in a single call.

```
my_fn(arg1, arg2)                          -- positional (user fn)
http.get(url: "…", timeout: 5s)            -- named (capability)
time.now()                                 -- positional, no args (builtin)
```

**Named args live in the argument list, separated by commas (no braces).** They are not map literals; they do not use `{ … }`.

### 4.2 Data construction — `{ … }`, `[ … ]`, `( … )`

All `{ … }`, `[ … ]`, and `( … )` constructs are **data**, not code blocks. Inside them, indentation is suspended.

**Map literal** — string or atom keys, always quoted/prefixed. A value-position `{`:
```
let m = { "host": "a.example", "port": 8080 }       -- str keys
let c = { :ok: 200, :err: 0 }                       -- atom keys
```

**Record construction** — field names, unquoted, prefixed by the type name:
```
let p = Post { uri: "…", author: a, created_at: time.now() }
```

Field shorthand: if an in-scope binding has the same name as a field, writing the field name alone is equivalent to `field: field`.
```
let uri = "at://…"
let p   = Post { uri, author: a, created_at: time.now() }    -- uri: uri shortened
```

All declared fields must appear exactly once. Missing or duplicated fields → `LoadError :type`. Unknown fields → `LoadError :type`. Field order in the literal is free.

**Variant construction** — atom + optional payload:
```
let s   = :active (temp: 25.0, max: 30.0)          -- named-tuple payload
let ok  = :ok 200                                  -- single-value payload
let tup = :point (10, 20)                          -- positional-tuple payload
let e   = :err :timeout
let n   = :none
```

**Named-tuple literal** — field names, unquoted, inside `( … )`:
```
let opt = (label: "Dark", value: :dark)
```

The parser disambiguates `(` forms as follows:
- `(f: expr, …)` with an identifier followed by `:` → named tuple.
- `(expr, expr, …)` with two or more comma-separated expressions → positional tuple.
- `(expr)` with exactly one expression → parenthesised expression.

**List literal:**
```
let xs = [1, 2, 3]
```

**Positional tuple literal:**
```
let pt = (x, y)
let xs = ("name", 5, :ok)
```

**Map vs record disambiguation:** a `{` after a `TypeName` is record construction; otherwise a map literal. A map literal whose keys are unquoted identifiers → `LoadError :parse`.

### 4.3 Record update

`with { field: value, … }` is a postfix expression that returns a new record with the listed fields overridden; all other fields are copied unchanged.

```
let updated = post with { likes: post.likes + 1 }
let multi   = cfg  with { theme: :dark, sync_every: 10m }
let same    = post with { }                       -- identity copy, legal but useless
```

Rules:
- LHS must evaluate to a record value.
- Inside the brace-list, each entry is `field: expr`; field names are **unquoted** (same shape as record construction, §4.2). This is not a map literal, so quoted-string keys are `LoadError :parse`.
- Every listed field must be declared on the record's type; unknown fields are `LoadError :type`.
- Every expression must match the field's declared type; mismatches are `LoadError :type` (static) or runtime type error (when the type cannot be statically resolved).
- `with` is left-associative: `r with { a: 1 } with { b: 2 }` ≡ `r with { a: 1, b: 2 }`.
- Field-shorthand (`r with { likes }` ≡ `r with { likes: likes }` if `likes` is in scope) is admitted per §4.2.

There is no `record.update` builtin. `with` is the single form (invariant §0.2).

**Memory cost.** `with` performs a shallow copy of the record header and overrides the listed fields; fields not listed share structure with the original record (immutable references). Per-call cost is O(K) where K is the number of overridden fields, independent of record size.

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

**Equality (`==` / `!=`)** is structural for every value: primitives by value, lists / tuples / named tuples / records element-wise, maps by (key, value) set equality, variants by constructor-and-payload. Two values of different declared types are never equal (`1 == "1"` → `false`, not a type error).

**Ordering (`< <= > >=`)** is defined only for `int`, `float`, `str` (lexicographic on code points), `atom` (lexicographic on the atom name), `bool` (`false < true`), `Timestamp`, `Duration`, `Size`. Ordering comparison on lists, maps, tuples, records, variants, or `Bytes` → `LoadError :type`.

**No implicit numeric coercion.** `int` and `float` are distinct; mixing them in arithmetic, comparison, or concat → `LoadError :type`. Conversion is explicit: `float(i)` and `int_of_float(f)` (the latter truncates and may `panic :bug` on values outside `int` range or on a non-finite float — but non-finite floats cannot exist by §2.1).

Rationale: `(2^53 + 1) + 0.0` would silently lose precision under implicit coercion. Forcing explicit conversion makes precision loss visible at the call site.

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

Any multi-line body position (fn body, match arm body, `@on` body, content handler body, state hook body) is an implicit sequence of **top-level expressions**. Each non-`let` top-level expression must produce `unit` or `Result unit E`; values of other types escaping the body are `LoadError :type`. The last expression of a body that has a declared non-unit return type escapes as the body's value (e.g. a pure `fn` returning `int`).

A top-level expression may span multiple physical lines by:
- being wrapped in `( … )` / `[ … ]` / `{ … }` (data delimiters suspend indentation, §1.3),
- starting each continuation line with `|>` (pipe continuation, §14.3), or
- being in a `match` arm body whose deeper indentation is its own sub-block.

There is no `do` keyword. Semicolons do not exist. A newline at the same indent as the enclosing body starts the next top-level expression.

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
  | [h, ...t]                    -> …
  | [h1, h2]                     -> …
  | Post { likes: n }            -> …
  | (a, b)                       -> …
  | (x: a, y: b)                 -> …
  | _                            -> …
```

**Pattern forms:**

| Form | Binds | Notes |
|---|---|---|
| Literal (`42`, `"x"`, `true`, `:atom`, `unit`) | nothing | value equality |
| Identifier | the value | binder |
| `_` | nothing | wildcard |
| `:ctor` | nothing | atom-variant, no payload |
| `:ctor pat` | from sub-pattern | variant with one-value payload |
| `:ctor (pat₁, pat₂, …)` | from sub-patterns | variant with positional-tuple payload |
| `:ctor (f₁: pat, …)` | from sub-patterns | variant with named-tuple payload |
| `Type { field: pat, … }` | from sub-patterns | record; partial is allowed |
| `Type { field }` | `field` | record with field-shorthand |
| `(pat, pat, …)` | from sub-patterns | positional tuple |
| `(f: pat, …)` | from sub-patterns | named tuple |
| `(f, …)` | each field by same name | named-tuple shorthand |
| `[pat, pat, …]` | from sub-patterns | fixed-length list |
| `[head, ...rest]` | `head: T`, `rest: [T]` | cons pattern |
| `[]` | nothing | empty list |
| `pat when expr` | from sub-pattern | guard clause |

Patterns compose recursively: every `pat` slot above may be any form in the table.

**Exhaustiveness** is load-verified by the algorithm of Maranget (1992): patterns are expanded by constructor, and each leaf must be reached by at least one arm or by a wildcard. Concretely:

- For closed variants: every constructor must be matched by at least one arm; the arm's payload sub-pattern itself must be exhaustive (recursively).
- For tuples and named tuples of known arity: every position must be matched, recursively.
- For records of known type: a partial pattern (`Type { f: pat }`) covers all values of that type — record exhaustiveness is single-constructor.
- For list patterns: the value space is `[] ∪ [h, ...t]`. An exhaustive set covers both. Fixed-length patterns `[a, b]` are sub-cases of `[h, ...t]` and never make a set exhaustive on their own.
- Literal sub-patterns (`:some 0`, `(0, 0)`, `Post { likes: 5 }`) leave a gap — another arm must handle the complement.
- Guards (`pat when expr`) do not contribute to exhaustiveness — the loader treats a guarded arm as if it might fail.
- For `any`, one `_` arm is mandatory.

Non-exhaustive matches → `LoadError :exhaustive` with the first uncovered constructor pattern listed in `context`.

**Construction vs pattern.** The same `{ … }` / `( … )` / `[ … ]` shapes appear on both sides. The parser disambiguates by position: on the LHS of `->` in a match arm or after `let`, the form is a pattern; elsewhere it is construction.

Arm bodies are implicit sequences.

### 5.7 No `is` operator

Machine state test uses `==` against the machine's declared name:
```
Session.state == :authed
```

Type test uses a builtin:
```
type_of(v) == :Post
```

`type_of` returns an atom whose name matches the declared `@type` name for user types, or one of `:int :float :bool :str :atom :unit :list :map :tuple :named_tuple :optional :bytes :fn :fragment :stream` for builtin shapes.

Two forms; zero dedicated parser rules; no `is` keyword.

### 5.8 `panic`

```
panic "expected positive, got {t}"
```

Terminal. The `panic` **keyword** is only valid in `!` fns and other impure bodies (@on, state hooks, service methods). Writing `panic` in a pure fn → `LoadError :type`.

Automatic panics (`:bug` and `:limit` per §11.2) can arise from any fn, pure or impure, when the runtime detects divide-by-zero, integer overflow, a failed destructuring, a stack overflow, or a heap-limit breach. A pure fn's inability to use the keyword does not make it panic-free at runtime — it only forbids the author from emitting a panic explicitly.

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

- `entry:` names the root `@machine`. The machine of that name is addressable via `<Name>.send(…)` / `<Name>.state` from anywhere in the app (§13.4).
- `edition:` pins syntax + semantics.
- `icon:` is either the name of an `@assets` entry declared with `as: :icon` (resolved to that asset) or a short literal glyph string ≤ 4 characters. Anything else → `LoadError :resource`.
- `tags:` is a free-form list of short lowercase strings used for discovery / search. No closed vocabulary.
- `orientation:` is a bridge constraint (`:portrait` / `:landscape` / `:any`).
- `log_level:` default minimum log level (`:trace` … `:error`).
- `license:` is a free-form atom; common values (`:mit`, `:apache2`, `:gpl3`, `:bsd3`, `:proprietary`) are documentation-only and not enforced by the loader.
- `serves:` lists service IDs exposed by this app (§18).

---

## 8 · `@needs` — requirements contract

```
@needs
  deck_level: 2
  deck_os:    ">= 2"
  runtime:    ">= 1.0"
  max_heap:   128KB
  max_stack:  512
  caps:
    network.http:  ">= 2"
    nvs:           ">= 1"
    notifications: optional
  services:
    "social.bsky.app/feed": ">= 1"
```

Evaluated at load. Missing or incompatible entries → `LoadError :incompatible`. `optional` caps / services degrade to `:err :unavailable` at call sites.

- `max_heap:` — app heap budget (§22.1). Default 64 KB. Accepts any `Size` literal.
- `max_stack:` — VM stack depth, in frames (§22.5). Default 512. Integer. Platform may cap below the requested value, in which case `LoadError :resource` with the platform's maximum in `context`.
- `deck_level:` — integer minimum conformance level (§25 open: DL matrix).

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

**Per-entry fields:**

| Field | Type | Values / Notes |
|---|---|---|
| `reason` | `str` | User-facing rationale shown by the OS. Required for any permission that prompts. |
| `prompt` | atom | `:at_install` / `:on_first_use` / `:never`. Default `:on_first_use`. `:never` means the capability works only if the user has pre-granted it via Settings. |
| `persist` | bool | Used by `logging` and other write-sensitive entries to opt into on-device storage. Default `false`. |

Unknown fields are `LoadError :type`. An entry whose `prompt` is not `:never` must carry a `reason`.

---

## 11 · Errors — three-level model

### 11.0 `Result T E` vs `T?` — which to use

Deck offers two "may-not-have-value" shapes. The choice is semantic, not stylistic:

- `T?` (sugar for `:some T | :none`) — the absence is **expected and routine**, carrying no useful diagnostic. Examples: `map.get(m, k)`, `list.head(xs)`, `Name.last()` on a named source with no emissions yet.
- `Result T E` — the absence is **exceptional** and the caller may want to distinguish reasons. Examples: `fs.read(path)` (`:not_found` vs `:permission_denied` vs `:io`), `http.get(url)` (`:timeout` vs `:dns` vs `:status_4xx`).

This distinction is binding for capabilities and standard-library builtins (the catalog audit, §25, enforces it). Apps follow the same rule by convention; mixing the two for the same conceptual operation in two places of an app's API is bad design but not a load error.

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
  pinned      : [str]     = []
```

Every entry is typed with a default. Stored in NVS namespace `app.{id}`. Range validation uses `min:` / `max:` fields (no `range:` keyword, no `..` literal).

**Allowed field types:** `int`, `float`, `bool`, `str`, `atom`, `Timestamp`, `Duration`, `Size`, `T?`, `[T]` where `T` is any allowed type, `{str: V}` where `V` is any allowed type, and user `@type` records whose own fields are themselves allowed types. `any`, `Stream T`, `fragment`, and `Bytes` are forbidden in `@config` entries — the first three by purity (§2.5–2.8), `Bytes` because opaque byte sequences belong in capabilities, not persistent config.

**Constraint fields:** `min:` / `max:` apply to `int`, `float`, `Duration`, `Size`. For `str`, use `min_length:` / `max_length:` (both `int`). For `[T]`, use `min_items:` / `max_items:`. For atoms declared with a closed set, use `in: [:a, :b, :c]`. Violations at set-time → `:err :out_of_range`.

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

- **Atomic and transactional**: a `config.set` either succeeds and persists the new value before returning, or fails (`:err`) and leaves the prior value intact. There is no observable intermediate state. The runtime is responsible for crash-safe persistence (write-ahead or temp-and-rename, platform-dependent — declared in `12-deck-service-drivers` SDI contract, post §25 audit).
- Fires `@on os.config_changed (key: atom, old: any, new: any)` **after** the body that called `config.set` completes (queued, single-threaded — §14.10).
- Within the same body, reads after a `config.set` see the new value.
- Error domain — `@errors config`:

  ```
  @errors config
    :unknown_key    "field not declared in @config"
    :type_mismatch  "value does not match declared type"
    :out_of_range   "value violates min/max/in constraint"
    :storage_full   "NVS partition full"
    :io             "persistence write failed"
  ```
  References use `config.Error` (§2.4).
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
state :name (field: Type, …)             -- typed payload (a named tuple, §2.2)
state :name machine: OtherMachine        -- composition

state :name
  on enter -> effect_expr                -- fires on entry
  on leave -> effect_expr                -- fires on exit
  content = …                             -- §15
```

**Payload access.** When a state declares a payload, every field is in scope inside that state's `on enter`, `on leave`, and `content =` bodies as a plain identifier. Example:

```
state :search (query: str)
  on enter -> log.info("search opened: {query}")
  content =
    text name: :q value: query on change -> Launcher.send(:update_query, q: event.value)
```

Fields shadow outer bindings within the state body. If the payload shape has a field named the same as a keyword (`match`, `let`, …), that field is not reachable by name from the body — rename the payload field.

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
- `to previous` returns to the state immediately prior (one level of navigation history). `previous` is a keyword (§1.8) — it is the history register, not a state name. The history register holds **at most one** state (the immediate predecessor); successive `to previous` does not unwind further. If history is empty (the machine is at its initial state and no transition has occurred), `to previous` is a no-op and logs `:warn`.
- Transitions fire only via `<MachineName>.send(…)`. Reactive triggers use `@on watch expr` that calls `<MachineName>.send(…)` (§14.1). There is no `watch:` attribute on transitions.
- `when expr` must be pure. Impure guard → `LoadError :type`.
- **Transitions have no body.** Pre/post effects go in the destination's `on enter` or the source's `on leave`. If an effect is specific to a single transition (not a state), declare a distinct event per case.

### 13.4 Dispatch

Each declared `@machine Name` exposes three builtin accessors (no new language mechanism; just name resolution on the machine name):

```
Launcher.send(:open_search)
Launcher.send(:update_query, q: "hello")
Launcher.state                    -- current state atom
Launcher.state == :grid           -- state test
```

- If no transition matches the current state × event, `send` is silently ignored (no error, no log).
- `send` accepts **named args only** — the parameter names are declared by the transition's `(param: Type, …)` clause.
- Cross-machine sends are allowed (`OtherMachine.send(…)`); the target machine must be declared in the same app or imported via `@use`.
- There is no generic `Machine` keyword; always refer to a machine by its declared name. Examples throughout the spec use the actual machine name (`Launcher`, `Timeline`, `Compose`).

### 13.5 Hook execution order

Transition S → D:

```
1. state S on leave
2. [state change: payload bound on D]
3. state D on enter
```

Initial entry (`:__init` pseudo-transition, one-time at machine birth): only step 3 fires, on the `initial` state.

If any hook returns `Result :err` that isn't consumed (no `?`, no `match`), the transition rolls back: machine remains in S with original payload. Automatic `:warn` log entry with the unhandled error.

**Atomicity.** A transition is observable only as committed-or-rolled-back. The state-change step (2 above) is **not committed** until `D.on enter` completes successfully. If `S.on leave` panics, the machine remains in S with the original payload. If the state-change step or `D.on enter` panics, the machine rolls back to S; the panic itself is logged at `:error` per §16.7. Suspend signals arriving mid-transition: the runtime allows the in-flight transition to complete (best-effort within the suspend budget of 200 ms, §14.7); if the budget is exceeded, the partial transition rolls back and the machine suspends in S.

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
| `@on every <duration>` | Periodic timer (monotonic clock; minimum 50ms) | any app |
| `@on after <duration>` | One-shot timer from launch or resume (monotonic; 0ms allowed) | any app |
| `@on watch <pure_bool_expr>` | Reactive: fires on false→true edge | any app |
| `@on source <stream_expr>` | Subscription to `Stream T` | any app |

All 14 forms produce the same AST node `On { source, params?, modifiers?, body }`. The lexer recognizes the short-form names (`launch`, `resume`, `every`, etc.) as contextual keywords *after* `@on`; a single parser rule handles all cases.

**`watch` purity:** the expression in `@on watch <expr>` must be **pure** — no `!` calls, no capability reads. It may read `<MachineName>.state`, named sources (§14.4), and `config.*`. An impure `watch` expression is `LoadError :type`. See §14.10 for the dependency-tracking algorithm that determines re-evaluation cadence.

**`every` minimum interval.** `@on every D` with `D < 50ms` is `LoadError :resource`. For higher-frequency reactions, use `@on watch <expr>` (event-driven, not polled). `@on after 0ms` is allowed (one-shot, fires on the next scheduler tick after launch / resume).

**Clock semantics.** `@on every D` and `@on after D` both use the **monotonic clock**. The interval is measured from the most recent fire (or launch / resume for the first fire); on resume after a suspend, the interval re-starts from `D` regardless of how long the suspend lasted. Apps that need to "fire if missed" use `@on watch (time.now() - last_fire >= D)` plus a state-stored `last_fire`.

### 14.2 Parameter binding

Inside any `@on` body, `event` is an **automatic binding** introduced by the handler context. It holds the event's full payload as a named tuple (or `unit` for sources that carry no payload). `event` is not a keyword; a body may shadow it with `let event = …` if needed (this is rarely useful but legal, mirrors the rule for `previous` / `current` / state-payload fields).

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
  buffer: 32
  ->
    Notifier.send(:message, payload: event)

-- elsewhere:
Messages.last()          -- T?
Messages.recent(10)      -- [T]
Messages.count()         -- int, 0..buffer
```

`as Name` gives the source identity and exposes read-only accessors:

| Accessor | Returns | Notes |
|---|---|---|
| `.last()` | `T?` | Most-recent emission, or `:none` if the buffer is empty. |
| `.recent(n)` | `[T]` | Up to `n` most-recent, newest first. `n` may exceed the buffer capacity; the list is capped. |
| `.count()` | `int` | Number of emissions currently held (0..`buffer`). |

**Buffer sizing.** Each named source retains a bounded ring buffer. Declare it with a `buffer: N` modifier line; default is 16. Maximum is platform-dependent (`system.info.versions().max_source_buffer`). Exceeding causes the oldest emission to be dropped silently. Without `as`, the `@on` is a fire-and-forget subscriber and no buffer is allocated.

### 14.5 Body introduction

`@on` bodies are introduced by a bare `->` whenever *any* modifier line (`as Name`, `buffer: N`, `keep:`, `max_run_ms:`, or a stream operator chain `|> …`) appears between the source header and the body. If no modifiers appear, the body is the direct indented block after the source line.

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

`@on back` is the **one** `@on` source whose body is required to produce a value (not `unit`). The return type is the closed variant:

```
@type BackResult =
  | :handled
  | :unhandled
  | :confirm (prompt: str, confirm: (str, atom), cancel: (str, atom))
```

The handler must produce a `BackResult`; load-verified. Values:

- `:handled` — app consumed the back gesture.
- `:unhandled` — delegate to the OS (typically suspends the app).
- `:confirm (…)` — named-tuple payload. The bridge renders a confirmation UI whose labels are the first element of each `(str, atom)` pair; the second element is the `BackResult` returned when the user picks that option (always one of `:handled` / `:unhandled`).

Constructing `:confirm` is ordinary variant construction with a named-tuple payload (§4.2). No special syntax.

**Panic in `@on back`.** If the body panics, the OS treats the gesture as `:unhandled` (the safe default — the user retains the ability to leave the app via the OS-level back). The panic itself logs at `:error` per §16.7.

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

**`@on watch` dependency tracking.** At load, the loader walks each `watch` expression and extracts its **dependency set** — the keys, sources, and machine states it reads. Permitted reads:

- `config.<key>` — adds `(:config, :key)` to the set.
- `<MachineName>.state` — adds `(:state, :MachineName)`.
- `<NamedSource>.last()` / `.recent(_)` / `.count()` — adds `(:source, :NamedSource)`.
- Pure builtins — propagate dependencies of their arguments.

The runtime re-evaluates a `watch` only after an event that touches its dependency set:

| Event | Re-evaluates watches whose set contains |
|---|---|
| `config.set(:k, v)` returned `:ok` | `(:config, :k)` |
| `<M>.send(:e, …)` triggered a transition | `(:state, :M)` |
| Named source `N` emitted | `(:source, :N)` |

Re-evaluation result `b'`: if previous result was `false` and `b'` is `true`, body fires. Otherwise no fire (edge detection only).

Watches whose dependency set is empty (constant expressions) → `LoadError :type` (the watch can never change).

A watch that rapidly toggles its own condition (>100 fires/sec on the same watch) is capped by the runtime and logs `:warn`.

### 14.11 `@on overrun`

Fires when one of *this app's own* handlers exceeded its `max_run_ms`.

```
@on overrun
  -- event: (handler: atom, elapsed_ms: int, source: str)
  log.warn("{event.handler} overran by {event.elapsed_ms}ms")
```

- `handler` — atom identifier of the overrunning handler. For `@on source … as <Name>`, the atom is the lowercase form of `<Name>`. For every other `@on`, the atom is derived from the source: `:launch`, `:resume`, `:every_30m`, `:os_wifi_changed`, etc. If two handlers would collide on the derived name, the loader appends a numeric suffix (`:every_30m_2`).
- `elapsed_ms` — how long the body ran before being cancelled.
- `source` — the source text of the handler header (truncated to 64 characters), for debugging.

`max_run_ms:` is a per-handler metadata field (§22.2), alongside `keep:`, `buffer:`, `as`. Default 200 ms.

`@on overrun` is a best-effort diagnostic channel; its own `max_run_ms` is fixed at 50 ms and cannot be overridden.

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

**`list` rules:**
- `expr` must evaluate to `[T]`. Other types → `LoadError :type`.
- `binder ->` names the per-item variable used inside the item body.
- `on empty` is rendered only when `expr` is `[]`.
- `has_more:` and `on more` are paired. Declaring one without the other is `LoadError :parse`.
- `event.page` on `on more` starts at 1 and increments across successive taps. The app decides when `has_more:` flips to `false`.

**`group` rules:**
- `"label"` is a str literal, capturing the semantic purpose of the group (not a heading — the bridge may or may not render it).

**`form` rules:**
- `on submit -> action` is mandatory.
- `submit_label:` is optional (default `"Submit"`). The bridge is responsible for rendering the submission affordance (button, keyboard enter, voice confirm) using this label.
- **Aggregation:** `event.values` collects every input intent anywhere in the subtree — including nested `group`s, `when` blocks, and content-fn splices — keyed by its `name:` atom. Input intents without a `name:` are a LoadError.
- `form`s cannot nest (`LoadError :parse`). A screen with two independent forms uses two siblings.
- `action` intents (`navigate` / `trigger` / `confirm` / `create`) inside a form are permitted but do **not** submit the form; they fire their own `->` action directly.
- Empty forms (zero input intents) are a LoadError — a form is for collecting values.

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
media expr                                     -- expr: str (path or URL) or AssetRef (§20)
  alt:  str_expr
  role: (:avatar | :cover | :thumbnail | :inline)?

rich_text expr                                 -- expr: str with inline markup (see below)

status expr                                    -- expr: any; label describes what it is
  label: str_expr

chart expr                                     -- expr: [float] OR [(float, float)] (see below)
  label:   str_expr?
  x_label: str_expr?
  y_label: str_expr?

progress
  value: float_expr                            -- 0.0..1.0 (out of range → clamped by bridge)
  label: str_expr?

markdown content_expr                          -- content_expr: str (full Markdown document)
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

**`rich_text` inline markup.** The expression is a `str`. Recognised inline markers: `**bold**`, `*italic*`, `` `code` ``, `[label](url)`. Block structures (headings, lists, quotes, code blocks, images) are **not** recognised by `rich_text` — use `markdown` for block content. `rich_text` exists so small pieces of formatted copy (a post body, a chat message) can render inline without invoking a full Markdown renderer.

**`chart` expression shapes.** The bridge inspects the value at runtime:
- `[float]` — treat as Y values; X is the index.
- `[(float, float)]` — treat as (x, y) pairs.
- Anything else — `LoadError :type` if statically detected; runtime type error otherwise.

**`MdEditorState`** is a builtin named tuple exposed alongside `markdown_editor`:

```
@type MdEditorState                            -- builtin
  cursor    : int                              -- character offset into value
  selection : MdRange?                         -- :none if no selection
  formats   : [atom]                           -- active inline formats at cursor, e.g. [:bold, :code]

@type MdRange                                  -- builtin
  start : int
  end   : int
```

Apps consuming `markdown_editor` typically store an `MdEditorState` in their state machine payload and pass it via `controlled_by:` so the editor reflects external changes.

### 15.4 Intents (14)

Intents split into three categories by how their action is wired:

**Input intents (9)** — the user produces a value; the handler takes it. Shape: `<kind> <fields> on <event> -> action`.

```
toggle
  name: :lights
  state: s
  on change -> Lights.send(:toggle, value: event.value)

range
  name: :volume
  value: v
  min: 0
  max: 100
  step: 1
  on change -> Audio.send(:set_vol, value: event.value)

choice
  name: :theme
  value: current
  options: [(label: "Dark", value: :dark), (label: "Light", value: :light)]
  on change -> config.set(:theme, event.value)

multiselect
  name: :filters
  value: selected
  options: […]
  on change -> Filters.send(:set, values: event.value)

text
  name: :query
  value: q
  hint: "Search"
  max_length: 200
  on change -> Search.send(:set_query, q: event.value)

password
  name: :pass
  value: p
  hint: "Password"
  on change -> Login.send(:set_pass, p: event.value)

pin
  name: :code
  length: 6
  on complete -> Login.send(:verify, code: event.value)

date
  name: :when
  value: d
  hint: "Pick a date"
  on change -> Schedule.send(:set_date, ts: event.value)

search
  name: :q
  value: s
  hint: "Filter"
  on change -> Browse.send(:filter, q: event.value)
```

**Action intents (4)** — the user triggers; there is no editable value. Shape: `<kind> <fields> -> action` (direct `->`, no handler name).

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

**Passive intent (1)** — declarative only, no handler:
```
share expr
  label: "Share this post"
```

**Intent field catalog:**

| Intent | Required | Optional | Value type (emitted on `event.value`) |
|---|---|---|---|
| `toggle` | `name`, `state`, `on change` | — | `bool` |
| `range` | `name`, `value`, `min`, `max`, `on change` | `step` | `int` or `float` (matches the declared value) |
| `choice` | `name`, `value`, `options`, `on change` | — | payload type of the selected option's `value` field |
| `multiselect` | `name`, `value`, `options`, `on change` | — | `[T]` where T = option value type |
| `text` | `name`, `value`, `on change` | `hint`, `max_length` | `str` |
| `password` | `name`, `value`, `on change` | `hint`, `max_length` | `str` (never logged; bridge masks display) |
| `pin` | `name`, `length`, `on complete` | — | `str` of exactly `length` characters, `[0-9]+` |
| `date` | `name`, `value`, `on change` | `hint`, `min_date`, `max_date` | `Timestamp` (midnight in device local tz for the chosen day) |
| `search` | `name`, `value`, `on change` | `hint` | `str` |
| `navigate` | `label`, `->` action | `badge: int` | (action intent; no `event.value`) |
| `trigger` | `label`, `->` action | `badge: int` | (action intent; no `event.value`) |
| `confirm` | `label`, `prompt`, `->` action | — | (action intent; action fires only on confirm) |
| `create` | `label`, `->` action | `badge: int` | (action intent; no `event.value`) |
| `share` | (target expression) | `label` | (passive; no handler) |

**`options` shape.** `choice` and `multiselect` accept `[(label: str, value: V)]` — a list of named tuples (§2.2). All items must carry the same `V`. Empty list → nothing selectable; the intent still renders but `on change` never fires.

**`confirm` dismissal contract.** When the user accepts the prompt, the action after `->` fires. When the user cancels, nothing fires. An app that needs to react to cancellation should model the confirmation as a state transition instead (push a `:confirming` state; `on leave` or a sibling transition handles cancel).

**`create` vs `navigate`.** Both take the user elsewhere. `create` signals "begin a new thing" (reserves a fresh composition context, may allocate storage); `navigate` signals "go somewhere already existing." Bridges use the distinction for affordance placement (e.g. a dedicated FAB for `create`).

### 15.5 Event bindings

| Handler | Event |
|---|---|
| `on change` (toggle / range / choice / multiselect / text / password / date / search / markdown_editor) | `event.value : T` (typed per intent, §15.4 catalog) |
| `on complete` (pin) | `event.value : str` |
| `on submit` (form) | `event.values : {atom: any}` keyed by each child's `name:` atom |
| `on more` (list) | `event.page : int` |
| `on empty` (list) | (no payload) |
| `on link` (markdown) | `event.url : str`, `event.text : str` |
| `on image` (markdown) | `event.url : str`, `event.alt : str` |
| `on cursor` (markdown_editor) | `event.cursor : int`, `event.formats : [atom]` |
| `on selection` (markdown_editor) | `event.selection : MdRange`, `event.text : str` |
| Action intents (`navigate` / `trigger` / `confirm` / `create`) | (no event binding — direct action on `->`) |
| `on enter` / `on leave` (state hooks, §13.1) | (no event payload) |

The `event` binding is always a named tuple (§2.2) whose fields depend on the handler. There is no general `event.*` structure — a handler can only read fields listed for its own event.

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

`AssetRef` is a builtin opaque record:

```
@type AssetRef                                -- builtin
  name   : atom
  kind   : atom                               -- :icon, :tls_cert, :sound, :image, :data, …
  size   : Size?                              -- :none until first resolution
  ready  : bool                               -- for download: entries, false until cache populated
```

Apps pass `AssetRef` values to capabilities (`media ref alt: …`, `http.get(…, ca: ref)`, `audio.play(ref)`); the consumer resolves it. An `AssetRef` cannot be converted to raw bytes from app code.

**Not-ready handling** is the consumer's responsibility:
- `media` with a non-ready `AssetRef` — bridge shows fallback (spinner + alt text) until ready, then re-renders.
- `http.get(…, ca: ref)` with a non-ready cert — `:err :asset_unavailable`.
- `audio.play(ref)` with a non-ready sound — `:err :asset_unavailable`.

Apps may force a refresh:

```
assets.refresh (name: atom) -> Result unit assets.Error !
```

`assets.Error` domain (§2.4 dotted form): `:not_declared`, `:network`, `:io`, `:checksum`, `:cache_full`.

`Bytes` builtin module (mandatory for any platform):

```
bytes.len     (b: Bytes)                      -> int                  -- pure
bytes.eq      (a: Bytes, b: Bytes)            -> bool                 -- pure (== works too; this is alias)
bytes.slice   (b: Bytes, start: int, end: int) -> Bytes               -- pure; out-of-range → panic :bug
bytes.concat  (parts: [Bytes])                -> Bytes                -- pure
bytes.to_str  (b: Bytes, encoding: atom)      -> Result str bytes.Error -- pure; encoding ∈ {:utf8, :ascii, :latin1}
```

`bytes.Error` domain: `:invalid_encoding`, `:malformed`. `==` on `Bytes` is structural (§5.1).

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

| Lives | Persisted? | Restored on resume? |
|---|---|---|
| `let` bindings, stack frames | no | no (re-derived from state + config) |
| `@config` | yes (NVS, atomic per `config.set` — §12) | yes |
| Machine state (current atom + payload) for every `@machine` | yes (app's service partition, at clean suspend; transition-atomic — §13.5) | yes — without replaying `@on launch` |
| Named-source buffers (`@on source … as Name`) | no | no (subscription re-starts; `Name.last()` is `:none` until first new emission) |
| Background handler timers (`@on every`, `@on after`) | no | re-armed from scratch on resume; the interval restarts from `D` (monotonic, §14.1) |
| Open streams (`@on source …`) | no | re-subscribed on resume |
| Previous-state history register (`to previous`) | no | reset to empty on resume (the post-resume initial position has no predecessor) |

Flow:
- Clean suspend (`@on suspend` budget respected) → machine state + `@config` persisted.
- Resume → `@on resume` runs, views re-render from current `config` + machine state. Named sources and timers restart.
- Unclean termination (panic, OS kill) may lose the last machine state write; `@on launch` runs on next spawn as if fresh.

Apps that need to restore a scroll position, the last message in a chat, or similar transient state must store it in `@config` or in their machine payload — nothing else is preserved.

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

- **Removed:** `where`, `do`, `history`, `is`, `via`, effect aliases after `!`.
- **Added (global):** `panic`, `previous`, `current`, `with`.
- **Contextual (new):** `service` (in `@use`), `state`, `initial`, `content` (in `@machine`), `source` (after `@on`), content- and intent-primitive names (in content bodies), handler-name tokens (`empty`, `more`, `submit`, `change`, `complete`, `link`, `image`, `cursor`, `selection`, `enter`, `leave`).

### 23.3 Simplifications

- **`where` → chained `let`.** No topological sort.
- **`with { … }` postfix** for record update, reusing the record-field-literal shape (unquoted field names). No separate `record.update` builtin.
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

- Three-level error model with postfix `?` propagation — usable in any impure body (fn, `@on`, state hook, `@service` method).
- Structured log system with automatic trace stamping.
- Cooperative single-threaded VM with event queue.
- Foreground / background / service lifecycle with explicit budgets.
- `@service` IPC contract.
- Memory, time, stack budgets per app; `Size` literal type.
- Suspend / resume persistence model.
- Long-running operations as capability-exposed `Stream T`.
- `on :name (params)` as the unified shape for every named-invocable with a signature (`@machine` transitions, `@on` events, `@service` methods).
- Named tuples (`(f₁: T₁, f₂: T₂)`) as first-class anonymous records; used by variant payloads, intent options, `@on back` confirm returns.
- `Bytes` opaque primitive (replaces a scalar `byte`); binary data always flows through capability buffers, never as literals; mandatory `bytes.*` builtin module.
- `@errors <domain>` produces the type referenced as `<domain>.Error`; dotted type paths are reserved to this form.
- Field-shorthand (`Post { uri, author }`) at construction and in `with { … }` update.
- Cons pattern `[h, ...t]` and the full pattern vocabulary (table in §5.6).
- Allowed-type whitelist for `@config` entries; `config.Error` declared via `@errors`.
- `buffer: N` modifier on named `@on source` bindings.
- **Soundness invariants:** no implicit int↔float coercion, NaN / Inf are panic-only (no in-band sentinels), `Result` propagation requires identical error domains (use `map_err` to bridge), Maranget exhaustiveness, well-foundedness on `@type` SCCs, `Stream T` linearity, pure/impure function types with stdlib HOF duplication, transitions atomic against panic and suspend, `@on watch` dependency tracking specified, `@on every` minimum 50ms on monotonic clock, top-level fn annotations required.

---

## 24 · Implementation size notes (non-normative)

**Parser:**
- 1 rule per annotation × 14 annotations (of which 7 share a "key-value indent block" shape: `@app`, `@needs`, `@grants`, `@config`, `@assets`, `@handles`, `@migrate`).
- 1 rule for `@on` with 14 short-form contextual keywords. All forms produce the same AST node.
- 1 rule for content primitives across 3 structural + 2 markers + 7 wrappers + 14 intents = 26 kinds, all one AST node.
- 1 rule for transitions, shared with `@service` methods and named `@on` events (unified `on :atom (params)` shape).
- 1 rule for type declarations covering both records and variants.

Total: ~20 parser functions.

**Walker:**
- 1 dispatch table for content kinds (26 entries).
- 1 dispatch table for `@on` sources (14 entries).
- 1 dispatch for transitions.

**Loader passes:**
- Lex / parse.
- Bind capability aliases + read `.deck-os` config schemas.
- Verify `@type` and `@errors` (no direct recursion — indirect via `[T]` / `T?` / `{K: V}` allowed, no duplicate constructors, fields match parametric arity, all field types are in the allowed domain §2.3).
- Verify match and `let` exhaustiveness for closed variants, tuples, named tuples, records, and list patterns (§5.6).
- Verify record construction: every declared field present exactly once; no unknown fields; field-shorthand binding in scope.
- Verify `with { field: value }` fields are declared on the target record type; types compatible.
- Verify named-tuple field names are unique within each literal / pattern.
- Verify dotted type paths resolve to a declared `@errors` domain or capability alias (§2.4).
- Verify `?` placement (in any impure body, **identical** error domain — `map_err` required to bridge domains).
- Verify pure / impure function-type signatures match at every call site; pure HOFs reject impure callable args.
- Verify well-foundedness of every `@type` SCC (Issue: well-formedness, §2.3).
- Verify `Stream T` linearity (only in `@on source` source position).
- Extract `@on watch` dependency sets; reject empty (constant) watches.
- Verify `@on every D` minimum (`D ≥ 50ms`).
- Verify top-level fns carry full type annotations.
- Verify parametric `@type` instantiations are resolvable from literals.
- Verify `on <name>` handler names match the content primitive's declared set.
- Verify transitions reference declared states; no duplicate `initial`.
- Verify `@on watch` expressions are pure.
- Verify `@on back` body produces `BackResult`.
- Verify `list has_more:` and `on more` are paired.
- Verify `form` `name:` atoms are unique within a form subtree.
- Bind `@use cap as alias { config }` values (pure-evaluated once).
- Verify `@needs.caps` / `@needs.services` against platform; verify `@needs.max_stack` against platform cap.
- Verify `@grants` covers declared permissions; every non-`:never`-prompt entry carries `reason`.
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
- Builtin catalog audit (reduce ~187 to target ~80; naming consistency); enforce `T?` vs `Result T E` discipline per §11.0.
- Capability namespace normalization (bare `nvs` / `fs` vs qualified `network.http`).
- `@component` extension point for content primitives beyond §15.
- Effect inference mode (whether `!` can be derived).
- Hot reload contract across `@config` / `@migrate` / VM state.
- Per-user profile partitioning of `@config`.
- Composite map keys (`{(int, int): V}`) — requires stable structural hash; deferred.

**Closed in this revision (no longer open):**
- `@on watch` dependency tracking — formalised in §14.10.
- `Stream T` substructural rules — §2.7.
- NaN / Inf semantics — §2.1 (panic, no in-band).
- `Result` domain bridging — §1.10 (`map_err`).
- Pure/impure function types — §2.6.
- Transition atomicity — §13.5.
- Well-foundedness of `@type` — §2.3.

---

**End of draft. Promote, amend, or discard as a whole.**
