# Deck Language Specification
**Version 2.0 — Core Language**

---

## 1. Purpose and Scope

Deck is a domain-specific, interpreted, purely functional language for embedded applications. It is not a general-purpose language. Every feature exists because embedded apps need it; nothing exists for completeness.

**Design invariants:**
- One way to express each concept — no syntactic alternatives
- Pure by default; effects are explicit and named
- No null, no exceptions, no mutation, no inheritance, no classes
- Pattern matching is the only branching construct beyond binary `if/then/else`
- The interpreter controls everything; the developer describes intentions
- Every type error surfaces at load time with an actionable message

---

## 2. Lexical Structure

### 2.1 Encoding
Source files are UTF-8. File and directory names: lowercase, underscores: `sensor_view.deck`.

### 2.2 Comments
```
-- Single line comment (to end of line)

---
Multi-line comment.
Continues until the closing triple-dash.
---
```

### 2.3 Indentation
Deck is indentation-sensitive. One level = 2 spaces. Tabs are a lexer error. Inside `()`, `[]`, or `{}` delimiters, indentation is suspended (content may span lines freely). A block opens on the line ending with `=` or `:`, and closes when indentation returns to the enclosing level.

### 2.4 Identifiers

```
value_name     -- lowercase snake_case: values, functions, fields, params
TypeName       -- UpperCamelCase: @type names, @machine names
:atom          -- colon-prefixed atom (symbolic constant)
```

- Value identifiers: `[a-z][a-zA-Z0-9_]*`
- Type identifiers: `[A-Z][a-zA-Z0-9]*`
- Atoms: `:[a-z][a-zA-Z0-9_]*`

### 2.5 Literals

```
42             -- int
-7             -- int
3.14           -- float
-0.5           -- float
true  false    -- bool
unit           -- the unit value
"hello"        -- str, UTF-8, double-quoted
"line\nnext"   -- escape sequences: \n \t \\ \" \{
```

### 2.6 String Interpolation
Inside any double-quoted string, `{expr}` evaluates `expr` and calls `str()` on the result:
```
let msg  = "Temperature: {temp}°C"
let full = "Hello, {user.name}! You have {len(items)} messages."
let math = "Result: {a + b}"
```
To include a literal `{`, escape it: `"\{"`. Interpolation applies to all string literals, including map keys and annotation values.

### 2.7 Multi-line Strings
Triple-quoted strings strip the common leading indentation of all lines and the leading newline:
```
let sql = """
  SELECT value, ts
  FROM readings
  WHERE ts > ?
  ORDER BY ts DESC
"""
-- Result: "SELECT value, ts\nFROM readings\nWHERE ts > ?\nORDER BY ts DESC\n"
```
Interpolation works inside triple-quoted strings.

### 2.8 Duration Literals
```
500ms   1s   30s   5m   1h   12h   1d
```
Type `Duration`. Valid only in contexts that accept `Duration` (task `every:`, timeout fields, cache TTL, etc.).

### 2.9 Range Literals
```
1..10        -- int range, inclusive
0.0..1.0     -- float range, inclusive
```
Used in `@config range:` and pattern guards. Not a collection type.

### 2.10 Keywords
```
let  fn  match  when  is  as  and  or  not  with
from  to  on  every  optional  true  false  unit  do
```

---

## 3. Type System

Deck is dynamically typed at the runtime level — types are checked when values are used — but strongly typed in semantics. Type annotations in code are documentation and runtime assertions combined. There is no static type inference algorithm; the loader performs structural verification at load time on what can be verified statically.

### 3.1 Primitive Types

| Type | Values | Notes |
|------|--------|-------|
| `int` | 64-bit signed integer | Runtime error on overflow |
| `float` | 64-bit IEEE 754 | `:nan` atom returned on invalid ops instead of NaN propagation |
| `bool` | `true` / `false` | |
| `str` | UTF-8 string | Immutable |
| `byte` | 0–255 | For binary data |
| `unit` | `unit` | The single value of the unit type; returned by side-effecting operations with no meaningful result |

### 3.2 Composite Types

