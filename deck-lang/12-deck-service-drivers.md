# Deck Service Driver Interface (SDI)
**Version 1.0 — Hardware-Agnostic Contract for Hosting Deck on Any SoC**

---

## 1. Why This Document Exists

Deck has two layers visible to applications:

- **The capability surface** (`03-deck-os`) — what apps see. Atom-named methods, typed arguments, Result-typed returns. Pure abstraction.
- **The platform implementation** (`13-deck-cyberdeck-platform` for ESP32-S3, others to come) — what actually runs. ESP-IDF components, FreeRTOS tasks, hardware drivers.

There has been no formal interface between them. A platform implementor had to read `03-deck-os` to know *what* to provide, and `06-deck-native` to know the C API for registering it, and then improvise everything in between.

This document is the missing layer: a **Service Driver Interface (SDI)** — a hardware-agnostic vtable that any host platform must implement to support Deck. It maps each capability in `03-deck-os` to a precise C contract: function signatures, lifecycle, threading, error codes, ownership rules.

Once a platform implements the SDI, the Deck runtime, all bridge UI services, and every Deck app run on it without changes. New SoCs (RP2350, STM32, ESP32-C6, future Espressif targets, hosted Linux/macOS/Windows for development) become Deck targets by writing one SDI implementation.

### Layering recap

```
┌─────────────────────────────────────────────────┐
│  Deck apps (.deck files)                        │
│  Use capability paths declared in 03-deck-os     │
├─────────────────────────────────────────────────┤
│  Deck runtime (11-deck-implementation)          │
│  Pure C99, single-threaded VM model             │
├─────────────────────────────────────────────────┤
│  Service Driver Interface (THIS DOCUMENT)       │
│  Hardware-agnostic vtable — the platform contract│
├─────────────────────────────────────────────────┤
│  Per-SoC implementation (13-cyberdeck-platform) │
│  ESP-IDF components, drivers, services          │
├─────────────────────────────────────────────────┤
│  SoC HAL + hardware                              │
└─────────────────────────────────────────────────┘
```

The SDI is the exact line at which portability lives. Above the line, everything is identical across platforms. Below the line, every SoC is different.

### Companion documents

