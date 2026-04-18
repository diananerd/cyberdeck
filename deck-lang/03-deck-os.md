# Deck OS Interface Specification
**Version 2.0 — OS Surface, Capabilities, Bridge Protocol, and Security**

---

## 1. Architecture

```
┌────────────────────────────────────────────────────┐
│  Deck App (.deck files)                            │
│  Declares @use aliases; calls alias.method()       │
├────────────────────────────────────────────────────┤
│  Deck Interpreter                                  │
│  Reads .deck-os; verifies @use; routes effects;   │
│  enforces sandboxing; manages streams/subscriptions│
├────────────────────────────────────────────────────┤
│  OS Bridge  (platform C/C++ — only platform code)  │
│  Maps interpreter requests to actual OS/HW calls   │
├────────────────────────────────────────────────────┤
│  Embedded OS + Hardware (monolithic, compiled)     │
│  Drivers, services, display, storage, radios       │
└────────────────────────────────────────────────────┘
```

The bridge is the **only** platform-specific code. The interpreter and all `.deck` files are identical across platforms.

> **Implementation references**: the abstract bridge protocol described here is implemented per-platform. For the interpreter side (capability registration timing, effect dispatch algorithm, async/sync semantics, IPC mailbox) see `11-deck-implementation.md §12, §19, §20`. For the formal hardware-agnostic Service Driver Interface (SDI) every platform implements, see `12-deck-service-drivers.md`. For the ESP32-S3 reference implementation mapping every capability listed in §4 to a concrete `cap_*` C module backed by ESP-IDF v6.0 components, see `13-deck-cyberdeck-platform.md §14.3`.

---

## 2. The .deck-os File

Written once by the OS/board author. Compiled into the OS image or placed at a known path. Read by the interpreter at startup before loading any app. Absence or malformation is a fatal interpreter error.

**Search order:**
1. `$DECK_OS_SURFACE` environment variable path
2. `/etc/deck/system.deck-os`
3. Compiled-in default (for minimal builds)

### 2.1 Top-Level Structure

```
@os
  name:    str
  version: str
  arch:    atom   -- :arm32 | :arm64 | :riscv32 | :riscv64 | :x86 | :x64

@const deck_os_version: int = N
  -- The Surface API Level. Strictly increasing integer. Bumped on every
  -- forwards-compatible expansion of the surface (new capability, new method,
  -- new event, new atom variant). See 15-deck-versioning §5 for policy.
  -- Apps target a range via `@requires deck_os: ">= N"` in app.deck.

@const deck_editions_supported: [int] = [2026]
  -- The set of language editions this surface is paired with. The runtime
  -- declares its own editions_supported in system.info.versions(); the
  -- intersection is what apps can target. See 15-deck-versioning §4.

@include "relative/path/to/other.deck-os"
  -- Inserts all declarations from the referenced file as if they were written
  -- inline at this position. Path is relative to the including file.
  -- Circular includes are a fatal parse error.

@builtin module_name
  fn_name (param: Type) -> ReturnType
  ...

@capability capability.path
  @version 1                       -- Capability surface version. Increments on
                                   -- breaking changes to this capability's contract
                                   -- (see 15-deck-versioning §11).
  @flag :flag_name                 -- Optional features. Drivers report which they
  @flag :other_flag                -- support via capability_flags; apps probe at
                                   -- runtime. See 15-deck-versioning §13.
  method_name (params...) -> ReturnType
  method_name (params...) -> Stream Type
  @errors
    :atom  "description"
  @requires_permission             -- if present, must appear in app's @permissions
  @deprecated since: N replacement: "other.capability"
                                   -- Marks the capability as deprecated. The loader
                                   -- emits a :deprecated_capability warning. See
                                   -- 15-deck-versioning §10.

@event event_name
@event event_name (field: Type, ...)

-- Record type with named fields (same as in .deck files):
@type TypeName
  field : Type
  ...

-- Union-of-atoms alias (ONLY valid in .deck-os, not in .deck source files):
-- Documents a constrained set of valid atoms for a parameter or return value.
@type TypeName = :atom1 | :atom2 | :atom3

-- Opaque handle type (connection/session handles managed by the bridge):
-- The runtime holds the value as DECK_OPAQUE; the bridge owns the underlying pointer.
-- Register with deck_register_opaque_type() for better error messages.
@opaque TypeName
```

---

## 3. Builtins

Always in scope. No `@use`. No `!effect`. Implemented by the interpreter, identical on all platforms.