```
[int]                -- List: ordered, homogeneous, immutable, any length
{str: int}           -- Map: string keys, homogeneous values, immutable
(int, str, bool)     -- Tuple: fixed arity, heterogeneous, immutable
```

Lists and maps are persistent data structures with structural sharing.

### 3.3 Optional
```
int?      -- Either :some int  or  :none
```
There is no `null`. Every absent value is explicitly optional. Accessing a field through `:none` is a runtime error with a descriptive message.

### 3.4 Union Types
```
int | str | bool
```
A union value is exactly one of the listed types. Consumed via pattern matching. Used in atoms-with-data variants and OS surface type definitions.

### 3.5 Result Type
```
Result T E
```
Either `:ok T` (success) or `:err E` (failure). `E` is typically an atom or an error domain type. Functions that can fail return `Result`. Functions that cannot fail do not. There are no exceptions — errors are values.

### 3.6 Atoms and Atom Variants

Atoms are symbolic constants. They carry no value:
```
:ok   :idle   :loading   :connected   :none
```

Atom variants carry a payload:
```
:some 42                       -- :some with int payload
:err :timeout                  -- :err with atom payload
:active (temp: 82.3, max: 90.0) -- named-field variant
```

Atom variants are constructed inline and destructured via pattern matching. They are how Deck expresses sum types without a separate `type` declaration for each variant.

### 3.7 Named Record Types (`@type`)
User-defined immutable record types. Declared with the `@type` annotation (see §8). Field access via `.`, update via `with`. Structurally typed — a map `{str: any}` is not the same as a declared `@type`, and they are not interchangeable.

### 3.8 Effect-Annotated Return Types
```
T !capability_alias
```
Not a standalone type — a function return type qualifier. A function returning `float !temp` produces a float and performs the `temp` capability side effect. Functions without `!` are guaranteed pure. Multiple effects: `unit !http !store`. The interpreter verifies at load time that every `!alias` in a function signature is declared in `@use`.

### 3.9 Stream Type
```
Stream T
```
A continuous, potentially infinite sequence of `T` values. Produced by OS capabilities (`watch` methods) and consumed via `@stream` declarations. Not a list — has no length, cannot be iterated imperatively.

### 3.10 Component Type
```
component
```
A fragment of the view component tree. Can be returned by functions and used in view body expressions. Only valid as a function return type when that function is called from within a view body. Cannot be stored in data structures, passed to non-view functions, or used as a `@stream` type. The interpreter validates this at load time.

---

## 4. Named Record Types

`@type` declares an immutable record type. Can appear in any `.deck` file. Accessible in any file that imports it via `@use`.

```
@type TypeName
  field_name : Type
  field_name : Type?
  field_name : [Type]
  field_name : TypeName    -- recursive: only via ? or []
```

**Direct self-reference is not allowed.** Recursive types must wrap the self-reference in `[T]` or `T?`:
```
-- Allowed:
@type Thread
  post    : Post
  replies : [Thread]    -- recursive via list

-- Not allowed:
@type Node
  next : Node           -- load error: unbounded direct recursion
```

### 4.1 Construction

```
let p = Post {
  uri:        "at://did:plc:abc/app.bsky.feed.post/tid",
  text:       "Hello world",
  author:     author,
  created_at: time.now(),
  likes:      0
}
```

All fields must be provided. Missing fields are a load error. Extra fields are a load error. Field order in construction is irrelevant.

### 4.2 Field Access

```
p.text
p.author.handle
p.author.display_name
```

Accessing a field that does not exist on the type is a load error. Accessing a field on a `:none` optional is a runtime error.

### 4.3 Record Update (`with`)

Produces a new value with specified fields changed:
```
let updated = post with { likes: post.likes + 1 }
let moved   = user with { handle: new_handle, updated_at: time.now() }
```

Fields not mentioned in `with { }` keep their values from the original. The original is unchanged. This is the only way to "modify" a record.

### 4.4 Pattern Matching on Records

Pattern matching on a `@type` value binds specific fields:
```
match p
  | Post { likes: n } when n > 1000 -> "viral"
  | Post { likes: n } when n > 100  -> "popular"
  | _                               -> "normal"
```

Unmentioned fields are ignored. The pattern `Post { field: binding }` requires the value to be of type `Post`.

