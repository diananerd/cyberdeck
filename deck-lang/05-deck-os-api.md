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

@opaque DbHandle  -- bridge-owned database connection; passed to exec_on/query_on

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

Rows are `{str: any}` maps. The `row` builtin module (declared in `03-deck-os §3`, `@builtin row`) provides pure helpers that extract typed fields safely. They are always in scope with no `@use` required. All four return `:none` if the column is absent or if the value is SQL NULL. They do not throw. See `03-deck-os §3` for the full signatures.

### 2.4 Transactions

```
let result = db.transaction(_ ->
  do
    db.exec("INSERT INTO a VALUES (?)", [x])
    db.exec("UPDATE b SET count = count + 1 WHERE id = ?", [y])
)
-- If fn returns :err, the transaction is rolled back automatically
```

### 2.5 Schema in @migration

Schema migrations use `@migration`'s block-of-`from N:`-entries form (see `02-deck-app §15`). Integer versions run in ascending order once per device.

```
@migration
  from 1:
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
  keys      ()                     -> Result [str] nvs.Error
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

@opaque FsWriter  -- bridge-owned write handle; passed to write_chunk/close_write

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
  status     : int
  body       : str        -- response body as UTF-8 string
  body_bytes : [byte]     -- raw response body bytes (same data as body)
  json       : any?       -- auto-populated if Content-Type: application/json
  headers    : {str: str}
  cached     : bool
  latency    : Duration

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
- **TLS cert resolution**: before opening any TLS connection, `api_client` checks the app's TLS trust map (built at load time from `@assets for_domain:` declarations). If the target hostname matches an entry, the associated CA cert and/or client cert are applied automatically — no need to pass them in `configure()` or per-request options. Explicit `tls_ca_cert` in a request overrides the trust map for that call.

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
  cache.get_or_set("timeline", 30s, _ ->
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
  connected   ()                             -> bool   @pure

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

**Stream lifecycle**: Calling `mqtt.unsubscribe(topic)` signals the corresponding `subscribe` stream with `deck_stream_end()` — the stream terminates cleanly. Any content bodies that depend on the stream stop receiving updates. Subsequent calls to `StreamName.last()` return the last received value or `:none` if nothing was received before the stream ended.

---

## 8. AES Encryption (`crypto.aes`)

Symmetric encryption using **AES-CBC with PKCS#7 padding**. Required for any key material — do not store session tokens or secrets in NVS or storage.local as plaintext.

```
@capability crypto.aes
  encrypt (key: [byte], iv: [byte], data: [byte]) -> Result [byte] crypto.Error
  -- AES-CBC, PKCS#7 padding. key: 16, 24, or 32 bytes. iv: exactly 16 bytes.
  -- Returns the ciphertext (length = ceil(list.len(data)/16)*16).
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