```
@builtin math
  abs        (x: float)               -> float
  round      (x: float, n: int)       -> float
  floor      (x: float)               -> float
  ceil       (x: float)               -> float
  sqrt       (x: float)               -> float
  pow        (base: float, exp: float) -> float
  min        (a: float, b: float)     -> float
  max        (a: float, b: float)     -> float
  clamp      (x: float, lo: float, hi: float) -> float
  lerp       (a: float, b: float, t: float)   -> float
  sin        (x: float)               -> float
  cos        (x: float)               -> float
  tan        (x: float)               -> float
  asin       (x: float)               -> float
  acos       (x: float)               -> float
  atan       (x: float)               -> float
  atan2      (y: float, x: float)     -> float
  exp        (x: float)               -> float
  ln         (x: float)               -> float
  log2       (x: float)               -> float
  log10      (x: float)               -> float
  sign       (x: float)               -> float
  is_nan     (x: float)               -> bool
  is_inf     (x: float)               -> bool
  abs_int    (x: int)                 -> int
  min_int    (a: int, b: int)         -> int
  max_int    (a: int, b: int)         -> int
  clamp_int  (x: int, lo: int, hi: int) -> int
  gcd        (a: int, b: int)         -> int
  lcm        (a: int, b: int)         -> int
  to_radians (deg: float)             -> float
  to_degrees (rad: float)             -> float
  pi  : float
  e   : float
  tau : float

@builtin text
  split      (s: str, sep: str)              -> [str]
  join       (parts: [str], sep: str)        -> str
  trim       (s: str)                        -> str
  upper      (s: str)                        -> str
  lower      (s: str)                        -> str
  length     (s: str)                        -> int
  contains   (s: str, sub: str)             -> bool
  starts     (s: str, prefix: str)          -> bool
  ends       (s: str, suffix: str)          -> bool
  replace    (s: str, from: str, to: str)   -> str
  replace_all(s: str, from: str, to: str)   -> str
  slice      (s: str, start: int, end: int) -> str
  index_of   (s: str, sub: str)             -> int?
  pad_left   (s: str, n: int, ch: str)      -> str
  pad_right  (s: str, n: int, ch: str)      -> str
  pad_center (s: str, n: int, ch: str)      -> str
  truncate   (s: str, n: int)               -> str
  truncate   (s: str, n: int, suffix: str)  -> str
  is_empty   (s: str)                       -> bool
  is_blank   (s: str)                       -> bool
  lines      (s: str)                       -> [str]
  words      (s: str)                       -> [str]
  count      (s: str, sub: str)             -> int
  repeat     (s: str, n: int)               -> str
  format     (tmpl: str, args: {str: any})  -> str
  base64_encode (s: str)                    -> str
  base64_decode (s: str)                    -> str?
  url_encode    (s: str)                    -> str
  url_decode    (s: str)                    -> str
  hex_encode    (b: [byte])                 -> str
  hex_decode    (s: str)                    -> [byte]?
  query_build   (params: {str: str})        -> str
  query_parse   (s: str)                    -> {str: str}
  json       (v: any)                       -> str
  from_json  (s: str)                       -> any?
  bytes      (s: str)                       -> [byte]
  from_bytes (b: [byte])                    -> str?
  -- NOTE: str(), int(), float(), bool() are global builtins (01-deck-lang §11.1),
  -- not members of the text module. Do not call them as text.str(), text.int(), etc.

@builtin time
  now           ()                              -> Timestamp
  since         (t: Timestamp)                 -> Duration
  until         (t: Timestamp)                 -> Duration
  format        (t: Timestamp, fmt: str)       -> str
  parse         (s: str, fmt: str)             -> Timestamp?
  to_iso        (t: Timestamp)                 -> str
  from_iso      (s: str)                       -> Timestamp?
  add           (t: Timestamp, d: Duration)    -> Timestamp
  sub           (t: Timestamp, d: Duration)    -> Timestamp
  before        (a: Timestamp, b: Timestamp)   -> bool
  after         (a: Timestamp, b: Timestamp)   -> bool
  epoch         ()                             -> Timestamp
  date_parts    (t: Timestamp)                 -> {str: int}
  day_of_week   (t: Timestamp)                 -> int
  start_of_day  (t: Timestamp)                 -> Timestamp
  duration_parts(d: Duration)                  -> {str: int}
  duration_str  (d: Duration)                  -> str
  ago           (t: Timestamp)                 -> str
  @type Timestamp   -- opaque, ordered, comparable
  @type Duration    -- opaque; literals: 500ms 1s 5m 1h 1d

@builtin regex
  match      (pattern: str, s: str)                    -> bool
  find       (pattern: str, s: str)                    -> str?
  find_all   (pattern: str, s: str)                    -> [str]
  groups     (pattern: str, s: str)                    -> [str]
  replace    (pattern: str, s: str, rep: str)          -> str
  replace_all(pattern: str, s: str, rep: str)          -> str
  split      (pattern: str, s: str)                    -> [str]

@builtin bytes
  concat     (a: [byte], b: [byte])                    -> [byte]
  slice      (b: [byte], start: int, end: int)         -> [byte]
  to_int_be  (b: [byte])                               -> int
  to_int_le  (b: [byte])                               -> int
  from_int   (n: int, size: int, endian: atom)         -> [byte]
  xor        (a: [byte], b: [byte])                    -> [byte]
  fill       (value: byte, count: int)                 -> [byte]

@builtin crypto
  sha256     (data: str)                               -> str
  sha256b    (data: [byte])                            -> [byte]
  hmac_sha256(key: str, data: str)                     -> str
  md5        (data: str)                               -> str
  crc32      (data: [byte])                            -> int

@builtin log
  debug (msg: str) -> unit   -- no-op in production unless OS captures
  info  (msg: str) -> unit
  warn  (msg: str) -> unit   -- always captured
  error (msg: str) -> unit   -- always captured
  inspect (label: str, value: any) -> any
  assert  (condition: bool, msg: str) -> unit
  -- assert behavior: in debug mode (log_level "debug"), false condition causes
  -- a runtime error with the message and suspends in debugger if DAP is active.
  -- In production (log_level "warn" or "error"), behaves as log.error(msg) with no crash.

@builtin random
  -- Non-deterministic. @impure — no @use or !effect required but results vary.
  -- Seeded from hardware entropy during deck_runtime_create().
  int    (min: int, max: int)     -> int   @impure   -- min..max inclusive
  float  ()                       -> float @impure   -- 0.0..1.0
  float  (min: float, max: float) -> float @impure
  bool   ()                       -> bool  @impure
  pick   (list: [T])              -> T?    @impure   -- :none if list empty
  pick_n (list: [T], n: int)      -> [T]   @impure   -- sampled without replacement
  shuffle(list: [T])              -> [T]   @impure
  uuid   ()                       -> str   @impure   -- UUID v4
  bytes  (n: int)                 -> [byte]@impure
  seed   (n: int)                 -> unit             -- deterministic; use in @test only

@builtin row
  -- Pure helpers for extracting typed fields from SQLite result rows ({str: any}).
  -- Always in scope; no @use required.
  int   (row: {str: any}, col: str) -> int?
  float (row: {str: any}, col: str) -> float?
  str   (row: {str: any}, col: str) -> str?
  bool  (row: {str: any}, col: str) -> bool?

-- Opaque handle to an app asset declared in @assets.
-- The bridge resolves the underlying file path when needed.
-- The Deck heap never holds raw image / cert / audio bytes via this handle.
@opaque AssetRef

@builtin assets
  -- Always in scope. No @use. No !effect.
  -- Available only inside apps that have an @assets declaration;
  -- calling from an app with no @assets block is a compile-time error.
  asset      (name: atom)  -> AssetRef
  -- Returns a handle to a declared asset. Pure — no I/O.
  -- Fails at compile time if `name` is not declared in @assets.
  -- For optional assets, returns the handle regardless; the bridge returns
  -- :err :not_found at the point of use if the file is absent.

  asset_bytes(name: atom)  -> Result [byte] :not_found
  -- Loads the full asset file into a Deck [byte] list.
  -- Use only for small assets (custom data formats, tiny fonts, etc.).
  -- For images, certs, and audio, prefer passing an AssetRef to the
  -- relevant capability — the bridge does the I/O without heap pressure.

  asset_from_bytes(data: [byte]) -> AssetRef
  -- Wraps an in-memory byte slice as an ephemeral AssetRef.
  -- The bridge copies `data` into its own buffer; Deck retains only the handle.
  -- Lifetime: the bridge holds the buffer until the AssetRef goes out of scope
  -- (or the app terminates). Not backed by any file; only valid for capabilities
  -- that accept AssetRef (e.g., system.audio, ui images).

-- Markdown types. Used by @builtin md and @capability markdown (§4.4).
@type MdDocument
  source       : str
  nodes        : [MdNode]
  toc          : [MdHeading]
  word_count   : int
  image_urls   : [str]
  front_matter : {str: any}?     -- populated when md.parse(opts: {"front_matter": true})

@type MdNode
  type     : atom
  -- :heading | :paragraph | :code_block | :blockquote | :list | :list_item
  -- :hr | :image | :table | :table_row | :table_cell | :html_block
  -- :inline_code | :bold | :italic | :strikethrough | :link | :text
  level    : int?           -- :heading only: 1–6
  text     : str?           -- text-bearing nodes: raw text content
  lang     : str?           -- :code_block only
  url      : str?           -- :link, :image
  alt      : str?           -- :image
  title    : str?           -- :link title attribute
  ordered  : bool?          -- :list: true = ordered, false = bullet
  tight    : bool?          -- :list: items are tight (no blank lines between)
  checked  : bool?          -- :list_item with GFM task syntax: - [ ] / - [x]
  align    : atom?          -- :table_cell: :left | :center | :right | :none
  is_header: bool?          -- :table_cell: true if in header row
  children : [MdNode]

@type MdHeading
  level : int
  text  : str
  id    : str               -- URL-safe slug (e.g. "getting-started")
  node  : MdNode

@type MdRange
  start : int               -- byte offset in source string
  end   : int

@type MdInlineFormat
  type  : atom              -- :bold | :italic | :code | :strikethrough | :link
  range : MdRange
  url   : str?              -- for :link

@type MdPatch
  -- Incremental update from streaming render
  type     : atom           -- :append | :replace | :finalize
  text     : str?           -- new text appended (for :append)
  document : MdDocument?    -- final full parse (for :finalize)

@type MdEditorState
  content       : str
  cursor        : int       -- byte offset
  selection     : MdRange?
  history_len   : int       -- number of undoable actions
  active_formats: [atom]    -- formats active at cursor: [:bold, :italic, ...]

@builtin md
  -- Pure markdown processing. Always in scope; no @use required.
  -- For incremental / streaming parses and editor-state operations, see
  -- @capability markdown in §4.4.
  parse                   (src: str)                            -> MdDocument
  parse                   (src: str, opts: {str: any})          -> MdDocument
  -- opts keys (all optional, with defaults shown):
  --   gfm_tables:   bool = true     GFM tables
  --   gfm_tasks:    bool = true     - [ ] / - [x] task lists
  --   smart_quotes: bool = false    "quotes" → "quotes"
  --   allow_html:   bool = false    inline HTML passthrough
  --   heading_ids:  bool = true     generate slug IDs for headings
  --   front_matter: bool = false    YAML front matter parsing
  to_plain                (src: str)                            -> str
  to_html                 (src: str)                            -> str
  excerpt                 (src: str, max_chars: int)            -> str
  excerpt                 (src: str, max_chars: int, suffix: str) -> str
  word_count              (src: str)                            -> int
  reading_time            (src: str)                            -> Duration
  reading_time            (src: str, wpm: int)                  -> Duration
  headings                (src: str)                            -> [MdHeading]
  headings                (doc: MdDocument)                     -> [MdHeading]
  heading_id              (text: str)                           -> str
  strip_images            (src: str)                            -> str
  extract_links           (src: str)                            -> [(str, str)]
  -- each tuple: (link_text, url)
  extract_code            (src: str)                            -> [(lang: str, code: str)]
  has_front_matter        (src: str)                            -> bool
  front_matter            (src: str)                            -> {str: any}
  body_after_front_matter (src: str)                            -> str
  sanitize                (src: str)                            -> str
  sanitize                (src: str, allow_html: bool)          -> str
  node_text               (node: MdNode)                        -> str
  -- recursive plain text
  node_children           (node: MdNode, type: atom)            -> [MdNode]
  toc_markdown            (doc: MdDocument)                     -> str
  -- render the ToC as a markdown bullet list
```