| Doc | Role |
|---|---|
| `03-deck-os` | What capabilities exist (the apps' view) |
| `06-deck-native` | How to register native code with the runtime |
| `11-deck-implementation` | How the runtime works internally |
| `13-deck-cyberdeck-platform` | The reference SDI implementation on ESP32-S3 |
| `14-deck-components` | How SDIs and components are packaged and published |

---

## 2. Service Driver Catalog

Every service driver has a globally unique **driver path** (mirrored from the capability path it backs) and a **vtable** of function pointers. The runtime composes the active set of drivers at boot.

```
                 Service Driver                             Capabilities backed
─────────────────────────────────────────────────────────  ─────────────────────────────
deck.driver.storage.local          (KV string store)        storage.local
deck.driver.storage.nvs            (atomic flash KV)        nvs
deck.driver.storage.fs             (file system)            fs
deck.driver.storage.db             (SQL database)           db
deck.driver.storage.cache          (in-memory LRU)          cache
deck.driver.network.wifi           (WiFi station)           network.wifi
deck.driver.network.http           (HTTP/HTTPS client)      network.http, api_client
deck.driver.network.socket         (raw TCP/UDP)            network.socket
deck.driver.network.mqtt           (MQTT client)            mqtt
deck.driver.network.downloader     (queued large downloads) (used by ota.app, media)
deck.driver.network.notifications  (poll/MQTT-driven push)  notifications
deck.driver.display.panel          (raster output)          (consumed by bridge UI)
deck.driver.display.touch          (capacitive touch input) (consumed by bridge UI)
deck.driver.display.theme          (UI theme registry)      display.theme
deck.driver.display.notify         (transient toast)        display.notify
deck.driver.display.screen         (brightness / power)     display.screen
deck.driver.bridge.ui              (LVGL or other GUI lib)  (consumes display.* drivers)
deck.driver.system.info            (device id, model)       system.info
deck.driver.system.locale          (locale, timezone)       system.locale
deck.driver.system.time            (NTP + RTC)              system.time
deck.driver.system.battery         (level, charging)        system.battery
deck.driver.system.security        (PIN, lockscreen, perms) system.security
deck.driver.system.shell           (status bar, navbar)     system.shell
deck.driver.system.apps            (app stack management)   system.apps
deck.driver.system.tasks           (process monitor)        system.tasks
deck.driver.system.crashes         (crash log read)         system.crashes
deck.driver.system.audio           (audio out)              system.audio
deck.driver.ota.firmware           (whole-device update)    ota
deck.driver.ota.app                (per-app bundle update)  (used by app installer)
deck.driver.ble                    (Bluetooth Low Energy)   ble
deck.driver.bt_classic             (Bluetooth Classic)      bt_classic
deck.driver.crypto.aes             (AES encryption)         crypto.aes
deck.driver.sensors.<kind>         (one per sensor type)    sensors.*
deck.driver.hardware.uart          (raw UART ports)         hardware.uart
deck.driver.hardware.gpio          (raw GPIO; rare)         (out of public surface)
deck.driver.background_fetch       (periodic wake)          background_fetch
```

A platform need not provide every driver. Drivers absent at boot mark their backing capabilities as **unavailable**. Apps that declared the capability with `optional` continue to run; apps that declared it as required fail to launch with a clear error.

A platform MAY provide additional drivers beyond this catalog by registering custom capabilities (`06-deck-native §10`). Custom drivers follow the same contract.

---

## 3. The DeckService Vtable Contract

Every driver is a struct named `Deck<Service>Driver` whose first field is a `DeckServiceHeader`:

```c
typedef struct {
  const char     *driver_path;        /* "deck.driver.storage.fs" */
  uint16_t        sdi_version;         /* SDI version this driver targets; see §11 */
  uint16_t        impl_version;        /* implementation's own semver-encoded version */
  const char     *impl_name;           /* "esp_idf_v6.0_sdspi", "rp2350_littlefs", etc. */
  uint32_t        capability_flags;    /* bitmap of optional features supported */
} DeckServiceHeader;

typedef struct DeckServiceDriver {
  DeckServiceHeader header;
  /* per-service function pointers follow */
} DeckServiceDriver;
```

`sdi_version` is the version of THIS specification that the driver targets. The runtime refuses to load drivers whose `sdi_version` is incompatible. Initial value: `0x0100` (1.0).

`capability_flags` is per-service; defined in each service section below.

### 3.1 Lifecycle pattern

Every driver implements a four-phase lifecycle:

```c
typedef enum {
  DECK_DRIVER_STATE_UNINITIALIZED = 0,
  DECK_DRIVER_STATE_INITIALIZED,
  DECK_DRIVER_STATE_RUNNING,
  DECK_DRIVER_STATE_FAULTED
} DeckDriverState;

typedef struct {
  /* Mandatory lifecycle hooks. NULL means "no-op success." */
  DeckResult (*init)   (void *config, void **driver_handle);
  DeckResult (*start)  (void *driver_handle);
  DeckResult (*stop)   (void *driver_handle);
  DeckResult (*destroy)(void *driver_handle);
  DeckDriverState (*state)(void *driver_handle);
} DeckLifecycleOps;
```

- `init(config, &handle)` — Allocate state, validate config. Move to INITIALIZED. Idempotent.
- `start(handle)` — Begin operation. Open hardware, spawn background tasks, register handlers. Move to RUNNING.
- `stop(handle)` — Cease operation. Cancel pending work. Move back to INITIALIZED.
- `destroy(handle)` — Release all resources. Driver handle becomes invalid.
- `state(handle)` — Report current state. Always callable.

The runtime calls them in order at boot: `init → start`. At shutdown: `stop → destroy`. On suspend (light sleep): typically `stop` is not called; the driver retains state but reduces activity. On termination of the whole runtime: `stop → destroy`.

If `init` or `start` returns `:err`, the driver moves to FAULTED. The runtime logs and continues; capabilities backed by FAULTED drivers report `:unavailable`.

### 3.2 Result type

```c
typedef struct {
  bool        ok;
  DeckAtomId  err_atom;        /* meaningful only if !ok */
  const char *err_message;     /* may be NULL; caller does not free */
  void       *value;           /* meaningful only if ok; type per call */
} DeckResult;

#define DECK_OK(v)      ((DeckResult){.ok = true,  .value = (v)})
#define DECK_ERR(a, m)  ((DeckResult){.ok = false, .err_atom = (a), .err_message = (m)})
```

The runtime translates `DeckResult` to a Deck-side `Result T E` value via the dispatcher's marshaling layer (`11-deck-implementation §12`). Drivers do not construct Deck values themselves — they return raw C values, and the dispatcher wraps them.

### 3.3 Threading contract

Each driver declares its threading contract via `capability_flags`:

```c
#define DECK_DRIVER_THREADING_MAIN_LOOP   (1u << 0)  /* All calls must come from the runtime thread */
#define DECK_DRIVER_THREADING_ANY_THREAD  (1u << 1)  /* Calls are safe from any thread */
#define DECK_DRIVER_THREADING_ISR_SAFE    (1u << 2)  /* A subset of methods marked ISR-safe (rare) */
#define DECK_DRIVER_THREADING_INTERNAL    (1u << 3)  /* Driver owns one or more background threads */
```

A driver with `THREADING_MAIN_LOOP` is the most common — it is invoked synchronously from the runtime's effect dispatcher. A driver with `THREADING_INTERNAL` owns its own task(s) and posts results back via the runtime mailbox (`11-deck-implementation §19`).

### 3.4 Memory contract

Drivers MUST follow ownership rules from `06-deck-native §14` and `11-deck-implementation §4.5`. Briefly:

- Buffers passed to drivers are borrowed unless documented otherwise.
- Buffers returned from drivers are caller-owned; the caller releases via the matching free function the driver provides.
- Allocations within a driver should use the supplied `DeckAllocator` (passed via `init.config`) so the platform can route through `heap_caps_*` or equivalent.
- No driver may hold a `DeckValue*` across calls.

### 3.5 Error atom vocabulary

Common error atoms reused across drivers (each driver section also lists service-specific atoms):

| Atom | Meaning |
|---|---|
| `:unavailable` | Driver / hardware not present |
| `:permission` | Caller lacks the required permission |
| `:io` | Underlying I/O failed |
| `:timeout` | Operation timed out |
| `:busy` | Resource currently in use; try again |
| `:not_found` | Named resource does not exist |
| `:invalid` | Argument failed validation |
| `:full` | Container full (storage, queue) |
| `:closed` | Resource was closed before operation completed |
| `:cancelled` | Operation was cancelled |
| `:no_memory` | Allocation failed |
| `:not_supported` | Driver does not implement this method on this platform |
| `:internal` | Unrecoverable internal error in the driver |

Service-specific atoms add to (never override) this set.

---

## 4. Storage Service Drivers

### 4.1 `deck.driver.storage.local` — Simple KV string store

Backs `storage.local`. File-backed, sandboxed per app. Survives reboot but not filesystem corruption.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  /* Per-app namespacing: app_id is opaque to the driver but used as scope */
  DeckResult (*get)   (void *h, const char *app_id, const char *key,
                       char **out_value, size_t *out_len);   /* returns malloc'd buffer */
  DeckResult (*set)   (void *h, const char *app_id, const char *key,
                       const char *value, size_t len);
  DeckResult (*delete)(void *h, const char *app_id, const char *key);
  DeckResult (*keys)  (void *h, const char *app_id,
                       char ***out_keys, size_t *out_count);
  DeckResult (*clear) (void *h, const char *app_id);
  void       (*free_buf)(void *h, void *buf);
} DeckStorageLocalDriver;
```

Errors: `:full`, `:permission`, `:corrupt`, `:io`.

Capability flags: none currently defined.

Threading: typically `MAIN_LOOP`. A driver MAY mark itself `INTERNAL` if it asynchronously batches writes.

### 4.2 `deck.driver.storage.nvs` — Atomic flash KV

Backs `nvs`. Per-key atomic. Always survives power loss. Bounded key length.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*get)     (void *h, const char *app_id, const char *key,
                          char **out, size_t *out_len);
  DeckResult (*get_int) (void *h, const char *app_id, const char *key, int64_t *out);
  DeckResult (*set)     (void *h, const char *app_id, const char *key,
                          const char *value, size_t len);
  DeckResult (*set_int) (void *h, const char *app_id, const char *key, int64_t value);
  DeckResult (*delete)  (void *h, const char *app_id, const char *key);
  DeckResult (*keys)    (void *h, const char *app_id,
                          char ***out_keys, size_t *out_count);
  DeckResult (*clear)   (void *h, const char *app_id);
  void       (*free_buf)(void *h, void *buf);

  /* Implementation-specific limits exposed for runtime validation */
  uint16_t   max_key_length;       /* most NVS impls cap at 15 */
  uint32_t   max_value_bytes;      /* per-value cap */
} DeckStorageNvsDriver;
```

Errors: `:full`, `:not_found`, `:invalid_key` (key too long), `:write_fail`.

Capability flags:

```c
#define DECK_NVS_FLAG_ENCRYPTED       (1u << 4)  /* Driver provides encryption-at-rest */
#define DECK_NVS_FLAG_NAMESPACED      (1u << 5)  /* Driver enforces app_id namespace; otherwise runtime prefixes keys */
```

The runtime always passes `app_id` to scope operations; drivers without `FLAG_NAMESPACED` must prefix keys themselves with `app_id + ":"`.

### 4.3 `deck.driver.storage.fs` — File system

Backs `fs`. Sandboxed per app to a virtual root the driver presents as `/`.

```c
typedef struct DeckFsHandle DeckFsHandle;   /* opaque file handle */

typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*open) (void *h, const char *app_id, const char *path,
                      uint32_t mode_flags, DeckFsHandle **out);
  DeckResult (*read) (void *h, DeckFsHandle *fh, void *buf, size_t len, size_t *out_read);
  DeckResult (*write)(void *h, DeckFsHandle *fh, const void *buf, size_t len, size_t *out_wr);
  DeckResult (*seek) (void *h, DeckFsHandle *fh, int64_t offset, int whence, int64_t *out_pos);
  DeckResult (*close)(void *h, DeckFsHandle *fh);
  DeckResult (*stat) (void *h, const char *app_id, const char *path, DeckFsStat *out);
  DeckResult (*list) (void *h, const char *app_id, const char *dir,
                      DeckFsEntry **out_entries, size_t *out_count);
  DeckResult (*mkdir)(void *h, const char *app_id, const char *path);
  DeckResult (*remove)(void *h, const char *app_id, const char *path);
  DeckResult (*rename)(void *h, const char *app_id, const char *from, const char *to);
  DeckResult (*sync) (void *h, DeckFsHandle *fh);   /* fsync-equivalent */
  void       (*free_entries)(void *h, DeckFsEntry *entries, size_t count);

  /* Mount status for graceful degradation (e.g. removable SD) */
  bool      (*is_mounted)(void *h);
  /* Posts to the runtime event bus on mount/unmount transitions */
  void      (*set_mount_callback)(void *h,
                                   void (*cb)(bool mounted, void *user),
                                   void *user);
} DeckStorageFsDriver;
```