---

## 5. Values and Bindings

### 5.1 Let Bindings
```
let x = 42
let name : str = "deck"
let post : Post = Post { ... }
```

All bindings are immutable. There is no reassignment. Type annotations are optional; the runtime checks the type on first use. Bindings are lexically scoped.

**Shadowing**: a `let` in an inner scope creates a new binding with the same name; the outer binding is inaccessible for the duration of the inner scope and resumes afterward.

### 5.2 Where Bindings
```
fn process (raw: float) -> str =
  "{adjusted}°C"
  where
    adjusted = math.round(raw - offset, 1)
    offset   = 2.5
```

`where` bindings are evaluated lazily, visible only within the function body, and may reference each other (evaluated in dependency order). Circular `where` references are a load error.

---

## 6. Functions

### 6.1 Pure Functions
```
fn name (param: Type, param: Type) -> ReturnType =
  body_expression
```

A function is pure if its return type has no `!` qualifier. Pure functions have no observable effects beyond their return value: no I/O, no OS calls, no randomness.

Multi-step body via `let` chain:
```
fn bmi (weight: float, height_m: float) -> float =
  let h2 = height_m * height_m
  weight / h2
```

### 6.2 Functions with Effects
```
fn fetch_profile (did: str) -> Result Profile str !api =
  match api.get("/app.bsky.actor.getProfile?actor={did}")
    | :err e  -> :err (api_error(e))
    | :ok r   -> parse_profile(r.json)
```

The effect annotations `!alias` are checked by the loader. Every `!alias` must correspond to a name declared in `@use`.

### 6.3 Functions Returning Components
```
fn post_card (p: Post) -> component =
  column
    text p.author.name   style: :heading
    text p.text
    text p.age           style: :muted :small
```

`component`-returning functions may be called from view body expressions and from other `component`-returning functions. They may not use `!effect` capabilities; they are pure view constructors. Effect capability calls from within a component function body are a load error.

### 6.4 Recursion
Functions may call themselves. Direct recursion:
```
fn factorial (n: int) -> int =
  if n <= 1 then 1
  else n * factorial(n - 1)
```

Mutual recursion (functions in the same file calling each other) is supported. Forward references within a file are resolved by the loader — declaration order is irrelevant within a file.

Tail calls are optimized via trampolining. Non-tail recursion is limited to a stack depth configured by the OS (default: 512). Exceeding this limit is a runtime error.

### 6.5 Lambdas
```
x -> x * 2                    -- single argument
(a, b) -> a + b               -- multiple arguments
(p: Post) -> p.likes > 100    -- with type annotation
```

Lambdas capture their lexical scope. They are values and can be passed to functions or stored in `let` bindings.

### 6.6 Function Application

Positional:
```
fn_name(arg1, arg2)
```

Named (when function declares named parameters or when calling OS capabilities with named args):
```
temp.watch(hz: 2)
store.set(key: "cfg", v: data)
```

Positional and named cannot be mixed in one call.

---

## 7. Expressions

Every construct in Deck is an expression — it produces a value. There are no statements.

### 7.1 Arithmetic
```
a + b    a - b    a * b    a / b    a % b
```
`/` on integers: integer division, truncated toward zero. `%`: modulo, sign follows dividend.

### 7.2 Comparison
```
a == b    a != b    a < b    a > b    a <= b    a >= b
```
`==` and `!=` work on all types. Ordering comparisons work on `int`, `float`, `str`. Comparing incompatible types is a runtime error.

### 7.3 Logical
```
a and b    a or b    not a
```
Short-circuits: `and` skips `b` if `a` is false; `or` skips `b` if `a` is true.

### 7.4 String Concatenation
```
"Hello, " ++ name ++ "!"
```
Valid only on `str`. Use `str(v)` to convert other types. For complex assembly, prefer string interpolation `"Hello, {name}!"`.

### 7.5 Binary `if`
```
if condition then value_a else value_b
```
Both branches must produce the same type. The `else` branch is mandatory. For more than two outcomes, use `match`.