---

## 4. Capability Catalog

### 4.1 Sensors

```
@capability sensors.temperature
  read  ()              -> Result float sensors.temperature.Error
  watch (hz: int)       -> Stream float
  @errors
    :unavailable   "Sensor not responding"
    :out_of_range  "Reading outside calibrated range"
    :permission    "Access denied"
  @requires_permission

@capability sensors.humidity
  read  ()              -> Result float sensors.humidity.Error
  watch (hz: int)       -> Stream float
  @errors
    :unavailable  "Sensor not responding"
    :out_of_range "Reading outside calibrated range"
  @requires_permission

@capability sensors.pressure
  read  ()              -> Result float sensors.pressure.Error
  watch (hz: int)       -> Stream float
  @errors
    :unavailable  "Sensor not responding"

@capability sensors.accelerometer
  read  ()              -> Result (float, float, float) sensors.accelerometer.Error
  watch (hz: int)       -> Stream (float, float, float)
  @errors
    :unavailable  "Accelerometer not available"

@capability sensors.gyroscope
  read  ()              -> Result (float, float, float) sensors.gyroscope.Error
  watch (hz: int)       -> Stream (float, float, float)
  @errors
    :unavailable  "Gyroscope not available"

@capability sensors.gps
  read  ()              -> Result Location sensors.gps.Error
  watch ()              -> Stream Location
  @errors
    :unavailable   "GPS not available"
    :no_fix        "Cannot determine location"
    :permission    "Location access denied"
  @requires_permission

@type Location
  lat      : float
  lon      : float
  alt      : float?
  accuracy : float
  speed    : float?
  heading  : float?
  ts       : Timestamp

@capability sensors.light
  read  ()              -> Result float sensors.light.Error
  watch (hz: int)       -> Stream float
  @errors
    :unavailable  "Light sensor not available"
```

### 4.2 Storage

Four complementary storage primitives — choose based on data size, durability requirements, and access pattern. Full API docs in `05-deck-os-api`.

```
-- Simple string KV store. File-backed. Survives app restart; not guaranteed to survive
-- filesystem corruption. Use for blobs, cached data, user preferences that can be reconstructed.
@capability storage.local
  get    (key: str)              -> str?
  set    (key: str, v: str)      -> Result unit storage.Error
  delete (key: str)              -> Result unit storage.Error
  keys   ()                      -> Result [str] storage.Error
  clear  ()                      -> Result unit storage.Error
  @errors
    :full        "Storage is full"
    :permission  "Storage access denied"
    :corrupt     "Storage data is corrupted"
  @requires_permission

-- Flash NVS. Atomic per-key. Always survives power loss and filesystem corruption.
-- Use for tokens, counters, feature flags, values that must never be lost.
-- KEY LENGTH LIMIT: 15 characters maximum (ESP32 NVS driver constraint).
-- Calls with keys longer than 15 characters return :err :invalid_key immediately,
-- before reaching the NVS driver. Use short, stable, versioned key names:
-- "acc_token", "theme_v1", "pin_hash".
@capability nvs
  get       (key: str)                -> str?
  get_int   (key: str)                -> int?
  set       (key: str, value: str)    -> Result unit nvs.Error
  set_int   (key: str, value: int)    -> Result unit nvs.Error
  delete    (key: str)                -> Result unit nvs.Error
  keys      ()                        -> Result [str] nvs.Error
  clear     ()                        -> Result unit nvs.Error
  @errors
    :full        "NVS partition full"
    :not_found   "Key does not exist"
    :invalid_key "Key exceeds 15 characters (ESP32 NVS limit)"
    :write_fail  "Flash write failed"

-- SQLite database. One database file per app. Full SQL — transactions, indices, foreign keys.
-- Use for structured data, search, relations. Backed by SD card.
@capability db
  exec        (sql: str)                         -> Result unit db.Error
  query       (sql: str)                         -> Result [{str: any}] db.Error
  query_one   (sql: str)                         -> Result {str: any}? db.Error
  scalar      (sql: str)                         -> Result any? db.Error
  transaction (body: () -> Result unit db.Error) -> Result unit db.Error
  @errors
    :syntax   "SQL syntax error"
    :corrupt  "Database file is corrupted"
    :locked   "Database is locked"
    :full     "Disk full"
  @requires_permission

-- Filesystem. Sandboxed to /sdcard/{app.id}/. Full file and directory operations.
-- Use for large blobs, downloads, media, config files.
@capability fs
  read        (path: str)                          -> Result str fs.Error
  read_bytes  (path: str)                          -> Result [byte] fs.Error
  write       (path: str, content: str)            -> Result unit fs.Error
  write_bytes (path: str, data: [byte])            -> Result unit fs.Error
  append      (path: str, content: str)            -> Result unit fs.Error
  delete      (path: str)                          -> Result unit fs.Error
  exists      (path: str)                          -> bool
  list        (dir: str)                           -> Result [FsEntry] fs.Error
  mkdir       (path: str)                          -> Result unit fs.Error
  move        (from: str, to: str)                 -> Result unit fs.Error
  @errors
    :not_found   "File or directory not found"
    :permission  "Access denied"
    :full        "Disk full"
    :io          "I/O error"
  @requires_permission

@type FsEntry
  name     : str
  is_dir   : bool
  size     : int
  modified : Timestamp

-- In-memory TTL cache. Not persisted. Lost on app restart. For API responses
-- or computed values acceptable to recompute. Full detail + ErrorT in `05-deck-os-api §6`.
@capability cache
  get    (key: str)                          -> any?
  set    (key: str, value: any, ttl: Duration) -> unit
  set    (key: str, value: any)              -> unit
  delete (key: str)                          -> unit
  exists (key: str)                          -> bool
  ttl    (key: str)                          -> Duration?
  clear  ()                                  -> unit
```

### 4.3 Network