Path validation: drivers MUST reject `..` and absolute paths that escape the per-app sandbox; the canonical sandbox root is `/<app_id>/` from the driver's perspective.

Errors: `:not_found`, `:permission`, `:io`, `:invalid` (bad path), `:full`, `:closed`.

Capability flags:

```c
#define DECK_FS_FLAG_REMOVABLE         (1u << 4)  /* Storage is removable (SD card); set_mount_callback meaningful */
#define DECK_FS_FLAG_SUPPORTS_SYMLINKS (1u << 5)
#define DECK_FS_FLAG_CASE_SENSITIVE    (1u << 6)
```

### 4.4 `deck.driver.storage.db` — SQL database

Backs `db`. One database per app. Full SQL with prepared statements and transactions.

```c
typedef struct DeckDbConn DeckDbConn;
typedef struct DeckDbStmt DeckDbStmt;

typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*open)    (void *h, const char *app_id, DeckDbConn **out);
  DeckResult (*close)   (void *h, DeckDbConn *conn);
  DeckResult (*exec)    (void *h, DeckDbConn *conn, const char *sql);
  DeckResult (*prepare) (void *h, DeckDbConn *conn, const char *sql, DeckDbStmt **out);
  DeckResult (*bind_int)   (void *h, DeckDbStmt *s, int idx, int64_t v);
  DeckResult (*bind_double)(void *h, DeckDbStmt *s, int idx, double v);
  DeckResult (*bind_text)  (void *h, DeckDbStmt *s, int idx, const char *v, size_t len);
  DeckResult (*bind_blob)  (void *h, DeckDbStmt *s, int idx, const void *v, size_t len);
  DeckResult (*bind_null)  (void *h, DeckDbStmt *s, int idx);
  DeckResult (*step)       (void *h, DeckDbStmt *s, bool *out_has_row);
  /* Column accessors valid only after step → has_row=true */
  int64_t     (*col_int)   (void *h, DeckDbStmt *s, int col);
  double      (*col_double)(void *h, DeckDbStmt *s, int col);
  const char *(*col_text)  (void *h, DeckDbStmt *s, int col, size_t *out_len);
  const void *(*col_blob)  (void *h, DeckDbStmt *s, int col, size_t *out_len);
  bool        (*col_is_null)(void *h, DeckDbStmt *s, int col);
  DeckResult  (*reset)     (void *h, DeckDbStmt *s);
  DeckResult  (*finalize)  (void *h, DeckDbStmt *s);
  DeckResult  (*begin_txn) (void *h, DeckDbConn *conn);
  DeckResult  (*commit)    (void *h, DeckDbConn *conn);
  DeckResult  (*rollback)  (void *h, DeckDbConn *conn);
  int64_t     (*last_insert_rowid)(void *h, DeckDbConn *conn);
  int         (*changes)   (void *h, DeckDbConn *conn);
} DeckStorageDbDriver;
```

The interface is intentionally close to SQLite's because SQLite is the obvious reference implementation. Other backends (a simple journal-backed KV, a remote SQL service) can implement the same interface.

Errors: `:not_found`, `:permission`, `:io`, `:invalid` (SQL parse), `:busy` (locked), `:constraint`.

### 4.5 `deck.driver.storage.cache` — In-memory LRU

Backs `cache`. Per-app, sized at registration time. Volatile (lost on suspend or reboot).

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*get)   (void *h, const char *app_id, const char *key,
                        void **out_buf, size_t *out_len, uint64_t *out_expires_ms);
  DeckResult (*set)   (void *h, const char *app_id, const char *key,
                        const void *buf, size_t len, uint64_t ttl_ms);
  DeckResult (*delete)(void *h, const char *app_id, const char *key);
  DeckResult (*clear) (void *h, const char *app_id);
  size_t     (*size)  (void *h, const char *app_id);
  void       (*free_buf)(void *h, void *buf);
} DeckStorageCacheDriver;
```

Errors: `:not_found`, `:full` (eviction failed to make room).

---

## 5. Network Service Drivers

### 5.1 `deck.driver.network.wifi` — Station mode

Backs `network.wifi`. Scan / connect / disconnect / forget. Posts events to the runtime event bus.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*scan)        (void *h, DeckWifiScanResult **out, size_t *out_count);
  DeckResult (*connect)     (void *h, const char *ssid, const char *psk);
  DeckResult (*disconnect)  (void *h);
  DeckResult (*forget)      (void *h, const char *ssid);
  DeckResult (*status)      (void *h, DeckWifiStatus *out);
  void       (*free_scan)   (void *h, DeckWifiScanResult *r, size_t n);

  /* Power-save hint: 0 = none, 1 = min modem, 2 = max modem */
  DeckResult (*set_power_save)(void *h, uint8_t mode);
} DeckNetworkWifiDriver;
```

Capability flags:

```c
#define DECK_WIFI_FLAG_AP_MODE         (1u << 4)  /* Driver supports SoftAP */
#define DECK_WIFI_FLAG_5GHZ            (1u << 5)
#define DECK_WIFI_FLAG_WPA3            (1u << 6)
```

Errors: `:not_found` (unknown SSID), `:permission` (auth failed), `:timeout`, `:busy` (already connecting).

The driver MUST post `os.wifi_changed` events through the runtime's event API when association state changes (`bridge.event_emit` per `06-deck-native §9.2`).

### 5.2 `deck.driver.network.http` — HTTPS client

Backs `network.http` and `api_client`. Per-call request/response with TLS, headers, body.

```c
typedef struct DeckHttpRequest DeckHttpRequest;   /* opaque */

typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*request)(void *h,
                        const char *method,    /* "GET", "POST", ... */
                        const char *url,
                        const DeckHttpHeader *headers, size_t header_count,
                        const void *body, size_t body_len,
                        const DeckHttpOptions *opts,
                        DeckHttpResponse **out);
  /* Async: returns request_id; result via runtime mailbox MSG_EFFECT_RESULT */
  DeckResult (*request_async)(void *h, /* same params */, uint64_t *out_request_id);
  DeckResult (*cancel)  (void *h, uint64_t request_id);
  void       (*free_response)(void *h, DeckHttpResponse *resp);

  /* TLS trust map management */
  DeckResult (*tls_add_root)(void *h, const char *for_domain,
                              const uint8_t *cert_pem, size_t pem_len);
  DeckResult (*tls_clear_app)(void *h, const char *app_id);
} DeckNetworkHttpDriver;

typedef struct {
  uint32_t  timeout_ms;
  bool      follow_redirects;
  uint8_t   max_redirects;
  const char *user_agent;
  const uint8_t *tls_ca_cert_override;     /* per-call override */
  size_t      tls_ca_cert_override_len;
  bool      stream_body;                    /* if true, response body is delivered in chunks */
} DeckHttpOptions;
```