### 7.6 Do Block
A sequence of effectful expressions evaluated purely for their side effects, producing `unit`:
```
do
  App.send(:start)
  store.set("last_seen", str(time.now()))
  notify.send("Welcome back")
```

`do` blocks appear in `@on` hooks, `@task` run bodies, and view event handlers. Each line is a separate expression. All must produce `unit` or `Result unit E` (the result value is discarded). The `do` block itself produces `unit`.

A `let` binding inside a `do` block is scoped to the remainder of the `do` block:
```
do
  let session = load_session()
  App.send(:session_loaded, session: session)
  log.info("Session loaded for {session.handle}")
```

### 7.7 Pipe Operator `|>`
```
value |> function
```
Equivalent to `function(value)`. The piped value is always the **first** argument. Additional arguments follow in parentheses:
```
raw |> math.round(1)        -- math.round(raw, 1)
list |> text.join(", ")     -- text.join(list, ", ")
```

Chains left-to-right:
```
raw_input
|> text.trim
|> text.lower
|> validate
|> process
```

### 7.8 Error-Propagating Pipe `|>?`
```
value |>? function
```
If `value` is `:err e`: short-circuit, return `:err e` without calling `function`.
If `value` is `:ok v`: unwrap to `v`, pipe into `function`.
If `value` is `:none`: short-circuit, return `:none`.
If `value` is `:some v`: unwrap, pipe into `function`.
Otherwise: runtime error (used on non-Result/Optional type).

```
fn process (input: str) -> Result Data ProcessError =
  input
  |>? parse
  |>? validate
  |>? transform
```

---

## 8. Pattern Matching

Pattern matching is the primary branching construct. It is exhaustive — the loader verifies all possible shapes of the matched type are covered. Failure to cover all cases is a load error, not a runtime error.

### 8.1 Syntax
```
match expression
  | pattern         -> result_expression
  | pattern when guard -> result_expression
  | _               -> default_expression
```

All arms must produce the same type. The `_` wildcard matches anything and must be last if present.

### 8.2 All Pattern Forms

```
-- Wildcard:
| _  -> ...

-- Literal:
| 0      -> ...
| "ok"   -> ...
| true   -> ...

-- Atom:
| :idle  -> ...
| :error -> ...

-- Atom variant, positional payload:
| :some v     -> use(v)
| :err e      -> handle(e)

-- Atom variant, named fields:
| :active (temp: t, max: m) -> show(t, m)
| :alert  (temp: t)         -> warn(t)

-- Record type:
| Post { likes: n, text: t } -> render(n, t)

-- Optional:
| :some v  -> use(v)
| :none    -> default()

-- Result:
| :ok  v  -> process(v)
| :err e  -> handle(e)

-- Tuple:
| (0, 0)    -> "origin"
| (x, y)    -> coords(x, y)

-- List:
| []              -> "empty"
| [single]        -> "one: {single}"
| [head, ...tail] -> "first: {head}"

-- Guard:
| n when n > 100 -> "high"
| n when n > 0   -> "positive"
| _              -> "non-positive"
```

### 8.3 Exhaustiveness

The loader checks match exhaustiveness using type information. If the matched value is a `Result T E` with known error type, all error variants must be handled or `_` used. For `bool`, both `true` and `false` must appear. For `@type` records, the pattern matching is structural.

If a match arm is unreachable (shadowed by a prior arm that always matches), the loader emits a warning.

---

## 9. Named Record Types (Language Level)

This section covers the language mechanics. `@type` annotation syntax is covered in §4.

### 9.1 Where @type Can Appear
- Any `.deck` file
- Not inside function bodies (top-level of the file only)
- Multiple `@type` declarations per file

### 9.2 Field Types Allowed
Any type expression: `int`, `float`, `bool`, `str`, `byte`, `unit`, `[T]`, `{str: T}`, `(T, U)`, `T?`, `Result T E`, `TypeName` (other `@type`s), `Timestamp`, `Duration` (OS builtin types).

Fields may **not** use `Stream T` or `component` — those are not storable data.

### 9.3 Type Equality
Two values are equal (`==`) if they are the same `@type` with equal field values, recursively. Equality on `@type` is deep structural equality.

