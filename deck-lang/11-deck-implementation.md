# Deck Implementation Specification
**Version 1.0 — Interpreter Backend, Wire Formats, and Runtime Algorithms**

---

## 1. Purpose and Scope

This document specifies **how** the Deck runtime is implemented, in enough detail that two independent teams could produce interoperable interpreters. It is the implementation peer of the language spec (`01-deck-lang`), the app model (`02-deck-app`), the OS surface (`03-deck-os`), and the bridge surfaces (`05-deck-os-api`, `06-deck-native`, `10-deck-bridge-ui`).

It covers:

- Threading and concurrency model
- Heap layout and allocator strategy
- `DeckValue` binary representation and ownership rules
- Reference counting semantics and cycle prevention
- String interning
- Lexer, parser, loader, evaluator algorithms with edge cases
- Pattern matching compilation and exhaustiveness checking
- Effect dispatcher routing, async semantics, ordering
- Scheduler, condition tracker, stream operators with backpressure
- Navigation manager and reactive transition firing
- Snapshot / restore protocol for suspended VMs
- Crash isolation and panic recovery
- `DeckViewContent` (DVC) wire format
- Runtime ↔ bridge IPC framing
- Capability registration timing and scope
- Startup and shutdown sequences
- Performance budgets

Anything **board-specific or ESP-IDF-specific** belongs in `13-deck-cyberdeck-platform`. The **abstract platform contract** that any host must implement to back this runtime lives in `12-deck-service-drivers` (the SDI). This document is intentionally hardware-agnostic; the same interpreter must work on a hypothetical desktop / Linux bridge with no code changes.

This runtime is published as a standalone IDF Component (`deck-runtime`) — see `14-deck-components` for packaging, distribution, and the cross-SoC porting model.

### Companion documents

| Doc | Role |
|---|---|
| `01-deck-lang` | What the language means |
| `02-deck-app` | What an app looks like |
| `03-deck-os` | What the OS surface declares |
| `04-deck-runtime` | High-level runtime architecture (lexer→parser→loader→evaluator). Section overlap with this doc is intentional; **this doc supersedes** `04` on any algorithmic conflict |
| `05-deck-os-api` | High-level service capabilities |
| `06-deck-native` | Public C API for binding native code |
| `09-deck-shell` | Shell/launcher integration |
| `10-deck-bridge-ui` | Bridge UI semantic catalog |
| `12-deck-service-drivers` | The hardware-agnostic SDI contract this runtime composes against |
| `13-deck-cyberdeck-platform` | ESP-IDF / CyberDeck reference SDI implementation |
| `14-deck-components` | How the runtime and drivers are packaged and published |
| `15-deck-versioning` | Edition / surface / runtime / SDI / app version policy + compatibility check at load |

### Convention: pseudocode

Code blocks are pseudocode unless explicitly tagged with a language. They use C-like syntax for C-bridge concerns and Deck-like syntax for runtime-internal concerns.

---

## 2. Threading and Concurrency Model

### 2.1 The single-VM-thread rule

Every Deck VM runs on **exactly one OS thread**, the **runtime thread** for that VM. All evaluation — every lexer call, every AST walk, every pattern match, every effect dispatch — happens on this one thread.

Concretely:

- The runtime thread owns `vm->heap`, `vm->machines`, `vm->nav_state`, `vm->stream_buffers`, `vm->task_registry`. None of these structures are protected by locks because they are never touched from another thread.
- All `DeckValue` allocations and refcount mutations on a VM's heap happen on the runtime thread.
- The evaluator does not yield mid-expression. There is no coroutine, no green thread, no preemption inside Deck code.

**Why a single thread.** Deck is a pure-by-default language with explicit effects. The complexity budget for the runtime is small. A single-threaded evaluator eliminates an entire class of bugs (data races on the heap, refcount race conditions, observer-during-mutation) and makes refcount + immutability deliver real correctness guarantees rather than aspirational ones.

**Per-VM, not per-runtime.** Multiple VMs may run on multiple runtime threads in parallel (one Deck app per VM). The single-thread rule scopes to *one* VM's heap and state. Inter-VM communication crosses thread boundaries through the bridge.

### 2.2 Bridge threads

The bridge — anything implementing the C surface in `06-deck-native` — runs on **its own threads**, distinct from the runtime thread. There is no constraint on how many bridge threads exist; typical layouts:

- **One UI thread** that owns the display library (LVGL on CyberDeck) and runs frame ticks
- **One network thread** that owns the HTTP / TLS / MQTT clients
- **One event loop thread** that fans out OS events
- **N worker threads** for blocking syscalls (filesystem, sensor reads)

Bridge threads must not call into the evaluator or the heap. They communicate with the runtime thread through three primitives:

1. **The runtime mailbox** — a single MPSC queue per VM. Bridge threads enqueue messages; the runtime thread dequeues. Messages are described in §19.
2. **`deck_dispatch_main`** — a one-shot enqueue helper for bridge threads that need a callback to run on the runtime thread.
3. **Stream pushes** — `deck_stream_push(subscription, value)` enqueues a stream value into the runtime mailbox via the subscription's owning VM.

### 2.3 The runtime main loop

The runtime thread runs a strict loop:

```
forever:
  msg = mailbox.recv(timeout: TICK_MS)
  if msg:
    dispatch(msg)               # may run for up to MSG_BUDGET_MS
  scheduler.tick(now())         # process timers, when: re-eval, watch: re-eval
  if pressure_due():
    memory_monitor.check()
```

Tunables (per-VM, with defaults):

| Tunable | Default | Notes |
|---|---|---|
| `TICK_MS` | 50 ms | Mailbox poll quantum; lower → tighter scheduler reactivity, higher CPU |
| `MSG_BUDGET_MS` | 100 ms | Max wall-clock per `dispatch(msg)`. If exceeded the runtime emits a `runtime.slow_message` warning; it does not interrupt the call. Going over budget twice in a row escalates to crash if `slow_message_strict=true`. |
| `PRESSURE_INTERVAL_MS` | 1000 ms | How often `memory_monitor.check()` fires. The host platform may override this (`13-deck-cyberdeck-platform §9.5` does). |

### 2.4 Reentrancy and nested dispatch

`dispatch(msg)` may produce side effects that themselves enqueue mailbox messages — for example, a `MSG_OS_EVENT` runs an `@on` hook, which calls `Machine.send(:event)`, which fires reactive `watch:` transitions, each of which may queue further work.

The runtime processes these in a **flat** style: every nested `send`, `watch:` fire, or stream emission is buffered into a per-message work queue and drained before returning to the mailbox. The work queue uses BFS ordering (oldest first).

Re-entrant `dispatch` from within an effect dispatcher callback is **forbidden**: a bridge implementation that calls back into the runtime synchronously from an effect handler is a contract violation and triggers a panic. Effects must complete and return their result; if they need to deliver a follow-up event they enqueue a mailbox message.

### 2.5 ISR safety

ISR code (interrupt handlers) must use **only** `deck_event_fire_isr_safe(rt, event_name, preallocated_payload)` — a special variant that performs no allocation, no interning, no logging. Behavior of any other Deck API called from ISR context is undefined.

`deck_event_fire_isr_safe` enqueues an internal `MSG_ISR_EVENT` to the runtime mailbox using a lockless ring buffer of fixed size (`DECK_ISR_RING_DEPTH`, default 32). On overflow, the oldest unread ISR message is dropped silently and a counter (`rt->stats.isr_drops`) is incremented.

---

## 3. Heap and Allocator

### 3.1 Per-VM heap

Each VM owns a private heap. Heaps do not share allocations; passing a `DeckValue*` across VM boundaries is a contract violation (caught in debug builds, undefined in release).

A heap is structured as:

```
DeckHeap {
  Allocator       *alloc       # see §3.2
  StringTable      strings     # interning, see §6
  RefStats         stats       # alloc/free/peak counters
  size_t           soft_limit  # bytes; configured at vm_create
  size_t           hard_limit  # bytes; allocations above this fail with :err :oom
  size_t           in_use      # currently allocated bytes including header
  void            *opaque      # bridge-defined slot for platform integration
}
```

`soft_limit` is advisory — when crossed, the runtime emits `os.memory_pressure (level: :low)`. `hard_limit` is enforced — allocations above it return NULL and the calling effect/expression fails with `:err :oom`. Both default to 256 KB unless the platform doc overrides them.

### 3.2 Allocator interface

The runtime does not bundle an allocator. It calls into a vtable supplied at `vm_create` time:

```
typedef struct {
  void *(*alloc)  (size_t bytes, AllocClass class, void *user);
  void *(*realloc)(void *ptr, size_t old_bytes, size_t new_bytes, AllocClass class, void *user);
  void  (*free)   (void *ptr, size_t bytes, AllocClass class, void *user);
  void *user;
} DeckAllocator;
```

`AllocClass` is an enum tag the host can use for routing — for example, on CyberDeck the host routes `ALLOC_LARGE` (>1 KB) to PSRAM and everything else to internal SRAM. Classes:

| Class | Used for |
|---|---|
| `ALLOC_SMALL` | DeckValue headers, strings ≤ 64 B, list / record nodes |
| `ALLOC_LARGE` | Strings > 64 B, byte buffers, list bodies, view content arenas |
| `ALLOC_SNAPSHOT` | Suspended-VM serialized heaps |
| `ALLOC_INTERN` | Interned string storage; never freed during VM lifetime |

The runtime always passes the original `bytes` to `free` and `realloc`, so the allocator can implement size classes without per-allocation metadata.

### 3.3 Arenas for ephemeral data

Two short-lived arenas exist alongside the heap:

- **Render arena** (`vm->render_arena`) — used to build the `DeckViewContent` tree on each render. Reset at the start of every render call. Lifetime: until `bridge.render` returns.
- **Parse arena** (`vm->parse_arena`) — holds AST nodes for the duration of a single load or hot-reload. Released after the loader finishes (success or failure).

Arenas allocate by bumping a pointer; freeing an individual allocation is a no-op. They share the underlying `DeckAllocator` (class `ALLOC_LARGE`) for backing pages of 4 KB.

---

## 4. DeckValue Binary Representation

### 4.1 In-memory layout

`DeckValue` is a 16-byte tagged value (on 32-bit platforms) or 24-byte (64-bit). The first 4 bytes are a header, followed by an inline payload or pointer to heap data.

```
struct DeckValue {           // 16 bytes on 32-bit
  uint8_t  type;             //  1   DeckType enum (§4.2)
  uint8_t  flags;            //  1   bit 0: interned   bit 1: literal (read-only)
                              //       bit 2: opaque-handle-cleanup-pending
                              //       bits 3..7: reserved (must be 0)
  uint16_t refcount;          //  2   non-atomic, see §5
  union {                     // 12   payload, type-discriminated
    int32_t   i;
    float     f;
    uint8_t   b;              // bool: 0 / 1
    uint8_t   byte;           // raw byte
    AtomId    atom;           // see §4.3
    StrRef    str;            // {len: u32, ptr: void*} packed
    ListRef   list;
    MapRef    map;
    TupleRef  tuple;
    RecordRef rec;
    FnRef     fn;
    StreamRef stream;
    OpaqueRef opaque;
  } payload;
};
```

On 64-bit the header is 8 bytes (with 4 bytes of padding) and the payload is 16 bytes. Allocator alignment is 8 bytes regardless.

The header is **never** serialized to disk or wire; the snapshot format (§16) and the DVC wire format (§18) use their own canonical encodings.

### 4.2 Type enum (`DeckType`)

```
DECK_UNIT       =  0
DECK_BOOL       =  1
DECK_INT        =  2          // i32
DECK_FLOAT      =  3          // IEEE-754 binary32
DECK_BYTE       =  4          // u8
DECK_STR        =  5
DECK_BYTES      =  6          // [byte]
DECK_ATOM       =  7          // bare or variant; payload distinguishes
DECK_LIST       =  8
DECK_MAP        =  9          // {str: DeckValue}; insertion-ordered
DECK_TUPLE      = 10
DECK_RECORD     = 11          // @type instances
DECK_FN         = 12          // Deck closures and bound capability methods
DECK_STREAM     = 13
DECK_OPAQUE     = 14          // bridge-owned handle
DECK_RESULT_OK  = 15          // wraps one DeckValue
DECK_RESULT_ERR = 16          // wraps one DeckValue (typically atom-or-record)
DECK_OPT_SOME   = 17          // wraps one DeckValue
DECK_OPT_NONE   = 18          // no payload
DECK_TIMESTAMP  = 19          // i64 ms since Unix epoch
DECK_DURATION   = 20          // i64 ms
```