Capability flags:

```c
#define DECK_HTTP_FLAG_HTTP2       (1u << 4)
#define DECK_HTTP_FLAG_HTTP3       (1u << 5)
#define DECK_HTTP_FLAG_BROTLI      (1u << 6)
#define DECK_HTTP_FLAG_KEEP_ALIVE  (1u << 7)
#define DECK_HTTP_FLAG_STREAM_BODY (1u << 8)
```

Errors: `:dns_fail`, `:connect_fail`, `:tls_fail`, `:timeout`, `:cancelled`, `:no_memory`, `:status_4xx`, `:status_5xx`.

Threading: typically `INTERNAL` — the driver owns network worker tasks and serves both sync and async via its own thread pool.

### 5.3 `deck.driver.network.socket` — Raw TCP/UDP

Backs `network.socket`. For protocols not covered by HTTP/MQTT.

```c
typedef struct DeckSocket DeckSocket;

typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*open)    (void *h, DeckSocketKind kind, DeckSocket **out);
  DeckResult (*connect) (void *h, DeckSocket *s, const char *host, uint16_t port);
  DeckResult (*send)    (void *h, DeckSocket *s, const void *buf, size_t len, size_t *out_sent);
  DeckResult (*recv)    (void *h, DeckSocket *s, void *buf, size_t cap,
                          size_t *out_read, uint32_t timeout_ms);
  DeckResult (*close)   (void *h, DeckSocket *s);
  DeckResult (*set_opt) (void *h, DeckSocket *s, DeckSocketOpt opt, const void *val, size_t len);
} DeckNetworkSocketDriver;
```

Errors: `:dns_fail`, `:connect_fail`, `:closed`, `:timeout`, `:io`.

### 5.4 `deck.driver.network.mqtt` — MQTT client

Backs `mqtt`. One client per (broker, app) pair.

```c
typedef struct DeckMqttClient DeckMqttClient;

typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*connect)    (void *h, const DeckMqttConfig *cfg, DeckMqttClient **out);
  DeckResult (*disconnect) (void *h, DeckMqttClient *c);
  DeckResult (*publish)    (void *h, DeckMqttClient *c, const char *topic,
                             const void *payload, size_t len, uint8_t qos, bool retain);
  DeckResult (*subscribe)  (void *h, DeckMqttClient *c, const char *topic_filter, uint8_t qos);
  DeckResult (*unsubscribe)(void *h, DeckMqttClient *c, const char *topic_filter);
  /* Inbound delivery via callback registered at connect time (in DeckMqttConfig). */
} DeckNetworkMqttDriver;
```

Capability flags:

```c
#define DECK_MQTT_FLAG_V5          (1u << 4)
#define DECK_MQTT_FLAG_TLS         (1u << 5)
#define DECK_MQTT_FLAG_WEBSOCKET   (1u << 6)
```

### 5.5 `deck.driver.network.downloader` — Queued large downloads

This driver is **not directly exposed as a Deck capability** in the user-app surface. It is a shared service consumed by:

- `deck.driver.ota.app` (downloading new app bundles to SD staging)
- `deck.driver.ota.firmware` (downloading firmware images, in implementations that do not use `esp_https_ota` directly)
- Multimedia apps (downloading podcasts, music files, large images) via custom builtins exposed in their `.deck-os`

The downloader is queued, supports HTTP Range resume, and writes to a destination path provided by the caller (typically on the SD).

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  /* Enqueue: returns immediately with job_id. Progress and completion via callback. */
  DeckResult (*enqueue)(void *h,
                        const char *url,
                        const char *dest_path,           /* fs path, driver-validated */
                        const DeckDownloadOptions *opts,
                        DeckDownloadCallbacks *cbs,
                        uint64_t *out_job_id);
  DeckResult (*cancel) (void *h, uint64_t job_id);
  DeckResult (*pause)  (void *h, uint64_t job_id);
  DeckResult (*resume) (void *h, uint64_t job_id);
  DeckResult (*status) (void *h, uint64_t job_id, DeckDownloadStatus *out);
  DeckResult (*list)   (void *h, DeckDownloadStatus **out, size_t *out_count);
} DeckNetworkDownloaderDriver;

typedef struct {
  uint32_t  retry_count;
  uint32_t  retry_backoff_ms;
  bool      verify_sha256;
  uint8_t   expected_sha256[32];   /* if verify_sha256 */
  uint64_t  resume_offset;         /* 0 to start from beginning */
  uint32_t  rate_limit_kbps;       /* 0 = unlimited */
  bool      delete_partial_on_cancel;
} DeckDownloadOptions;

typedef struct {
  void (*on_progress)(uint64_t job_id, uint64_t bytes_downloaded,
                      uint64_t total_bytes, void *user);
  void (*on_complete)(uint64_t job_id, DeckResult result, void *user);
  void *user;
} DeckDownloadCallbacks;
```

Capability flags:

```c
#define DECK_DOWNLOADER_FLAG_RESUME       (1u << 4)
#define DECK_DOWNLOADER_FLAG_RATE_LIMIT   (1u << 5)
#define DECK_DOWNLOADER_FLAG_PERSISTENT   (1u << 6)  /* Queue survives reboot via on-disk journal */
```

Errors: `:dns_fail`, `:connect_fail`, `:tls_fail`, `:hash_mismatch`, `:disk_full`, `:cancelled`, `:not_found` (job_id), `:no_resume_support` (server refused Range request).

### 5.6 `deck.driver.network.notifications` — Push notification source poller

Backs `notifications`. Manages per-app notification sources that poll HTTP endpoints or subscribe to MQTT topics, extract entries via JSONPath, deduplicate by id, and store them.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*post_local)        (void *h, const char *app_id, const DeckNotifLocal *opts,
                                    char **out_id);
  DeckResult (*list)              (void *h, const char *app_id,
                                    DeckNotifEntry **out, size_t *out_count);
  DeckResult (*unread_count)      (void *h, const char *app_id, uint32_t *out);
  DeckResult (*mark_read)         (void *h, const char *app_id, const char *id);
  DeckResult (*clear)             (void *h, const char *app_id, const char *id);
  DeckResult (*register_source)   (void *h, const char *app_id, const DeckNotifSource *src);
  DeckResult (*unregister_source) (void *h, const char *app_id, const char *source_id);
  DeckResult (*sources)           (void *h, const char *app_id,
                                    DeckNotifSource **out, size_t *out_count);
  void       (*free_entries)      (void *h, DeckNotifEntry *e, size_t n);
  void       (*free_sources)      (void *h, DeckNotifSource *s, size_t n);
} DeckNetworkNotificationsDriver;
```

The driver typically has `THREADING_INTERNAL` and runs its own polling task that fires `os.notification` events.

---

## 6. Display Service Drivers

### 6.1 `deck.driver.display.panel` — Raster output