### 9.4 JSON Round-trip
The interpreter can serialize and deserialize `@type` values from JSON strings. Field names match JSON object keys exactly. This is used in storage, API responses, and NVS persistence.

---

## 10. Module System

### 10.1 File = Module
Every `.deck` file is a module. The module name is the filename without extension. Directory paths are reflected in import syntax but not in the module name — the name is always the last segment.

```
utils/format.deck    -- module name: format,  access: format.fn_name()
views/main.deck      -- module name: main,    access: main.fn_name()
```

### 10.2 Public and Private
Everything in a module is public by default. Mark private with `@private` directly before a definition. `@private` applies to the immediately following definition only.

```
@private
fn internal (x: float) -> float = x * 1.8 + 32.0

fn to_fahrenheit (c: float) -> str = "{internal(c)}°F"
```

### 10.3 Importing
Via `@use ./path` in the importing file. Once imported, access as `module_name.definition`. Wildcard imports do not exist.

### 10.4 Cross-module @type Usage
If module `models/post` declares `@type Post`, and `app.deck` uses `./models/post`, then all files in the project that also use `./models/post` can reference `Post` as a type. Types are not re-exported automatically — each file must import the module whose types it uses.

### 10.5 No Circular Imports
Circular dependencies are a load error. The loader reports the full cycle.

### 10.6 Module-Level Definitions Allowed
- `fn` definitions
- `let` bindings (constants)
- `@type` declarations
- `@errors` declarations
- `@machine` definitions
- `@stream` declarations
- `@view` declarations
- `@task` declarations
- `@test` cases
- `@doc` annotations
- `@private` markers

### 10.7 Definitions Not Allowed Outside app.deck
- `@app`
- `@use`
- `@permissions`
- `@config`
- `@on`
- `@nav`

---

## 11. Standard Vocabulary (Builtins)

The following are always in scope, require no `@use`, and are pure (except `random`, documented as an impure builtin). Implemented by the interpreter, not the OS.

### 11.1 Type Conversion
```
str   (v: any)      -> str      -- any value to string representation
int   (s: str)      -> int?     -- parse decimal integer
float (s: str)      -> float?   -- parse decimal float
bool  (s: str)      -> bool?    -- "true"/"false" to bool
```

### 11.2 List Operations
```
len        (list: [T])                           -> int
head       (list: [T])                           -> T?
tail       (list: [T])                           -> [T]
last       (list: [T])                           -> T?
append     (list: [T], item: T)                  -> [T]
prepend    (item: T, list: [T])                  -> [T]
reverse    (list: [T])                           -> [T]
take       (list: [T], n: int)                   -> [T]
drop       (list: [T], n: int)                   -> [T]
contains   (list: [T], item: T)                  -> bool
map        (list: [T], fn: T -> U)               -> [U]
filter     (list: [T], fn: T -> bool)            -> [T]
reduce     (list: [T], init: U, fn: (U,T) -> U)  -> U
flat_map   (list: [T], fn: T -> [U])             -> [U]
flatten    (list: [[T]])                         -> [T]
zip        (a: [T], b: [U])                      -> [(T, U)]
zip_with   (a: [T], b: [U], fn: (T,U) -> V)     -> [V]
scan       (list: [T], init: U, fn: (U,T) -> U)  -> [U]
enumerate  (list: [T])                           -> [(int, T)]
tabulate   (n: int, fn: int -> T)                -> [T]
chunk      (list: [T], size: int)                -> [[T]]
window     (list: [T], size: int)                -> [[T]]
partition  (list: [T], fn: T -> bool)            -> ([T], [T])
any        (list: [T], fn: T -> bool)            -> bool
all        (list: [T], fn: T -> bool)            -> bool
none       (list: [T], fn: T -> bool)            -> bool
find       (list: [T], fn: T -> bool)            -> T?
find_index (list: [T], fn: T -> bool)            -> int?
count_where(list: [T], fn: T -> bool)            -> int
group_by   (list: [T], fn: T -> str)             -> {str: [T]}
unique     (list: [T])                           -> [T]
unique_by  (list: [T], fn: T -> any)             -> [T]
sort       (list: [T])                           -> [T]
sort_by    (list: [T], fn: T -> float)           -> [T]
sort_by_str(list: [T], fn: T -> str)             -> [T]
sort_desc  (list: [T])                           -> [T]
sort_by_desc(list: [T], fn: T -> float)          -> [T]
sum        (list: [int])                         -> int
sum_f      (list: [float])                       -> float
avg        (list: [float])                       -> float?
min_by     (list: [T], fn: T -> float)           -> T?
max_by     (list: [T], fn: T -> float)           -> T?
interleave (a: [T], b: [T])                      -> [T]
```