Result and Option are **separate** type tags — not just sugar over atom-variants — so the dispatcher can fast-path them without inspecting payloads.

### 4.3 Atoms

An `AtomId` is a `uint32_t` index into a global, append-only **atom table**. Each VM has its own atom table; cross-VM atom values are remarshalled at the bridge boundary. The first 256 atom IDs are reserved for predeclared atoms: `:ok`, `:err`, `:none`, `:some`, `:true`, `:false`, `:portrait`, `:landscape`, `:connected`, `:offline`, `:locked`, `:unlocked`, `:matrix`, `:amber`, `:neon`, `:lt`, `:eq`, `:gt`, `:int`, `:float`, `:bool`, `:str`, `:byte`, `:unit`, `:list`, `:map`, `:tuple`, `:fn`, `:atom`, `:stream`, `:result_ok`, `:result_err`, `:opt_some`, `:opt_none`, `:record`, `:nan`, etc. The full predeclared list lives in the runtime header.

**Variant atoms.** `:foo "bar"` and `:err (code: 42, message: "x")` are encoded by storing the atom name in `payload.atom.id` and the payload `DeckValue*` in `payload.atom.variant_payload` (heap-allocated). For bare atoms `variant_payload` is NULL.

### 4.4 Composite payloads

```
StrRef   { uint32_t len; const char *bytes; }       // bytes NOT NUL-terminated
ListRef  { uint32_t len; uint32_t cap; DeckValue *items; }
MapRef   { uint32_t count; uint32_t cap; MapEntry *entries; }   // entries are insertion-sorted
TupleRef { uint8_t  arity; uint8_t pad[3]; DeckValue *items; }   // arity ≤ 16
RecordRef{ uint16_t type_id; uint16_t pad; DeckValue *fields; } // fields ordered by @type decl
FnRef    { FnKind kind; uint16_t arity; FnBody *body; EnvRef env; }
StreamRef{ StreamId id; StreamBuffer *buf; }                    // see §13
OpaqueRef{ uint16_t type_id; uint32_t handle_id; void *ptr; OnRelease release; }
```

`MapEntry` is `{ AtomId key_intern; DeckValue value; }` — keys are always interned strings or atoms; the runtime canonicalizes string keys at construction time.

`RecordRef.type_id` indexes into the VM's record-type table, which maps `type_id → (type_name_atom, [field_name_atoms])`. The record's own `fields[]` array is positional, matching the type declaration order.

### 4.5 Ownership and lifetime — the four rules

Every C function operating on `DeckValue*` falls into one of four ownership classes. Documentation for each function in `06-deck-native` MUST tag which class it implements.

1. **Borrow** — caller retains ownership; callee may read but must not free or store the pointer beyond the call. Most accessors (`deck_get_str`, `deck_record_get`, etc.) are borrow.
2. **Consume** — callee takes ownership; caller must not touch the pointer after the call. Most constructors that wrap an inner value (`deck_ok(inner)`, `deck_some(inner)`, `deck_list_push(list, item)`) consume the inner value.
3. **Return-owned** — return value is owned by caller, who must `deck_value_release` it eventually. All `deck_*_new` constructors return-owned.
4. **Return-borrowed** — return value is owned by another structure; caller may read but must not release. `deck_record_get` is return-borrowed (the field belongs to the record).

The runtime never silently up- or down-grades ownership classes. A function that needs to store a borrowed value across calls must call `deck_value_retain` to get its own owned reference.

### 4.6 Conversion shorthand `deck_*` constructors

```
DeckValue *deck_unit   (void);                          // singleton; never freed
DeckValue *deck_bool   (bool b);
DeckValue *deck_int    (int32_t i);
DeckValue *deck_float  (float f);
DeckValue *deck_str    (const char *s, size_t len);     // copies bytes; interns if len ≤ 32
DeckValue *deck_bytes  (const uint8_t *b, size_t len);
DeckValue *deck_atom   (AtomId a);
DeckValue *deck_atom_v (AtomId a, DeckValue *payload);  // consumes payload
DeckValue *deck_list   (DeckValue **items, size_t n);   // consumes items
DeckValue *deck_map    (size_t n_pairs, ...);           // varargs: alternating key (const char*) and DeckValue* (consumed)
DeckValue *deck_tuple  (size_t arity, ...);             // varargs: DeckValue* (consumed)
DeckValue *deck_record (uint16_t type_id, DeckValue **fields, size_t n);  // consumes fields
DeckValue *deck_ok     (DeckValue *v);                   // consumes v
DeckValue *deck_err    (DeckValue *e);                   // consumes e
DeckValue *deck_some   (DeckValue *v);                   // consumes v
DeckValue *deck_none   (void);                           // singleton; never freed
DeckValue *deck_opaque (uint16_t type_id, uint32_t handle_id, void *ptr, OnRelease release);
```

`deck_unit` and `deck_none` return statically-allocated singletons with `refcount = UINT16_MAX` (saturating; see §5.2). They are never freed.

---

## 5. Reference Counting

### 5.1 Non-atomic, single-threaded discipline

`DeckValue.refcount` is a plain `uint16_t`. It is **not** atomic. Concurrent retain/release from multiple threads is undefined behavior. This is sound because the single-VM-thread rule (§2.1) guarantees only one thread ever touches a value's refcount.

The two primitives:

```
void       deck_value_retain (DeckValue *v);   // ++refcount
void       deck_value_release(DeckValue *v);   // --refcount; if 0 → free
```

`retain` saturates at `UINT16_MAX` and disables further refcount changes for the value (it becomes immortal). Saturation is logged as `runtime.refcount_saturated`. Static singletons (`deck_unit`, `deck_none`) ship pre-saturated.

### 5.2 Free cascade

`deck_value_release` at refcount 0:

1. If the value owns a payload (list items, map entries, tuple/record fields, atom variant payload, stream buffer, opaque handle), recursively call `deck_value_release` on each owned child.
2. Free the payload buffer (list `items`, map `entries`, etc.) via `alloc->free`.
3. For `DECK_OPAQUE`, invoke `release(handle_id, ptr)` if non-NULL, before freeing the handle struct.
4. For `DECK_STR`, decrement the intern table entry's refcount (§6); free the string buffer only if non-interned.
5. Free the `DeckValue` header itself.

The cascade is iterative under the hood (an explicit work stack inside the heap struct, not the C stack) to prevent overflow on deeply-nested lists or recursive records.

### 5.3 Cycle prevention

Deck values form a strict DAG. The language design forbids constructs that would create cycles:

- Records and tuples are constructed bottom-up; field values must already exist when the record is built.
- Closures capture a snapshot of their lexical environment at definition time. The captured environment is itself a DAG of values. A closure cannot reference itself by name (recursion uses the named binding, which resolves through the env without creating a self-reference in the value graph).
- Maps and lists are immutable; insertion produces a new container that shares the original payloads but does not cycle back.
- Streams hold callbacks that are functions, but the callback is captured by `id`, not by pointer; the dispatcher resolves the function lazily via the VM's function table.

The loader (§9, stage 7) statically rejects programs whose AST would necessarily produce a cycle. Runtime allocations that bypass the loader (e.g. record construction inside an effect handler) are still safe because the construction order makes back-references impossible.

The runtime therefore performs **no** cycle detection at refcount time. There is no GC pass.

### 5.4 Refcount and snapshots

When a VM is suspended (§16), refcounts are **not** preserved. Snapshot serialization rebuilds the value graph during restore with refcount = 1 on every node. Apps must not rely on observable refcount semantics (there is no Deck-level API to read refcounts).

---

## 6. String Interning

### 6.1 The intern table

Each VM has a `StringTable` keyed by the FNV-1a 64-bit hash of the byte content. Entries:

```
StringEntry {
  uint64_t  hash;
  uint32_t  len;
  uint16_t  refcount;        // wraps to UINT16_MAX (immortal) on overflow
  uint16_t  flags;           // bit 0: pinned by a literal in loaded code
  uint8_t   bytes[];         // flexible array
};
```

Strings are interned when:

- `len ≤ DECK_INTERN_MAX_LEN` (default 32 bytes) and
- The string is constructed via `deck_str()`, `deck_atom()`, an atom literal, a string literal in source code, or the `text.intern(s)` builtin.

Strings produced by `text.concat`, `text.format`, network reads, file reads, etc. are **not** interned by default — they go straight to the heap.

### 6.2 Pinned strings

Every string literal in the loaded program is interned with the `pinned` flag set. Pinned entries have refcount saturated to `UINT16_MAX` and are never evicted while the program is loaded. On hot reload (§22), the loader rewalks all literals and re-pins; entries that are no longer referenced have their pin cleared and become eligible for eviction at refcount 0.

### 6.3 Eviction

The intern table is sized at VM create (`DECK_INTERN_TABLE_BYTES`, default 16 KB on small platforms, 64 KB on hosted). When the table is full and a new entry needs to be added:

1. Walk the table in scan order, looking for the first unpinned entry with `refcount == 0`.
2. If found, free its bytes and reuse the slot.
3. If not found, **bypass interning** for the new string: allocate it on the heap as a non-interned `DECK_STR` and proceed. Increment `rt->stats.intern_full`.

The runtime never reallocates the intern table at runtime; resize happens only at VM create.

### 6.4 Atoms and interning

Atom names live in the atom table (§4.3), not the string table. They are conceptually interned (one entry per unique atom), but the storage is separate. The atom table grows monotonically over the VM's lifetime; entries are never freed. This is acceptable because atom space is bounded by the program's source code (atoms are syntactic literals, not derivable from data).

---

## 7. Lexer

### 7.1 Algorithm overview

The lexer is **table-driven**, single-pass, with **one token of pushback** for operator disambiguation. It emits a flat `[Token]` stream consumed by the parser.

```
Lexer {
  source:        byte[]
  pos:           u32
  line, col:     u32
  indent_stack:  [u32]      # indent column counts; starts as [0]
  pending_dedents: u32      # see §7.3
  pushback:      Token?
  errors:        [LexError]
  string_brace_depth: u32   # see §7.4
}
```

### 7.2 Tokens

Token shape: `{ kind: TokenKind, span: (line, col, len), literal: TokenLit? }`.

`TokenKind` enumerates: identifier, atom, integer, float, string-piece, string-end, multi-line-string, duration, range, operator (each operator is its own kind), keyword (each keyword is its own kind), `INDENT`, `DEDENT`, `NEWLINE`, `EOF`, `ERROR`.

`TokenLit` carries: parsed integer / float / interned-string-id / atom-id / span of error message, depending on the kind.

### 7.3 INDENT / DEDENT

The lexer tracks indentation as **column counts**. Each new logical line:

1. Skip blank lines and full-line comments without producing tokens.
2. Count leading spaces. Tabs are an error (`E_TAB_INDENT`). Mixed-character indentation is an error.
3. Compare to top of `indent_stack`:
   - **Greater**: push the new column count onto `indent_stack`; emit one `INDENT`.
   - **Equal**: emit a `NEWLINE` (separator).
   - **Less**: emit a `NEWLINE`, then pop entries off `indent_stack` until the top matches the current column. For each pop, emit one `DEDENT`. If the current column is not in the stack at any depth (e.g. baseline 0,2,4 but the line is at column 3), emit `ERROR(E_INCONSISTENT_DEDENT)` and recover by aligning to the nearest lower stack entry.

`pending_dedents` exists because a single source line can simultaneously close several indent levels; the lexer emits one `DEDENT` per call, decrementing `pending_dedents`, before resuming normal token production on the next call.