Provides the framebuffer the bridge UI library renders into. Not directly exposed as a capability — consumed by `deck.driver.bridge.ui`.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  void       (*get_geometry)(void *h, DeckPanelGeometry *out);
  /* Push a region of pixels. Blocking until the panel has accepted them. */
  DeckResult (*flush)       (void *h, const DeckPixelRegion *region,
                              const void *pixels, size_t pixel_count);
  /* For panels with internal frame buffers, swap them. Returns immediately. */
  DeckResult (*swap_buffers)(void *h);
  /* Register a callback fired on each VSYNC (for tear-free rendering). */
  void       (*set_vsync_cb)(void *h, void (*cb)(void *user), void *user);
  /* Brightness 0.0 to 1.0; backlight on/off. */
  DeckResult (*set_brightness)(void *h, float level);
  DeckResult (*set_on)       (void *h, bool on);
} DeckDisplayPanelDriver;

typedef struct {
  uint16_t  width;
  uint16_t  height;
  uint8_t   bpp;                 /* bits per pixel */
  uint8_t   pixel_format;        /* 0 = RGB565, 1 = ARGB8888, ... */
  bool      has_internal_fb;     /* if true, swap_buffers is meaningful */
  uint8_t   buffer_count;        /* 1, 2, 3 — anti-tear capability */
} DeckPanelGeometry;
```

Capability flags:

```c
#define DECK_PANEL_FLAG_ROTATABLE       (1u << 4)
#define DECK_PANEL_FLAG_PARTIAL_UPDATE  (1u << 5)
#define DECK_PANEL_FLAG_HARDWARE_SCROLL (1u << 6)
#define DECK_PANEL_FLAG_DMA_FLUSH       (1u << 7)
```

### 6.2 `deck.driver.display.touch` — Capacitive touch input

Provides touch events to the bridge UI library.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  /* Poll-based: runtime calls read; driver returns current touch state. */
  DeckResult (*read)        (void *h, DeckTouchState *out);
  /* Or interrupt-based: driver invokes callback on touch events. */
  void       (*set_callback)(void *h, void (*cb)(const DeckTouchState*, void *user),
                              void *user);
  DeckResult (*calibrate)   (void *h, const DeckTouchCalibration *cal);
} DeckDisplayTouchDriver;

typedef struct {
  bool      pressed;
  uint8_t   point_count;
  struct {
    int16_t x;
    int16_t y;
    uint8_t pressure;     /* 0–255 if available; else 0 */
  } points[5];
  uint64_t  timestamp_ms;
} DeckTouchState;
```

Capability flags: `MULTITOUCH`, `PRESSURE`, `GESTURE_DETECTION` (built-in swipe/pinch detection), `HOVER`.

### 6.3 `deck.driver.bridge.ui` — UI bridge

Implements the bridge UI services from `10-deck-bridge-ui` on top of the panel + touch drivers. This is **the** consumer of `display.panel` and `display.touch`. It is not exposed as a Deck capability; rather it implements `bridge.render` (`11-deck-implementation §19`).

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  /* Receive a DVC tree from the runtime. The driver renders it to the panel. */
  DeckResult (*render)             (void *h, const DvcNode *tree);
  /* UI services from 10-deck-bridge-ui */
  void       (*toast)              (void *h, const char *message, uint16_t ms);
  void       (*confirm)            (void *h, const char *title, const char *body,
                                     const char *confirm_label, const char *cancel_label,
                                     void (*cb)(bool confirmed, void *user), void *user);
  void       (*loading_show)       (void *h, const char *label);
  void       (*loading_hide)       (void *h);
  void       (*progress_show)      (void *h, const char *title, bool cancellable);
  void       (*progress_set)       (void *h, float percent);  /* -1 = indeterminate */
  void       (*progress_hide)      (void *h);
  void       (*keyboard_show)      (void *h, DeckKeyboardKind kind);
  void       (*keyboard_hide)      (void *h);
  /* Statusbar/Navbar */
  void       (*set_statusbar)      (void *h, const DeckStatusbarSpec *spec);
  void       (*set_navbar)         (void *h, const DeckNavbarSpec *spec);
  /* Theme */
  DeckResult (*set_theme)          (void *h, DeckThemeId theme);
} DeckBridgeUiDriver;
```

A platform may bind LVGL, SDL, raylib, terminal-mode, or a custom renderer here. The contract is the `render(tree)` function and the UI service primitives.

### 6.4 `deck.driver.display.theme` — Theme registry

Backs `display.theme`. Stores the current theme, exposes a stream that emits on theme change, and lets system apps switch.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*current)(void *h, DeckThemeId *out);
  DeckResult (*set)    (void *h, DeckThemeId theme);    /* fires watch() subscribers */
  void       (*set_watch_callback)(void *h,
                                    void (*cb)(DeckThemeId, void *user),
                                    void *user);
} DeckDisplayThemeDriver;
```

### 6.5 `deck.driver.display.notify` — Transient toast

Backs `display.notify`. Thin wrapper over the bridge UI's `toast` API exposed at the Deck level.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  void (*send)   (void *h, const char *app_id, const char *msg, DeckNotifLevel level);
  void (*dismiss)(void *h, const char *app_id);
} DeckDisplayNotifyDriver;
```

### 6.6 `deck.driver.display.screen` — Brightness / power

Backs `display.screen`.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*set_brightness)(void *h, float level);
  DeckResult (*get_brightness)(void *h, float *out);
  DeckResult (*set_on)        (void *h, bool on);
  DeckResult (*is_on)         (void *h, bool *out);
} DeckDisplayScreenDriver;
```

---

## 7. System Service Drivers

### 7.1 `deck.driver.system.info`, `system.locale`, `system.time`, `system.battery`

Each backs the eponymous capability. Standardized APIs:

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;
  DeckResult (*device_id)   (void *h, char *out, size_t cap);
  DeckResult (*device_model)(void *h, char *out, size_t cap);
  DeckResult (*os_version)  (void *h, char *out, size_t cap);
  DeckResult (*app_version) (void *h, const char *app_id, char *out, size_t cap);
  DeckResult (*free_heap)   (void *h, uint32_t *out);
  DeckResult (*uptime_ms)   (void *h, uint64_t *out);
} DeckSystemInfoDriver;

typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;
  DeckResult (*locale)        (void *h, char *out, size_t cap);    /* "en_US" */
  DeckResult (*set_locale)    (void *h, const char *locale);
  DeckResult (*timezone)      (void *h, char *out, size_t cap);    /* "America/Mexico_City" */
  DeckResult (*set_timezone)  (void *h, const char *tz);
  DeckResult (*country_code)  (void *h, char *out, size_t cap);    /* "MX" */
} DeckSystemLocaleDriver;

typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;
  DeckResult (*now_unix_ms)   (void *h, int64_t *out);
  DeckResult (*sync_ntp)      (void *h, const char *server);
  DeckResult (*set_time)      (void *h, int64_t unix_ms);
  bool       (*is_synced)     (void *h);
} DeckSystemTimeDriver;

typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;
  DeckResult (*level_pct)     (void *h, uint8_t *out);
  DeckResult (*charging)      (void *h, bool *out);
  void       (*set_charging_callback)(void *h,
                                       void (*cb)(bool charging, void *user),
                                       void *user);
} DeckSystemBatteryDriver;
```

### 7.2 `deck.driver.system.security` — Lockscreen, PIN, permissions

Backs `system.security`. Holds the PIN hash, manages lock state, persists permission grants.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*lock)          (void *h);
  DeckResult (*unlock)        (void *h, const char *pin);
  bool       (*is_locked)     (void *h);
  bool       (*pin_enabled)   (void *h);
  DeckResult (*set_pin)       (void *h, const char *pin);    /* hashed by driver */
  DeckResult (*clear_pin)     (void *h);
  DeckResult (*auto_lock_ms)  (void *h, uint32_t *out);      /* 0 = disabled */
  DeckResult (*set_auto_lock_ms)(void *h, uint32_t ms);

  /* Permissions */
  DeckResult (*permission_get)(void *h, const char *app_id, const char *cap_path,
                                DeckPermissionState *out);
  DeckResult (*permission_set)(void *h, const char *app_id, const char *cap_path,
                                DeckPermissionState state);
  DeckResult (*permission_request)(void *h, const char *app_id,
                                    const char *cap_path, const char *reason,
                                    void (*cb)(DeckPermissionState, void *user), void *user);
} DeckSystemSecurityDriver;
```

Capability flags:

```c
#define DECK_SECURITY_FLAG_HARDWARE_KEYSTORE  (1u << 4)  /* PIN hash protected by HMAC eFuse, etc. */
#define DECK_SECURITY_FLAG_BIOMETRIC          (1u << 5)  /* Future */
```

### 7.3 `deck.driver.system.shell`, `system.apps`, `system.tasks`, `system.crashes`

Backs the privileged system capabilities. Together they implement the OS shell from `09-deck-shell`.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  /* Statusbar / navbar (UI bridge concerns; mirrored to bridge.ui) */
  DeckResult (*set_statusbar_visible)(void *h, bool visible);
  DeckResult (*set_navbar_visible)   (void *h, bool visible);
  /* App stack control */
  DeckResult (*push_app)             (void *h, const char *app_id);
  DeckResult (*pop_to_launcher)      (void *h);
  /* System notifications */
  DeckResult (*post_notification)    (void *h, const DeckSysNotif *opts);
  DeckResult (*clear_notification)   (void *h, const char *id);
} DeckSystemShellDriver;

typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*list_running)   (void *h, DeckAppInfo **out, size_t *out_count);
  DeckResult (*list_suspended) (void *h, DeckAppInfo **out, size_t *out_count);
  DeckResult (*list_installed) (void *h, DeckAppInfo **out, size_t *out_count);
  DeckResult (*launch)         (void *h, const char *app_id);
  DeckResult (*launch_url)     (void *h, const char *app_id, const char *url);
  DeckResult (*bring_to_front) (void *h, const char *app_id);
  DeckResult (*kill)           (void *h, const char *app_id);
  DeckResult (*config_schema)  (void *h, const char *app_id,
                                 DeckConfigField **out, size_t *out_count);
  /* Stream of (app_id, unread_count) pairs for launcher badges */
  void       (*set_notif_counts_callback)(void *h,
                                           void (*cb)(const DeckAppNotifCount*, size_t n,
                                                      void *user),
                                           void *user);
  void       (*free_app_info)  (void *h, DeckAppInfo *a, size_t n);
  void       (*free_config)    (void *h, DeckConfigField *f, size_t n);
} DeckSystemAppsDriver;

typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*list_processes) (void *h, DeckProcessEntry **out, size_t *out_count);
  DeckResult (*kill_process)   (void *h, const char *id);
  DeckResult (*storage_info)   (void *h, const char *app_id, DeckStorageInfo *out);
  void       (*set_cpu_callback)(void *h,
                                  void (*cb)(const DeckProcessEntry*, size_t n, void *user),
                                  void *user);
  void       (*free_processes) (void *h, DeckProcessEntry *p, size_t n);
} DeckSystemTasksDriver;

typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*list)           (void *h, DeckCrashInfo **out, size_t *out_count);
  DeckResult (*list_for_app)   (void *h, const char *app_id,
                                 DeckCrashInfo **out, size_t *out_count);
  DeckResult (*record)         (void *h, const DeckCrashInfo *info);   /* called by runtime */
  DeckResult (*clear)          (void *h);
  DeckResult (*clear_for_app)  (void *h, const char *app_id);
  void       (*set_watch_callback)(void *h,
                                    void (*cb)(const DeckCrashInfo*, void *user),
                                    void *user);
  void       (*free)           (void *h, DeckCrashInfo *c, size_t n);
} DeckSystemCrashesDriver;
```

### 7.4 `deck.driver.system.audio` — Audio output

Backs `system.audio`.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*play_pcm)  (void *h, const void *pcm, size_t bytes,
                            uint32_t sample_rate_hz, uint8_t channels, uint8_t bits);
  DeckResult (*play_file) (void *h, const char *path);    /* WAV, MP3 if supported */
  DeckResult (*stop)      (void *h);
  DeckResult (*set_volume)(void *h, uint8_t pct);
  DeckResult (*get_volume)(void *h, uint8_t *out);
  bool       (*is_playing)(void *h);
} DeckSystemAudioDriver;
```

Capability flags: `BLUETOOTH_OUT`, `LINE_OUT`, `SPEAKER`, `MP3_DECODE`, `OPUS_DECODE`, `FLAC_DECODE`.

---

## 8. OTA Service Drivers

The OTA story has **two independent pipelines**: firmware (whole-device) and app (one bundle). They use separate drivers to keep concerns clean and to allow platforms with no firmware OTA capability (e.g. development build) to still ship app updates.

### 8.1 `deck.driver.ota.firmware` — Whole-device firmware update

