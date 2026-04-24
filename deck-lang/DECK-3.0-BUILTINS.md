# Deck 3.0 — Builtin Modules

**Status:** Draft. Companion to `DECK-3.0-DRAFT.md` and `DECK-3.0-CAPABILITIES.md`. Not yet authoritative.

**Edition:** 2027.

Builtins are **always-available modules** — no `@use` declaration, no grants, no platform contract. The runtime registers every builtin method by name; apps call them as `module.method(args)`. They cover pure computation (math, text processing, collection manipulation), some impure primitives whose cost is too low to gate (`time.now`, `log.info`), and the stdlib shapes required by the language itself (`result.*`, `option.*`, `stream.*`).

This document defines:

- **Part I** — the meta-spec: what a builtin IS, how any builtin must be shaped, and how it differs from a capability.
- **Part II** — the v1 catalog of builtin modules.
- **Part III** — the extension contract for adding new builtins in platform components.

---

# Part I — Meta-spec: the shape of a builtin

## 0 · Philosophy

Same rules as capabilities (§0 of `CAPABILITIES.md`), with two additional ones specific to builtins:

1. **Builtins are never gated.** No grants, no permissions, no runtime-denied calls. If a function would ever want to be denied, it belongs in a capability.
2. **Builtins are computation; capabilities are I/O.** A builtin reads no device state and speaks to no network. `time.now()` is the single exception — it reads the monotonic / wall clock, which is so pervasive and cheap that gating it behind `@use` would poison every expression in the language. Everything else that touches hardware is a capability.

The net effect: builtin calls are safe to sprinkle anywhere — inside content fns (when pure), inside `@on watch` expressions (when pure), inside guards (when pure). Apps reach for builtins without thinking, which is the point.

## 1 · The six dimensions of a builtin

