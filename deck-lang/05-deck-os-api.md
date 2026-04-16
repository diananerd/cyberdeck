# Deck OS API — High-Level Services
**Version 2.0 — Shared OS Services with Per-App Isolation**

---

## 1. Overview

Beyond hardware capabilities, the OS provides high-level services all apps share. The design:

- **Shared infrastructure**: one SQLite engine, one HTTP stack, one MQTT client — all apps use the same compiled-in implementation
- **Isolated context**: every service is namespaced by `app.id` automatically at the bridge level
- **No configuration**: declare in `@use`, use immediately

```
@use
  db              as db       -- SQLite, isolated per app
  nvs             as nvs      -- Non-volatile key-value (flash)
  fs              as fs       -- Filesystem, sandboxed to app directory
  api_client      as api      -- HTTP + session + cache + auth
  cache           as cache    -- In-memory TTL cache
  mqtt            as mqtt     -- MQTT pub/sub
  system.info     as sys
  system.locale   as locale
  ota             as ota
  background_fetch as bg
  crypto.aes      as aes
```

---

## 2. SQLite (`db`)

Full SQLite3 engine compiled into the OS. Each app gets an isolated database file at a path managed by the OS.

### 2.1 API

```
@capability db
  exec      (sql: str)                         -> Result unit db.Error
  exec      (sql: str, params: [any])          -> Result unit db.Error
  query     (sql: str)                         -> Result [{str: any}] db.Error
  query     (sql: str, params: [any])          -> Result [{str: any}] db.Error
  query_one (sql: str)                         -> Result {str: any}? db.Error
  query_one (sql: str, params: [any])          -> Result {str: any}? db.Error
  scalar    (sql: str)                         -> Result any? db.Error
  scalar    (sql: str, params: [any])          -> Result any? db.Error
  transaction (fn: unit -> Result unit db.Error) -> Result unit db.Error
  open      (name: str)                        -> Result DbHandle db.Error
  exec_on   (h: DbHandle, sql: str, params: [any]) -> Result unit db.Error
  query_on  (h: DbHandle, sql: str, params: [any]) -> Result [{str: any}] db.Error

  @errors
    :constraint    "Constraint violation"
    :syntax        "SQL syntax error"
    :no_such_table "Table does not exist"
    :type_mismatch "Column type mismatch"
    :full          "Storage is full"
    :corrupt       "Database file is corrupted"
```

`@capability db` requires `@permissions db reason: "..."`.

### 2.2 Parameter Binding

SQL uses `?` placeholders. Type mapping:

| Deck type | SQLite type |
|---|---|
| `int` | INTEGER |
| `float` | REAL |
| `str` | TEXT |
| `bool` | INTEGER (0/1) |
| `[byte]` | BLOB |
| `:none` / `unit` | NULL |

```
db.exec(
  "INSERT INTO readings (value, ts) VALUES (?, ?)",
  [temp, time.to_iso(time.now())]
)
```

### 2.3 Row Access Helpers

Rows are `{str: any}` maps. The `row` builtin module (declared in `03-deck-os §3`) provides pure helpers that extract typed fields safely. They are always in scope with no `@use` required:

```
row.int   (row: {str: any}, col: str) -> int?
row.float (row: {str: any}, col: str) -> float?
row.str   (row: {str: any}, col: str) -> str?
row.bool  (row: {str: any}, col: str) -> bool?
```

All four return `:none` if the column is absent or if the value is SQL NULL. They do not throw.

### 2.4 Transactions

```
let result = db.transaction(() ->
  do
    db.exec("INSERT INTO a VALUES (?)", [x])
    db.exec("UPDATE b SET count = count + 1 WHERE id = ?", [y])
)
-- If fn returns :err, the transaction is rolled back automatically
```

### 2.5 Schema in @migration

```
@migration from: "1.x"
  do
    db.exec("""
      ALTER TABLE posts ADD COLUMN score REAL DEFAULT 0.0
    """)
    db.exec("""
      CREATE INDEX IF NOT EXISTS idx_posts_score ON posts(score)
    """)
```

---

## 3. NVS — Non-Volatile Storage (`nvs`)

Writes directly to flash. Atomic per key. Survives filesystem corruption and power loss. For small, frequently-updated values: tokens, counters, offsets, feature flags.

**Versus `storage.local`**: `storage.local` is file-based (filesystem-dependent). NVS is flash-based, always atomic. Use NVS for values that must survive a failed filesystem, and `storage.local` for larger blobs that can be reconstructed.

**Versus `@config`**: `@config` is user-facing, OS-exposable, constrained. NVS is internal app data.