Backs the `ota` capability. Updates the running OS image (interpreter, bridge, all built-in apps, kernel).

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  /* Check the manifest URL. Returns latest available version. */
  DeckResult (*check)         (void *h, const char *manifest_url, DeckOtaInfo *out);
  /* Begin download; progress via callback; result via callback. */
  DeckResult (*download)      (void *h, const char *url,
                                DeckOtaCallbacks *cbs, uint64_t *out_job_id);
  DeckResult (*cancel)        (void *h, uint64_t job_id);
  /* Apply a fully-downloaded update (typically reboots). */
  DeckResult (*apply)         (void *h, uint64_t job_id);
  /* Confirm a freshly-applied firmware (called from validation window). */
  DeckResult (*confirm)       (void *h);
  /* Rollback to previous slot (must succeed before next reboot). */
  DeckResult (*rollback)      (void *h);
  /* Status: which slot is active, which is pending, last attempted version. */
  DeckResult (*status)        (void *h, DeckFirmwareStatus *out);
} DeckOtaFirmwareDriver;
```

Capability flags:

```c
#define DECK_OTA_FW_FLAG_PARTITION_SWAP   (1u << 4)  /* Dual-bank A/B */
#define DECK_OTA_FW_FLAG_ANTI_ROLLBACK    (1u << 5)
#define DECK_OTA_FW_FLAG_SIGNATURE_VERIFY (1u << 6)
#define DECK_OTA_FW_FLAG_ENCRYPTED_IMAGE  (1u << 7)
#define DECK_OTA_FW_FLAG_DELTA            (1u << 8)  /* Differential updates */
```

Errors: `:dns_fail`, `:tls_fail`, `:hash_mismatch`, `:signature_invalid`, `:rollback_protected`, `:disk_full`, `:incompatible` (target SoC mismatch).

### 8.2 `deck.driver.ota.app` — Single-app bundle update

Backs the app installer used by the Launcher / Settings. Updates one app's bundle on the SD (or wherever the FS driver is mounted), without touching firmware.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  /* Check a manifest URL for a specific app. */
  DeckResult (*check)        (void *h, const char *app_id, const char *manifest_url,
                               DeckAppOtaInfo *out);
  /* Begin install/upgrade. The driver downloads the bundle (typically via the
     downloader driver), unpacks to staging, verifies, and atomically swaps. */
  DeckResult (*install)      (void *h, const char *app_id, const char *bundle_url,
                               const DeckAppOtaOptions *opts,
                               DeckAppOtaCallbacks *cbs, uint64_t *out_job_id);
  /* Cancel an in-progress install. Cleans up staging. */
  DeckResult (*cancel)       (void *h, uint64_t job_id);
  /* Roll an installed app back to its previous version (kept under .old/ for one cycle). */
  DeckResult (*rollback)     (void *h, const char *app_id);
  /* Uninstall an app entirely. */
  DeckResult (*uninstall)    (void *h, const char *app_id);
  /* List installed apps with their versions. */
  DeckResult (*list_installed)(void *h, DeckAppOtaInstalled **out, size_t *out_count);
} DeckOtaAppDriver;

typedef struct {
  bool      verify_signature;
  uint32_t  retry_count;
  bool      run_migrations;     /* default true */
  bool      keep_user_data;     /* default true; if false, /files and /cache wiped */
  bool      keep_previous;      /* default true; .old/{app_id} retained for rollback */
} DeckAppOtaOptions;

typedef struct {
  void (*on_progress)(uint64_t job_id, DeckAppOtaPhase phase,
                      uint64_t bytes_done, uint64_t bytes_total, void *user);
  void (*on_complete)(uint64_t job_id, DeckResult result, void *user);
  void *user;
} DeckAppOtaCallbacks;

typedef enum {
  DECK_APP_OTA_PHASE_DOWNLOADING,
  DECK_APP_OTA_PHASE_VERIFYING,
  DECK_APP_OTA_PHASE_UNPACKING,
  DECK_APP_OTA_PHASE_MIGRATING,
  DECK_APP_OTA_PHASE_SWAPPING,
  DECK_APP_OTA_PHASE_DONE,
} DeckAppOtaPhase;
```

The reference implementation of `deck.driver.ota.app`:

1. Calls `deck.driver.network.downloader.enqueue` to fetch the bundle to `<fs>/.staging/{app_id}.tar.zst` (or `.zip`, format is implementation choice).
2. Verifies SHA-256 of the downloaded blob against the manifest.
3. Verifies Ed25519 signature if `verify_signature` is set.
4. Unpacks the archive into `<fs>/.staging/{app_id}/` (atomic-rename target).
5. Reads the new `manifest.json`; checks compatibility (`min_deck_runtime`, `deck_os_version`).
6. If the app is currently running, sends `MSG_SUSPEND` to its VM (deadline 500 ms), then snapshots and frees its slot.
7. Atomically renames `apps/{app_id}` → `apps/.old/{app_id}` (for rollback) and `.staging/{app_id}` → `apps/{app_id}`.
8. Runs migrations against the app's persistent storage (NVS namespace, SQLite DB).
9. On success, fires `EVT_APP_INSTALLED` (and `os.app_installed` to system.crash_reporter / launcher).
10. On any failure, rolls back: rename `.old/{app_id}` → `apps/{app_id}`. The user-visible app continues from its previous version.
11. After a confidence period (next successful launch), `apps/.old/{app_id}` is deleted asynchronously.

Capability flags:

```c
#define DECK_OTA_APP_FLAG_SIGNATURE_VERIFY (1u << 4)
#define DECK_OTA_APP_FLAG_ROLLBACK         (1u << 5)
#define DECK_OTA_APP_FLAG_DELTA            (1u << 6)
```

Errors: same as downloader plus `:signature_invalid`, `:incompatible`, `:migration_failed`, `:in_use` (cannot replace because file lock).

### 8.3 Why two drivers and not one

Concerns are different. Firmware OTA touches partition tables, requires reboot, depends on bootloader features (anti-rollback, secure boot). App OTA is filesystem-only, runs at runtime, can recover from failure without reboot. Combining them would either leak firmware concerns into the app installer or vice versa.

Different platforms can ship one without the other. A development build might have only `ota.app` (since firmware comes from `idf.py flash`). A locked-down kiosk might have only `ota.firmware` (no third-party apps).

---

## 9. Other Service Drivers

### 9.1 `deck.driver.ble`

Backs `ble`. BLE central role. Scan, connect, GATT read/write, notifications.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*scan_start)  (void *h, uint32_t duration_ms);
  DeckResult (*scan_stop)   (void *h);
  void       (*set_scan_callback)(void *h,
                                   void (*cb)(const DeckBleDevice*, void *user),
                                   void *user);
  DeckResult (*connect)     (void *h, const char *addr, DeckBleConn **out);
  DeckResult (*disconnect)  (void *h, DeckBleConn *c);
  DeckResult (*read_char)   (void *h, DeckBleConn *c, const char *uuid,
                              void *buf, size_t cap, size_t *out_read);
  DeckResult (*write_char)  (void *h, DeckBleConn *c, const char *uuid,
                              const void *buf, size_t len, bool with_response);
  DeckResult (*subscribe)   (void *h, DeckBleConn *c, const char *uuid,
                              void (*notify_cb)(const void *buf, size_t len, void *user),
                              void *user);
} DeckBleDriver;
```

Capability flags: `PERIPHERAL_ROLE`, `MESH`, `SECURE_PAIRING`.

### 9.2 `deck.driver.bt_classic`

Backs `bt_classic`. Bluetooth Classic over an external module (UART AT-commands) or via SoCs that support BR/EDR (ESP32 original, not S3/C-series).

Same shape as `ble` driver but targeting BR/EDR profiles (A2DP, HFP, SPP). Most platforms will ship a stub returning `:unavailable`.

### 9.3 `deck.driver.crypto.aes`

Backs `crypto.aes`.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*encrypt_cbc)(void *h, const uint8_t *key, size_t key_bytes,
                             const uint8_t *iv,
                             const uint8_t *plaintext, size_t pt_len,
                             uint8_t **out_ct, size_t *out_len);
  DeckResult (*decrypt_cbc)(void *h, const uint8_t *key, size_t key_bytes,
                             const uint8_t *iv,
                             const uint8_t *ciphertext, size_t ct_len,
                             uint8_t **out_pt, size_t *out_len);
  /* GCM if supported */
  DeckResult (*encrypt_gcm)(void *h, const uint8_t *key, size_t key_bytes,
                             const uint8_t *iv, size_t iv_len,
                             const uint8_t *aad, size_t aad_len,
                             const uint8_t *plaintext, size_t pt_len,
                             uint8_t **out_ct_with_tag, size_t *out_len);
  DeckResult (*decrypt_gcm)(/* mirror */);
  void       (*free_buf)(void *h, void *buf);
} DeckCryptoAesDriver;
```

Capability flags: `HARDWARE_ACCELERATED`, `GCM_SUPPORT`, `SUPPORTS_256`.

### 9.4 `deck.driver.sensors.<kind>`