At EOF, the lexer emits as many `DEDENT`s as remain on `indent_stack` (down to baseline 0), then `EOF`.

### 7.4 String interpolation

`"hello {name}!"` is lexed as a sequence:

```
STRING_PIECE("hello ")    INTERP_START    -- literal piece
IDENT("name")             ...             -- one or more tokens
INTERP_END                STRING_PIECE("!")
STRING_END
```

`INTERP_START` increments `string_brace_depth`. `}` decrements it; if depth is positive after the decrement, the lexer remains inside the string and resumes literal scanning. If depth becomes zero, the `}` is emitted as the close brace of an outer block expression.

Nested interpolations are supported up to `DECK_INTERP_MAX_DEPTH` (default 4). Exceeding this emits `ERROR(E_INTERP_DEPTH)` and recovers by treating the next `}` as a literal character.

### 7.5 Multi-line strings

`"""` opens multi-line mode. The opening `"""` MAY be followed by a newline, which is NOT included in the string. The closing `"""` MAY be preceded by indentation, which is stripped according to the column of the closing delimiter (Python-style dedent). Interpolation inside multi-line strings is permitted and follows §7.4 rules.

If `"""` is not closed before EOF, the lexer emits `ERROR(E_UNTERMINATED_STRING)` with a span pointing at the opening delimiter.

### 7.6 Escapes

Recognized escape sequences inside `"…"` and `"""…"""` strings:

```
\n  \r  \t  \\  \"  \{  \}  \0  \xHH  \uHHHH
```

`\u` followed by 4 hex digits encodes a Unicode code point as UTF-8 (1–3 bytes for BMP). Unknown escapes (`\q`) emit `ERROR(E_UNKNOWN_ESCAPE)` and the literal sequence is passed through as-is for recovery.

### 7.7 Number literals

Integer: `[0-9][_0-9]*` for decimal, `0x[0-9a-fA-F_]+` for hex, `0b[01_]+` for binary, `0o[0-7_]+` for octal. Underscores are accepted as digit separators and stripped before parsing. Range: i32. Out-of-range emits `ERROR(E_INT_RANGE)` and the token still parses (clamped to i32 limits) to keep the parser walking.

Float: `[0-9][_0-9]*\.[0-9][_0-9]*` with optional `[eE][+-]?[0-9]+` exponent. Range: f32. NaN and infinity literals are not part of the surface syntax (use `math.nan()`, `math.inf()`).

### 7.8 Duration and range

Duration: integer immediately followed by `ms`, `s`, `m`, `h`, `d`. No space. `5m` parses as `Duration(300_000)`. Mixed (`1h30m`) is **not** supported by the lexer; it is a parse error if combined.

Range: `expr..expr` and `expr..=expr`. The `..` and `..=` are operator tokens; the parser builds the `RangeExpr`.

### 7.9 Error recovery

After emitting any `ERROR` token, the lexer:

1. Continues from the next byte (or next line, if the error was structural like inconsistent indent).
2. Caps total errors per file at `DECK_LEX_ERR_MAX` (default 50). On overflow, emits `ERROR(E_TOO_MANY_ERRORS)` and switches to fast-skip mode that emits only `EOF`.

The parser is allowed to keep walking through `ERROR` tokens; they are reported and suppressed at AST construction time. This produces a single load failure with as many diagnostics as possible rather than one error at a time.

---

## 8. Parser

### 8.1 Algorithm

The parser is **recursive descent + Pratt** for expressions. Statements use plain recursive descent. Lookahead is bounded at 2 tokens (`peek()`, `peek2()`); cases that need more (e.g., distinguishing `let x = ...` from `let x : T = ...`) use the existing pushback slot in the lexer.

The parser allocates AST nodes from `vm->parse_arena` (§3.3). Source spans are stored in every node as `(start_line, start_col, len)` triples for diagnostics.

### 8.2 Operator precedence

Highest at the top:

| Level | Operators | Assoc |
|---|---|---|
| 12 | `f(x)`, `x.field`, `x[i]` | left |
| 11 | unary `-`, unary `not` | right |
| 10 | `*`  `/`  `%` | left |
| 9  | `+`  `-`  `++` (string concat) | left |
| 8  | `..`  `..=` | none |
| 7  | `<` `<=` `>` `>=` | none |
| 6  | `==`  `!=`  `is` | none |
| 5  | `and` | left |
| 4  | `or` | left |
| 3  | `\|>`  `\|>?` | left |
| 2  | `->` (lambda) | right |
| 1  | `=` (let / where binding) | n/a |

Operators at level 7 and 6 are non-associative — `a < b < c` is a parse error, not chained comparison.

### 8.3 NEWLINE and INDENT in the grammar

The parser treats `NEWLINE` as a statement separator inside blocks. It is **silently skipped** between operator and operand (`a +\n  b` parses as `a + b`). Between two complete expressions it terminates the first.

`INDENT` and `DEDENT` open and close blocks. The grammar rules that introduce blocks (function bodies, `do`, `match` arms, `@machine` body, `@flow` body, content bodies) all read an `INDENT … DEDENT` pair. Unbalanced indent inside an expected block is `ERROR(E_BLOCK_STRUCTURE)`.

### 8.4 Error recovery

The parser uses **statement-level synchronization**. On any error inside a statement, it skips tokens until it sees a synchronization point:

- `NEWLINE` at the current indent level
- `DEDENT` matching the current block depth
- A keyword that opens a top-level form (`@app`, `@use`, `@machine`, `@flow`, `@on`, `@stream`, `@task`, `@migration`, `@test`, `@config`, `@permissions`, `@errors`, `@type`, `@assets`, `@handles`, `@doc`, `@example`, `fn`)
- `EOF`

After synchronization, parsing resumes from the next statement. The error count cap is `DECK_PARSE_ERR_MAX` (default 50, as for the lexer).

### 8.5 AST node shapes

The full set is documented in `04-deck-runtime §4.2`. The implementation contract is:

- AST nodes are immutable after construction.
- Children are stored as `Node*` (pointers into the parse arena) or `[Node*]` (a length-prefixed array).
- No node holds back-references to its parent; the parser does not need them and the loader walks top-down.
- Nodes carry their own `kind` discriminator (one byte) plus a payload union (≤ 24 bytes on 32-bit). Total node size is bounded so a 200-line program stays under 32 KB of AST.

### 8.6 Validation in the parser vs. loader

The parser enforces **only** lexical and grammatical invariants:

- Operator precedence and associativity are correct
- All blocks are balanced
- All literal forms parse to valid values

Semantic validation — `to history` placement, `@on` references existing events, capability paths exist in the OS surface, type compatibility — is the loader's job (§9). The parser produces a valid AST even for semantically nonsensical programs and lets the loader explain what is wrong.

---

## 9. Loader

### 9.1 Stage list

The loader walks twelve stages in order. Stage N runs only if all stages 0..N-1 produced no errors. Each stage operates on the AST from the parser and may annotate or rewrite it. After stage 12 the resulting structure is the **load image**, ready to hand off to the evaluator.