### 11.3 Map Operations
```
map.get    (m: {str:T}, k: str)           -> T?
map.set    (m: {str:T}, k: str, v: T)    -> {str:T}
map.delete (m: {str:T}, k: str)          -> {str:T}
map.keys   (m: {str:T})                  -> [str]
map.values (m: {str:T})                  -> [T]
map.has    (m: {str:T}, k: str)          -> bool
map.merge  (a: {str:T}, b: {str:T})      -> {str:T}
map.count  (m: {str:T})                  -> int
map.is_empty(m: {str:T})                 -> bool
map.map_values(m: {str:T}, fn: T->U)     -> {str:U}
map.filter (m: {str:T}, fn:(str,T)->bool)-> {str:T}
map.to_list(m: {str:T})                  -> [(str, T)]
map.from_list(pairs: [(str,T)])          -> {str:T}
```

### 11.4 Tuple Operations
```
fst (t: (A, B))                    -> A
snd (t: (A, B))                    -> B
tup.third  (t: (A, B, C))          -> C
tup.swap   (t: (A, B))             -> (B, A)
tup.map_fst(t: (A,B), fn: A->C)    -> (C, B)
tup.map_snd(t: (A,B), fn: B->C)    -> (A, C)
```

### 11.5 Result Helpers
```
ok          (v: T)                  -> Result T E
err         (e: E)                  -> Result T E
is_ok       (r: Result T E)         -> bool
is_err      (r: Result T E)         -> bool
unwrap      (r: Result T E)         -> T        -- runtime error if :err
unwrap_or   (r: Result T E, d: T)   -> T
map_ok      (r: Result T E, fn: T->U) -> Result U E
map_err     (r: Result T E, fn: E->F) -> Result T F
and_then    (r: Result T E, fn: T->Result U E) -> Result U E
or_else     (r: Result T E, fn: E->Result T E) -> Result T E
all_ok      (list: [Result T E])    -> Result [T] E
```

### 11.6 Optional Helpers
```
some        (v: T)                  -> T?
is_some     (o: T?)                 -> bool
is_none     (o: T?)                 -> bool
unwrap_opt  (o: T?)                 -> T        -- runtime error if :none
unwrap_opt_or(o: T?, d: T)          -> T
map_opt     (o: T?, fn: T->U)       -> U?
and_then_opt(o: T?, fn: T->U?)      -> U?
opt_to_result(o: T?, e: E)          -> Result T E
result_to_opt(r: Result T E)        -> T?
```

### 11.7 Comparison
```
compare (a: T, b: T) -> atom   -- :lt | :eq | :gt
                                -- valid for int, float, str, atoms, tuples of same
```

### 11.8 Type Inspection
```
type_of  (v: any) -> str   -- "int"|"float"|"bool"|"str"|"byte"|"unit"
                           -- "list"|"map"|"tuple"|"fn"|"atom"|"stream"
                           -- "result_ok"|"result_err"|"opt_some"|"opt_none"
is_int   (v: any) -> bool
is_float (v: any) -> bool
is_str   (v: any) -> bool
is_list  (v: any) -> bool
is_map   (v: any) -> bool
```

### 11.9 Functional Utilities
```
identity  (x: T)             -> T
const_fn  (v: T)             -> (any -> T)
compose   (f: B->C, g: A->B) -> (A -> C)
flip      (f: (A,B)->C)      -> (B,A) -> C
```

### 11.10 Random (Impure Builtin)
`random` is an intentional exception to the pure-by-default rule. It is available without `@use`, requires no `!effect` annotation, but is non-deterministic. The interpreter seeds it from OS entropy at startup. Functions using `random` do not need to declare it in their signatures. Use `random.seed(n)` in `@test` blocks for reproducibility.