```
-- Per-request options for network.http calls.
-- tls_ca_cert / tls_client_cert override the app's TLS trust map for this single call.
-- Pass AssetRef values returned by asset() — bridge resolves to PEM bytes at call time.
-- See 02-deck-app §19 for @assets declaration syntax.
@type HttpOptions
  headers         : {str: str}?
  timeout         : Duration?
  tls_ca_cert     : AssetRef?   -- PEM CA cert asset; overrides trust map for this call
  tls_client_cert : AssetRef?   -- PEM client cert asset for mutual TLS

@capability network.http
  get      (url: str)                              -> Result Response network.Error
  get      (url: str, opts: HttpOptions)           -> Result Response network.Error
  post     (url: str, body: str)                   -> Result Response network.Error
  post     (url: str, body: str, opts: HttpOptions)-> Result Response network.Error
  put      (url: str, body: str)                   -> Result Response network.Error
  put      (url: str, body: str, opts: HttpOptions)-> Result Response network.Error
  delete   (url: str)                              -> Result Response network.Error
  delete   (url: str, opts: HttpOptions)           -> Result Response network.Error
  download (url: str, dest_path: str)              -> Result unit network.Error
  -- Streams the response body directly to the filesystem at dest_path
  -- (relative to the app's sandboxed /sdcard/{app.id}/ directory).
  -- Use for large files (firmware, media, databases) that must not fit in Deck heap.
  -- Progress events fire as os.download_progress during the transfer.
  -- Requires @use network.http AND fs capabilities.
  download (url: str, dest_path: str, opts: HttpOptions) -> Result unit network.Error
  @errors
    :offline        "No network connection"
    :timeout        "Request timed out"
    :unauthorized   "401 Authentication required"
    :forbidden      "403 Access denied"
    :not_found      "404 Not found"
    :server_error   "5xx Server error"
    :dns_failure    "Cannot resolve hostname"
    :tls_failure    "TLS handshake failed"
    :too_many       "429 Rate limited"
  @requires_permission

@type Response
  status     : int
  body       : str
  body_bytes : [byte]   -- raw response body (same data as body, as bytes)
  headers    : {str: str}

-- High-level HTTP client with session state, response caching, retry logic,
-- and auth management. Built on network.http but stateful — maintains
-- configuration and session across calls. Full detail in `05-deck-os-api §5`.
-- Backed by the same driver as network.http (`12-deck-service-drivers §5.2`).
-- api_client requires @permissions network.http.
@capability api_client
  configure         (opts: ApiConfig)                  -> Result unit api.Error
  get               (path: str)                        -> Result ApiResponse api.Error
  get               (path: str, opts: ReqOpts)         -> Result ApiResponse api.Error
  post              (path: str, body: any)             -> Result ApiResponse api.Error
  post              (path: str, body: any, opts: ReqOpts) -> Result ApiResponse api.Error
  put               (path: str, body: any)             -> Result ApiResponse api.Error
  delete            (path: str)                        -> Result ApiResponse api.Error
  post_multipart    (path: str, parts: [MultipartPart]) -> Result ApiResponse api.Error
  set_token         (token: str)                       -> unit
  set_token         (token: str, kind: atom)           -> unit    -- :bearer | :basic | :custom
  clear_token       ()                                 -> unit
  set_header        (name: str, value: str)            -> unit
  clear_header      (name: str)                        -> unit
  invalidate        (path: str)                        -> unit
  invalidate_prefix (prefix: str)                      -> unit
  @errors
    :invalid_config  "Configuration is malformed"
    :not_configured  "api_client.configure() not called before request"
    :offline         "No network connection"
    :timeout         "Request timed out"
    :unauthorized    "401 Authentication failed"
    :forbidden       "403 Access denied"
    :not_found       "404 Not found"
    :server_error    "5xx Server error"
    :rate_limited    "429 Rate limited"
    :parse_error     "Response body parse failed"
  @requires_permission

@opaque WsConn     -- WebSocket connection handle; bridge owns underlying connection

@type WsMessage
  type : atom     -- :text | :binary | :ping | :pong | :close
  text : str?
  data : [byte]?
  code : int?

@capability network.ws
  connect  (url: str)                        -> Result WsConn network.ws.Error
  connect  (url: str, headers: {str: str})   -> Result WsConn network.ws.Error
  send     (conn: WsConn, msg: str)          -> Result unit network.ws.Error
  send_bytes(conn: WsConn, data: [byte])     -> Result unit network.ws.Error
  messages (conn: WsConn)                    -> Stream WsMessage
  close    (conn: WsConn)                    -> Result unit network.ws.Error
  is_open  (conn: WsConn)                    -> bool   @pure
  @errors
    :offline   "No network connection"
    :timeout   "Connection timed out"
    :refused   "Connection refused"
    :protocol  "WebSocket protocol error"
    :closed    "Connection closed by remote"
  @requires_permission

@opaque Socket    -- TCP socket handle; bridge owns underlying socket

@capability network.socket
  open   (host: str, port: int)              -> Result Socket network.Error
  send   (s: Socket, data: [byte])           -> Result unit network.Error
  recv   (s: Socket)                         -> Stream [byte]
  close  (s: Socket)                         -> Result unit network.Error
  @errors
    :offline  "No network connection"
    :refused  "Connection refused"
    :timeout  "Connection timed out"
    :closed   "Socket closed by remote"
  @requires_permission

-- WiFi network management. Read-only access (status, watch) is available to all apps.
-- scan(), connect(), disconnect(), forget() require @requires_permission and are
-- typically restricted to system apps (Settings) by the OS author.
@type WifiAP
  ssid    : str
  rssi    : int
  auth    : atom   -- :open | :wpa2 | :wpa3 | :wpa2_wpa3
  channel : int

@type WifiStatus
  ssid      : str?
  rssi      : int
  ip        : str?
  connected : bool

@capability network.wifi
  scan       ()                           -> Result [WifiAP] network.wifi.Error
  -- Performs an active scan. Returns results sorted by RSSI descending.
  -- Fires os.wifi_changed on completion if the connected network changed.
  connect    (ssid: str, password: str)   -> Result unit network.wifi.Error
  connect    (ssid: str)                  -> Result unit network.wifi.Error
  -- Saves credentials and connects. Fires os.wifi_changed on state change.
  disconnect ()                           -> unit
  forget     (ssid: str)                  -> Result unit network.wifi.Error
  -- Removes saved credentials for the given SSID.
  status     ()                           -> WifiStatus
  watch      ()                           -> Stream WifiStatus
  -- Emits on every WiFi state change: connect, disconnect, RSSI update.
  saved      ()                           -> [str]
  -- Returns SSIDs of all saved networks (credentials stored in NVS).
  @errors
    :unavailable        "WiFi hardware not available"
    :auth_failed        "Wrong password or authentication rejected"
    :not_found          "Network not found during scan"
    :timeout            "Connection timed out"
    :already_connected  "Already connected to this network"
    :permission         "WiFi management requires permission"
  @requires_permission

-- MQTT pub/sub. OS manages connection, reconnection, and QoS. One connection per app.
-- Full API docs in 05-deck-os-api §7.
@type MqttConfig
  broker   : str
  port     : int         -- default 1883 (plain) or 8883 (TLS)
  client_id: str?        -- auto-generated if absent
  username : str?
  password : str?
  tls      : bool        -- default false

@capability mqtt
  configure   (opts: MqttConfig)              -> unit
  publish     (topic: str, payload: str)      -> Result unit mqtt.Error
  publish     (topic: str, payload: str, qos: int) -> Result unit mqtt.Error
  subscribe   (topic: str)                    -> Stream str
  unsubscribe (topic: str)                    -> unit
  disconnect  ()                              -> unit
  @errors
    :not_configured  "mqtt.configure() not called"
    :offline         "No network connection"
    :refused         "Broker refused connection"
    :timeout         "Connection timed out"
  @requires_permission
```

### 4.4 Display

```
-- NotifLevel: union-of-atoms alias (valid .deck-os syntax, see §2.1)
@type NotifLevel = :info | :warning | :error | :success
  -- :info    — neutral informational message
  -- :warning — amber/yellow; something to be aware of
  -- :error   — red; an operation failed
  -- :success — green; an operation succeeded
  -- Unknown atoms passed as level are treated as :info with a runtime warning.

@capability display.notify
  send    (msg: str)                         -> unit   -- defaults to :info
  send    (msg: str, level: NotifLevel)      -> unit
  dismiss ()                                 -> unit

@capability display.screen
  brightness (level: float)            -> unit   -- 0.0..1.0; clipped to valid range
  on  ()                               -> unit
  off ()                               -> unit
  is_on ()                             -> bool
  -- Timeout: display turns off after inactivity. Use system.shell for system apps.
  -- User apps read current brightness via system.info; setting brightness requires this cap.

-- UI theme selection. All apps can read and subscribe. Only system.* apps can call set().
-- The active theme is stored in NVS by svc_settings and applied by the LVGL bridge.
@type ThemeName = :matrix | :amber | :neon

@capability display.theme
  current ()                 -> ThemeName
  watch   ()                 -> Stream ThemeName
  -- Emits when the active theme changes (same as os.theme_changed event).
  set     (theme: ThemeName) -> unit
  -- Ignored for non-system apps. System apps call this to commit the new theme;
  -- bridge receives EVT_SETTINGS_CHANGED (key:"theme") and rebuilds all screens.

-- Markdown — stateful operations (streaming, editor state). Pure markdown
-- processing lives in @builtin md (§3). The `markdown` and `markdown_editor`
-- view nodes (02-deck-app §12) consume both this capability and the builtin.
@capability markdown
  -- Streaming: incrementally parse a stream of string chunks (e.g. AI responses)
  stream_parse   (source: Stream str)                    -> Stream MdPatch

  -- Editor state machine — every operation returns a new immutable state
  editor_new     (content: str)                          -> MdEditorState
  editor_insert  (state: MdEditorState, text: str)       -> MdEditorState
  editor_insert  (state: MdEditorState, text: str, at: int) -> MdEditorState
  editor_delete  (state: MdEditorState, range: MdRange)  -> MdEditorState
  editor_replace (state: MdEditorState, range: MdRange, text: str) -> MdEditorState
  editor_format  (state: MdEditorState, format: atom)    -> MdEditorState
  -- format atoms: :bold | :italic | :code | :strikethrough | :link
  --               :heading_1..6 | :bullet_list | :ordered_list
  --               :blockquote | :code_block
  editor_format  (state: MdEditorState, format: atom, range: MdRange) -> MdEditorState
  editor_undo    (state: MdEditorState)                  -> MdEditorState
  editor_redo    (state: MdEditorState)                  -> MdEditorState
  editor_move    (state: MdEditorState, direction: atom, by: atom) -> MdEditorState
  -- direction: :forward | :backward; by: :char | :word | :line | :paragraph
  editor_select  (state: MdEditorState, range: MdRange)  -> MdEditorState
  editor_select_all (state: MdEditorState)               -> MdEditorState
  editor_set_cursor (state: MdEditorState, offset: int)  -> MdEditorState

  @errors
    :parse_failed  "Markdown parse error (malformed document)"
    :stream_closed "Source stream closed unexpectedly"
```