`crypto.aes` is a capability (not a builtin) for two reasons: (1) on platforms with hardware AES (e.g., ESP32's AES accelerator), the bridge routes `encrypt`/`decrypt` through the hardware engine rather than a software implementation; (2) `gen_key()` and `gen_iv()` draw from OS-level hardware entropy, which is a side-effecting OS call. Keys are passed explicitly as `[byte]` — this capability does not manage or store key material on behalf of the app.

The hash functions in `03-deck-os §3` (`sha256`, `hmac_sha256`) are software-only builtins and do not require `@use`.

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

@on os.background_fetch
  -- OS woke the app specifically for a background fetch window.
  -- Distinct from @on resume (which is user-initiated only).
  match App.state
    | :authenticated _ -> App.send(:background_refresh)
    | _                -> unit
```

`background_fetch` requires `@permissions background_fetch reason: "..."`.

---

## 10. Notifications (`notifications`)

A persistent OS service (`svc_notifications`) monitors registered sources on behalf of apps and maintains a notification history in its own SQLite database. Apps interact through the `notifications` capability; the service runs independently of any app's VM lifecycle.

The full capability declaration is in `03-deck-os §4.11`. Summary:

```
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
    :limit           "Notification quota exceeded"
    :invalid_source  "Source config is malformed"
    :not_found       "Notification or source not found"
    :duplicate_id    "A source with this id is already registered"
```

`notifications` requires `@permissions notifications reason: "..."`.

### 10.1 Local Notifications

Post a notification generated by the app itself (from a `@task` or `@on` hook):

```deck
@use notifications as notif

@task CheckThreshold
  every: 5m
  when: sensor.reading() > config.threshold
  run:
    notif.post_local(LocalNotifOpts {
      title: "Threshold exceeded"
      body:  "Current: {sensor.reading()}"
      url:   "myapp://readings"
    })
```

`post_local()` returns the assigned notification `id` as `Result str notifications.Error`.

### 10.2 Remote Source Registration

Register a remote source at app launch (or after authentication). The source config is **persisted by `svc_notifications` to its own NVS namespace** and survives app termination and device reboot. The app only needs to call `register_source()` once — typically after the user logs in.

```deck
@on launch
  notif.register_source(NotifSource {
    id:       "bsky_notifs"
    type:     :http_poll
    interval: 60s
    request:  HttpPollConfig {
      url:    "https://bsky.social/xrpc/app.bsky.notification.listNotifications?limit=20"
      method: :get
      auth:   (type: :bearer_nvs, key: "auth_token")
    }
    extract: NotifExtract {
      items_path:     "$.notifications[*]"
      id_path:        "$.cid"
      title_path:     "$.author.displayName"
      body_path:      "$.record.text"
      read_path:      "$.isRead"
      timestamp_path: "$.indexedAt"
    }
    enabled: true
  })
```

`svc_notifications` reads `auth_token` from the **app's own NVS namespace** at each poll. If the key is missing (user logged out), the poll is silently skipped. When the user logs in and saves the token to NVS, the next poll window picks it up automatically.

### 10.3 Receiving Notifications

```deck
@on os.notification (entry: NotifEntry)
  -- Fired for this app when svc_notifications stores a new notification.
  -- Works in foreground and when app is suspended (if background:true task exists).
  -- When app is terminated: stored, fired on next resume.
  match entry.source_id
    | :some "bsky_notifs" ->
        App.send(:increment_unread)
    | :none ->
        unit   -- local notification, already counted
```

### 10.4 Reading and Managing Notifications

```deck
let entries = notif.list()          -- all notifications for this app, newest first
let count   = notif.unread_count()  -- count of entries where read = false

notif.mark_read("some_cid")        -- marks one as read
notif.mark_all_read()              -- marks all as read (e.g. when user opens notif screen)
notif.clear("some_cid")           -- removes one from history
notif.clear_all()                 -- wipes all notification history for this app
```

### 10.5 Managing Sources

```deck
-- Pause a source without removing its config
notif.register_source(existing_source with { enabled: false })

-- Remove a source permanently (also removed from svc_notifications NVS)
notif.unregister_source("bsky_notifs")

-- List active sources
let srcs = notif.sources()
```

### 10.6 Storage and Limits

`svc_notifications` stores notifications in `/sdcard/system/notifications.db` (its own SQLite, not the app's). Per-app limits are set in `.deck-os` (default: 100 entries; oldest are evicted when the limit is hit). Source configs are stored in `svc_notifications`'s own NVS namespace (`notif_src`), not the app's.

---

## 11. Simple Key-Value Store (`storage.local`)

File-backed string KV store. Persists across restarts. Not guaranteed to survive filesystem corruption — use `nvs` for values that must never be lost. Use `storage.local` for larger blobs, cached content, or user data that can be reconstructed.

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
  @requires_permission
```

`storage.local` requires `@permissions storage.local reason: "..."`.

**Versus `nvs`**: `storage.local` is file-backed (SD card or flash filesystem). `nvs` is flash-based, always atomic, always survives power loss. Use `nvs` for tokens, auth credentials, and critical settings; use `storage.local` for caches, large blobs, and data that can be reconstructed.

**Versus `fs`**: `storage.local` is a flat KV store — no directories, no append, no streaming. `fs` offers full filesystem operations. Use `storage.local` when you just need `get`/`set` by key.

---

## 12. Device Info (`system.info`)

Read-only device and runtime information. No permission required. All methods are `@pure` — they may be called from pure functions and `when:` conditions.

```
@capability system.info
  device_id    ()  -> str        -- Unique hardware identifier (MAC-derived)
  device_model ()  -> str        -- Human-readable board name, e.g. "Waveshare ESP32-S3-Touch-LCD-4.3"
  os_name      ()  -> str        -- OS name, e.g. "CyberDeck"
  os_version   ()  -> str        -- Semver OS version, e.g. "1.0.0"
  app_id       ()  -> str        -- This app's own id (from @app id:), as the bridge sees it
  app_version  ()  -> str        -- This app's version string
  free_heap    ()  -> int        -- Free heap bytes (approximate, snapshot)
  uptime       ()  -> Duration   -- Time since last boot
  cpu_freq_mhz ()  -> int        -- Current CPU frequency in MHz
```

`system.info.app_id()` is the canonical way for an app to read its own identifier. The value is the same string as the `id:` field in its `@app` declaration.

---

## 13. Locale (`system.locale`)

Locale and formatting helpers. All methods are `@pure`. No permission required.

```
@capability system.locale
  language          ()                           -> str    -- ISO 639-1, e.g. "es"
  region            ()                           -> str    -- ISO 3166-1, e.g. "MX"
  timezone          ()                           -> str    -- IANA timezone, e.g. "America/Mexico_City"
  locale_str        ()                           -> str    -- BCP 47, e.g. "es-MX"
  uses_24h          ()                           -> bool
  first_day_of_week ()                           -> int    -- 0 = Sunday, 1 = Monday
  format_number     (n: float, decimals: int)    -> str    -- locale-formatted number
  format_date       (t: Timestamp)               -> str    -- locale-formatted date
  format_time       (t: Timestamp)               -> str    -- locale-formatted time (respects uses_24h)
```

These values are set by the user in OS settings and stored in NVS. They are read-only from apps — only the Settings app (via `system.time.set_timezone`) can change them.

---

## 14. OTA Updates (`ota`)

Over-the-air firmware update. The full capability declaration is in `03-deck-os §4.8`. Summary:

```
@capability ota
  check             (manifest_url: str) -> Result OtaInfo ota.Error
  download          (url: str)          -> Result unit ota.Error
  download_progress ()                  -> Stream OtaProgress
  apply             ()                  -> unit    -- triggers reboot, never returns
  current           ()                  -> OtaBuild
  rollback          ()                  -> unit    -- marks current partition invalid, reboots
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

@type OtaProgress
  downloaded_bytes : int
  total_bytes      : int     -- 0 if server did not send Content-Length
  percent          : float   -- 0.0..1.0; -1.0 if total unknown
```

**Usage pattern** — check, download with progress, then apply:

```deck
@use ota as ota

fn check_and_apply (manifest_url: str) -> unit !ota =
  match ota.check(manifest_url)
    | :err :no_update  -> unit
    | :err e           -> log.warn("OTA check failed: {str(e)}")
    | :ok info         ->
        let progress = ota.download_progress()
        match ota.download(info.url)
          | :err e -> log.error("OTA download failed: {str(e)}")
          | :ok _  -> ota.apply()   -- never returns; device reboots
```

`subscribe` to `download_progress()` **before** calling `download()` — the stream starts emitting immediately on `download()` call and closes when it returns.

`ota` requires `@permissions ota reason: "..."`.

---

## 15. Isolation Guarantees

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