A builtin module is fully specified by these six (the capability meta-spec's seven dimensions, minus grants):

| Dimension | What it says |
|---|---|
| **1 · Identity** | Dotted name `module.name` (e.g. `math.abs`, `text.len`, `stream.filter`). |
| **2 · Purity** | Pure (callable from any fn body, including pure fns) or impure `!` (callable only from impure bodies; §2.6 of the draft). |
| **3 · Signature** | Argument types and return type. Return is a direct value, `T?`, `Result T E`, or `Stream T`. |
| **4 · Error domain** (if it returns `Result`) | `@errors <module>` atoms, with the universal `:unavailable` omitted — a builtin is always available by definition. `:permission_denied` is also absent. Only the module-specific atoms (`:malformed`, `:out_of_range`, `:domain_error`) appear. |
| **5 · Panic conditions** | When the method panics (`:bug` for programmer errors; `:limit` for memory / stack). Builtins never panic for recoverable input — they return `Result` or `T?`. |
| **6 · Evaluation order** (if it accepts a fn-typed arg) | Does the HOF call its callable arg eagerly, lazily, or in a deterministic order over a collection? Documented per HOF. |

Nothing else belongs in a builtin's specification. Builtins have no state, no configuration, no events, no streams (other than the `stream.*` module whose outputs are themselves streams), no handles. If a module needs any of these, it is a capability instead.

## 2 · Pure vs impure builtins

Every builtin is tagged with its purity. The tag is part of the signature — the runtime's dispatch table maps `(module, method)` → `(fn_ptr, is_pure)`.

**Pure builtins** can be called from any context, including:

- Pure fn bodies.
- `@on watch` expressions (which must be pure, §14.10).
- Content fn bodies (which forbid `!`, §4.5).
- `when` guards on transitions (which must be pure, §13.3).

**Impure builtins** can be called only from impure contexts (`!` fn bodies, `@on` handlers, state hooks, `@service` methods).

The catalog in Part II marks impure methods explicitly with a trailing `!`.

## 3 · HOF duplication (pure vs impure callable args)

Per §2.6 of the draft, a pure HOF cannot accept an impure callable. Every HOF in the catalog is provided in two flavours:

- `module.op(xs, f)` — pure version; requires pure `f`; usable everywhere pure is allowed.
- `module.op_io(xs, f)` — impure version; accepts any `f`; usable only in impure contexts.

The two flavours share their implementation; the only difference is which callable they accept, enforced by the loader. This is the one cost the minimalism rule pays to avoid effect polymorphism (§2.6 of the draft).

The duplication applies to:

- `list`: `map`, `filter`, `reduce`, `find`, `each`, `any`, `all`.
- `map`: `map`, `filter`, `each`.
- `stream`: `map`, `filter`, `each` (the `_io` versions for when the pipeline body does I/O).
- `option` / `result`: `map`, `and_then`, `or_else`.

Where no HOF exists (pure-data methods like `list.len`, `text.split`), no duplication is needed.

## 4 · Error handling discipline (re §11.0 of the draft)

Builtins follow the same `T?` vs `Result T E` rule as capabilities:

- `T?` when absence is routine. Examples: `list.head(xs)`, `map.get(m, k)`, `option.unwrap_or(o, default)`.
- `Result T E` when absence is exceptional and the caller benefits from distinguishing the cause. Examples: `json.parse(s)`, `bytes.from_b64(s)`, `text.parse_int(s)`.

Builtins **panic** (with `:bug`) only on programmer errors — calling `list.nth(xs, i)` with `i` out of bounds when a signed index was invalid, dividing by zero with `int`/`float` arithmetic, slicing outside a string. These are bugs, not recoverable errors; the panic carries the offending value in its message for diagnosis.

## 5 · Determinism

Every pure builtin is **deterministic**: same arguments, same result, every time. Impure builtins (`time.*`, `log.*`, `rand.*`) are allowed non-determinism only in ways they declare (reading a clock, writing to a log, generating randomness).

Builtins never observe app-global or VM-global mutable state beyond what their arguments expose.

## 6 · Edition pinning

Like the language itself, builtin semantics are pinned by `@app.edition:`. The runtime ships exactly one implementation of each builtin per edition. Cross-edition differences (if any) are documented in the edition's release notes, not in per-call flags.

---

# Part II — v1 Catalog

## 7 · Summary

| # | Module | Purity | Purpose |
|---|---|---|---|
| 1 | `math` | pure | Arithmetic, trig, exp/log, predicates, constants |
| 2 | `text` | pure | String manipulation, parsing scalars, formatting |
| 3 | `list` | pure + `_io` variants | Collection operations on `[T]` |
| 4 | `map` | pure + `_io` variants | Key-value operations on `{K: V}` |
| 5 | `stream` | pure + `_io` variants | Stream pipeline operators |
| 6 | `bytes` | pure | Byte-sequence operations + encoding |
| 7 | `option` | pure + `_io` variants | `T?` composition |
| 8 | `result` | pure + `_io` variants | `Result T E` composition |
| 9 | `record` | pure | Dynamic reflection on record values |
| 10 | `json` | pure | JSON parse / encode |
| 11 | `time` | mixed | Timestamp / Duration math + clock reading |
| 12 | `log` | impure | Structured logging (§16 of the draft) |
| 13 | `rand` | impure | Randomness (ints, floats, UUIDs) |
| 14 | `type_of` | pure (single fn) | Runtime type tag (§5.7 of the draft) |

Total: **14 modules**, ~120 methods. DL1 minimum: everything except `stream` (which is DL2, since streams arrive with reactive sources).

---

## 8 · `math`

Pure arithmetic and numeric functions. All methods are pure.

### 8.1 Integer ops

```
math.abs_int   (x: int) -> int                               -- panic :bug on overflow (abs(INT_MIN))
math.min_int   (a: int, b: int) -> int
math.max_int   (a: int, b: int) -> int
math.clamp_int (x: int, lo: int, hi: int) -> int             -- panic :bug if lo > hi
math.sign_int  (x: int) -> int                               -- -1, 0, 1
math.gcd       (a: int, b: int) -> int                       -- non-negative
math.lcm       (a: int, b: int) -> int                       -- non-negative; panic :bug on overflow
math.pow_int   (base: int, exp: int) -> int                  -- panic :bug on exp < 0 or overflow
```

### 8.2 Float ops

```
math.abs   (x: float) -> float
math.floor (x: float) -> float
math.ceil  (x: float) -> float
math.round (x: float, digits: int?) -> float                 -- :none = round to int; digits ≥ 0
math.sign  (x: float) -> float                               -- -1.0, 0.0, 1.0
math.min   (a: float, b: float) -> float
math.max   (a: float, b: float) -> float
math.clamp (x: float, lo: float, hi: float) -> float
math.lerp  (a: float, b: float, t: float) -> float           -- a + (b-a)*t
math.sqrt  (x: float) -> float                               -- panic :bug on x < 0
math.pow   (base: float, exp: float) -> float                -- panic :bug on non-finite result
math.exp   (x: float) -> float
math.ln    (x: float) -> float                               -- panic :bug on x ≤ 0
math.log2  (x: float) -> float
math.log10 (x: float) -> float
```

### 8.3 Trigonometry (radians)

```
math.sin  (x: float) -> float
math.cos  (x: float) -> float
math.tan  (x: float) -> float
math.asin (x: float) -> float                                -- panic :bug on |x| > 1
math.acos (x: float) -> float                                -- panic :bug on |x| > 1
math.atan (x: float) -> float
math.atan2(y: float, x: float) -> float
math.to_radians (deg: float) -> float
math.to_degrees (rad: float) -> float
```

### 8.4 Predicates

```
math.is_finite (x: float) -> bool                            -- always true post-§2.1; future-proof
math.is_zero   (x: float) -> bool                            -- abs(x) < math.epsilon
```

### 8.5 Constants

```
math.pi      : float                                         -- 3.14159265358979...
math.e       : float                                         -- 2.71828182845904...
math.tau     : float                                         -- 2 * pi
math.epsilon : float                                         -- smallest positive float representable
math.max_int : int                                           -- 2^63 - 1
math.min_int : int                                           -- -2^63
```

All constants evaluate at bind time; no runtime cost.

### 8.6 Conversions

```
math.int_to_float      (x: int) -> float                     -- may lose precision for |x| > 2^53
math.float_to_int      (x: float) -> int                     -- truncates toward zero; panic :bug if out of int range
math.float_to_int_round(x: float) -> int                     -- rounds half-away-from-zero
math.float_to_int_floor(x: float) -> int
math.float_to_int_ceil (x: float) -> int
```

No `@errors math` domain — every failure in `math` is a panic `:bug` (programmer error), never a recoverable Result.

---

## 9 · `text`

String manipulation. All pure.

### 9.1 Length and query

```
text.len         (s: str) -> int                             -- byte length; for character count use text.chars
text.chars       (s: str) -> int                             -- UTF-8 code-point count
text.is_empty    (s: str) -> bool
text.is_blank    (s: str) -> bool                            -- whitespace or empty
text.starts      (s: str, prefix: str) -> bool
text.ends        (s: str, suffix: str) -> bool
text.contains    (s: str, needle: str) -> bool
text.index_of    (s: str, needle: str) -> int?               -- byte offset; :none if not found
text.last_index_of (s: str, needle: str) -> int?
text.count       (s: str, needle: str) -> int                -- non-overlapping occurrences
text.eq_i        (a: str, b: str) -> bool                    -- case-insensitive, ASCII only
```

### 9.2 Slicing and transformation

```
text.slice       (s: str, start: int, end: int) -> str       -- byte offsets; panic :bug on out-of-range
text.chars_slice (s: str, start: int, end: int) -> str       -- code-point offsets
text.substring   (s: str, start: int, len: int) -> str       -- shorthand; byte offsets
text.trim        (s: str) -> str                             -- leading + trailing whitespace
text.trim_start  (s: str) -> str
text.trim_end    (s: str) -> str
text.upper       (s: str) -> str                             -- ASCII only; locale-aware requires system.locale
text.lower       (s: str) -> str
text.reverse     (s: str) -> str                             -- by code point
text.repeat      (s: str, n: int) -> str                     -- panic :bug if n < 0 or result exceeds max_str_len
text.replace     (s: str, from: str, to: str) -> str         -- all non-overlapping occurrences
text.replace_once(s: str, from: str, to: str) -> str         -- first occurrence only
text.pad_left    (s: str, width: int, ch: str) -> str        -- ch must be single char; otherwise panic :bug
text.pad_right   (s: str, width: int, ch: str) -> str
```

### 9.3 Splitting and joining

```
text.split       (s: str, sep: str) -> [str]                 -- sep = "" → one-char list (by code point)
text.split_lines (s: str) -> [str]                           -- splits on \n; strips \r
text.join        (parts: [str], sep: str) -> str
```

### 9.4 Formatting

```
text.format (template: str, args: {atom: any}) -> Result str text.Error
```

`template` uses `{key}` placeholders; `args` maps atom keys to values. Each value is converted to string via the same rules as string interpolation (§1.6). Unknown keys → `:err :missing_key`. Literal `{` via `\{`.

### 9.5 Parsing scalars

```
text.parse_int   (s: str) -> int?                            -- whole-string match; :none if malformed or overflow
text.parse_float (s: str) -> float?                          -- whole-string match
text.parse_bool  (s: str) -> bool?                           -- "true" / "false" exactly; :none otherwise
```

These exist alongside `int()` / `float()` / `bool()` cast functions per §11.1 of the draft. The `text.parse_*` forms never panic; the cast-function forms may.

### 9.6 Encoding

```
text.b64_encode (s: str) -> str                              -- standard (non-URL-safe) base64
text.b64_decode (s: str) -> Result str text.Error            -- :err :malformed on bad input
text.url_encode (s: str) -> str                              -- percent-encoding per RFC 3986
text.url_decode (s: str) -> Result str text.Error
text.hex_encode (s: str) -> str                              -- lowercase
text.hex_decode (s: str) -> Result str text.Error
```

### 9.7 Error domain

```
@errors text
  :malformed       "Input could not be decoded"
  :missing_key     "Format template references a key not in args"
  :out_of_range    "Numeric argument outside valid range"
```

Most failures in `text` are `panic :bug` (e.g. slice out of range); only encoding/parsing methods return `Result`.

---

## 10 · `list`

Operations on `[T]`. All pure unless `_io` suffix.

### 10.1 Construction

```
list.empty  () -> [T]                                        -- type inferred from use
list.of     (items: any, …) -> [any]                         -- varargs; rarely useful — prefer `[…]` literal
list.repeat (item: T, n: int) -> [T]                         -- panic :bug if n < 0
list.range  (start: int, end: int) -> [int]                  -- exclusive end; empty if start ≥ end
list.range_step (start: int, end: int, step: int) -> [int]  -- panic :bug if step = 0
```

### 10.2 Query

```
list.len       (xs: [T]) -> int
list.is_empty  (xs: [T]) -> bool
list.head      (xs: [T]) -> T?
list.last      (xs: [T]) -> T?
list.nth       (xs: [T], i: int) -> T?                       -- 0-indexed; negative indexes from end; :none if OOB
list.contains  (xs: [T], v: T) -> bool                       -- structural equality
list.index_of  (xs: [T], v: T) -> int?
list.take      (xs: [T], n: int) -> [T]                      -- first n; fewer if list shorter
list.drop      (xs: [T], n: int) -> [T]                      -- skip first n
list.slice     (xs: [T], start: int, end: int) -> [T]
```

### 10.3 Transformation (pure)

```
list.map     (xs: [T], f: (T) -> U) -> [U]
list.filter  (xs: [T], f: (T) -> bool) -> [T]
list.reduce  (xs: [T], init: U, f: (U, T) -> U) -> U
list.flat_map(xs: [T], f: (T) -> [U]) -> [U]
list.find    (xs: [T], f: (T) -> bool) -> T?
list.any     (xs: [T], f: (T) -> bool) -> bool
list.all     (xs: [T], f: (T) -> bool) -> bool
list.each    (xs: [T], f: (T) -> unit) -> unit               -- primarily for its side-effect-free signature symmetry
```

### 10.4 Transformation (impure — `_io` variants)

Same semantics; callable arg may be `!`.

```
list.map_io    (xs: [T], f: (T) -> U !) -> [U] !
list.filter_io (xs: [T], f: (T) -> bool !) -> [T] !
list.reduce_io (xs: [T], init: U, f: (U, T) -> U !) -> U !
list.find_io   (xs: [T], f: (T) -> bool !) -> T? !
list.any_io    (xs: [T], f: (T) -> bool !) -> bool !
list.all_io    (xs: [T], f: (T) -> bool !) -> bool !
list.each_io   (xs: [T], f: (T) -> unit !) -> unit !
```

### 10.5 Combination

```
list.concat (xs: [T], ys: [T]) -> [T]
list.append (xs: [T], item: T) -> [T]                        -- xs ++ [item]
list.prepend(xs: [T], item: T) -> [T]                        -- [item] ++ xs
list.zip    (xs: [T], ys: [U]) -> [(T, U)]                   -- truncates to shorter
list.unzip  (xs: [(T, U)]) -> ([T], [U])
list.reverse(xs: [T]) -> [T]
list.distinct (xs: [T]) -> [T]                               -- by structural equality
list.sort     (xs: [T]) -> [T]                               -- natural ordering; panic :bug on non-ordered T
list.sort_by  (xs: [T], key: (T) -> K) -> [T]                -- K must be an orderable primitive
list.group_by (xs: [T], key: (T) -> K) -> {K: [T]}
list.chunks   (xs: [T], size: int) -> [[T]]                  -- panic :bug if size ≤ 0
```

### 10.6 Evaluation order

All HOFs iterate in index order (0, 1, 2, …). Lazy iteration is not a v1 feature; every `list.map` materialises the full output. Apps that need laziness compose through `stream.*` instead.

### 10.7 Error domain

None. Every failure is `panic :bug`:

- `list.nth` with out-of-bound index returns `:none` (not a panic).
- `list.sort` on a list of non-orderable values (records, tuples) panics.
- `list.range_step` with `step = 0` panics.

---

## 11 · `map`

Operations on `{K: V}`. All pure unless `_io`.

### 11.1 Construction and query

```
map.empty   () -> {K: V}
map.len     (m: {K: V}) -> int
map.is_empty(m: {K: V}) -> bool
map.has     (m: {K: V}, k: K) -> bool
map.get     (m: {K: V}, k: K) -> V?                          -- :none if absent
map.get_or  (m: {K: V}, k: K, default: V) -> V
map.keys    (m: {K: V}) -> [K]                               -- no stable iteration order
map.values  (m: {K: V}) -> [V]
map.entries (m: {K: V}) -> [(K, V)]
```

### 11.2 Mutation (returns new map — maps are immutable)

```
map.set     (m: {K: V}, k: K, v: V) -> {K: V}
map.delete  (m: {K: V}, k: K) -> {K: V}
map.merge   (a: {K: V}, b: {K: V}) -> {K: V}                 -- b wins on key collision
```

### 11.3 Transformation (pure)

```
map.map       (m: {K: V}, f: (K, V) -> W) -> {K: W}
map.filter    (m: {K: V}, f: (K, V) -> bool) -> {K: V}
map.each      (m: {K: V}, f: (K, V) -> unit) -> unit
map.from_pairs(pairs: [(K, V)]) -> {K: V}                    -- later pairs override earlier
```

### 11.4 Transformation (impure)

```
map.map_io    (m: {K: V}, f: (K, V) -> W !) -> {K: W} !
map.filter_io (m: {K: V}, f: (K, V) -> bool !) -> {K: V} !
map.each_io   (m: {K: V}, f: (K, V) -> unit !) -> unit !
```

### 11.5 Iteration order

`map.keys` / `map.values` / `map.entries` / `map.map` / `map.filter` / `map.each` iterate in **key-sort order** (keys compared by their natural ordering, per §5.1). Deterministic across runs.

### 11.6 Error domain

None. Every failure is `panic :bug` (e.g. inserting a key whose type doesn't match the declared `K`).

---

## 12 · `stream`

Stream pipeline operators. All pure (the operators are pure compositions; the upstream stream is impure by construction). Used in `@on source <stream>` pipelines (§14.3).

Every operator takes a `Stream T` and returns a `Stream T'`. Cancellation propagates backwards through the pipeline.

### 12.1 Filtering / transforming

```
stream.map    (s: Stream T, f: (T) -> U) -> Stream U
stream.filter (s: Stream T, f: (T) -> bool) -> Stream T
stream.each   (s: Stream T, f: (T) -> unit) -> Stream T      -- passthrough; emits T unchanged after running f
stream.distinct (s: Stream T) -> Stream T                    -- suppresses consecutive duplicates (structural eq)
stream.skip   (s: Stream T, n: int) -> Stream T              -- drops the first n emissions
stream.take   (s: Stream T, n: int) -> Stream T              -- terminates after n emissions
stream.take_while (s: Stream T, f: (T) -> bool) -> Stream T  -- terminates when f returns false
stream.scan   (s: Stream T, init: U, f: (U, T) -> U) -> Stream U  -- accumulating fold
```

### 12.2 Timing

```
stream.throttle (s: Stream T, d: Duration) -> Stream T       -- emits at most one value per d
stream.debounce (s: Stream T, d: Duration) -> Stream T       -- emits the last value after d of silence
stream.delay    (s: Stream T, d: Duration) -> Stream T       -- shifts every emission by d
```

### 12.3 Combining

```
stream.merge   (a: Stream T, b: Stream T) -> Stream T        -- interleaves; either upstream terminates → termination after all values flushed
stream.combine (a: Stream T, b: Stream U) -> Stream (T, U)   -- emits on any upstream change, carrying the latest value of each; first emission after both have emitted
stream.buffer  (s: Stream T, size: int) -> Stream [T]        -- emits a list when size values collected
stream.window  (s: Stream T, size: int) -> Stream [T]        -- emits a sliding window of the last size values on each upstream emission
```

### 12.4 Impure variants

```
stream.map_io    (s: Stream T, f: (T) -> U !) -> Stream U
stream.filter_io (s: Stream T, f: (T) -> bool !) -> Stream T
stream.each_io   (s: Stream T, f: (T) -> unit !) -> Stream T
```

### 12.5 Notes

- Operators compose left-to-right in a pipe chain: `src |> stream.filter(f) |> stream.throttle(5s) |> stream.map(g)`.
- The runtime holds a small queue (size 1 by default) between operators to absorb upstream bursts; exceeding triggers backpressure that drops the oldest value and logs `:warn`.
- `stream.buffer` and `stream.window` allocate — their `size` parameter is bounded by `system.info.versions().max_source_buffer`.

### 12.6 Error domain

None. Operators are pure; panics only on programmer errors (`stream.take(s, -1)` → `panic :bug`).

---

## 13 · `bytes`

Byte-sequence operations. All pure.

### 13.1 Core (mandatory per §20 of the draft)

```
bytes.len     (b: Bytes) -> int
bytes.eq      (a: Bytes, b: Bytes) -> bool                   -- structural equality; == also works
bytes.slice   (b: Bytes, start: int, end: int) -> Bytes      -- panic :bug on out-of-range
bytes.concat  (parts: [Bytes]) -> Bytes
bytes.to_str  (b: Bytes, encoding: atom) -> Result str bytes.Error
                                                             -- encoding ∈ {:utf8, :ascii, :latin1}
bytes.from_str(s: str, encoding: atom) -> Bytes              -- :utf8, :ascii, :latin1
```

### 13.2 Indexing

```
bytes.at         (b: Bytes, i: int) -> int?                  -- byte value 0..255; :none if OOB
bytes.index_of   (b: Bytes, needle: Bytes) -> int?
bytes.starts     (b: Bytes, prefix: Bytes) -> bool
bytes.ends       (b: Bytes, suffix: Bytes) -> bool
```

### 13.3 Encoding

```
bytes.to_hex    (b: Bytes) -> str                            -- lowercase
bytes.from_hex  (s: str) -> Result Bytes bytes.Error
bytes.to_b64    (b: Bytes) -> str                            -- standard (non-URL-safe) base64
bytes.from_b64  (s: str) -> Result Bytes bytes.Error
bytes.to_b64url (b: Bytes) -> str                            -- URL-safe variant (RFC 4648)
bytes.from_b64url(s: str) -> Result Bytes bytes.Error
```

### 13.4 Error domain

```
@errors bytes
  :invalid_encoding   "Encoding atom not recognised"
  :malformed          "Input is not valid for the requested decoding"
  :truncated          "Input ended unexpectedly"
```

---

## 14 · `option`

Combinators for `T?` values. All pure unless `_io`.

```
-- Construction
option.some (v: T) -> T?                                     -- equivalent to :some v
option.none ()    -> T?                                      -- equivalent to :none

-- Query
option.is_some (o: T?) -> bool
option.is_none (o: T?) -> bool

-- Extraction
option.unwrap_or (o: T?, default: T) -> T
option.unwrap_or_else (o: T?, f: () -> T) -> T

-- Composition (pure)
option.map       (o: T?, f: (T) -> U) -> U?
option.and_then  (o: T?, f: (T) -> U?) -> U?                 -- monadic bind
option.or_else   (o: T?, f: () -> T?) -> T?
option.filter    (o: T?, f: (T) -> bool) -> T?               -- :some if passes, :none otherwise

-- Composition (impure)
option.map_io      (o: T?, f: (T) -> U !) -> U? !
option.and_then_io (o: T?, f: (T) -> U? !) -> U? !
option.or_else_io  (o: T?, f: () -> T? !) -> T? !
```

No `@errors option` — every method is infallible by construction.

---

## 15 · `result`

Combinators for `Result T E` values. All pure unless `_io`.

```
-- Construction
result.ok  (v: T) -> Result T E                              -- equivalent to :ok v
result.err (e: E) -> Result T E                              -- equivalent to :err e

-- Query
result.is_ok  (r: Result T E) -> bool
result.is_err (r: Result T E) -> bool

-- Extraction
result.unwrap_or (r: Result T E, default: T) -> T
result.unwrap_or_else (r: Result T E, f: (E) -> T) -> T

-- Conversion
result.to_option (r: Result T E) -> T?                       -- :ok v → :some v; :err _ → :none

-- Composition (pure)
result.map       (r: Result T E, f: (T) -> U) -> Result U E
result.map_err   (r: Result T E1, f: (E1) -> E2) -> Result T E2   -- cross-domain bridge (§1.10 of draft)
result.and_then  (r: Result T E, f: (T) -> Result U E) -> Result U E
result.or_else   (r: Result T E, f: (E) -> Result T E) -> Result T E

-- Composition (impure)
result.map_io       (r: Result T E, f: (T) -> U !) -> Result U E !
result.map_err_io   (r: Result T E1, f: (E1) -> E2 !) -> Result T E2 !
result.and_then_io  (r: Result T E, f: (T) -> Result U E !) -> Result U E !
```

The `result.map_err` method is the **only** way to bridge error domains with `?` propagation (§1.10 of the draft).

No `@errors result` — every method is infallible by construction.

---

## 16 · `record`

Dynamic reflection on record values. All pure. Narrow use — prefer pattern matching — but necessary when a record's field set is not statically known (deserialisation, schema-introspection tools).

```
record.keys    (r: @type R) -> [atom]                        -- field names as atoms
record.to_map  (r: @type R) -> {atom: any}                   -- field → value, lossy (loses type identity)
record.from_map(type: atom, m: {atom: any}) -> Result @type E record.Error
                                                             -- type is the record type's atom (`:Post` etc.)
record.has_field (r: @type R, name: atom) -> bool
record.field     (r: @type R, name: atom) -> any?            -- :some value if field exists
```

**Error domain:**

```
@errors record
  :unknown_type        "Type atom does not name any declared @type"
  :missing_field       "Required field absent in map"
  :type_mismatch       "Field value does not match declared type"
  :unknown_field       "Map contains a key not declared on the type"
```

**Notes:**
- `record.to_map` is expected to round-trip through `record.from_map` only when every field has a type serialisable as `any` (primitives, lists of primitives, maps with atom/str keys). Nested records round-trip through their own `record.to_map` / `record.from_map`.
- Prefer `match` + field access for known types; `record.*` is the escape hatch.

---

## 17 · `json`

JSON parse / encode. All pure.

```
json.parse  (s: str) -> Result any json.Error                -- returns any; caller pattern-matches
json.encode (v: any) -> Result str json.Error                -- records serialised as tagged maps: {"__type": "Post", "uri": …}
json.encode_pretty (v: any) -> Result str json.Error         -- 2-space indent
```

**Shape of the parsed `any`:**

- `null` → `:none` (as `any?`)
- `true` / `false` → `bool`
- integer → `int`
- float → `float`
- string → `str`
- array → `[any]`
- object → `{str: any}`

Apps match on shape via:

```
match json.parse(body)
  | :ok v ->
      match type_of(v)
        | :map  -> extract_fields(v)
        | :list -> extract_items(v)
        | _     -> log.warn("unexpected shape")
  | :err e -> log.warn("parse failed: {e}")
```

**Error domain:**

```
@errors json
  :malformed          "Input is not valid JSON"
  :depth_exceeded     "Object / array nesting exceeds platform limit (typically 32)"
  :non_finite         "Source contains NaN / Inf, which Deck cannot represent (§2.1)"
  :unsupported_type   "Encode target contains a value with no JSON representation (Bytes, AssetRef, Stream)"
```

---

## 18 · `time`

Clock reading and Duration / Timestamp math. Mixed purity: math operations are pure; clock reads are `!`.

### 18.1 Clock reads (impure)

```
time.now         () -> Timestamp !                           -- epoch ms, wall clock
time.monotonic   () -> Duration  !                           -- since boot, unaffected by wall-clock changes
time.since       (ts: Timestamp) -> Duration !               -- now - ts
time.until       (ts: Timestamp) -> Duration !               -- ts - now
time.ago         (ts: Timestamp) -> str !                    -- "3 minutes ago", localised by system.locale if present
```

### 18.2 Pure math

```
time.before      (a: Timestamp, b: Timestamp) -> bool
time.after       (a: Timestamp, b: Timestamp) -> bool
time.add         (ts: Timestamp, d: Duration) -> Timestamp
time.sub         (ts: Timestamp, d: Duration) -> Timestamp
time.diff        (a: Timestamp, b: Timestamp) -> Duration    -- a - b; sign preserved
time.start_of_day(ts: Timestamp) -> Timestamp                -- UTC midnight of that day
time.day_of_week (ts: Timestamp) -> atom                     -- :monday .. :sunday (UTC)
time.date_parts  (ts: Timestamp) -> DateParts
time.duration_parts (d: Duration) -> DurationParts
time.duration_str  (d: Duration) -> str                      -- "2h 15m" / "450ms" / "3d"
```

### 18.3 ISO / custom formats (pure)

```
time.to_iso    (ts: Timestamp) -> str                        -- RFC 3339 UTC
time.from_iso  (s: str) -> Result Timestamp time.Error
time.format    (ts: Timestamp, fmt: str) -> Result str time.Error
time.parse     (s: str, fmt: str) -> Result Timestamp time.Error
```

Format tokens: `YYYY MM DD HH mm ss SSS`. Literal characters pass through. No locale-aware formatting in v1 (requires `system.locale`).

### 18.4 Types

```
@type DateParts
  year   : int
  month  : int   -- 1..12
  day    : int   -- 1..31
  hour   : int   -- 0..23
  minute : int   -- 0..59
  second : int   -- 0..59
  millis : int   -- 0..999

@type DurationParts
  days    : int
  hours   : int
  minutes : int
  seconds : int
  millis  : int
```

### 18.5 Error domain

```
@errors time
  :malformed       "ISO / format string could not be parsed"
  :out_of_range    "Timestamp would exceed representable range"
```

---

## 19 · `log`

Structured logging per §16 of the draft. All methods impure.

```
log.trace (msg: str, data: {str: any}?) -> unit !
log.debug (msg: str, data: {str: any}?) -> unit !
log.info  (msg: str, data: {str: any}?) -> unit !
log.warn  (msg: str, data: {str: any}?) -> unit !
log.error (msg: str, data: {str: any}?) -> unit !

log.peek  (label: str, value: T) -> T !                      -- logs at :debug; returns value unchanged (pipeline debugging)
```

**Notes:**
- `data` is capped at 1 KB serialised per entry; overflow truncates with `data.truncated: true`.
- `trace_id` is attached automatically per §16.4 of the draft; callers may override via `data.trace_id`.
- `log.peek` is the only method that takes a non-str, non-map value — its purpose is exactly `v |> log.peek("after fetch", _) |> process`.

No `@errors log` — logging never fails at the call site; buffer overflows are silent + post `os.log_quota_hit`.

---

## 20 · `rand`

Randomness. Impure (non-deterministic by definition).

```
rand.int    (lo: int, hi: int) -> int !                      -- inclusive-exclusive [lo, hi); panic :bug if lo ≥ hi
rand.float  () -> float !                                    -- [0.0, 1.0)
rand.range  (lo: float, hi: float) -> float !                -- [lo, hi); panic :bug if lo ≥ hi
rand.bool   (prob: float?) -> bool !                         -- prob: probability of true, default 0.5; 0..1
rand.choice (xs: [T]) -> T? !                                -- :none on empty list
rand.shuffle(xs: [T]) -> [T] !
rand.uuid   () -> str !                                      -- RFC 4122 v4, lowercase hex with dashes
rand.bytes  (n: int) -> Bytes !                              -- n random bytes; panic :bug if n ≤ 0 or too large
```

**Notes:**
- Entropy source is platform-provided (CyberDeck: ESP32's hardware RNG). On platforms without a hardware RNG, the runtime seeds from wall-clock + heap addresses + WiFi noise; guaranteed good enough for UI randomness but not cryptographic.
- `rand.uuid()` is suitable for app-generated identifiers but not as a cryptographic token.

No `@errors rand` — all failures are `panic :bug`.

---

## 21 · `type_of`

Single fn (§5.7 of the draft). Pure.

```
type_of (v: any) -> atom
```

Returns an atom whose name matches:
- A declared `@type` name for user types (`:Post`, `:Author`).
- One of the builtin shapes: `:int :float :bool :str :atom :unit :list :map :tuple :named_tuple :optional :bytes :fn :fragment :stream :record_handle :handle`.

Useful for debugging, deserialisation (`json.parse` returns `any` whose shape is checked via `type_of`), and dynamic dispatch at the edges of an app.

---

# Part III — Extension contract

## 22 · Adding a new builtin module

A platform component may register builtins beyond this catalog through the native extension mechanism (see `12-deck-service-drivers.md` follow-up). The shape must follow Part I:

1. Declare the module in a `.deck-builtins` manifest shipped with the component.
2. Each method declares: identity, purity, signature, error domain (if any), panic conditions.
3. Implement each method as a C/Rust function matching the declared signature; register in the runtime's builtin dispatch table at boot.
4. The loader aggregates all manifests and verifies app `module.method(…)` calls against the aggregate.

Custom builtins must follow every rule of Part I. The meta-spec's claim — "apps can predict behaviour from the method name" — holds only if every module plays by the same rules.

## 23 · Constraints on builtin authors

- **No capability-like behaviour.** If a builtin needs the network, a file, or user permission, it is a capability, not a builtin. The loader rejects builtin methods declared with side-effecting capability calls.
- **Pure unless observably non-deterministic.** A builtin is `!` only if it reads a clock, RNG, or audit log. Apps may call pure builtins from anywhere, including `@on watch` — the purity bit is load-verified.
- **No state.** A builtin has no mutable module-level state. Two calls with the same arguments return the same result (modulo the declared non-determinism for `!` builtins).
- **Naming uniformity.** Snake-case, dotted path, same conventions as Part II. `module.verb_noun` or `module.noun`. No camelCase, no abbreviations beyond the Part II vocabulary (`b64`, `hex`, `uuid`, `iso` are the whitelist).
- **Error discipline.** `Result` for recoverable, `T?` for routine absence, `panic :bug` for programmer errors. No bare values with sentinel meanings.

## 24 · Conformance tests

Every builtin module declared by a platform must pass:

- **Method coverage** — every method in the manifest is invokable; inputs produce outputs of the declared type.
- **Purity invariants** — pure methods return identical results on identical inputs across 1000 calls (deterministic); `!` methods are declared impure but never panic on valid inputs.
- **Error-domain closure** — every `Result`-returning method emits only atoms declared in its `@errors`; no stringly-typed errors, no unlisted atoms.
- **Panic discipline** — panic cases are explicitly tested; the runtime reports the correct `:bug` kind with the offending input in the message.

---

# Part IV — Deferred

- **`regex`** — complex; most uses covered by `text.starts` / `text.ends` / `text.contains` / `text.split`. Revisit if a real app needs pattern matching beyond these.
- **`xml` / `yaml` / `toml`** — userland. JSON is the one serialisation format the runtime hard-codes because it's the transport of choice for `network.http`.
- **`compress`** (gzip / zstd) — revisit when an app with >100 KB transfer payloads appears.
- **`crypto.hash`** (SHA256, HMAC) — defer until a concrete use case emerges that `http` TLS doesn't already cover.
- **`num` (BigInt)** — 64-bit `int` + `float` cover every observed arithmetic need.
- **`iter`** — explicit lazy iteration. `stream.*` covers the reactive case; userland code uses eager `list.*`. Revisit if a pure-compute app pushes against memory.

---

**End of builtins catalog draft. Promote, amend, or discard as a whole.**