### 4.5 BLE

```
@type BleDevice
  id      : str
  name    : str?
  rssi    : int
  address : str

@opaque BleConn   -- BLE connection handle; bridge owns underlying GATT connection

@capability ble
  scan         (duration: Duration)              -> Result [BleDevice] ble.Error
  -- Synchronous bounded scan; blocks until `duration` elapses and returns all discovered devices.
  scan_stream  ()                                -> Stream BleDevice
  -- Continuous scan; each discovered device is emitted as a stream item.
  -- Call stop_scan() to terminate. The stream closes when stop_scan() returns.
  stop_scan    ()                                -> unit
  -- Stops an in-progress scan_stream(). No-op if no scan is active.
  connect      (device_id: str)                  -> Result BleConn ble.Error
  disconnect   (conn: BleConn)                   -> Result unit ble.Error
  read         (conn: BleConn, service: str, char: str) -> Result [byte] ble.Error
  write        (conn: BleConn, service: str, char: str, data: [byte]) -> Result unit ble.Error
  write_no_rsp (conn: BleConn, service: str, char: str, data: [byte]) -> Result unit ble.Error
  notify       (conn: BleConn, service: str, char: str) -> Stream [byte]
  is_connected (conn: BleConn)                   -> bool   @pure
  @errors
    :unavailable    "BLE hardware not available or disabled"
    :permission     "BLE permission denied"
    :not_found      "Device not found during scan"
    :connect_failed "Connection attempt failed"
    :timeout        "Operation timed out"
    :disconnected   "Device disconnected unexpectedly"
    :invalid_handle "Service or characteristic UUID not found"
  @requires_permission
```

### 4.6 System

```
-- Audio playback for local asset files (WAV / MP3 declared in @assets as :audio_*).
-- On CyberDeck: output routes to the external BT Classic module (UART1) if connected,
-- otherwise the call fails with :no_output. There is no onboard speaker or headphone jack.
@capability system.audio
  play    (asset: atom)                    -> Result unit system.audio.Error
  -- Plays the asset referenced by the given atom (must be declared :audio_* in @assets).
  -- Blocks until playback completes or an error occurs. Call from a @task for async use.
  stop    ()                               -> unit
  -- Stops playback immediately. No-op if nothing is playing.
  volume  ()                               -> float
  -- Current volume 0.0..1.0. Reads from the external BT module's reported level.
  @errors
    :no_output    "No audio output device available (BT module not connected)"
    :not_found    "Asset atom not declared in @assets"
    :unsupported  "Audio format not supported"
    :busy         "Another audio asset is already playing"

@capability system.battery
  level          ()  -> int            -- 0..100 percent
  watch          ()  -> Stream int     -- emits on level change (coalesced; not every ADC tick)
  is_charging    ()  -> bool
  charging_watch ()  -> Stream bool    -- emits true on plug-in, false on unplug

@capability system.time
  -- NTP sync and timezone management.
  sync         ()         -> Result unit system.time.Error
  -- Forces an NTP query immediately. Fires os.time_change on success.
  -- Requires network to be connected; returns :unavailable otherwise.
  timezone     ()         -> str
  -- Returns IANA timezone string, e.g. "America/Mexico_City". Read from NVS.
  set_timezone (tz: str)  -> Result unit system.time.Error
  -- Validates tz against the compiled-in zone database; saves to NVS.
  -- Fires os.time_change after updating the local clock offset.
  @errors
    :unavailable   "NTP sync unavailable — no network connection"
    :sync_failed   "NTP server did not respond within timeout"
    :invalid_tz    "Unknown IANA timezone string"
  @requires_permission

@capability system.info
  device_id    ()  -> str
  device_model ()  -> str
  os_name      ()  -> str
  os_version   ()  -> str
  app_id       ()  -> str
  app_version  ()  -> str
  free_heap    ()  -> int
  uptime       ()  -> Duration
  cpu_freq_mhz ()  -> int
  versions     ()  -> Versions
  -- Returns the complete version envelope of the running system.
  -- Apps use it for telemetry, conditional feature use, "about" screens.
  -- The OS shell uses it for Settings → Device → Versions.
  -- See 15-deck-versioning §12 for the full Versions and DriverVersionEntry types.

@type Versions
  edition_current      : int
  editions_supported   : [int]
  deck_os              : int
  runtime              : str            -- semver string
  runtime_build        : str            -- vendor build identifier (commit hash, etc.)
  sdi_major            : int
  sdi_minor            : int
  drivers              : [DriverVersionEntry]
  extensions           : [ExtensionEntry]
  app_version          : str
  app_id               : str

@type DriverVersionEntry
  capability           : str            -- "network.http"
  driver_path          : str            -- "deck.driver.network.http"
  capability_version   : int
  impl_name            : str            -- "esp_idf_v6.0_lwip"
  impl_version         : str            -- semver string
  capability_flags     : [atom]         -- enabled flags as human-readable atoms
  state                : :running | :degraded | :unavailable

@type ExtensionEntry
  name                 : str            -- "ext.cyberdeck.battery_curve"
  level                : int
  vendor               : str?

@capability system.locale
  language          ()  -> str
  region            ()  -> str
  timezone          ()  -> str
  locale_str        ()  -> str
  uses_24h          ()  -> bool
  first_day_of_week ()  -> int
  format_number  (n: float, decimals: int)  -> str
  format_date    (t: Timestamp)             -> str
  format_time    (t: Timestamp)             -> str
```

### 4.7 Hardware I/O

```
@capability i2c
  write  (addr: int, data: [byte])       -> Result unit i2c.Error
  read   (addr: int, len: int)           -> Result [byte] i2c.Error
  @errors
    :nack       "Device did not acknowledge"
    :bus_error  "Bus error"
    :permission "I2C access restricted"

@capability spi
  transfer (data: [byte])                -> Result [byte] spi.Error
  @errors
    :bus_error  "SPI bus error"

@capability gpio
  read   (pin: int)                      -> Result bool gpio.Error
  write  (pin: int, high: bool)          -> Result unit gpio.Error
  watch  (pin: int)                      -> Stream bool
  -- watch emits on both rising and falling edges.
  -- The emitted bool is the new pin level: true = HIGH, false = LOW.
  -- The current state is NOT emitted at subscription time; only on change.
  @errors
    :invalid_pin  "Pin not valid on this hardware"
    :permission   "GPIO access restricted"

-- UART access for external peripheral modules (e.g. BT audio module on UART1, GPS dongles).
-- Available ports and default baud rates are declared by the OS author in the .deck-os file.
-- On CyberDeck: port 1 = UART1 (GPIO 15 TX / GPIO 16 RX), used for external BT module.
-- The bridge auto-detects the BT module at boot; use bt_classic for higher-level access.
@capability hardware.uart
  send      (port: int, data: [byte])    -> Result unit hardware.uart.Error
  recv      (port: int)                  -> Stream [byte]
  -- recv stream emits one chunk per received frame (up to 256 bytes).
  configure (port: int, baud: int)       -> Result unit hardware.uart.Error
  -- Changes baud rate at runtime. Port must already be open.
  @errors
    :invalid_port  "Port not available on this board"
    :not_open      "Port not configured"
    :io_error      "UART transmission error"
  @requires_permission

-- Higher-level capability for the external BT Classic audio module (UART1).
-- Only available when the module is detected at boot (auto-detected via AT handshake).
-- BLE (Bluetooth Low Energy) is native on ESP32-S3; see the ble capability (§4.5).
-- BT Classic A2DP audio is only possible via this external module — the ESP32-S3 SoC
-- does not have BT Classic in hardware.
@type BtClassicDevice
  address : str
  name    : str?
  rssi    : int

@opaque BtClassicConn   -- connection handle; bridge owns underlying UART session

@capability bt_classic
  available ()                           -> bool
  -- Returns false if the external module was not detected at boot.
  scan      (duration: Duration)         -> Result [BtClassicDevice] bt_classic.Error
  connect   (address: str)               -> Result BtClassicConn bt_classic.Error
  disconnect(conn: BtClassicConn)        -> Result unit bt_classic.Error
  send      (conn: BtClassicConn, data: [byte]) -> Result unit bt_classic.Error
  recv      (conn: BtClassicConn)        -> Stream [byte]
  is_connected (conn: BtClassicConn)     -> bool  @pure
  @errors
    :unavailable      "External BT module not detected at boot"
    :scan_failed      "Scan failed"
    :connect_failed   "Connection attempt failed"
    :disconnected     "Device disconnected unexpectedly"
    :timeout          "Operation timed out"
  @requires_permission
```

