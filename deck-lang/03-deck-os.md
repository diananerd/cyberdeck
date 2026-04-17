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

@include "relative/path/to/other.deck-os"
  -- Inserts all declarations from the referenced file as if they were written
  -- inline at this position. Path is relative to the including file.
  -- Circular includes are a fatal parse error.
  -- Useful for splitting large surfaces into per-subsystem files.
  -- Processed by the loader at Stage 2 (OS Surface Loading), depth-first.

@builtin module_name
  fn_name (param: Type) -> ReturnType
  ...

@capability capability.path
  method_name (params...) -> ReturnType
  method_name (params...) -> Stream Type
  @errors
    :atom  "description"
  @requires_permission   -- if present, must appear in app's @permissions

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

```
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
```

### 4.3 Network

```
@capability network.http
  get    (url: str)                          -> Result Response network.Error
  get    (url: str, headers: {str: str})     -> Result Response network.Error
  post   (url: str, body: str)               -> Result Response network.Error
  post   (url: str, body: str, headers: {str: str}) -> Result Response network.Error
  put    (url: str, body: str)               -> Result Response network.Error
  delete (url: str)                          -> Result Response network.Error
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
  status  : int
  body    : str
  headers : {str: str}

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
  brightness (level: float)            -> unit   -- 0.0..1.0
  on  ()                               -> unit
  off ()                               -> unit
  is_on ()                             -> bool
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
  scan_stream  ()                                -> Stream BleDevice
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
@capability system.battery
  level      ()  -> int      -- 0..100 percent
  watch      ()  -> Stream int
  is_charging()  -> bool

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
```

### 4.8 OTA Updates

```
@capability ota
  check    (manifest_url: str) -> Result OtaInfo ota.Error
  download (url: str)          -> Result unit ota.Error
  apply    ()                  -> unit   -- triggers reboot, never returns
  current  ()                  -> OtaBuild
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

---

## 5. Events

OS events are pushed to the interpreter — never polled. The interpreter routes them to `@on` hooks and re-evaluates `when:` conditions.

```
@event os.suspend
@event os.resume               -- user-initiated resume only; NOT fired for background_fetch
@event os.terminate
@event os.background_fetch     -- OS woke the app for a background fetch window
@event os.low_battery    (level: int)
@event os.network_change (status: atom)    -- :connected | :offline
@event os.time_change
@event os.storage_pressure
@event os.permission_change (capability: str, granted: bool)

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
| `alias is :available` | `os.permission_change` event |
| `alias is :unavailable` | `os.permission_change` event |
| `App is :state_name` | every `send()` to the named machine (internal event) |

`when:` condition expressions are pure read-only expressions. They may call `@pure`-marked capability methods (e.g., `ble.is_connected(conn)`, `system.battery.level()`), access `@config` values, and use the `is` operator. They may **not** contain `!effect` calls, `send()`, `do` blocks, or `match` on mutable state.