One driver per sensor type. Same shape across kinds — `read` returns one value, `watch` provides a stream.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*read)  (void *h, DeckSensorReading *out);
  void       (*set_watch_callback)(void *h, uint16_t hz,
                                    void (*cb)(const DeckSensorReading*, void *user),
                                    void *user);
} DeckSensorDriver;
```

The `DeckSensorReading` struct is a tagged union covering scalar (temperature, humidity, light), vector (accelerometer, gyroscope, magnetometer), and structured (GPS Location) readings.

### 9.5 `deck.driver.hardware.uart`

Backs `hardware.uart`. Direct UART access for advanced apps (e.g., a serial terminal app, or apps that drive proprietary peripherals).

```c
typedef struct DeckUart DeckUart;

typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*open)   (void *h, uint8_t port_num, const DeckUartConfig *cfg, DeckUart **out);
  DeckResult (*write)  (void *h, DeckUart *u, const void *buf, size_t len);
  DeckResult (*read)   (void *h, DeckUart *u, void *buf, size_t cap,
                         size_t *out_read, uint32_t timeout_ms);
  DeckResult (*close)  (void *h, DeckUart *u);
} DeckHardwareUartDriver;
```

### 9.6 `deck.driver.background_fetch`

Backs `background_fetch`. Schedules wakeups for periodic background work (RSS check, sync, etc.).

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  DeckResult (*schedule)  (void *h, const char *app_id, uint32_t period_minutes);
  DeckResult (*unschedule)(void *h, const char *app_id);
  DeckResult (*list)      (void *h, DeckBackgroundJob **out, size_t *out_count);
  void       (*free_jobs) (void *h, DeckBackgroundJob *j, size_t n);
} DeckBackgroundFetchDriver;
```

Implementations may use deep sleep with RTC alarm (microcontrollers), `cron` (POSIX hosts), or platform-specific job schedulers.

---

## 10. Driver Registration and Composition

### 10.1 The driver registry

A platform builds a `DeckDriverRegistry` at boot:

```c
typedef struct {
  size_t  count;
  size_t  capacity;
  /* Indexed by driver_path; lookups via hash. */
  DeckServiceDriver **drivers;
  DeckHashMap        path_index;
} DeckDriverRegistry;

DeckResult deck_registry_register(DeckDriverRegistry *reg, DeckServiceDriver *drv);
DeckServiceDriver *deck_registry_get(DeckDriverRegistry *reg, const char *driver_path);
```

The bridge then composes the registry with the runtime by mapping each registered driver's path to a capability path:

```c
DeckResult deck_bridge_bind_capabilities(DeckBridge *br, DeckDriverRegistry *reg);
```

This walks the driver list, looks up each driver's intended capability path (per the catalog in §2 or via a static binding table), and calls `deck_register_capability` (`06-deck-native §5`) with the appropriate method table that wraps the driver's vtable.

### 10.2 Multiple implementations of the same driver

A platform MAY register multiple drivers for the same path — useful for fallbacks. The registry resolves by **registration order** (first wins) unless a driver explicitly declares it is a fallback via:

```c
#define DECK_DRIVER_FLAG_FALLBACK  (1u << 31)
```

Fallback drivers are tried only if no non-fallback driver for the path was registered.

This permits a platform to ship, say, a hardware-accelerated `crypto.aes` driver that uses the SoC's AES peripheral as the primary, and a software fallback that runs on a SoC without AES hardware as the secondary.

### 10.3 Capability flags discovery

After binding, the runtime exposes per-capability flags to apps via the (read-only) capability metadata API:

```deck
@use crypto.aes as aes
-- ...
when aes.has_capability(:hardware_accelerated)
  use_aes_for_large_buffer()
```

(This is part of `06-deck-native` and is mentioned here only to clarify that `capability_flags` propagate to the app surface.)

---

## 11. SDI Versioning

The SDI is versioned with a 16-bit major.minor scheme:

```
sdi_version = (major << 8) | minor
```

- Backward-compatible additions (new capability flags, new optional methods at the end of vtables) bump **minor**. Drivers compiled against an older minor still work.
- Incompatible changes (renaming methods, changing signatures, reordering vtable fields) bump **major**. The runtime refuses to load drivers whose `major` differs from its own.

Initial version: **1.0** = `0x0100`.

The runtime checks every registered driver's `sdi_version` and:

- If `major(driver) == major(runtime)` and `minor(driver) <= minor(runtime)`, load it.
- Otherwise, reject and log `SDI_VERSION_MISMATCH`.

A driver MAY indicate it requires a newer minor than its own by setting an upper bound; this is rare and not formally specified yet.

> **For the broader version policy** — how SDI versioning relates to editions, surface API levels, runtime semver, app semver, and per-driver impl versions — see `15-deck-versioning`. That doc is the single source of truth on who-bumps-what and the compatibility check sequence at app load time.

---

## 12. Conformance Test Suite

A reference conformance suite (separate repo, see `14-deck-components`) exercises every driver method with happy-path and edge-case inputs against a mock runtime. To certify a platform implementation as SDI-conformant, it MUST pass:

- All non-optional method calls return correctly typed values
- All documented error atoms are produced under their documented conditions
- Lifecycle order is enforced (cannot call methods in INITIALIZED state without START)
- Memory ownership rules are honored (no leaks, no double-frees under valgrind)
- Threading contracts are respected (drivers tagged `MAIN_LOOP` are never called from another thread)

The suite is a discoverable test app under `examples/sdi-conformance/` in the `deck_runtime` component repository. A platform vendor runs it once during CI and, on success, can claim SDI conformance for their published driver components.

---

## 13. Why an Interface, Not a Bunch of Hooks

The temptation is to give the runtime "init hooks" that a platform fills in ad-hoc. We rejected that because:

- An interface is a contract. Any platform that implements it can host Deck. There is no mystery about what "hosting Deck" means.
- An interface is mock-able. Tests for the runtime, the bridge, and apps can substitute mock drivers without depending on real hardware.
- An interface is portable. The same `.deck` apps run on ESP32-S3, on a future RP2350 port, on a Linux desktop simulator. Apps cannot tell the difference because they only see the capability layer.
- An interface is publishable. Each driver implementation is a discrete artifact (an IDF Component, a Cargo crate, a Conan package) with a clear role: "this is how to do storage on RP2350."
- An interface enables third-party drivers. A new sensor module ships with a `deck.driver.sensors.foo` implementation; users add it to their platform composition without modifying the runtime.

The cost is one more layer of indirection. We pay it consciously because the alternative — every platform improvising — guarantees fragmentation and makes Deck a single-target language by accident.

---

## 14. What This Document Is Not

- **Not a binary ABI.** The structs above are defined in C headers shipped with the runtime. Drivers are linked at build time, not loaded dynamically. ABI compatibility across runtime versions is enforced by `sdi_version`.
- **Not a complete C header.** This document is the spec; the canonical headers live in `deck_runtime/include/deck/sdi/*.h`. Drivers `#include` those.
- **Not a process boundary.** Drivers run in the same address space as the runtime. There is no IPC. Platforms that want isolation (e.g. running the runtime in a Linux process and drivers in another) must marshal through their own RPC layer above the SDI.
- **Not a complete platform spec.** The SDI specifies *what* drivers must do, not *how* a platform organizes them, what its boot sequence is, or how it manages memory. Those are platform concerns; see `13-deck-cyberdeck-platform` for the ESP32-S3 reference.