### 4.8 OTA Updates

```
@type OtaProgress
  downloaded_bytes : int
  total_bytes      : int     -- 0 if server did not send Content-Length
  percent          : float   -- 0.0..1.0; -1.0 if total unknown

@capability ota
  check             (manifest_url: str) -> Result OtaInfo ota.Error
  download          (url: str)          -> Result unit ota.Error
  -- Blocking call; subscribe to download_progress() before calling.
  -- Returns :ok only after full download and signature verification.
  download_progress ()                  -> Stream OtaProgress
  -- Emits periodically during download. Subscribe before calling download().
  -- Stream closes (completes) when download() returns.
  apply             ()                  -> unit   -- triggers reboot, never returns
  current           ()                  -> OtaBuild
  rollback          ()                  -> unit
  -- Marks the current partition as invalid and reboots to the previous firmware.
  -- Use only when the update is confirmed bad.
  @errors
    :no_update           "No update available"
    :download_failed     "Download failed or interrupted"
    :invalid_signature   "Update signature verification failed"
    :insufficient_space  "Not enough flash space for update"
    :permission          "OTA permission denied"
  @requires_permission

@type OtaInfo
  version  : str
  url      : str
  size     : int
  notes    : str?
  required : bool

@type OtaBuild
  version  : str
  built_at : Timestamp
  commit   : str?
```

### 4.9 Cryptography

```
-- AES-CBC symmetric encryption with PKCS#7 padding. Uses ESP32 hardware AES when available.
-- Stateless — no connection, no config. Full API docs in 05-deck-os-api §8.
@capability crypto.aes
  encrypt (key: [byte], iv: [byte], data: [byte]) -> Result [byte] crypto.Error
  -- key: 16 | 24 | 32 bytes. iv: 16 bytes. Returns ciphertext (PKCS#7-padded length).
  decrypt (key: [byte], iv: [byte], data: [byte]) -> Result [byte] crypto.Error
  -- Strips PKCS#7 padding. Returns :err :decrypt_failed if padding is invalid.
  @errors
    :bad_key         "Key must be 16, 24, or 32 bytes"
    :bad_iv          "IV must be 16 bytes"
    :decrypt_failed  "Decryption failed — wrong key or corrupted data"
```

### 4.10 Background Fetch

```
-- Lets the OS wake the app periodically while suspended to do background work.
-- The app receives @on os.background_fetch events at approximately min_interval.
-- Full API docs in 05-deck-os-api §9.
@capability background_fetch
  register   (min_interval: Duration)  -> unit
  unregister ()                        -> unit
  @errors
    :not_supported  "Background fetch not supported on this platform"
    :permission     "Permission denied"
  @requires_permission
```

### 4.11 Privileged System Capabilities

These capabilities are available only to apps whose `app.id` starts with `"system."`. The Loader rejects any other app that declares them in `@use`. They expose the OS app stack and process monitor to system apps (Launcher, Task Manager, Lock Screen).

```
@capability system.apps
  running         ()                        -> [AppInfo]
  suspended       ()                        -> [AppInfo]
  installed       ()                        -> [AppInfo]
  search          (query: str)              -> [AppInfo]
  bring_to_front  (id: str)                -> unit
  launch          (id: str)                -> Result unit system.Error
  launch_url      (id: str, url: str)      -> Result unit system.Error
  kill            (id: str)                -> unit
  notif_counts_watch ()                     -> Stream [(app_id: str, unread: int)]
  -- Emits a full snapshot whenever the unread notification count changes for any app.
  -- Used by the Launcher to render badges on app grid icons.
  -- Only meaningful for system apps; user apps receive an empty stream.
  config_schema (app_id: str)               -> [ConfigFieldInfo]
  -- Returns the declared @config schema of the given app, with current values included.
  -- Used by the Settings app to render per-app config screens.
  -- Returns [] if the app has no @config entries.

  @errors
    :not_found     "App not installed"
    :already_front "App is already in foreground"
    :unauthorized  "Only system apps can use this capability"

@type AppInfo
  id           : str
  name         : str
  version      : str
  icon         : str?
  thumbnail    : [byte]?
  suspended_at : Timestamp?
  is_launcher  : bool

@type ConfigFieldInfo
  name          : str
  type          : atom        -- :int | :float | :bool | :str | :atom
  default_value : any
  current_value : any
  range         : (float, float)?   -- min..max for int/float fields
  options       : [atom]?           -- valid values for atom fields
  unit          : str?              -- display unit label

@capability system.tasks
  tree          ()                              -> [ProcessEntry]
  kill          (app_id: str)                   -> unit
  kill_task     (app_id: str, task_name: str)   -> unit
  storage       (app_id: str)                   -> StorageInfo
  cpu_watch     ()                              -> Stream [ProcessEntry]
  -- cpu_watch emits an updated [ProcessEntry] snapshot every 5 s.

  @errors
    :not_found     "App or task not found"
    :unauthorized  "Only system apps can use this capability"

@type ProcessEntry
  id         : str          -- "bsky.app" | "bsky.app:SyncPosts"
  app_id     : str
  kind       : :main | :background
  task_name  : str?         -- nil for :main entries
  state      : ProcessState
  heap_kb    : int
  cpu_pct    : float        -- rolling 5 s average (0.0–100.0)
  uptime_ms  : int

@type ProcessState = :running | :suspended | :waiting_effect | :idle | :dead

@type StorageInfo
  local_kb   : int
  db_kb      : int
  nvs_bytes  : int

@capability system.shell
  -- Status and navigation bars
  set_status_bar       (visible: bool)   -> unit
  set_status_bar_style (style: atom)     -> unit
  status_bar_height    ()                -> int   @pure
  set_navigation_bar   (visible: bool)   -> unit
  navigation_bar_height()                -> int   @pure

  -- Display
  set_brightness    (level: float)       -> unit
  get_brightness    ()                   -> float @pure
  set_always_on     (on: bool)           -> unit
  screen_timeout    ()                   -> Duration @pure
  set_screen_timeout(d: Duration)        -> unit

  -- System state (read-only mirror, for system apps that need it without @use)
  battery_level  ()  -> int   @pure
  wifi_ssid      ()  -> str?  @pure
  bluetooth_on   ()  -> bool  @pure
  storage_available() -> int  @pure

  -- OS app stack control
  push_app       (id: str)  -> unit
  pop_to_launcher()         -> unit

  -- Notifications and crash
  post_notification    (opts: SysNotifOpts) -> unit
  clear_notification   (id: str)            -> unit
  clear_all_notifications ()               -> unit
  report_crash         (info: CrashInfo)   -> unit

  @errors
    :unauthorized "Only system apps can use this capability"

@type SysNotifOpts
  id      : str
  title   : str
  message : str
  app_id  : str
  icon    : str?
  url     : str?
  expires : Duration?

@type CrashInfo
  app_id   : str
  message  : str
  stack    : str
  occurred : Timestamp

-- ─────────────────────────────────────────────────────────────────
-- Crash history. Restricted to "system.crash_reporter" specifically
-- (not all system.* apps). The Crash Reporter (09-deck-shell §12)
-- receives every new crash via the @on crash_report hook (02-deck-app §11)
-- and persists it. This capability is the read/clear surface over that
-- store, and is what the Crash Reporter UI uses to render the crash list.
-- ─────────────────────────────────────────────────────────────────
@capability system.crashes
  list             ()                                   -> [CrashInfo]
  -- Most-recent-first. Bounded to the last N crashes (config: 50 default).
  list_for_app     (app_id: str)                        -> [CrashInfo]
  -- Filter by the originating app id.
  count            ()                                   -> int
  clear            ()                                   -> unit
  clear_for_app    (app_id: str)                        -> unit
  watch            ()                                   -> Stream CrashInfo
  -- Emits a value every time a new crash is recorded.
  -- Used by the Crash Reporter app to update its list reactively.

  @errors
    :unauthorized "Only system.crash_reporter can use this capability"
    :io           "Crash log storage is unavailable"

-- Lockscreen and PIN management. Available only to system.* apps (typically system.lockscreen
-- and system.settings). The bridge calls app_manager_lock() to push the lockscreen activity.
@capability system.security
  lock                     ()            -> unit
  -- Pushes the lockscreen immediately, regardless of PIN state.
  -- If no PIN is set, the lockscreen shows but is dismissible without a code.
  unlock                   (pin: str)    -> Result unit system.security.Error
  -- Validates PIN against the stored hash. On :ok fires os.unlocked and pops lockscreen.
  is_locked                ()            -> bool
  pin_enabled              ()            -> bool
  set_pin                  (pin: str)    -> Result unit system.security.Error
  -- Hashes and stores PIN in NVS. Enables PIN requirement on lock.
  clear_pin                ()            -> unit
  -- Removes stored PIN. Lockscreen will not require a code after this.
  auto_lock_timeout        ()            -> Duration?
  -- Returns nil if auto-lock is disabled.
  set_auto_lock_timeout    (d: Duration?) -> unit
  -- Pass nil to disable auto-lock. Pass duration to enable (e.g. 30s, 5m).
  @errors
    :wrong_pin    "Incorrect PIN"
    :no_pin       "No PIN is set; call set_pin first"
    :unauthorized "Only system apps can use this capability"

-- ─────────────────────────────────────────────────────────────────
-- Notifications capability (all apps, requires @permissions notifications)
-- ─────────────────────────────────────────────────────────────────

@capability notifications
  list              ()                           -> [NotifEntry]
  unread_count      ()                           -> int
  mark_read         (id: str)                    -> unit
  mark_all_read     ()                           -> unit
  clear             (id: str)                    -> unit
  clear_all         ()                           -> unit
  post_local        (opts: LocalNotifOpts)       -> Result str notifications.Error
  register_source   (src: NotifSource)           -> Result unit notifications.Error
  unregister_source (id: str)                    -> unit
  sources           ()                           -> [NotifSource]

  @errors
    :permission      "Requires @permissions notifications"
    :limit           "Notification quota exceeded (max per app)"
    :invalid_source  "Source config is malformed"
    :not_found       "Notification or source not found"
    :duplicate_id    "A source with this id is already registered"

@type NotifEntry
  id         : str
  source_id  : str?       -- nil for local notifications
  title      : str
  body       : str?
  read       : bool
  received   : Timestamp
  url        : str?       -- deep link; delivered to @on os.notification as open target

@type LocalNotifOpts
  title    : str
  body     : str?
  url      : str?
  expires  : Duration?

@type NotifSource
  id       : str
  type     : :http_poll | :mqtt
  interval : Duration?             -- required for :http_poll
  request  : HttpPollConfig?       -- required for :http_poll
  mqtt     : MqttSourceConfig?     -- required for :mqtt
  extract  : NotifExtract
  enabled  : bool                  -- default true; set false to pause without unregistering

@type HttpPollConfig
  url     : str
  method  : :get | :post
  headers : {str: str}?
  body    : str?
  auth    : NotifAuth

@type MqttSourceConfig
  broker : str
  topic  : str
  auth   : NotifAuth

-- NotifAuth: how svc_notifications authenticates when polling on behalf of this app.
-- bearer_nvs / basic_nvs: the service reads the token from the APP's NVS namespace
-- at poll time (key must exist; if missing the poll is skipped and a warning logged).
@type NotifAuth = :none
               | (type: :bearer_nvs, key: str)
               | (type: :basic_nvs,  key: str)
               | (type: :static,     token: str)

@type NotifExtract
  -- Subset of JSONPath (RFC 9535). Applied to the HTTP response body or MQTT payload.
  items_path     : str    -- e.g. "$.notifications[*]"  (array of items)
  id_path        : str    -- unique ID per item, relative to item root
  title_path     : str?
  body_path      : str?
  read_path      : str?   -- bool field; true = already read on server (skip posting)
  url_path       : str?
  timestamp_path : str?   -- ISO 8601 or unix epoch int
```