```
random.int    (min: int, max: int)     -> int
random.float  ()                       -> float    -- 0.0..1.0
random.float  (min: float, max: float) -> float
random.bool   ()                       -> bool
random.pick   (list: [T])              -> T?
random.pick_n (list: [T], n: int)      -> [T]
random.shuffle(list: [T])              -> [T]
random.uuid   ()                       -> str      -- UUID v4
random.bytes  (n: int)                 -> [byte]
random.seed   (n: int)                 -> unit
```

---

## 12. Formal Grammar (EBNF, partial)

```ebnf
program      = { top_level } ;
top_level    = annotation | fn_def | let_binding | type_def ;

type_def     = "@type" TYPE_IDENT INDENT { field_decl } DEDENT ;
field_decl   = IDENT ":" type_ann NEWLINE ;

fn_def       = "fn" IDENT "(" param_list ")" "->" type_ann
               [ "!" effect_list ] [ INDENT doc_ann DEDENT ] "=" expr ;
let_binding  = "let" IDENT [ ":" type_ann ] "=" expr ;

type_ann     = "int" | "float" | "bool" | "str" | "byte" | "unit"
             | "any" | "component" | "Duration" | "Timestamp"
             | "[" type_ann "]"
             | "{" type_ann ":" type_ann "}"
             | "(" type_ann { "," type_ann } ")"
             | type_ann "?" | type_ann "|" type_ann
             | "Result" type_ann type_ann
             | "Stream" type_ann
             | TYPE_IDENT ;

expr         = let_expr | match_expr | if_expr | do_expr
             | pipe_expr | block_expr ;

let_expr     = "let" IDENT [":" type_ann] "=" expr { NEWLINE let_expr } expr
               [ "where" INDENT { let_binding NEWLINE } DEDENT ] ;

match_expr   = "match" expr INDENT { "|" pattern [guard] "->" expr NEWLINE } DEDENT ;
guard        = "when" expr ;

if_expr      = "if" expr "then" expr "else" expr ;

do_expr      = "do" INDENT { do_stmt NEWLINE } DEDENT ;
do_stmt      = let_binding | expr ;

pipe_expr    = pipe_expr "|>" call_expr
             | pipe_expr "|>?" call_expr
             | call_expr ;

call_expr    = call_expr "(" arg_list ")"
             | call_expr "." IDENT
             | call_expr "with" "{" field_update_list "}"
             | unary_expr ;

primary_expr = INT_LIT | FLOAT_LIT | STR_LIT | INTERP_STR | MULTILINE_STR
             | BOOL_LIT | "unit" | IDENT | ATOM | atom_variant
             | TYPE_IDENT "{" field_init_list "}"
             | tuple_lit | list_lit | map_lit | lambda_expr
             | "(" expr ")" ;

pattern      = "_" | INT_LIT | FLOAT_LIT | STR_LIT | BOOL_LIT
             | ATOM | ATOM pattern | ATOM "(" named_field_pats ")"
             | TYPE_IDENT "{" named_field_pats "}"
             | "(" pattern_list ")"
             | "[" "]" | "[" pattern_list "]"
             | "[" pattern_list "," "..." IDENT "]" ;
```

---

## 13. Error Format

Every error has: stage, location, problem, and when possible a suggestion.

```
Load error [exhaustiveness]: views/timeline.deck:31
  match on 'sensor.Error' is missing variant: :out_of_range
  Hint: add '| :err :out_of_range -> ...' or use '_' to ignore it.

Load error [effect-check]: tasks/sync.deck:14
  function 'api.post' uses capability 'api' but 'api' is not in @use.
  Hint: add 'api_client as api' to @use in app.deck.

Load error [type-check]: models/post.deck:8
  field 'author' in Post construction expects type 'Author', got '{str: any}'.
  Hint: use a Post construction expression or the post.parse() function.

Runtime error [record-access]: views/thread.deck:22
  accessed field 'text' on :none (Post? was :none).
  Hint: unwrap the optional first with 'match' or 'unwrap_opt_or'.
```