```
@capability nvs
  get       (key: str)             -> str?
  get_int   (key: str)             -> int?
  get_float (key: str)             -> float?
  get_bool  (key: str)             -> bool?
  get_bytes (key: str)             -> [byte]?

  set       (key: str, v: str)     -> Result unit nvs.Error
  set_int   (key: str, v: int)     -> Result unit nvs.Error
  set_float (key: str, v: float)   -> Result unit nvs.Error
  set_bool  (key: str, v: bool)    -> Result unit nvs.Error
  set_bytes (key: str, v: [byte])  -> Result unit nvs.Error

  delete    (key: str)             -> Result unit nvs.Error
  keys      ()                     -> [str]
  clear     ()                     -> Result unit nvs.Error

  @errors
    :full        "NVS partition is full"
    :invalid_key "Key exceeds 15 characters"
    :write_fail  "Flash write failed"
```

**Key limit**: 15 characters (ESP-IDF constraint). No permission required.

---

## 4. Filesystem (`fs`)

Sandboxed to `/sdcard/{app.id}/`. The app never sees the full path. All paths relative to app root.

```
@capability fs
  read        (path: str)                    -> Result str fs.Error
  read_bytes  (path: str)                    -> Result [byte] fs.Error
  write       (path: str, content: str)      -> Result unit fs.Error
  write_bytes (path: str, data: [byte])      -> Result unit fs.Error
  append      (path: str, content: str)      -> Result unit fs.Error
  delete      (path: str)                    -> Result unit fs.Error
  rename      (from: str, to: str)           -> Result unit fs.Error
  copy        (from: str, to: str)           -> Result unit fs.Error
  exists      (path: str)                    -> bool
  size        (path: str)                    -> Result int fs.Error
  modified    (path: str)                    -> Result Timestamp fs.Error
  list        (dir: str)                     -> Result [FsEntry] fs.Error
  mkdir       (dir: str)                     -> Result unit fs.Error
  rmdir       (dir: str)                     -> Result unit fs.Error
  open_write  (path: str)                    -> Result FsWriter fs.Error
  write_chunk (w: FsWriter, data: [byte])    -> Result unit fs.Error
  close_write (w: FsWriter)                  -> Result unit fs.Error

  @errors
    :not_found      "Path does not exist"
    :permission     "Path escapes sandbox"
    :already_exists "Already exists"
    :not_empty      "Directory is not empty"
    :full           "Filesystem is full"
    :is_dir         "Expected file, got directory"
    :not_dir        "Expected directory, got file"
    :io_error       "Hardware I/O error"

@type FsEntry
  name     : str
  path     : str
  is_dir   : bool
  size     : int
  modified : Timestamp
```

`@capability fs` requires `@permissions fs reason: "..."`.

**Path rules**: `/` separator, no leading `/`, no `..` (rejected at bridge, not runtime error), max 8 directory levels.

---

## 5. API Client (`api_client`)

High-level HTTP client with session state, response caching, retry logic, and auth management. Built on `network.http` but stateful — maintains configuration and session across calls.

```
@capability api_client
  configure     (opts: ApiConfig)                -> Result unit api.Error
  -- Returns :err :invalid_config if base_url is malformed or timeout is zero.
  -- Must be called before any request method; requests before configure return
  -- :err :not_configured.
  get           (path: str)                      -> Result ApiResponse api.Error
  get           (path: str, opts: ReqOpts)       -> Result ApiResponse api.Error
  post          (path: str, body: any)           -> Result ApiResponse api.Error
  post          (path: str, body: any, opts: ReqOpts) -> Result ApiResponse api.Error
  put           (path: str, body: any)           -> Result ApiResponse api.Error
  delete        (path: str)                      -> Result ApiResponse api.Error
  post_multipart(path: str, parts: [MultipartPart]) -> Result ApiResponse api.Error
  set_token     (token: str)                     -> unit
  set_token     (token: str, kind: atom)         -> unit   -- :bearer | :basic | :custom
  clear_token   ()                               -> unit
  set_header    (name: str, value: str)          -> unit
  clear_header  (name: str)                      -> unit
  invalidate    (path: str)                      -> unit
  invalidate_prefix (prefix: str)                -> unit

  @errors
    :invalid_config  "Configuration is malformed (bad URL or zero timeout)"
    :not_configured  "api_client.configure() not called before request"
    :offline         "No network connection"
    :timeout         "Request timed out"
    :unauthorized    "401 Authentication failed"
    :forbidden       "403 Access denied"
    :not_found       "404 Not found"
    :server_error    "5xx Server error"
    :rate_limited    "429 Rate limited"
    :parse_error     "Response body parse failed"

@type ApiConfig
  base_url     : str
  timeout      : Duration
  cache_ttl    : Duration?
  retry_count  : int
  retry_on     : [atom]
  user_agent   : str?

@type ReqOpts
  cache    : atom?           -- :skip | :force | :no_store
  timeout  : Duration?
  headers  : {str: str}?
  query    : {str: str}?

@type ApiResponse
  status   : int
  body     : str
  json     : any?
  headers  : {str: str}
  cached   : bool
  latency  : Duration

@type MultipartPart
  name     : str
  filename : str?
  content  : str | [byte]
  mime     : str
```