---

## 5. Events

OS events are pushed to the interpreter — never polled. The interpreter routes them to `@on` hooks and re-evaluates `when:` conditions.

```
@event os.suspend
@event os.resume               -- user-initiated resume only; NOT fired for background_fetch
@event os.terminate
@event os.background_fetch     -- OS woke the app for a background fetch window
@event os.low_battery    (level: int)
@event os.battery_changed (level: int, charging: bool)
-- Fired on every battery level change and on charge state change (plug/unplug).
-- Supersedes os.low_battery for apps that need full battery state.
@event os.network_change (status: atom)    -- :connected | :offline
@event os.wifi_changed (ssid: str?, rssi: int, connected: bool)
-- More detailed than os.network_change: fired on WiFi connect, disconnect, and RSSI update.
-- ssid is nil when disconnected.
@event os.time_change
@event os.display_rotated (orientation: atom)   -- :portrait | :landscape
-- Fired when the user changes display orientation in Settings.
-- The bridge calls ui_activity_recreate_all() in response; all active screens rebuild.
-- Apps receive on_create with intent_data = NULL after rotation — read state from app_state_get().
@event os.theme_changed (theme: atom)           -- :matrix | :amber | :neon
-- Fired when the active UI theme changes. Bridge rebuilds all active screens automatically.
-- Apps do not need to handle this event unless they cache theme-derived values in NVS.
@event os.storage_changed (mounted: bool)
-- Fired when the SD card is inserted (mounted: true) or removed (mounted: false).
-- Apps using fs or db capabilities must handle unmount gracefully — any pending I/O
-- returns :err :io after the card is removed.
@event os.locked
-- Fired when the lockscreen is activated (auto-lock timer, explicit app_manager_lock()).
@event os.unlocked
-- Fired when the correct PIN is entered and the lockscreen is dismissed.
@event os.storage_pressure
@event os.permission_change (capability: str, granted: bool)
@event os.config_change (field: str, value: any)
-- Fired when the user changes a @config value via the Settings app.
-- Only fired for the app whose @config was modified — other apps do not see it.
-- The app should re-read config.field_name on next access; @config values update
-- in-place (no restart required).
@event os.notification   (entry: NotifEntry)
-- Fired by svc_notifications when a new notification arrives for this app.
-- Fires if app is in foreground or if it has a background:true @task.
-- If app is terminated, notification is stored; event fires on next resume.
@event os.download_progress (url: str, bytes_written: int, total_bytes: int, percent: float)
-- Fired periodically during a network.http.download() call.
-- total_bytes is 0 and percent is -1.0 if the server did not send Content-Length.
-- Not fired for ota.download() — use ota.download_progress() stream instead.

@event os.memory_pressure (level: atom)   -- :low | :critical
-- Fired by the runtime memory monitor (09-deck-shell §19) when free heap
-- crosses a configured threshold. :low → encourage cooperative cleanup
-- (apps may flush caches); :critical → eviction is imminent. Apps that
-- handle this event can drop large derived data and prepare for suspend.
-- Fired to ALL running apps (foreground + suspended), not only the top app.

@event os.app_launched   (app_id: str)
-- Fired by the OS when any app is launched (foreground or relaunched from
-- suspended). Restricted: only delivered to apps that have @use system.apps.
-- Used by the Launcher and Task Manager to update their lists; normal apps
-- never receive this event.

@event os.app_suspended  (app_id: str)
-- Fired by the OS when any app is suspended (sent to background by the
-- user, by another launch, or by memory pressure eviction). Restricted:
-- only delivered to apps that have @use system.apps. Counterpart of
-- os.app_launched for the Task Manager / Launcher views.

-- Markdown viewport / interaction events. Fired by the bridge for the
-- `markdown` view node (02-deck-app §12).
@event markdown.link_tap      (url: str, text: str)
-- User tapped a [link text](url) in a rendered markdown body.

@event markdown.image_tap     (url: str, alt: str)
-- User tapped an inline image. The bridge does not auto-open the image;
-- the app decides (e.g. zoom, share, save).

@event markdown.heading_enter (id: str, level: int, text: str)
@event markdown.heading_exit  (id: str)
-- Fired as the user scrolls and headings enter/leave the viewport.
-- Used by reader apps to highlight the current section in a sticky ToC.

-- Physical hardware buttons (board-specific, declared by OS author)
@event hardware.button (id: int, action: atom)   -- :press | :long_press | :release
```