| Stage | Name | Inputs | Outputs |
|---|---|---|---|
| 0 | Bundle inspection | App directory on disk | Manifest, asset list, signature verification result |
| 0.5 | Migration | Previous on-disk schema version (read from app's NVS namespace) and current `@migration` declarations | Updated DB / NVS / storage; new schema version recorded |
| 1 | Module graph | `app.deck` and `@use ./path` entries | List of module ASTs in topological order |
| 2 | OS surface attach | Loaded `.deck-os` for the platform | Capability table, event table, builtin table |
| 3 | Type checking | All ASTs + OS surface | Annotated AST (every expression has a resolved type) |
| 4 | Effect checking | Annotated AST | List of `(function, effect_set)` pairs |
| 5 | Permission negotiation | `@permissions` + capabilities marked `@requires_permission` | Permission map (`{capability_path → granted | denied | absent}`) |
| 6 | Capability binding | `@use` aliases + permission map | `alias → capability_path → C callback` map; absent capabilities flagged |
| 7 | Topology validation | `@machine`, `@flow`, `@stream` declarations | Static dependency graph, watch fan-out table, stream graph |
| 8 | Migration verification | DB schema vs current `@migration` chain | Confirms no gap remains |
| 9 | Stream graph | `@stream` declarations + their derivations | Compiled stream pipelines |
| 10 | Pattern compilation | All `match` expressions and transition `from` clauses | Decision trees; exhaustiveness diagnostics |
| 11 | Asset registration | `@assets` block | Asset trust map; asset descriptors registered with bridge |
| 12 | Binding resolution | Annotated AST | Final image with `VarExpr` nodes replaced by direct env-slot indices |

### 9.2 Atomicity

The loader is **fail-atomic** with respect to runtime state: any failure between stages aborts the load and leaves the runtime exactly as it was before `deck_runtime_load` was called. The parse arena and any partial annotations are freed.

The exception is **stage 0.5 (migration)**, which mutates persistent storage. Migrations follow a separate transaction protocol (§9.4) that is robust to mid-migration crashes.

### 9.3 Module graph (stage 1)

Algorithm: depth-first walk starting from `app.deck`, with cycle detection via a coloring stack (white / grey / black).

```
load_modules(root):
  graph    = {}                # path → ModuleAST
  visiting = ordered_set()     # for cycle detection
  visited  = set()

  def visit(path):
    if path in visited: return
    if path in visiting:
      cycle = visiting.suffix_from(path) ++ [path]
      error(E_MODULE_CYCLE, cycle)
      return
    visiting.push(path)
    ast = parse(path)
    for entry in ast.use_entries.local:
      visit(resolve(entry, base=path))
    visiting.pop()
    visited.add(path)
    graph[path] = ast

  visit(root)
  return topological_sort(graph)        # roots first
```

Topological sort places modules with no `@use ./` dependencies first; the type checker walks them in that order so every module is type-checked after its dependencies.

### 9.4 Migration protocol (stage 0.5)

Each app's NVS namespace stores `__deck_schema_version: int`. The loader reads it, then walks the chain of `@migration { from: vN }` declarations from the current version up to the app's declared latest.

For each migration:

1. Begin a SQLite transaction over the app's DB.
2. Acquire an NVS write lock (the platform doc specifies the exact mechanism; typically a magic key `__deck_mig_inprogress = 1`).
3. Run the migration body in a restricted context (only `db`, `nvs`, `fs`, `config.set` are accessible). Any other capability call is a load error.
4. On success: commit the SQLite transaction, write `__deck_schema_version = vN+1`, clear the in-progress key.
5. On failure: rollback the SQLite transaction, leave `__deck_schema_version` unchanged, leave the in-progress key set.

If the loader sees `__deck_mig_inprogress = 1` at startup, the previous run crashed mid-migration. Behavior: emit `runtime.migration_recovery` and rerun the migration from the current `__deck_schema_version`. This is safe because each migration body is required to be idempotent (the spec states this in `02-deck-app §15`); if a migration is non-idempotent the app developer is responsible.

### 9.5 Permission negotiation (stage 5)

The loader emits one bridge call per permission entry:

```
bridge.permission_request(capability_path, reason) → {grant, deny, defer}
```

`grant` and `deny` resolve immediately. `defer` means the bridge needs user input; the loader **blocks** for up to `DECK_PERM_TIMEOUT_MS` (default 60_000). On timeout the result is treated as `deny` and a warning is logged.

The bridge is responsible for caching grants across launches; the loader never asks twice for a permission already granted in NVS. The loader does not implement that cache; it trusts the bridge to short-circuit.

If a non-`optional` capability with `@requires_permission` ends up `denied` or `absent`, this stage produces a load error (`E_PERMISSION_DENIED`). For `optional` capabilities, the loader records the absence and continues; calls to absent capabilities at runtime return `:err :unavailable` automatically (the dispatcher implements this in §12.4).

### 9.6 Pattern compilation (stage 10)

Every `match` expression and every `transition … from` clause is compiled to a **decision tree**. The algorithm is the standard one due to Maranget (2008): for each row of the pattern matrix, pick a column that disambiguates the most rows, branch on it, and recurse.

The exhaustiveness check runs on the same matrix:

- For atom-typed scalars with explicit options (atom variants, bools), all listed values must be covered or a wildcard arm must exist.
- For records, the union of patterns must cover every constructor of the record's type.
- For Result and Option, both `:ok`/`:err` (or `:some`/`:none`) must be covered.
- For all other types, a wildcard arm is required.

Non-exhaustive matches are a load error in `match` expressions. In transition `from` clauses, non-exhaustiveness is **not** an error — un-matched states simply do not fire the transition (this is the documented behavior in `02-deck-app §8.4`).

### 9.7 Binding resolution (stage 12)

Every `VarExpr { name }` node is rewritten to one of:

- `LocalSlot { depth, slot }` — frame-relative index for let-bound names
- `ParamSlot { slot }` — current-frame parameter
- `GlobalRef { module_id, slot }` — top-level binding from another module
- `CapMethod { cap_id, method_id }` — bound capability call
- `BuiltinRef { builtin_id }` — language or OS builtin

After resolution, the evaluator never does string-keyed lookup at runtime. All name resolution is `O(1)` via array indexing.

### 9.8 Load result

The loader returns `DeckLoadResult { ok: bool, errors: [LoadError], warnings: [LoadWarning], image: LoadImage? }`. `errors` and `warnings` are NUL-terminated arrays of `(stage, span, code, message)` records. The bridge displays them; on `ok = true` the runtime caller receives the image and may proceed to `deck_runtime_start(image)`.

---

## 10. Evaluator

### 10.1 Strategy: tree-walking with a value stack

The evaluator is a **tree-walking interpreter** with an explicit value stack and call-frame stack. There is no bytecode compilation step. The decision tradeoff:

- **Pro tree-walking:** smaller binary, simpler debugger story, faster cold-start (no JIT/codegen pass), fewer attack surfaces for an embedded target.
- **Con:** ~3× slower per opcode than a register-based bytecode VM. Acceptable because the hot paths in real apps are effect calls (network, DB, render), not Deck-level arithmetic.

Future implementations may add a bytecode mode behind a `DeckRuntimeConfig.eval_mode` flag; it must not change observable semantics.

### 10.2 Frame and stack

```
Frame {
  fn:           FnRef              # currently executing function
  pc:           NodePtr             # current AST node (for error spans)
  locals:       DeckValue[fn.max_locals]
  parent_env:   EnvRef             # captured at call time
  caller:       Frame*
}

VM state (per VM):
  frames:       Frame*             # linked list, deepest first
  frame_depth:  u16                # current depth
  value_stack:  DeckValue*         # ring buffer; size = DECK_VALUE_STACK_DEPTH (default 256)
  vsp:          u16                # value stack pointer
```

Call: push a new `Frame`, copy/wrap arguments into `locals[0..arity]`, set `pc` to the function body root, evaluate. Return: pop the frame, push the return value onto the value stack of the caller.

Call depth is bounded by `DECK_FRAME_DEPTH_MAX` (default 64). Overflow raises `:err :stack_overflow` carrying the offending span and a synthetic backtrace of up to 16 most-recent frames.

### 10.3 Tail call optimization

A call is a **tail call** if it appears in tail position relative to its enclosing function:

- Final expression of a function body
- Final expression of a `do` block that is itself in tail position
- Right-hand side of a `match` arm whose `match` is in tail position
- Right-hand side of a `\|>` chain's last stage

Tail calls **reuse the current frame** (overwrite `locals` and `pc`, do not push). They are detected at load time (stage 12) and the AST node carries a `tail_call: bool` flag. There is no runtime detection.

This makes mutual recursion via tail calls and `\|>` chains arbitrarily deep without growing the frame stack.

### 10.4 Closures

A closure is an `FnRef` with `kind = FN_CLOSURE` and an `env: EnvRef` pointer. The env is a reference-counted snapshot of the captured locals at definition time. Because locals are immutable, the snapshot is a shallow copy — captured `DeckValue*` pointers are retained, then released when the env's refcount drops to 0.

Closures captured from the same enclosing scope share the same `EnvRef` (the runtime checks at definition time and dedups).

### 10.5 Pattern matching execution

The decision tree from stage 10 is walked left-to-right, picking the branch whose discriminator matches the subject. Bindings introduced by patterns are pushed onto the current frame's locals before the arm body executes; they are popped after.

`when` guards on a pattern arm execute after pattern binding succeeds. If the guard returns false, evaluation backs out (releases pattern bindings) and continues with the next arm.

### 10.6 Side effects in expressions

Functions tagged with `!effect` annotations are valid only in `do` blocks, function bodies whose own signature carries the same effects, and content body intent handlers. The evaluator does not re-check this at call time — the loader (stage 4) has already verified.

When the evaluator encounters an effect call, it routes through the dispatcher (§12). For synchronous capabilities, the result `DeckValue*` is pushed to the value stack and execution continues. For asynchronous capabilities, the evaluator's behavior depends on whether the result is awaited:

- **Awaited** (via `\|>`, direct binding, or guard): the evaluator parks the current frame in a continuation table keyed by the request id, returns control to the runtime main loop, and resumes when the dispatcher fires `MSG_EFFECT_RESULT`.
- **Fire-and-forget** (effect call as a statement in a `do` block whose return type is `unit`): the call is queued in the dispatcher and the `do` block continues immediately. The value stack receives `unit`.

### 10.7 NaN and float behavior

Float arithmetic follows IEEE-754 binary32 semantics. NaN propagates through arithmetic; the result is a `DECK_FLOAT` whose payload is the IEEE NaN bit pattern.

`type_of(NaN)` returns `:nan` (a special atom; this is the only place NaN is observable as anything other than `DECK_FLOAT`). Comparisons involving NaN return false; `NaN == NaN` is false. `is :nan` is the canonical way to test for NaN in Deck code.

### 10.8 Send, machine state, watch fan-out

`Machine.send(:event, args)` is implemented as:

1. Look up the machine by its bound name (resolved at stage 12).
2. Find the transition whose `event` and `from` match. If none, return silently.
3. Evaluate the `when` guard (if any). If false, return silently.
4. Run `before` hook (if any) and the from-state's `on leave` (if any).
5. Compute new payload by evaluating the `to` expression list with the `from` binding in scope.
6. Atomically replace `machine.current_state` and `machine.payload`.
7. Run the to-state's `on enter` (if any) and the transition's `after` (if any).
8. Re-evaluate any `when:` conditions or `watch:` clauses that reference this machine. New transitions that fire are appended to the work queue (BFS, see §2.4).
9. If any active screen reads from this machine, mark the navigation manager's render-dirty flag (§15).

Steps 4–7 run in the same dispatch tick; reactive `watch:` transitions in step 8 may run later in the same tick (BFS) but no further than the work queue drains.

### 10.9 Recovering from an evaluator panic

A **panic** is an unrecoverable evaluator error: stack overflow, division by zero on integers, OOM during allocation, pattern match failure that the loader could not statically reject (this should never happen but the runtime guards it), or any internal invariant violation.

On panic:

1. The evaluator unwinds the current frame stack via setjmp/longjmp (the bridge installs the longjmp target in `deck_runtime_dispatch`).
2. The runtime emits a `runtime.panic` event with the panic kind, the span of the offending node, and the backtrace.
3. The bridge calls `system.shell.report_crash` (or its equivalent) with `CrashInfo`.
4. The runtime is left in a **degraded** state: no further messages are dispatched until the bridge calls `deck_runtime_terminate`. A panicked VM cannot recover.

Panics inside `@on` hooks follow the same path; the OS event that triggered the hook is logged as undelivered.

---

## 11. Pattern Matching Compilation

### 11.1 Pattern matrix

For each `match`, the loader builds a matrix where each row is one arm and each column is one component of the subject. For nested patterns, the matrix is expanded by introducing fresh occurrences for sub-positions.

### 11.2 Decision tree algorithm

The loader picks one column at a time using the following heuristic (in order):

1. Column whose patterns are all literal constructors with no wildcards
2. Column with the smallest number of distinct top-level constructors
3. Leftmost column among ties

It then partitions rows by the selected column's constructor and recurses on the residual matrix for each branch. A wildcard branch is added if any row has a wildcard in the column.

### 11.3 Output: a compact decision tree

The compiled tree is an array of nodes:

```
DTNode = Switch { occurrence: u16, branches: [(Constructor, NodeIdx)], default: NodeIdx? }
       | Leaf   { arm: u16, bindings: [(local_slot: u16, source: PathSpec)] }
       | Fail   { reason: NoMatchReason }
```

The evaluator walks the tree with the subject in hand. At a `Switch`, it inspects the occurrence path (e.g. `subject.field_2.list[0]`) and branches. At a `Leaf`, it copies the source paths into the named local slots and jumps to the arm body. `Fail` runs only for non-exhaustive `match` (a panic) or for non-matching transition `from` clauses (silent skip).

### 11.4 Exhaustiveness algorithm

Same Maranget approach. A pattern matrix is exhaustive if and only if there is no value of the subject's type that fails to match any row. The loader computes the **useless rows** (each row's redundant patterns are flagged as warnings) and **missing patterns** (concrete examples of values that no arm matches).

Missing-pattern reporting is best-effort: the loader produces up to 8 example unmatched constructors per match and lists them in the load warning.

---

## 12. Effect Dispatcher

### 12.1 Capability table

After loading, the runtime has a **flat array** of capability entries, indexed by `cap_id` assigned at load time:

```
CapEntry {
  path:           StrRef        # e.g. "network.http"
  alias:          StrRef        # the name used in the app's @use
  permission:     enum {grant, optional, absent}
  methods:        [MethodEntry] # indexed by method_id
}

MethodEntry {
  signature_id:   u16           # for argument marshaling
  flags:          u8            # bit 0: @pure  bit 1: @stream  bit 2: @singleton  bit 3: @impure
  c_callback:     void*         # bridge-supplied; resolved at registration
  user_data:      void*         # opaque to runtime; passed back to bridge
}
```

Lookup of `alias.method(args)` at runtime is two array indexings: the load image carries the resolved `cap_id` and `method_id` for every effect call site (resolved in stage 12). There is no string-keyed lookup at evaluation time.

### 12.2 Argument marshaling

Each call site has an associated **signature** (built at stage 6) that describes how to marshal Deck values into the C call. The signature stores positional argument types, named argument names, and the C callback's expected argv layout.

Marshaling is a copy: the dispatcher allocates a `DeckValue*[argc]` from the **call arena** (a per-VM bump allocator reset per dispatch), fills it with retained references to the Deck values, and passes the array to the C callback. The C callback owns the array for the duration of its call; on return, the dispatcher releases each value.

Unmarshaling the C return value is symmetric — the C callback returns a `DeckValue*` that the dispatcher pushes onto the value stack with the same ownership semantics as `deck_*_new` constructors (return-owned).

### 12.3 Synchronous vs asynchronous

`@pure` capabilities are always synchronous — they must return on the calling thread within the C call. The dispatcher does not park the frame.

Non-`@pure` capabilities default to synchronous unless the bridge returns a sentinel `DECK_PENDING` value. `DECK_PENDING` carries:

```
DECK_PENDING { request_id: u64, cancel_fn: void(*)(u64), bridge_data: void* }
```

The dispatcher records `(request_id → frame)` in a continuation table and yields control back to the main loop. When the bridge calls `deck_dispatch_result(rt, request_id, result)`, the runtime restores the frame, pushes `result` to its value stack, and resumes evaluation.

A request that completes synchronously (the bridge returned a real `DeckValue*` not `DECK_PENDING`) bypasses the continuation table.

### 12.4 Optional / absent capabilities

If a method's `permission` is `absent` (capability is optional and unavailable), the dispatcher short-circuits **without calling the bridge**: it returns `:err :unavailable` immediately. The cap method's signature must be `Result T E` where `:unavailable` is a documented error variant; this is enforced at load time.

This makes the optional-capability pattern zero-cost when the capability is missing — the bridge is never entered.

### 12.5 Singleton enforcement