**Behavior details:**
- `body: any` is serialized as JSON if a map or list, as plain string otherwise
- `json` field in `ApiResponse` is auto-populated if `Content-Type: application/json`
- Rate limiting: on 429, the client reads `Retry-After` and defers retry automatically (counts as one of `retry_count`)
- Caching: responses with `cache_ttl` set are cached by path; `invalidate` clears by exact path, `invalidate_prefix` by prefix

`api_client` requires `@permissions network.http reason: "..."`.

---

## 6. In-Memory Cache (`cache`)

Fast, TTL-based cache. Not persisted. Lost on app restart. For API responses or computed values acceptable to recompute.

```
@capability cache
  get         (key: str)                           -> any?
  set         (key: str, value: any, ttl: Duration)-> unit
  set         (key: str, value: any)               -> unit    -- no expiry
  delete      (key: str)                           -> unit
  exists      (key: str)                           -> bool
  ttl         (key: str)                           -> Duration?
  clear       ()                                   -> unit
  get_or_set  (key: str, ttl: Duration, fn: unit -> any) -> any
```

`get_or_set` is atomic — if two concurrent tasks call it simultaneously, `fn` is called only once. No permission required.

```
fn timeline_posts () -> [{str: any}] !api !cache =
  cache.get_or_set("timeline", 30s, () ->
    match api.get("/feed.getTimeline")
      | :ok r  -> unwrap_opt_or(r.json, [])
      | :err _ -> []
  )
```

---

## 7. MQTT (`mqtt`)

MQTT pub/sub client. The OS manages connection, reconnection, and QoS bookkeeping.

```
@capability mqtt
  configure   (opts: MqttConfig)              -> unit
  publish     (topic: str, payload: str)      -> Result unit mqtt.Error
  publish     (topic: str, payload: str, qos: int) -> Result unit mqtt.Error
  subscribe   (topic: str)                    -> Stream str
  unsubscribe (topic: str)                    -> unit
  connected   ()                             -> bool

  @errors
    :not_configured  "mqtt.configure() not called"
    :not_connected   "Not connected to broker"
    :publish_failed  "Message delivery failed"

@type MqttConfig
  broker_url    : str
  client_id     : str
  username      : str?
  password      : str?
  keepalive     : Duration
  clean_session : bool
  qos           : int
```

`mqtt` requires `@permissions mqtt reason: "..."`.

---

## 8. AES Encryption (`crypto.aes`)

Symmetric encryption using **AES-CBC with PKCS#7 padding**. Required for any key material — do not store session tokens or secrets in NVS or storage.local as plaintext.

```
@capability crypto.aes
  encrypt (key: [byte], iv: [byte], data: [byte]) -> Result [byte] crypto.Error
  -- AES-CBC, PKCS#7 padding. key: 16, 24, or 32 bytes. iv: exactly 16 bytes.
  -- Returns the ciphertext (length = ceil(len(data)/16)*16).
  decrypt (key: [byte], iv: [byte], data: [byte]) -> Result [byte] crypto.Error
  -- Strips PKCS#7 padding. Returns :err :decrypt_failed if padding is invalid
  -- (wrong key or corrupted ciphertext).
  gen_key (bits: int)                             -> [byte]   -- bits: 128, 192, or 256
  gen_iv  ()                                      -> [byte]   -- 16 random bytes from OS entropy

  @errors
    :invalid_key    "Key must be 16, 24, or 32 bytes"
    :invalid_iv     "IV must be exactly 16 bytes"
    :invalid_bits   "bits must be 128, 192, or 256"
    :decrypt_failed "Wrong key or corrupted/padded data"
```

The hash functions in `03-deck-os §3` (`sha256`, `hmac_sha256`) are builtins and do not require `@use`. `crypto.aes` is a capability because it uses OS-level secure storage for key material on some platforms.

---

## 9. Background Fetch (`background_fetch`)

Allows the OS to wake the app periodically while suspended.

```
@capability background_fetch
  register (min_interval: Duration) -> unit
  @errors
    :not_supported  "Background fetch not supported on this OS"
    :permission     "Permission denied"
```

```
@on launch
  bg.register(15m)

@on resume
  -- Background fetch woke us; do the work
  when App is :authenticated
    App.send(:background_refresh)
```

`background_fetch` requires `@permissions background_fetch reason: "..."`.

---

## 10. Isolation Guarantees

All services enforce these at the bridge level. App code cannot bypass them.

| Service | Isolation mechanism |
|---|---|
| `db` | Separate SQLite file per `app.id` |
| `nvs` | NVS namespace per `app.id` |
| `fs` | Chrooted to `/sdcard/{app.id}/`; `..` rejected before OS call |
| `api_client` | Session, token, cache are per-app instances |
| `cache` | Memory region tagged by `app.id` |
| `mqtt` | Client ID includes `app.id`; topic ACL enforced by OS policy |
| `storage.local` | File scoped to app directory |
| `system.info` | Read-only; `app_id()` returns own ID only |

An app cannot observe the existence or count of other apps through any of these services. The services do not expose cross-app enumeration.