Custom hardware events:
```
@event hardware.door_opened
@event hardware.power_connected
```

Events declared but not referenced in any `@on` or `when:` are silently ignored.

---

## 6. Security Model

### 6.1 Capability Enforcement

The interpreter enforces `@use` declarations strictly. An app cannot call a capability method it did not declare in `@use`. This is checked:
1. **At load time**: the loader verifies every `!effect` function signature references a declared alias. Methods marked `@pure` in the OS surface are exempt from the `!effect` requirement — they may be called from pure functions without an `!alias` annotation.
2. **At runtime**: the effect dispatcher checks the alias is in the app's registered capability set before routing any call.
3. **Permission + optional**: if a `@use` entry references a capability with `@requires_permission` and the app's `@permissions` does not include it — load error if the entry is not `optional`, load warning if it is `optional`.

There is no way for app code to bypass this. The bridge is never called directly by app code.

### 6.2 Data Isolation

Every service (database, NVS, filesystem, cache, storage.local) namespaces all operations by `app.id` at the bridge level. The app cannot override or observe this namespacing. An app cannot read, write, enumerate, or infer the existence of another app's data through any service.

### 6.3 Filesystem Sandbox

Path components `..` in any `fs.*` operation are rejected by the bridge before reaching the OS — they never even reach the filesystem. The app's root is `/sdcard/{app.id}/`; this prefix is applied by the bridge, not the app. App code only provides relative paths.

### 6.4 No Dynamic Code Execution

There is no `eval`, no `exec(str)` that produces executable code, no dynamic module loading, no way for app code to generate and run new Deck code at runtime. The app's behavior is entirely determined by its `.deck` source files loaded at startup.

### 6.5 Memory Isolation

Each app runs in its own interpreter instance. There is no shared heap between apps. Apps cannot read or write each other's memory through any mechanism available to Deck code.

### 6.6 App Signing (Optional, OS-Dependent)

If the OS enables app signing (declared in `.deck-os` as `@security signing: :required`), the interpreter refuses to load any app whose source bundle lacks a valid signature. The signing mechanism is OS-defined. Deck itself does not mandate a signing scheme — it mandates that the OS can mandate one.

### 6.7 Permission Denial Behavior

Denying a permission makes the capability behave identically to an `optional` capability that is currently absent. No crash, no exception — calls return `:err :permission` as a `Result` value. The app handles this via pattern matching like any other error.

### 6.8 What the Interpreter Guarantees

- An app cannot access capabilities it did not declare
- An app cannot access another app's data
- An app cannot execute arbitrary code beyond its loaded `.deck` files
- An app cannot exhaust system memory without the OS noticing (the interpreter reports its heap usage via `system.info.free_heap()` and the OS can impose limits)
- Effect types in function signatures are verified — a pure function is provably pure at load time

---

## 7. OS Bridge C Interface

The bridge is the only OS-specific code. Platform authors implement this interface.

```c
/* Synchronous capability call.
   Named args are passed alongside positional args; positional and named
   are mutually exclusive in one Deck call (see 01-deck-lang §6.6).
   The full normative DeckCapFn signature (with named args) is in
   06-deck-native §5.1. This interface is the abstract protocol layer;
   the implementation uses the full signature.
   Rendering uses DeckViewContent (semantic structure) — see deck_bridge_render below. */
DeckResult deck_bridge_call(
  const char*  capability,       /* e.g. "sensors.temperature" */
  const char*  method,           /* e.g. "read" */
  DeckValue**  args,
  int          argc,
  const char** named_arg_keys,   /* NULL if positional call */
  DeckValue**  named_arg_vals,   /* NULL if positional call */
  int          named_argc,       /* 0 if positional call */
  DeckValue*   out_result
);

/* Subscribe to a stream capability */
DeckSubscription* deck_bridge_subscribe(
  const char*    capability,
  const char*    method,
  DeckValue**    args,
  int            argc,
  DeckCallback   on_value,
  DeckCallback   on_error,
  void*          user_data
);

void deck_bridge_unsubscribe(DeckSubscription* sub);

/* OS event registration */
void deck_bridge_on_event(
  const char*  event_name,
  DeckCallback callback,
  void*        user_data
);

/* Permission negotiation */
DeckPermResult deck_bridge_request_permissions(
  const char** capabilities,
  const char** reasons,
  int          count,
  bool*        granted_out   /* output: one bool per capability */
);

/* App lifecycle reporting */
void deck_bridge_app_ready();
void deck_bridge_app_suspended();
void deck_bridge_app_terminated(const char* reason);

/* Rendering — DeckViewContent is defined in 04-deck-runtime §4.3.
   The runtime calls deck_bridge_render whenever the evaluated semantic content
   changes. The bridge receives the complete current content; diffing against
   previous state is the bridge's responsibility. */
void deck_bridge_render(
  const char*      view_name,
  DeckViewContent* content    /* opaque; traverse via deck_content_* accessors in 04-deck-runtime §4.3 */
);

/* Intent event dispatched from the OS back to the interpreter.
   intent_name is the name: atom declared on the intent in the view body.
   payload carries event.value for input intents (toggle, range, choice,
   multiselect, text, password, pin, date, search); NULL for intents that
   do not produce a value (navigate, trigger, confirm, create, share). */
void deck_bridge_handle_intent(
  const char* view_name,
  const char* intent_name,   /* matches the name: atom of the intent */
  DeckValue*  payload        /* NULL when the intent type carries no event.value */
);
```

### 7.1 Value Representation

> **Canonical definition**: The `DeckType` enum and `DeckValue` struct are defined authoritatively in `06-deck-native §4.1`. That definition supersedes the abbreviated form below. The abbreviated form is kept here as a quick conceptual reference for bridge implementors reading this document first.

```c
/* Abbreviated — see 06-deck-native §4.1 for the full, normative definition */
typedef enum {
  DECK_INT, DECK_FLOAT, DECK_BOOL, DECK_STR, DECK_BYTE,
  DECK_UNIT, DECK_ATOM,
  DECK_VARIANT,   /* covers :ok, :err, :some, :none, and all atom variants */
  DECK_LIST, DECK_MAP, DECK_TUPLE, DECK_RECORD,
  DECK_OPAQUE, DECK_STREAM, DECK_DURATION, DECK_TIMESTAMP
} DeckType;
/* Full struct with all union members: see 06-deck-native §4.1 */
```

**Overload resolution**: When a capability declares multiple methods with the same name (e.g., `get(url)` and `get(url, headers)`), the bridge selects the overload by argument count (`argc`). If two overloads have the same arity, the first argument's `DeckType` is used as a secondary discriminator. If ambiguity remains after both criteria, it is a `.deck-os` authoring error (caught by the loader at startup).

### 7.2 Threading

The bridge may invoke callbacks from any thread. The interpreter queues all callbacks and processes them on the main interpreter loop (single-threaded from the evaluator's perspective). Bridge implementations must be thread-safe for concurrent capability calls. The interpreter guarantees that `deck_bridge_unsubscribe` completes before any further callbacks for that subscription are delivered.

---

## 8. Condition Evaluation

The interpreter evaluates `when:` conditions continuously in response to OS events and machine state changes. The interpreter subscribes to relevant OS events to trigger re-evaluation efficiently — no polling.

| Condition | Re-evaluated on |
|---|---|
| `network is :connected` | `os.network_change` event |
| `network is :offline` | `os.network_change` event |
| `battery > N%` | `system.battery.watch()` stream |
| `battery < N%` | `system.battery.watch()` stream |
| `display is :portrait` | `os.display_rotated` event |
| `display is :landscape` | `os.display_rotated` event |
| `wifi is :connected` | `os.wifi_changed` event |
| `wifi is :offline` | `os.wifi_changed` event |
| `alias is :available` | `os.permission_change` event |
| `alias is :unavailable` | `os.permission_change` event |
| `App is :state_name` | every `send()` to the named machine (internal event) |

`when:` condition expressions are pure read-only expressions. They may call `@pure`-marked capability methods (e.g., `ble.is_connected(conn)`, `system.battery.level()`), access `@config` values, and use the `is` operator. They may **not** contain `!effect` calls, `send()`, `do` blocks, or `match` on mutable state.