`@singleton` capability methods (typically network requests with shared state) carry a `singleton_lock_id`. The dispatcher tracks one in-flight request per id; concurrent calls wait in a per-singleton FIFO of bounded depth (`DECK_SINGLETON_QUEUE_DEPTH`, default 8). On overflow, the new call returns `:err :busy`.

The lock is released when `deck_dispatch_result` is called for the in-flight request, regardless of whether it produced `:ok` or `:err`. If the bridge never calls `deck_dispatch_result` (a bridge bug), the lock is held until VM termination — there is no timeout reaper. The bridge implementer is responsible for never dropping a request on the floor.

### 12.6 Cancellation

The dispatcher supports cancellation only via `cancel_fn` on `DECK_PENDING`. The runtime calls `cancel_fn(request_id)` when:

- The VM is being terminated.
- A reactive transition fired that invalidates the call's purpose (rare; only when the call is explicitly inside a `watch:` body).

The bridge is expected to be cooperative — `cancel_fn` should attempt to abort the underlying operation. The dispatcher does not wait for cancellation acknowledgment; it removes the continuation from the table and treats any subsequent `deck_dispatch_result` for that id as a no-op.

### 12.7 Ordering guarantees

The dispatcher guarantees:

1. **Per-call-site FIFO**: two calls from the same source location resolve in source order.
2. **Per-singleton FIFO**: see §12.5.
3. No global ordering across call sites or capabilities.

If an app needs to serialize across capabilities, it must use machine state to gate (e.g. transition to a `:waiting` state until the first call's result fires the next transition).

---

## 13. Scheduler and Streams

### 13.1 What the scheduler does

The scheduler is responsible for:

- Firing `@task` invocations at their scheduled time
- Re-evaluating `when:` conditions on `@use` and `@task` declarations
- Re-evaluating `watch:` conditions on transitions
- Pumping stream operators (filter, map, throttle, debounce, combine_latest, merge)

It runs once per main-loop tick (§2.3, called from `scheduler.tick(now())`).

### 13.2 Task registry

```
TaskEntry {
  decl:         &TaskDecl              # AST decl + compiled body
  id:           u16                    # stable id for diagnostics
  next_fire_at: Timestamp?             # nil if condition-only
  conds:        [ConditionId]          # references into condition tracker
  battery_hint: enum {normal, efficient, power_saving}
  in_flight:    bool
  last_fire_at: Timestamp?
  miss_count:   u16
}
```

Tasks are stored in a min-heap keyed by `next_fire_at` for O(log n) extraction of the earliest-due task.

Each tick:

1. Pop tasks whose `next_fire_at ≤ now()`.
2. For each, re-check all `conds`; if any is false, reschedule for the next interval and continue.
3. Otherwise, set `in_flight = true` and enqueue a `MSG_TASK_RUN` message into the runtime mailbox.
4. After `MSG_TASK_RUN` completes, the dispatcher sets `in_flight = false` and recomputes `next_fire_at`.

If a task's previous run has not finished by its next scheduled time, the scheduler **skips** the new fire and increments `miss_count`. After 3 consecutive misses, the runtime emits `runtime.task_overrun` warning naming the task.

### 13.3 Condition tracker

A condition is a boolean expression that the runtime monitors for change. Examples: `network is :connected`, `Auth is :authenticated`, `battery > 20%`.

```
Condition {
  id:           u16
  expr:         &Expr
  deps:         [(MachineId | StreamId | OSStateId)]
  current:      bool
  subscribers:  [(TaskId | TransitionId | UseEntryId)]
}
```

`deps` is computed at load time by walking the expression's AST and listing every reactive source it reads. When any dep changes, the runtime re-evaluates affected conditions. If the result transitioned, the runtime notifies subscribers.

A subscriber may be:

- A `@task` (which becomes eligible to fire if all its conds are true)
- A `watch:` transition (which fires when its condition becomes true)
- An `@use … when:` capability gate (which becomes available / unavailable)

The condition tracker uses **edge** semantics: subscribers are notified only on false→true transitions for `@task when:` and `watch:`, and on either edge for `@use … when:` (because both edges affect availability).

### 13.4 Stream operators

A stream is a directed graph from sources (capabilities returning `Stream T`) to sinks (subscribers — typically machine `@stream` declarations or `combine_latest` operators). Operators:

- `filter(fn)` — drops values where `fn(v) = false`
- `map(fn)` — transforms each value
- `throttle(d)` — emits at most one value per `d` window
- `debounce(d)` — emits only after `d` of silence
- `combine_latest(other)` — emits a tuple of `(self_latest, other_latest)` whenever either source emits, after both have emitted at least once
- `merge(other)` — emits values from either source as they arrive
- `take(n)` — emits the first `n` values then completes
- `skip(n)` — drops the first `n` values

Each operator is a node in the stream graph with state:

```
StreamNode {
  id:          StreamId
  kind:        StreamKind
  upstream:    [StreamId]
  downstream:  [StreamId]
  buffer:      RingBuffer<DeckValue*>      # see §13.5
  state:       union { ThrottleState, DebounceState, CombineState, ... }
}
```

### 13.5 Buffer sizing and backpressure

Each stream node has a ring buffer of bounded size. Defaults:

| Node kind | Default capacity | Rationale |
|---|---|---|
| Source | 8 | Producers may burst; consumers drain on the next tick |
| `filter`, `map`, `take`, `skip` | 0 (passthrough; no buffer) | Pure transforms, no async needed |
| `throttle`, `debounce` | 1 (latest only) | These operators inherently keep one value |
| `combine_latest` | 1 per upstream | Stores last-seen per source |
| `merge` | sum of upstream caps | Direct concatenation |
| Sink (machine `@stream`) | 4 | Apps usually consume immediately |

Capacities can be overridden per-stream via `@stream foo (buffer: N) source: …`.

**Backpressure policy:** when a buffer overflows, the **default** is `:drop_oldest` (the new value evicts the oldest). Other policies (`:drop_newest`, `:block`, `:err`) can be selected per stream:

```
@stream foo (buffer: 16, on_overflow: :err) source: bar.watch()
```

`:block` blocks the producer (the bridge thread that called `deck_stream_push`) for up to `DECK_STREAM_BLOCK_MS` (default 100 ms) before falling back to drop. `:err` causes the consumer's next read to receive `:err :overflow`.

The runtime exposes per-stream stats (`pushed`, `dropped`, `peak_depth`) via `system.tasks.stream_stats(stream_id)` for diagnostics.

### 13.6 Operator timing

`throttle` and `debounce` use the runtime's monotonic clock (`runtime.now_monotonic()`), not the wall-clock. Wall-clock changes (NTP sync, manual time set) do not perturb these operators.

Timer resolution is `TICK_MS` (default 50 ms). Operators with a window smaller than `TICK_MS` round up to `TICK_MS`. The runtime documents this as a load warning when a duration literal smaller than `TICK_MS` is used in `throttle` or `debounce`.

### 13.7 Stream subscription lifecycle

A stream is **inactive** until at least one downstream consumer subscribes. The runtime defers calling the source's `start` callback (in the bridge) until the first subscription. When the last subscriber unsubscribes, `stop` is called.

This avoids paying for sensor / network costs on streams declared but never consumed. Apps should not rely on the source running before consumption.

Subscriptions are tracked with refcounts. `unsubscribe()` decrements; `stop` is called on transition to 0.

---

## 14. Reactive Re-evaluation

### 14.1 Dependency graph

Build at load time: for every `when:` and `watch:` expression, walk its AST and record every `MachineId`, `StreamId`, `ConfigField`, or `OSState` (e.g. `network`, `battery`) it reads. This produces a `(reactive_source → [Condition])` mapping.

### 14.2 Notification protocol

When a reactive source changes value, the runtime walks the affected conditions in **declaration order** (the order they appear in source code). For each condition that flipped:

1. If subscribers include `watch:` transitions, fire each in declaration order. State changes from those transitions append to the work queue.
2. If subscribers include `@task` entries, mark them as immediately eligible; the next scheduler tick will fire them.
3. If subscribers include `@use … when:` gates, update the capability entry's `permission` flag.

### 14.3 Avoiding infinite loops

Reactive transitions can cascade — A's transition fires, which changes A's state, which causes a `watch: A is :foo` transition on B to fire, which changes B's state, which causes another condition to flip. The runtime caps cascade depth at `DECK_REACTIVE_CASCADE_DEPTH` (default 16) per outer dispatch. On overflow:

1. Emits `runtime.reactive_cascade` with the cycle (machines and conditions that fired).
2. Drops the remaining queued work.
3. The VM continues; the cycle is treated as a recoverable bug, not a panic.

### 14.4 Stale read prevention

While inside a single dispatch tick, the value of every reactive source is **frozen** at the start of the tick. Conditions re-evaluate against the frozen values; new values from the bridge arriving mid-tick are queued for the next tick.

This is the runtime's main consistency guarantee: a single user action or a single OS event produces a well-defined, sequential cascade with no read-write races.

---

## 15. Navigation Manager

### 15.1 Per-VM navigation state

```
NavState {
  root_machine_id:   MachineId           # the @app entry: machine
  flow_history:      [FlowFrame]         # one per @flow nesting level
  modal_stack:       [ModalFrame]        # screens declared modal:
  render_dirty:      bool
  last_render_token: u64                 # changes per render to invalidate caches
}

FlowFrame {
  flow_id:        FlowId
  current_state:  AtomId                 # within the flow
  history:        [AtomId]               # bounded; one level of `to history`
}

ModalFrame {
  origin_state:   AtomId                 # the state that opened the modal
  modal_machine: MachineId               # if the modal is itself a machine
}
```

### 15.2 `to history`

When a transition specifies `to history`:

1. Pop the most recent state from the relevant `FlowFrame.history`.
2. If `history` is empty, the transition silently does nothing (logged as `runtime.history_empty`).
3. The popped state becomes the new current state. The transition's `before`/`after` hooks still run; the from-state's `on leave` and the to-state's `on enter` run as if the transition had named the popped state explicitly.

History is bounded by `DECK_FLOW_HISTORY_DEPTH` (default 8). Beyond that, older entries are dropped (FIFO). Most flows do not approach this; tab-style flows that swap states without pushing history are not affected.

### 15.3 Render trigger

`render_dirty` is set when:

- Any machine state changes (§10.8 step 9)
- Any `@stream` value changes (downstream sinks the render reads)
- The display rotation changes
- The theme changes
- The config value changes for a field referenced in any active content body

Once dirty, the next main-loop tick collects the **active content tree** by walking from the root machine through nested `@flow`s and `@machine` delegations, producing a `DeckViewContent` tree (§18). The tree is handed to the bridge via `MSG_RENDER`; `render_dirty` is cleared.

The runtime does not perform diffing — that is the bridge's job. The runtime always emits a complete tree.

### 15.4 Active content tree assembly

Algorithm:

```
build_active_tree(machine):
  state = machine.current_state
  decl  = machine.state_decl(state)
  if decl.flow:
    return build_active_tree(decl.flow.current_machine)   # delegate
  if decl.machine:
    return build_active_tree(decl.machine)                # delegate
  if decl.content:
    nodes = eval_content_body(decl.content, machine.payload)
    return DVC_FLOW { machine_id, state, children: nodes }
  return DVC_EMPTY
```

The result is one `DVC_FLOW` node per machine in the active path, with the inner content as the leaf.

### 15.5 Modal handling

A modal-marked state (`state :overlay modal: true (…)`) does not replace the current screen — it pushes onto `modal_stack`. The render emits both the underlying state's content and the modal's content. The bridge layers them; the modal occupies `lv_layer_top` in CyberDeck's case.

Closing a modal pops `modal_stack` and re-renders without it. `to history` from inside a modal first dismisses the modal (one frame at a time) before walking history.

---

## 16. VM Snapshot and Restore

### 16.1 When snapshots happen

A VM is snapshotted when the OS suspends it (the user navigates to another app, or memory pressure forces eviction of a foreground non-system app while it transitions to background). The snapshot captures everything needed to resume the VM **bit-identically** later, including:

- All machine states and their current payloads
- All bound config values
- The flow history
- All stream subscriptions and their last-seen values (no in-flight buffers)
- The task registry (next-fire times relative to `now`)
- The continuation table (in-flight effect calls — see §16.3)
- Any opaque handle references (with restore policy — see §16.4)

The render arena and parse arena are **not** snapshotted; they are recreated empty on restore.

### 16.2 Snapshot binary format

```
SnapshotHeader (32 bytes):
  magic:           u32       # 0xDECC1B0A
  format_version:  u16       # currently 1
  flags:           u16       # bit 0: compressed (lz4)  bit 1: encrypted
  app_id_len:      u16
  total_size:      u32       # entire snapshot bytes including header
  payload_offset:  u32       # bytes from start of file to first payload section
  payload_size:    u32       # bytes after compression
  uncompressed_size: u32     # bytes when expanded
  crc32:           u32       # of payload bytes (post-compression, pre-encryption)
  reserved:        u32

Followed by app_id_len bytes of UTF-8 app id (NUL-padded to 4-byte alignment).
```

After the header, the payload is a sequence of typed sections:

```
Section:
  kind:       u16
  flags:      u16
  size:       u32
  body:       byte[size]
```

Section kinds:

| Kind | Name | Body |
|---|---|---|
| `0x01` | `STRING_TABLE` | Length-prefixed UTF-8 strings; later sections reference by index |
| `0x02` | `ATOM_TABLE` | Indexed atom names; the snapshot remaps the runtime's atom table on restore |
| `0x03` | `TYPE_TABLE` | Record type ids → (name, field names) |
| `0x04` | `MACHINE_STATE` | One entry per machine: machine_id, current_state_atom, payload (DeckValue) |
| `0x05` | `FLOW_HISTORY` | One entry per active flow frame: flow_id, current_state, history list |
| `0x06` | `CONFIG_VALUES` | Field name → value (covers any config the VM has set/changed) |
| `0x07` | `STREAM_LATEST` | Per-subscribed-stream: subscription_id, last_value (DeckValue), pending count |
| `0x08` | `TASK_REGISTRY` | Per task: task_id, ms_until_next_fire (relative; recomputed on restore) |
| `0x09` | `CONTINUATIONS` | In-flight effect calls — see §16.3 |
| `0x0A` | `OPAQUE_HANDLES` | Per opaque value: type_id, handle_id, restore policy — see §16.4 |
| `0x0B` | `STATS` | Telemetry counters (alloc_total, mailbox_high_water, etc.) |

Sections appear in numeric order. Unknown sections are skipped (forward compatibility).

`DeckValue` is encoded inside section bodies using a canonical wire encoding:

```
WireValue:
  type:    u8         # DeckType
  payload: type-discriminated:
    DECK_UNIT       → 0 bytes
    DECK_BOOL       → 1 byte
    DECK_INT        → 4 bytes (LE i32)
    DECK_FLOAT      → 4 bytes (LE f32)
    DECK_BYTE       → 1 byte
    DECK_STR        → 4 bytes len + n bytes UTF-8 OR 4 bytes string-table index (high bit set)
    DECK_BYTES      → 4 bytes len + n bytes
    DECK_ATOM       → 4 bytes atom-table index, then optional WireValue if variant
    DECK_LIST       → 4 bytes len + n × WireValue
    DECK_MAP        → 4 bytes count + n × (4 bytes string-table key + WireValue)
    DECK_TUPLE      → 1 byte arity + n × WireValue
    DECK_RECORD     → 2 bytes type_id + n × WireValue (n implied by type)
    DECK_FN         → not snapshotted; an error to encounter
    DECK_STREAM     → 4 bytes subscription_id (the runtime restores subscription state)
    DECK_OPAQUE     → see §16.4
    DECK_RESULT_OK  → WireValue
    DECK_RESULT_ERR → WireValue
    DECK_OPT_SOME   → WireValue
    DECK_OPT_NONE   → 0 bytes
    DECK_TIMESTAMP  → 8 bytes LE i64
    DECK_DURATION   → 8 bytes LE i64
```

A function value (`DECK_FN`) **cannot** appear in a snapshot. Closures captured in machine state are an error at snapshot time (logged as `runtime.snapshot_closure`); the snapshot is aborted and the VM is terminated rather than suspended. App authors should not store closures in machine state; the spec rejects this at type-check time when possible (functions of effectful types in record fields).

### 16.3 Continuations in snapshots

In-flight effect calls present a hazard: at restore time, the bridge that issued the call no longer has the request. The runtime handles this with a per-method **restore policy**:

| Policy | Meaning |
|---|---|
| `replay` | Reissue the effect call on restore with the original args |
| `cancel` | Drop the continuation; the awaiting frame receives `:err :cancelled` |
| `fail` | Drop the continuation; the awaiting frame receives `:err :suspended` |

The default is `cancel`. Capability methods that are idempotent (HTTP GET, sensor reads) may declare `@on_restore replay`; methods with side effects should never declare `replay`.

The continuation section serializes each pending call as:

```
(request_id, cap_id, method_id, restore_policy, marshaled_args)
```

`marshaled_args` is a list of `WireValue`s. On restore (`replay`), the runtime re-issues the call. On `cancel` or `fail`, the awaiting frame is restored with the appropriate `:err` value pre-pushed onto its value stack.

### 16.4 Opaque handles in snapshots

Opaque handles are bridge-owned references whose validity is unknown across snapshots. The bridge declares per opaque type a restore policy:

| Policy | Meaning |
|---|---|
| `transient` | The handle is not snapshotted; the VM holds `:none` after restore. App must check. |
| `serialized` | The bridge provides `serialize(handle)` and `restore(bytes)` callbacks. The handle round-trips. |
| `recreate` | The bridge re-acquires the resource at restore time using a key stored alongside the handle (e.g., a file path). |

Default is `transient`. Bridge implementers must opt into `serialized` or `recreate` per opaque type via `deck_register_opaque_type`'s extended struct (specified in `06-deck-native §8.3`).

### 16.5 Snapshot size budgets

Per the implementation, snapshots are bounded:

- `DECK_SNAPSHOT_MAX_BYTES` (default 256 KB) — hard cap
- `DECK_SNAPSHOT_SOFT_BYTES` (default 64 KB) — emits `runtime.snapshot_large` warning

If snapshot exceeds the hard cap, the suspend operation fails. The bridge then has the choice to terminate the app instead (which is what `09-deck-shell` recommends).

The compression bit in the header indicates the payload is LZ4-compressed (frame format, no checksum since the header carries CRC32 over compressed bytes). LZ4 is mandatory for portability; future versions may negotiate alternatives via `format_version`.

### 16.6 Restore protocol

Restore is the inverse of snapshot:

1. Verify magic, format_version, CRC32. Failure → restore aborts; the VM is terminated and the app is launched fresh (this is the worst-case fallback the spec allows).
2. Allocate a fresh VM with the same allocator vtable.
3. Decompress payload into a working buffer.
4. Walk sections in order; rebuild atom table, type table, machine states, flow history, config, streams, tasks, continuations, opaque handles.
5. Resubscribe streams (the bridge is informed of all existing subscriptions). The bridge may immediately push the latest values; these are buffered into the work queue but not dispatched until step 7.
6. Run any `@on resume` hook the app declares.
7. Drain the work queue.

The VM is now indistinguishable from the pre-suspend state (modulo continuations subject to `cancel`/`fail` policies).

---

## 17. Crash Isolation

### 17.1 Threat model

The runtime must survive any of the following without corrupting other VMs or the bridge:

- An app calls a builtin with arguments the bridge mishandles
- A bridge callback returns NULL where a value is expected
- A native capability segfaults (out of the runtime's control, but should be containable)
- A pure Deck panic (stack overflow, OOM, divide-by-zero, internal invariant violation)

### 17.2 Isolation primitive: setjmp/longjmp barrier

The runtime installs a `setjmp` target at the entry of every `deck_runtime_dispatch` call. Any `longjmp(rt->panic_buf, kind)` from within evaluator code unwinds back to the dispatch caller, which:

1. Resets the value stack and frame stack to empty.
2. Releases all values currently allocated to the call arena.
3. Reports the panic to the bridge via `MSG_PANIC` (the bridge typically translates this to `system.shell.report_crash`).
4. Marks the VM as **terminated**. Subsequent `deck_runtime_dispatch` calls return `:err :terminated` immediately.

This handles pure Deck panics. Bridge-side segfaults are **not** containable by the runtime — they are the host's responsibility (e.g., `13-deck-cyberdeck-platform §21` describes the ESP-IDF panic handler integration).

### 17.3 Protection of other VMs

VMs are isolated by their own runtime threads (§2.1). A panic on VM A's thread does not touch VM B. The bridge is responsible for ensuring that one VM's crash does not corrupt shared bridge state — for example, a network request in flight when VM A panics must be cancelled cleanly via the cap's `cancel_fn`.

Bridge callbacks that mutate shared state (e.g., a global cache) **must** be re-entrant safe. The runtime provides no help for this; it is documented as a bridge implementer responsibility (`06-deck-native §13`).

### 17.4 Crash payload

`MSG_PANIC`'s payload:

```
PanicInfo {
  kind:     PanicKind     # see §17.5
  message:  StrRef        # human-readable, max 256 chars
  stack:    StrRef        # synthetic backtrace, max 16 frames
  span:     SourceSpan    # AST node where the panic originated
  vm_state: VmStateBlob   # opaque; for the bridge's crash log
}
```

`vm_state` is a small (~512 bytes) blob the bridge can persist for diagnostics — it includes the active machine's name and state, the last 4 messages dispatched, and recent allocator stats.

### 17.5 Panic kinds

```
PANIC_STACK_OVERFLOW        — call depth exceeded DECK_FRAME_DEPTH_MAX
PANIC_OOM                   — allocation failed and no recovery path
PANIC_DIVIDE_BY_ZERO        — int / 0 or int % 0; floats produce NaN/inf, not panic
PANIC_PATTERN_MATCH_FAIL    — match expression with no exhaustiveness fallback hit a value
                              the loader could not statically reject
PANIC_INVARIANT             — internal invariant violation (refcount went negative,
                              evaluator visited a malformed AST node, etc.)
PANIC_BRIDGE_CONTRACT       — a bridge call violated its documented contract
                              (returned the wrong type, freed a borrowed value, etc.)
PANIC_USER                  — the program called deck_panic("...") (an explicit panic builtin)
```

`PANIC_USER` exists as a debugging aid (`assert(cond)` desugars to `if not cond → deck_panic("...")`). It does not appear in production code; the loader emits a load warning if `deck_panic` is called outside a `@test` block.

### 17.6 Panic inside `@on` hooks

A panic inside an `@on` hook follows the same protocol; the OS event that triggered the hook is logged as undelivered. The VM terminates. This is intentional — there is no correct way to "skip" the failed hook and resume because the hook may have done partial work that left machine state inconsistent.

Apps that need fault-tolerant event handling must wrap their hook bodies in `match` against `Result` types and avoid panicking explicitly.

### 17.7 OOM-vs-panic distinction

OOM during an allocation that returns `:err :oom` (e.g. a `text.format` call that produces a string above the heap soft limit) is **not** a panic — the call returns the error value. OOM during an allocation that has no error path (e.g. allocating a closure environment, allocating a frame on call) **is** a panic. The runtime tries to pre-validate the latter cases against the heap's headroom but cannot guarantee no-OOM in deeply-recursive scenarios.

---

## 18. DeckViewContent (DVC) Wire Format

### 18.1 What DVC is

`DeckViewContent` is the on-the-wire tree the runtime emits to the bridge each render. It is a flat-structured tree of typed nodes. The bridge interprets each node according to `10-deck-bridge-ui §4` (semantic catalog) and produces native widgets.

### 18.2 In-memory layout (in the render arena)

```
DvcNode {
  type:          u16             # DvcType (§18.3)
  flags:         u16             # node-type-specific bitfield
  child_count:   u16
  attr_count:    u16
  intent_id:     u32             # 0 = no intent
  attrs:         DvcAttr[attr_count]
  children:      DvcNode*[child_count]
}

DvcAttr {
  key:    AtomId
  type:   u8                     # subset of DeckType (no FN, STREAM, OPAQUE)
  value:  WireValue              # see §16.2
}
```

All allocations come from `vm->render_arena`. The arena is reset at the start of each render. The bridge must consume / copy the tree before the next render; the runtime does not retain anything across renders.

### 18.3 Node type catalog

Defined in `10-deck-bridge-ui §4`. Numeric values for the wire format:

```
DVC_EMPTY          = 0
DVC_FLOW           = 1     // wraps machine context; carries machine_id + state in attrs
DVC_GROUP          = 2     // bordered card with optional title attr
DVC_COLUMN         = 3     // vertical stack
DVC_ROW            = 4     // horizontal stack
DVC_GRID           = 5
DVC_LIST           = 6
DVC_LIST_ITEM      = 7
DVC_DATA_ROW       = 8     // dim label + primary value pair
DVC_TEXT           = 9     // text input
DVC_PASSWORD       = 10
DVC_SWITCH         = 11
DVC_SLIDER         = 12
DVC_CHOICE         = 13
DVC_TRIGGER        = 14    // button
DVC_NAVIGATE       = 15
DVC_TOGGLE         = 16
DVC_DATE_PICKER    = 17
DVC_PIN            = 18
DVC_LABEL          = 19    // static text
DVC_RICH_TEXT      = 20    // markdown / styled text
DVC_MEDIA          = 21    // image, audio, video
DVC_PROGRESS       = 22
DVC_LOADING        = 23
DVC_TOAST          = 24    // request a toast (handled by bridge UI service)
DVC_CONFIRM        = 25
DVC_SHARE          = 26
DVC_CHART          = 27
DVC_SPACER         = 28
DVC_DIVIDER        = 29
DVC_FORM           = 30
DVC_MARKDOWN       = 31    // markdown component (02-deck-app §12.3); first-class
DVC_MARKDOWN_EDITOR = 32   // markdown_editor component; first-class
DVC_CUSTOM         = 33    // type_id in attrs identifies a registered custom node
```

`DVC_MARKDOWN` and `DVC_MARKDOWN_EDITOR` are core types because the markdown surface (types, builtin, capability, view nodes) is part of the standard library; see `02-deck-app §12.3`, `03-deck-os §3` (`@builtin md`), and `03-deck-os §4.4` (`@capability markdown`).

New node types beyond the catalog may be added by registered modules; the wire format reserves type ids `≥ 128` for third-party module-registered types. The 32–127 range is reserved for future first-party additions.

### 18.4 Standard attribute names

Attribute keys are always atoms. Common ones:

| Atom | Meaning | Used by |
|---|---|---|
| `:title` | Header text | DVC_GROUP, DVC_GRID, DVC_LIST |
| `:label` | Inline label | DVC_TEXT, DVC_TRIGGER, DVC_TOGGLE |
| `:value` | Current value | DVC_TEXT, DVC_SLIDER, DVC_CHOICE, DVC_DATE_PICKER, DVC_PIN |
| `:placeholder` | Hint when empty | DVC_TEXT |
| `:options` | List of selectable values | DVC_CHOICE |
| `:min`, `:max` | Range bounds | DVC_SLIDER |
| `:step` | Increment | DVC_SLIDER |
| `:cols` | Column count | DVC_GRID |
| `:rows` | Row count or height | DVC_GRID, DVC_LIST |
| `:variant` | Style variant (e.g. `:primary`, `:danger`, `:dim`) | DVC_TRIGGER, DVC_LABEL |
| `:icon` | Icon name | DVC_TRIGGER, DVC_NAVIGATE |
| `:disabled` | Bool; disables interaction | All interactive nodes |
| `:length` | PIN length | DVC_PIN |
| `:cancellable` | Bool | DVC_PROGRESS, DVC_CONFIRM |
| `:prompt` | Question posed to the user | DVC_CONFIRM |
| `:reason` | Why the error exists | DVC_ERROR |
| `:confirm_label`, `:cancel_label` | Button labels | DVC_CONFIRM |
| `:src` | Asset ref or URL | DVC_MEDIA |
| `:alt` | Fallback text | DVC_MEDIA |
| `:role` | Semantic role of media (`:avatar`, `:cover`, `:thumbnail`, `:inline`) | DVC_MEDIA |
| `:has_more` | Whether more items exist beyond the current page | DVC_LIST |
| `:max_height`, `:max_width` | Layout bounds | DVC_RICH_TEXT, DVC_MEDIA |
| `:purpose` | `:reading`, `:reference`, `:fragment` | DVC_MARKDOWN |
| `:focus` | Heading id the user should be focused on (reactive) | DVC_MARKDOWN |
| `:placeholder` | Empty-state hint | DVC_TEXT, DVC_MARKDOWN_EDITOR |
| `:controlled_by` | `MdEditorState` for external programmatic control | DVC_MARKDOWN_EDITOR |
| `:describe` | Accessible description of the region | DVC_MARKDOWN, DVC_MARKDOWN_EDITOR (and most others) |

DVC_MARKDOWN and DVC_MARKDOWN_EDITOR carry **only semantic intent**. The bridge decides every presentation concern (density, max-width, ToC visibility, code-block affordances, image sizing, editor toolbar layout, preview placement, line numbers, virtual rendering) from the declared `:purpose`, the document content, and the device. See `02-deck-app §12.3` for the inference table and `10-deck-bridge-ui §4.2` for this board's concrete decisions.

Bridge implementations that encounter unknown attribute keys must **silently ignore** them (forward compatibility). They may log a debug message.

### 18.5 Intents

An interactive node may carry an `intent_id` (`u32`, nonzero). When the bridge fires the intent (e.g. user taps a `DVC_TRIGGER`), it calls:

```
deck_intent_fire(rt, intent_id, value: DeckValue*)
```

`value` is the carried payload — a string for `DVC_TEXT`, a bool for `DVC_TOGGLE`, an option atom for `DVC_CHOICE`, etc. `value` may be NULL for buttons with no payload.

The runtime resolves `intent_id → handler` via a per-render-pass intent table also stored in the render arena. After the next render, the table is reset; intents from a stale render fire as no-ops.

### 18.6 Render request from the bridge

The bridge polls or is notified that the runtime has a new render. The current trigger model is **runtime-pushes-on-dirty**: the runtime emits `MSG_RENDER` to the bridge mailbox when its `render_dirty` flag is set. The bridge consumes the tree and acks via `deck_render_ack(rt)`.

The runtime emits at most one outstanding `MSG_RENDER` per VM at a time. New renders coalesce until the bridge acks. This rate-limits the runtime under burst load.

### 18.7 Wire encoding (when crossing process / network boundaries)

For embedded targets the in-memory layout is the wire format — the bridge consumes pointers directly. For implementations where the bridge runs in a separate process or over IPC, the runtime emits a flattened encoding:

```
WireDvc:
  magic:    u16        # 0xDC0E
  version:  u8         # 1
  flags:    u8
  intent_table_count: u16
  intent_table:       [(intent_id, WireValue)]   # registered intents this render
  root_offset:        u32                        # bytes from start to root node
  nodes:              packed DvcNode-flat...
```

Each `DvcNode-flat` is `(type, flags, child_count, attr_count, intent_id)` followed by its attrs (each as `(key_atom, type, WireValue)`) followed by child nodes inline (depth-first). The format is self-describing and forward-compatible (unknown node types and attrs ignored).

---

## 19. Runtime ↔ Bridge IPC

### 19.1 Mailbox

Each VM has one **mailbox**: a single multi-producer-single-consumer queue. Producers are bridge threads; the consumer is the runtime thread. The mailbox is a fixed-size ring buffer of `MailMsg`.

`MAILBOX_DEPTH` defaults to 64 entries. Overflow behavior:

- For low-priority messages (`MSG_OS_EVENT` of type background fetch, stream values from sources marked `lossy: true`): drop the new message and increment `rt->stats.mailbox_drops`.
- For high-priority messages (`MSG_OS_EVENT` of type lifecycle, panics, render acks, intent fires): block the producer for up to `MAILBOX_BLOCK_MS` (default 100 ms), then drop.

The bridge can read `rt->stats.mailbox_high_water` to size the depth appropriately for its workload.

### 19.2 Message types

```
MSG_OS_EVENT          { event_atom, payload: DeckValue* }
MSG_INTENT_FIRE       { intent_id, value: DeckValue* }
MSG_EFFECT_RESULT     { request_id, result: DeckValue* }
MSG_STREAM_VALUE      { subscription_id, value: DeckValue* }
MSG_STREAM_END        { subscription_id }
MSG_STREAM_ERR        { subscription_id, err: DeckValue* }
MSG_TASK_RUN          { task_id }                         // internal — produced by scheduler
MSG_RENDER_ACK        {}
MSG_PERMISSION_RESULT { request_id, grant: enum {grant, deny} }
MSG_SUSPEND           {}                                  // bridge requests suspend
MSG_RESUME            {}                                  // bridge requests resume after suspend
MSG_TERMINATE         { reason: enum {user, system, oom, crash, evicted} }
MSG_DEEP_LINK         { url: str, params: {str: str} }    // routes to @on open_url
MSG_CRASH_REPORT      { info: CrashInfo }                 // sent only to system.crash_reporter
```

The runtime always processes one message at a time. Internal work (cascade fan-out, stream pumps) happens between messages.

### 19.3 Outbound from runtime to bridge

The runtime sends to the bridge via:

| API | Purpose |
|---|---|
| `bridge.render(tree)` | Push a new content tree (returns immediately; bridge ACKs via `MSG_RENDER_ACK`) |
| `bridge.intent_register(intent_id, handler_id)` | (Embedded inside `bridge.render` payload; not a separate call) |
| `bridge.cap_call(cap_id, method_id, args)` | Synchronous call; returns `DeckValue*` |
| `bridge.cap_call_async(cap_id, method_id, args, request_id)` | Async; result arrives via `MSG_EFFECT_RESULT` |
| `bridge.permission_request(cap_path, reason, request_id)` | Blocking or async (bridge decides); result via `MSG_PERMISSION_RESULT` |
| `bridge.event_emit(event_name, payload)` | Custom OS events fired from native code (see `06-deck-native §9`) |
| `bridge.crash_report(info)` | Notify the bridge a panic occurred |
| `bridge.log(level, message)` | Emit a log line; bridge routes (typically to platform logger) |
| `bridge.snapshot_persist(blob, length)` | Hand a snapshot blob to the bridge for storage |
| `bridge.snapshot_restore() → blob` | Request the saved blob at restore time |

Each call has a documented thread context (typically: must be called from runtime thread). Violations are bridge-side bugs.

### 19.4 Backpressure summary

The full backpressure picture, by hop:

| Direction | Hop | Mechanism |
|---|---|---|
| Bridge → runtime | Mailbox enqueue | `MAILBOX_DEPTH` ring; high-priority blocks up to `MAILBOX_BLOCK_MS`, low-priority drops |
| Runtime → bridge | `bridge.render` | Coalesce: at most 1 in-flight render per VM |
| Stream source → operator | Producer push | Per-stream ring buffer (§13.5), per-stream policy |
| Operator → operator | Internal pump | Synchronous within scheduler tick; no buffer needed |
| Operator → sink (machine) | `MSG_STREAM_VALUE` enqueue | Mailbox path (low-priority) |

The runtime never blocks the bridge thread for more than `MAILBOX_BLOCK_MS`. The bridge never blocks the runtime thread.

### 19.5 Cross-VM communication

VMs do not share a mailbox. Cross-VM communication (e.g. `system.apps.bring_to_front` from the launcher targeting another app) goes through the **bridge** as the broker:

1. Launcher VM calls `system.apps.bring_to_front("social.bsky.app")`.
2. The bridge translates this to OS-level activity stack manipulation. The target VM, if suspended, is restored. If terminated, it is launched.
3. The OS may emit `os.app_launched` or `os.app_suspended` to apps subscribed via `system.apps`.

There is no Deck-level IPC primitive. All inter-app communication is mediated by the OS through deep links (URLs) and observable OS state.

---

## 20. Capability Registration Timing and Scope

### 20.1 When the bridge registers capabilities

Capabilities are registered with the runtime **before** the loader runs:

```
order at boot:
  1. deck_runtime_create(config)        // empty runtime; no caps yet
  2. for each capability: deck_register_capability(rt, cap_path, method_table)
  3. for each builtin module: deck_register_builtin(rt, module_name, methods)
  4. for each opaque type: deck_register_opaque_type(rt, type_def)
  5. for each event: deck_register_event(rt, event_name, payload_signature)
  6. deck_runtime_load(rt, app_dir)     // loader runs; verifies caps against .deck-os
  7. deck_runtime_start(rt)             // first @on launch fires
```

After `deck_runtime_load`, no new capabilities may be registered. Attempting it returns `:err :runtime_locked`.

### 20.2 Validation against `.deck-os`

The loader reads the platform `.deck-os` file (§21) and walks the capability table. For each declared capability:

- It must be registered (or the loader emits `E_CAPABILITY_NOT_IMPLEMENTED`).
- The registered method signatures must match the `.deck-os` declaration (loader emits `E_CAPABILITY_SIGNATURE_MISMATCH` on conflict).

This means the bridge's set of registered capabilities is a **superset** of any single app's `@use` declarations. The `.deck-os` file is the contract.

### 20.3 Custom builtins and types

Third-party modules register at the same boot phase as core capabilities. Their `.deck-os` declarations live in `@include`d files (per `03-deck-os §2.1`) and are merged into the platform's surface at load time. The standard library (markdown included — `@builtin md` and `@capability markdown` are core, see `03-deck-os §3` and §4.4) registers via the same path; the only difference between standard and third-party is that standard registrations are part of the canonical surface and counted toward the surface API level.

Conflicts between modules registering the same capability path are a load error — the bridge implementer must resolve at integration time.

---

## 21. The `.deck-os` Surface File

### 21.1 Format

`.deck-os` is plain UTF-8 text. Its grammar is a strict subset of Deck syntax restricted to **declarations only**. No expressions. No `let`. No function bodies. Comments use `--` like Deck.

The grammar (EBNF, partial):

```
file        = { include | top_decl } ;
include     = "@include" string_literal NEWLINE ;
top_decl    = builtin_decl | capability_decl | event_decl | type_decl
            | opaque_decl | const_decl ;
builtin_decl    = "@builtin" identifier INDENT { method_sig } DEDENT ;
capability_decl = "@capability" path INDENT { method_sig | requires_perm | errors_block } DEDENT ;
method_sig      = identifier "(" [ param_list ] ")" "->" type_expr [ method_modifiers ] ;
param_list      = param ("," param)* ;
param           = identifier ":" type_expr ;
method_modifiers = ( "@pure" | "@stream" | "@singleton" | "@impure" )+ ;
event_decl      = "@event" path "(" [ param_list ] ")" ;
type_decl       = "@type" identifier ( union_def | record_def ) ;
opaque_decl     = "@opaque" identifier ;
const_decl      = "@const" identifier ":" type_expr "=" literal ;
requires_perm   = "@requires_permission" ;
errors_block    = "@errors" INDENT { atom string_literal } DEDENT ;
```

### 21.2 Loading the surface file

The host (typically the bridge's startup code) loads `.deck-os` once at boot:

```
1. Read the file (or its embedded representation; see §21.3).
2. Lex and parse it — same lexer/parser as the language; the grammar above is a subset.
3. Walk all `@include` directives recursively (paths relative to the including file). Cycles are an error.
4. Build the OS surface table: capabilities, events, builtins, types, opaque types, constants.
5. Hand the table to `deck_runtime_load_surface(rt, table)`.
```

The runtime then uses the table for:

- Validating app `@use` declarations (capability paths must exist)
- Validating app `@on` hooks (event names must exist)
- Validating app expressions (every effect call must reference a declared method)
- Marshaling effect calls (signatures come from the surface)

### 21.3 Where `.deck-os` lives

Two options:

- **Embedded**: the `.deck-os` text is compiled into the firmware as a string blob. The host reads the blob at boot. This is what `13-deck-cyberdeck-platform` specifies for the ESP32-S3 build.
- **On filesystem**: the `.deck-os` lives at a fixed path on the host filesystem. The host reads it at boot. This is appropriate for desktop/dev runtimes.

Either way, the surface is fixed for the runtime's lifetime. There is no dynamic surface manipulation.

### 21.4 Versioning

`@const deck_os_version: int = 1` (or higher) at the top of `.deck-os` declares the surface version. The runtime reports its minimum compatible version; loading a surface older than that is an error.

---

## 22. Startup and Shutdown

### 22.1 Cold start

```
1. Host process starts (e.g., main() on desktop, app_main() on ESP-IDF).
2. Host initializes its services (display, network stack, storage, etc.).
3. Host loads .deck-os surface (§21.2).
4. Host calls deck_runtime_create(config) — empty runtime.
5. Host registers capabilities, builtins, opaques, events (§20.1).
6. Host calls deck_runtime_load_surface(rt, table).
7. Host identifies which app to launch first (e.g., the launcher).
8. Host calls deck_runtime_load(rt, app_dir) — runs the 12 loader stages.
9. On success, host calls deck_runtime_start(rt) — fires @on launch on the runtime thread.
10. The runtime thread enters its main loop (§2.3).
```

`deck_runtime_create` allocates the runtime struct, the empty mailbox, the empty heap, and seeds `rt->random` from the platform RNG (the host supplies an `rng_seed_fn` in the config).

### 22.2 Warm start (snapshot restore)

```
1. Host calls deck_runtime_create(config).
2. Host registers capabilities, etc. (same as cold).
3. Host calls deck_runtime_restore(rt, snapshot_blob, blob_len) instead of load+start.
4. The runtime walks the snapshot (§16.6) and restores VM state.
5. Runtime fires @on resume.
6. Main loop resumes.
```

### 22.3 Suspend

```
1. Bridge sends MSG_SUSPEND.
2. Runtime fires @on suspend (with a 500 ms wall-clock deadline).
3. Runtime serializes a snapshot (§16.2).
4. Runtime calls bridge.snapshot_persist(blob, len).
5. Runtime cancels all in-flight effect continuations per their restore policies (§16.3).
6. Runtime unsubscribes all streams (sources are stopped; the bridge restarts them on resume).
7. Runtime enters suspended state. The runtime thread sleeps; the mailbox is drained but messages other than MSG_RESUME and MSG_TERMINATE are dropped.
```

If `@on suspend` exceeds the 500 ms deadline, the runtime aborts the hook (via the panic mechanism) and proceeds to snapshot. The hook's effects are not rolled back.

### 22.4 Resume

```
1. Bridge sends MSG_RESUME.
2. Runtime resubscribes streams.
3. Runtime fires @on resume.
4. Main loop continues.
```

### 22.5 Terminate

```
1. Bridge sends MSG_TERMINATE { reason }.
2. Runtime fires @on terminate (with a 500 ms deadline like suspend).
3. Runtime cancels all in-flight continuations with :err :terminated.
4. Runtime unsubscribes all streams.
5. Runtime releases all heap-allocated values (the heap is destroyed wholesale).
6. Runtime thread exits.
7. Host calls deck_runtime_destroy(rt) to free the runtime struct itself.
```

`@on terminate` is best-effort. If the reason is `crash`, the hook may not run at all (the runtime is already in a degraded state). The bridge should not depend on `@on terminate` for critical persistence; that is what `@on suspend` is for.

### 22.6 Hot reload (development only)

A development-mode runtime accepts `deck_runtime_reload(rt, app_dir)` while running. This:

1. Re-runs the loader against the new sources.
2. On success, swaps the load image atomically. Existing machine state is **preserved**: states whose declarations are unchanged keep their current value; states whose declarations changed are reset to their `initial`.
3. Restarts streams whose sources changed signature.
4. Fires a synthetic `os.dev_reload` event so apps can react.

Hot reload is not a production feature. Production runtimes return `:err :hot_reload_disabled` from `deck_runtime_reload`.

---

## 23. Performance Budgets

These are runtime targets, not strict guarantees. The platform doc (`13-deck-cyberdeck-platform`) refines them with measured numbers for the ESP32-S3.

| Operation | Target | Notes |
|---|---|---|
| Cold load (`deck_runtime_load`) for a 200-line app | < 200 ms | Includes parse, type check, effect check |
| Render tree assembly | < 5 ms / render | Bounded by the depth of the active flow graph |
| Effect dispatch (sync) | < 100 µs overhead | Above the bridge call's own cost |
| Effect dispatch (async) | < 200 µs overhead | Adds continuation table insertion |
| Pattern match (decision tree walk) | O(depth) | Typical: 1–5 µs for arms with ≤ 8 cases |
| Stream value pump | < 50 µs / hop | Per operator in the chain |
| Suspend (snapshot) for 64 KB heap | < 50 ms | Compression dominates |
| Restore (deserialize) for 64 KB blob | < 30 ms | No compression cost on small blobs |
| Mailbox enqueue (bridge thread) | < 5 µs | Lock-free path |
| Reactive cascade (10 fan-out conditions) | < 1 ms | Worst case at depth 16 cap |

The runtime exposes per-VM telemetry counters (`rt->stats`) for each of these so the host can verify in production:

```
runtime.stats.dispatch_us_p99
runtime.stats.render_us_p99
runtime.stats.snapshot_bytes_p99
runtime.stats.mailbox_high_water
runtime.stats.intern_full
runtime.stats.task_overruns
runtime.stats.reactive_cascade_max_depth
```

---

## 24. Conformance and Testing

### 24.1 Conformance tests

A reference test suite (not yet written; tracked separately) covers:

- Lexer: every token kind and edge case (deep nesting, all escapes, indent recovery)
- Parser: every grammar production with both happy-path and error-recovery inputs
- Loader: all 12 stages with passing and failing inputs
- Evaluator: every expression form, including TCO depth tests
- Effect dispatcher: sync, async, singleton queue, optional capability short-circuit
- Scheduler: task firing, condition tracking, stream operator behaviors
- Snapshot/restore: round-trip equality for every value type, every restore policy
- Crash isolation: each panic kind triggers the proper `MSG_PANIC` payload

A runtime is **conformant** when it passes the suite end-to-end. Bridge implementers may run the suite against a mock bridge included with the runtime.

### 24.2 Mock bridge

The runtime ships a minimal mock bridge (header `<deck/mock_bridge.h>`) that:

- Implements every capability with deterministic stubs (`network.http.get` returns a canned response, etc.)
- Records every outbound bridge call into an in-memory log
- Allows tests to inject inbound messages programmatically

The mock bridge is the substrate for `@test` blocks (`02-deck-app §16`). Tests that need a specific capability behavior override the mock by passing a `MockOverride { cap_path, method, fn }` table to the test harness.

---

## 25. Open Decisions and Out-of-Scope Items

The following are intentionally deferred or out of scope for v1:

- **Bytecode VM**: tree-walking only. A bytecode mode is a future option.
- **Multi-VM-per-thread scheduling**: each VM is one thread. Co-routine multiplexing is not part of the spec.
- **Foreign function interface beyond `06-deck-native`**: no dynamic loading of `.so`/`.dll` files. All capabilities are statically linked into the bridge.
- **Sandboxed execution of untrusted Deck code**: the language is safe-by-construction (no raw pointers, no `unsafe`), but the runtime does not implement resource quotas beyond `heap_limit_bytes` and `DECK_FRAME_DEPTH_MAX`. Untrusted apps require platform-level isolation (separate processes / VMs).
- **Distributed snapshots / cross-device migration**: snapshots are local. The format would need `format_version` bumps and explicit endianness specification; today the format is little-endian only.
- **Incremental garbage collection**: refcount only. If a future Deck program produces cycles (which the language currently forbids), a tracing GC would need to be added.
- **WebAssembly target**: the runtime is C99-portable but no WASM packaging is provided. A future port would require a JavaScript bridge implementing `06-deck-native`.
