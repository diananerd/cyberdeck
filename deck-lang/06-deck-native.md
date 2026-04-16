# Deck Native Bindings
**Version 2.0 — Extending the Runtime: Capabilities, Builtins, Events, and Types**

---

## 1. What This Document Is

The Deck interpreter is a closed evaluator — it produces no side effects and calls no OS code directly. Everything that touches the hardware or OS flows through two extension points:

1. **The `.deck-os` surface file** — a text declaration of what capabilities, events, types, and builtins exist
2. **The C bridge implementation** — C/C++ code that implements those declarations

This document is for the developer writing those two things: an embedded firmware engineer extending the platform, an OS author defining what apps can access, or a library author packaging a new hardware driver as a Deck capability.

Reading `03-deck-os` first is recommended. That document describes what the runtime expects from a bridge. This document describes how to implement one.

---

## 2. The Two Extension Points

```
Your C code                          Deck apps
─────────────────────────            ──────────────────────
deck_register_capability()    ←→    @use sensors.bmp280 as bmp
deck_register_builtin()       ←→    math.round(), bmp280.read()
deck_register_event()         ←→    @on hardware.altitude_change
deck_register_type()          ←→    @type BmpReading (in .deck-os)
deck_fire_event()             ───→  @on hooks receive it
DeckSubscription callbacks    ───→  Stream values delivered
```

The `.deck-os` file declares the surface (what exists). The C registration calls wire that surface to implementations. The two must match exactly — the loader verifies every declaration in `.deck-os` against the registered implementations at startup.

---

## 3. The .deck-os Authoring Reference

A complete `.deck-os` file:

```
-- Line comment
---
Block comment
---

@os
  name:    "MyBoard v2"
  version: "2.0.0"
  arch:    :arm32

-- ── Builtins ────────────────────────────────────────────────────────────────
-- Always in scope, no @use, no !effect, pure (except @impure marker).
@builtin module_name
  fn_name (param_name: Type, ...) -> ReturnType
  fn_name (param_name: Type, ...) -> ReturnType  @impure

-- ── Types ───────────────────────────────────────────────────────────────────
-- Types provided by the OS surface. Available in .deck-os and in Deck code
-- that @uses a capability exposing them.
@type TypeName
  field_name : Type
  field_name : Type?
  field_name : [Type]

-- ── Opaque Handle Types ──────────────────────────────────────────────────────
-- For connection/session handles owned by the bridge (DECK_OPAQUE).
-- Register with deck_register_opaque_type() for type-checked extraction.
@opaque TypeName

-- ── Capabilities ─────────────────────────────────────────────────────────────
@capability capability.path
  method_name (params...) -> ReturnType
  method_name (params...) -> ReturnType  @pure   -- no !effect annotation required
  method_name (params...) -> Stream Type

  @errors
    :atom_name  "Human description"

  @requires_permission   -- capability must appear in @permissions
  @singleton             -- only one active call at a time (calls queue)
  @deprecated "Use other.capability instead"

-- ── Events ───────────────────────────────────────────────────────────────────
@event event_name
@event event_name (field_name: Type, ...)
```

### 3.1 Type Expressions in .deck-os

All type expressions from the Deck language are valid in `.deck-os`. Additionally:
- `@type` declarations defined earlier in the same `.deck-os` file can be referenced by name
- Types from the interpreter's built-in set are always available: `int`, `float`, `bool`, `str`, `byte`, `unit`, `any`, `Timestamp`, `Duration`
- `Result T E` where `E` can be a capability's `@errors` domain: `sensors.bmp280.Error`
- `Stream T` for streaming capabilities

### 3.2 Method Modifiers

**`@pure`** — marks a method that takes no side effects from the Deck perspective. Calls do not require `!effect` in the calling function. Use for query-only methods that are deterministic from the app's view: `system.info.device_id()`, `ble.is_connected(conn)`. The runtime still routes the call through the bridge — `@pure` is a declaration of semantics, not an optimization.

**`@impure`** (on builtins only) — marks a builtin as non-deterministic (like `random`). Does not require `@use` or `!effect` but callers know the result varies. Cannot be used on capability methods — capability calls are always effectful.

**`@singleton`** — the bridge receives only one active invocation of this method at a time. If a call arrives while one is in progress, it is queued until the first completes. Use for hardware access that cannot be concurrent: SPI flash reads, single-channel ADC.

**`@requires_permission`** — apps must include this capability in `@permissions`. Without it: silent degradation (`:err :permission`), never a crash.

**`@deprecated`** — the loader emits a warning when an app `@use`s this capability. The capability remains fully functional.

### 3.3 Naming Conventions for Capability Paths

```
sensors.temperature          -- hardware sensor category
sensors.bmp280               -- specific chip
network.http                 -- protocol category
network.ws                   -- specific protocol
storage.local                -- storage category
display.notify               -- display category
custom.my_service            -- custom/app-specific, use "custom." prefix
board.relay                  -- board-specific feature, use "board." prefix
```

Paths are strings. Any dot-separated lowercase path is valid. The interpreter does not interpret the path structure — dots are cosmetic.

---

## 4. The DeckValue C API

Every value crossing the C↔Deck boundary is a `DeckValue`. This is the central data structure.

### 4.1 Type Definition

```c
/* deck_value.h */

typedef enum DeckType {
    DECK_INT,
    DECK_FLOAT,
    DECK_BOOL,
    DECK_STR,
    DECK_BYTE,
    DECK_UNIT,
    DECK_ATOM,           /* :ok, :none, :timeout ... */
    DECK_VARIANT,        /* :some value, :err value */
    DECK_LIST,
    DECK_MAP,
    DECK_TUPLE,
    DECK_RECORD,         /* @type instance */
    DECK_STREAM,         /* stream handle */
    DECK_OPAQUE,         /* capability connection handle */
    DECK_DURATION,       /* milliseconds */
    DECK_TIMESTAMP,      /* ms since epoch */
    DECK_FN,             /* callable (rare in bridge context) */
} DeckType;

typedef struct DeckStr {
    const char* data;
    size_t      len;       /* byte length, not character count */
    bool        owned;     /* true: bridge must call deck_str_free() */
} DeckStr;

typedef struct DeckValue {
    DeckType type;
    union {
        int64_t     int_val;
        double      float_val;
        bool        bool_val;
        uint8_t     byte_val;
        DeckStr     str_val;
        struct {
            char*       name;      /* atom string, interned — do not free */
        } atom_val;
        struct {
            char*       variant;   /* atom name, e.g. "ok", "some", "err" */
            struct DeckValue* payload; /* nullable for bare atoms */
            char**      field_names;   /* for named-field variants */
            size_t      field_count;
        } variant_val;
        struct {
            struct DeckValue* items;
            size_t            count;
        } list_val;
        struct {
            char**            keys;
            struct DeckValue* vals;
            size_t            count;
        } map_val;
        struct {
            struct DeckValue* items;
            size_t            count;
        } tuple_val;
        struct {
            char*             type_name;  /* @type name */
            char**            field_names;
            struct DeckValue* field_vals;
            size_t            field_count;
        } record_val;
        struct {
            uint64_t handle;   /* opaque handle ID, managed by runtime */
        } stream_val;
        struct {
            uint64_t  id;
            void*     ptr;     /* OS-managed data; runtime never dereferences */
            void    (*on_release)(void* ptr);  /* called when Deck drops it */
        } opaque_val;
        int64_t duration_ms;
        int64_t timestamp_ms;
    };
} DeckValue;
```

### 4.2 Value Constructors

```c
/* deck_bridge.h — value constructors (all return heap-allocated DeckValue*) */

/* Primitives */
DeckValue* deck_int      (int64_t n);
DeckValue* deck_float    (double n);
DeckValue* deck_bool     (bool b);
DeckValue* deck_str      (const char* s);          /* copies s */
DeckValue* deck_str_n    (const char* s, size_t n); /* copies n bytes */
DeckValue* deck_byte     (uint8_t b);
DeckValue* deck_unit     ();
DeckValue* deck_duration (int64_t ms);
DeckValue* deck_timestamp(int64_t ms_since_epoch);

/* Atoms and variants */
DeckValue* deck_atom          (const char* name);        /* :name */
DeckValue* deck_ok            (DeckValue* payload);      /* :ok payload */
DeckValue* deck_err           (DeckValue* payload);      /* :err payload */
DeckValue* deck_err_atom      (const char* atom_name);   /* :err :atom_name */
DeckValue* deck_some          (DeckValue* payload);      /* :some payload */
DeckValue* deck_none          ();                        /* :none */
DeckValue* deck_variant_named (const char* variant,
                                const char** field_names,
                                DeckValue**  field_vals,
                                size_t       count);

/* Collections */
DeckValue* deck_list          (DeckValue** items, size_t count); /* copies array */
DeckValue* deck_list_empty    ();
DeckValue* deck_map           (const char** keys, DeckValue** vals, size_t count);
DeckValue* deck_map_empty     ();
DeckValue* deck_tuple         (DeckValue** items, size_t count);
DeckValue* deck_tuple2        (DeckValue* a, DeckValue* b);
DeckValue* deck_tuple3        (DeckValue* a, DeckValue* b, DeckValue* c);

/* @type records */
DeckValue* deck_record        (const char*  type_name,
                                const char** field_names,
                                DeckValue**  field_vals,
                                size_t       field_count);

/* Opaque handles */
DeckValue* deck_opaque        (void* ptr, void (*on_release)(void*));

/* Free */
void deck_value_free  (DeckValue* v);
void deck_value_retain(DeckValue* v);  /* increment refcount */
void deck_value_release(DeckValue* v); /* decrement; frees when 0 */
```

### 4.3 Value Accessors

```c
/* Reading values from incoming DeckValue* (from Deck code calling your method) */

/* Type checking */
bool deck_is_int    (const DeckValue* v);
bool deck_is_float  (const DeckValue* v);
bool deck_is_bool   (const DeckValue* v);
bool deck_is_str    (const DeckValue* v);
bool deck_is_atom   (const DeckValue* v);
bool deck_is_list   (const DeckValue* v);
bool deck_is_map    (const DeckValue* v);
bool deck_is_record (const DeckValue* v);
bool deck_is_ok     (const DeckValue* v);
bool deck_is_err    (const DeckValue* v);
bool deck_is_some   (const DeckValue* v);
bool deck_is_none   (const DeckValue* v);
bool deck_is_opaque (const DeckValue* v);
bool deck_is_unit   (const DeckValue* v);

/* Extraction */
int64_t     deck_get_int      (const DeckValue* v);   /* asserts type */
double      deck_get_float    (const DeckValue* v);
bool        deck_get_bool     (const DeckValue* v);
const char* deck_get_str      (const DeckValue* v);   /* null-terminated, valid until v is freed */
size_t      deck_get_str_len  (const DeckValue* v);
uint8_t     deck_get_byte     (const DeckValue* v);
const char* deck_get_atom     (const DeckValue* v);   /* e.g. "ok", "timeout" */
int64_t     deck_get_duration (const DeckValue* v);   /* ms */
int64_t     deck_get_timestamp(const DeckValue* v);   /* ms since epoch */
void*       deck_get_opaque   (const DeckValue* v);

/* Unwrap variants */
DeckValue*  deck_get_ok_val   (const DeckValue* v);   /* payload of :ok */
DeckValue*  deck_get_err_val  (const DeckValue* v);   /* payload of :err */
DeckValue*  deck_get_some_val (const DeckValue* v);   /* payload of :some */

/* Collections */
size_t      deck_list_count   (const DeckValue* v);
DeckValue*  deck_list_at      (const DeckValue* v, size_t i);  /* 0-based */
size_t      deck_map_count    (const DeckValue* v);
DeckValue*  deck_map_get      (const DeckValue* v, const char* key); /* NULL if absent */
const char* deck_map_key_at   (const DeckValue* v, size_t i);
DeckValue*  deck_map_val_at   (const DeckValue* v, size_t i);
size_t      deck_tuple_count  (const DeckValue* v);
DeckValue*  deck_tuple_at     (const DeckValue* v, size_t i);

/* Records */
const char* deck_record_type  (const DeckValue* v);
DeckValue*  deck_record_get   (const DeckValue* v, const char* field); /* NULL if absent */
size_t      deck_record_count (const DeckValue* v);
const char* deck_record_field_name(const DeckValue* v, size_t i);
DeckValue*  deck_record_field_val (const DeckValue* v, size_t i);

/* Named-field variant payload access */
DeckValue*  deck_variant_get  (const DeckValue* v, const char* field);
```

---

## 5. Registering Capabilities

### 5.1 The Registration API

```c
/* deck_bridge.h */

/* Synchronous capability method */
typedef DeckValue* (*DeckCapFn)(
    DeckRuntime* rt,
    DeckValue**  args,
    size_t       argc,
    const char** named_arg_keys,
    DeckValue**  named_arg_vals,
    size_t       named_argc
);

/* Streaming capability — returns a subscription ID; values delivered via deck_stream_push() */
typedef uint64_t (*DeckStreamFn)(
    DeckRuntime* rt,
    DeckValue**  args,
    size_t       argc
);
typedef void (*DeckUnsubFn)(
    DeckRuntime* rt,
    uint64_t     subscription_id
);

int deck_register_capability(
    DeckRuntime*  rt,
    const char*   path,          /* "sensors.bmp280" */
    const char*   method,        /* "read" */
    DeckCapFn     fn,
    bool          is_stream,
    DeckStreamFn  stream_fn,     /* NULL if not stream */
    DeckUnsubFn   unsub_fn       /* NULL if not stream */
);
```

### 5.2 Synchronous Capability — Example

Implementing `sensors.bmp280`:

**sensors.bmp280 in .deck-os:**
```
@type BmpReading
  temp_c     : float
  pressure_pa: float
  humidity_pct: float?

@capability sensors.bmp280
  read ()               -> Result BmpReading sensors.bmp280.Error
  read_all (samples: int) -> Result [BmpReading] sensors.bmp280.Error
  @errors
    :unavailable   "BMP280 not found on I2C bus"
    :timeout       "Sensor read timed out"
    :overrun       "Sensor data overrun"
  @requires_permission
```

**C implementation:**
```c
#include "deck_bridge.h"
#include "bmp280_driver.h"

static DeckValue* bmp280_read(
    DeckRuntime* rt,
    DeckValue**  args,
    size_t       argc,
    const char** named_keys,
    DeckValue**  named_vals,
    size_t       named_argc
) {
    bmp280_data_t data;
    esp_err_t err = bmp280_read_compensated(&data);

    if (err == ESP_ERR_NOT_FOUND) {
        return deck_err_atom("unavailable");
    }
    if (err == ESP_ERR_TIMEOUT) {
        return deck_err_atom("timeout");
    }
    if (err != ESP_OK) {
        return deck_err_atom("overrun");
    }

    /* Build the BmpReading @type record */
    const char* field_names[] = { "temp_c", "pressure_pa", "humidity_pct" };
    DeckValue*  field_vals[3];

    field_vals[0] = deck_float(data.temperature);
    field_vals[1] = deck_float(data.pressure);

    /* humidity_pct is float? — use :some or :none */
    if (data.has_humidity) {
        field_vals[2] = deck_some(deck_float(data.humidity));
    } else {
        field_vals[2] = deck_none();
    }

    DeckValue* record = deck_record("BmpReading", field_names, field_vals, 3);

    /* Free intermediate values — deck_record copies them */
    deck_value_free(field_vals[0]);
    deck_value_free(field_vals[1]);
    deck_value_free(field_vals[2]);

    /* Wrap in :ok, then free the unwrapped record (deck_ok copies it) */
    DeckValue* result = deck_ok(record);
    deck_value_free(record);
    return result;   /* runtime takes ownership */
}

static DeckValue* bmp280_read_all(
    DeckRuntime* rt,
    DeckValue**  args,
    size_t       argc,
    const char** named_keys,
    DeckValue**  named_vals,
    size_t       named_argc
) {
    int samples = (int)deck_get_int(args[0]);  /* first positional arg */
    if (samples < 1 || samples > 100) {
        return deck_err_atom("overrun");
    }

    DeckValue** readings = malloc(sizeof(DeckValue*) * samples);
    for (int i = 0; i < samples; i++) {
        DeckValue* reading = bmp280_read(rt, NULL, 0, NULL, NULL, 0);
        if (deck_is_err(reading)) {
            /* Free all records accumulated so far, then the error wrapper */
            for (int j = 0; j < i; j++) deck_value_free(readings[j]);
            free(readings);
            return reading;  /* pass :err up; caller (runtime) takes ownership */
        }
        /* deck_ok() copies inner, so we must extract the inner value BEFORE
           freeing the :ok wrapper. deck_get_ok_val returns a pointer INTO reading.
           We copy it by calling deck_value_retain on the inner, then free the outer.
           After deck_value_free(reading), the inner's refcount keeps it alive. */
        readings[i] = deck_get_ok_val(reading);
        deck_value_retain(readings[i]);   /* keep inner alive independent of outer */
        deck_value_free(reading);         /* free :ok wrapper; inner refcount is now 1 */
    }

    /* deck_list() copies all items into a new list value */
    DeckValue* list   = deck_list(readings, samples);
    DeckValue* result = deck_ok(list);   /* copies list */
    deck_value_free(list);               /* free intermediate list */
    for (int i = 0; i < samples; i++) deck_value_release(readings[i]);  /* release our refs */
    free(readings);
    return result;   /* runtime takes ownership of :ok [BmpReading] */
}

void bmp280_register(DeckRuntime* rt) {
    deck_register_capability(rt, "sensors.bmp280", "read",
                              bmp280_read, false, NULL, NULL);
    deck_register_capability(rt, "sensors.bmp280", "read_all",
                              bmp280_read_all, false, NULL, NULL);
}
```

---

## 6. Named Arguments

When a Deck app calls a method with named args:

```deck
temp.watch(hz: 2)
store.set(key: "cfg", v: data)
```

The bridge receives them in the `named_arg_keys` / `named_arg_vals` arrays. Positional and named args are mutually exclusive in one call. Use the helper to extract them:

```c
/* deck_bridge.h */
DeckValue* deck_named_arg(
    const char** keys,
    DeckValue**  vals,
    size_t       count,
    const char*  name          /* target key */
);                             /* returns NULL if not present */

/* Usage: */
DeckValue* hz_val = deck_named_arg(named_keys, named_vals, named_argc, "hz");
int hz = hz_val ? (int)deck_get_int(hz_val) : 1;  /* default 1 */
```

---

## 7. Streaming Capabilities

### 7.1 The Stream Pattern

A streaming capability delivers values continuously over time. The app subscribes once; values are pushed by the bridge; the runtime delivers them to `@stream` consumers and re-evaluates any content bodies that depend on the stream.

**Push API:**
```c
/* Push a value into a stream. Thread-safe. */
void deck_stream_push(
    DeckRuntime* rt,
    uint64_t     subscription_id,
    DeckValue*   value          /* runtime copies; caller retains ownership */
);

/* Signal a stream error (stream becomes inactive) */
void deck_stream_error(
    DeckRuntime* rt,
    uint64_t     subscription_id,
    DeckValue*   error
);

/* Signal stream end (cleanly finished) */
void deck_stream_end(
    DeckRuntime* rt,
    uint64_t     subscription_id
);
```

### 7.2 Stream Capability — Example

**sensors.bmp280 streaming in .deck-os:**
```
@capability sensors.bmp280
  ...
  watch (hz: int)  -> Stream BmpReading
```

**C implementation:**
```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    uint64_t   sub_id;
    int        hz;
    bool       active;
    DeckRuntime* rt;
} BmpWatchCtx;

static void bmp280_watch_task(void* arg) {
    BmpWatchCtx* ctx = (BmpWatchCtx*)arg;
    TickType_t   delay = pdMS_TO_TICKS(1000 / ctx->hz);

    while (ctx->active) {
        bmp280_data_t data;
        esp_err_t err = bmp280_read_compensated(&data);

        if (!ctx->active) break;   /* check again after potentially slow read */

        if (err == ESP_OK) {
            const char* field_names[] = { "temp_c", "pressure_pa", "humidity_pct" };
            DeckValue*  fv[3] = {
                deck_float(data.temperature),
                deck_float(data.pressure),
                data.has_humidity ? deck_some(deck_float(data.humidity))
                                  : deck_none()
            };
            DeckValue* record = deck_record("BmpReading", field_names, fv, 3);
            deck_value_free(fv[0]); deck_value_free(fv[1]); deck_value_free(fv[2]);

            deck_stream_push(ctx->rt, ctx->sub_id, record);
            deck_value_free(record);
        } else {
            deck_stream_error(ctx->rt, ctx->sub_id, deck_err_atom("unavailable"));
            break;
        }
        vTaskDelay(delay);
    }
    free(ctx);
    vTaskDelete(NULL);
}

static uint64_t bmp280_watch_start(
    DeckRuntime* rt,
    DeckValue**  args,
    size_t       argc
) {
    /* named arg 'hz' — caller passes deck_register_capability args */
    int hz = 1;
    /* In practice, named args passed to stream fn via a different signature:
       see deck_register_stream() below which passes named args */

    uint64_t sub_id = deck_new_subscription_id(rt);

    BmpWatchCtx* ctx = malloc(sizeof(BmpWatchCtx));
    *ctx = (BmpWatchCtx){
        .sub_id = sub_id,
        .hz     = hz,
        .active = true,
        .rt     = rt
    };
    deck_subscription_set_ctx(rt, sub_id, ctx);

    xTaskCreate(bmp280_watch_task, "bmp280_watch", 4096, ctx, 5, NULL);
    return sub_id;
}

static void bmp280_watch_stop(DeckRuntime* rt, uint64_t sub_id) {
    BmpWatchCtx* ctx = deck_subscription_get_ctx(rt, sub_id);
    if (ctx) ctx->active = false;
    /* Task will notice and free ctx */
}

void bmp280_register(DeckRuntime* rt) {
    deck_register_capability(rt, "sensors.bmp280", "read",
                              bmp280_read, false, NULL, NULL);
    deck_register_stream(rt, "sensors.bmp280", "watch",
                         bmp280_watch_start, bmp280_watch_stop);
}
```

**Full stream registration API:**
```c
/* Separate entry point for stream methods — gives access to named args */
typedef uint64_t (*DeckStreamStartFn)(
    DeckRuntime* rt,
    DeckValue**  args,        size_t argc,
    const char** named_keys,  DeckValue** named_vals,  size_t named_argc
);
typedef void (*DeckStreamStopFn)(
    DeckRuntime* rt,
    uint64_t     subscription_id
);

void deck_register_stream(
    DeckRuntime*      rt,
    const char*       path,
    const char*       method,
    DeckStreamStartFn start_fn,
    DeckStreamStopFn  stop_fn
);

/* Subscription context slot — store your per-sub state */
uint64_t deck_new_subscription_id (DeckRuntime* rt);
void     deck_subscription_set_ctx(DeckRuntime* rt, uint64_t sub_id, void* ctx);
void*    deck_subscription_get_ctx(DeckRuntime* rt, uint64_t sub_id);
void     deck_subscription_clear_ctx(DeckRuntime* rt, uint64_t sub_id);
```

---

## 8. Opaque Handles

Capabilities that maintain per-connection state (BLE connections, WebSocket sessions, I2C handles, file writers) use opaque handles. The Deck runtime holds the handle as a value, passes it back on subsequent calls, and notifies you via the `on_release` callback when Deck code drops it.

### 8.1 Creating a Handle

```c
/* In your capability method that opens a connection: */
static DeckValue* ws_connect(DeckRuntime* rt, DeckValue** args, size_t argc,
                              const char** nkeys, DeckValue** nvals, size_t nc) {
    const char* url = deck_get_str(args[0]);

    ws_client_t* client = ws_client_connect(url);
    if (!client) {
        return deck_err_atom("refused");
    }

    /* Wrap in opaque handle.
       on_release is called when Deck code lets go of this value. */
    DeckValue* handle = deck_opaque(client, ws_client_on_release);
    DeckValue* result = deck_ok(handle);
    deck_value_free(handle);   /* deck_ok copies */
    return result;
}

static void ws_client_on_release(void* ptr) {
    ws_client_t* client = (ws_client_t*)ptr;
    ws_client_disconnect(client);
    ws_client_free(client);
}
```

### 8.2 Receiving a Handle in Subsequent Calls

```c
static DeckValue* ws_send(DeckRuntime* rt, DeckValue** args, size_t argc,
                           const char** nkeys, DeckValue** nvals, size_t nc) {
    /* args[0] = WsConn opaque handle, args[1] = str message */
    ws_client_t* client = (ws_client_t*)deck_get_opaque(args[0]);
    const char*  msg    = deck_get_str(args[1]);

    if (!client) return deck_err_atom("closed");

    int err = ws_client_send(client, msg);
    /* network.ws.send() -> Result unit network.ws.Error — must wrap success in :ok */
    if (err == 0) {
        DeckValue* u      = deck_unit();
        DeckValue* result = deck_ok(u);
        deck_value_free(u);
        return result;
    }
    return deck_err_atom("refused");
}
```

### 8.3 Multiple Opaque Types

Register type names with the runtime for better error messages. Use the type name declared with `@opaque` in `.deck-os`:

```c
void deck_register_opaque_type(
    DeckRuntime* rt,
    const char*  type_name,   /* "WsConn" — matches @opaque declaration in .deck-os */
    bool       (*type_check)(void* ptr)  /* optional: for runtime validation */
);

/* Extract with type check */
void* deck_get_opaque_typed(
    const DeckValue* v,
    const char*      type_name    /* runtime error if mismatch */
);
```

---

## 9. Custom OS Events

Events are notifications fired by the OS into all running apps. They are not targeted at a specific app — every app with a matching `@on event_name` hook receives them.

### 9.1 Declaring Events in .deck-os

```
@event hardware.altitude_change (altitude_m: float, rate: float)
@event board.door_opened
@event board.power_level_change (level: atom)   -- :normal | :low | :critical
@event custom.sensor_alert      (sensor_id: int, value: float, threshold: float)
```

### 9.2 Firing Events from C

```c
/* Fire to all apps. Thread-safe. */
void deck_fire_event(
    DeckRuntime* rt,
    const char*  event_name,
    DeckValue*   payload     /* NULL for events with no fields */
);

/* For events with named fields, build a map: */
void deck_fire_event_map(
    DeckRuntime*  rt,
    const char*   event_name,
    const char**  field_names,
    DeckValue**   field_vals,
    size_t        field_count
);
```

**Example — firing `hardware.altitude_change`:**
```c
static void altitude_sensor_isr(float altitude, float rate) {
    const char* names[] = { "altitude_m", "rate" };
    DeckValue*  vals[2] = {
        deck_float(altitude),
        deck_float(rate)
    };
    deck_fire_event_map(global_rt, "hardware.altitude_change", names, vals, 2);
    deck_value_free(vals[0]);
    deck_value_free(vals[1]);
}
```

**In a Deck app:**
```deck
@use
  sensors.altimeter as alt

@on hardware.altitude_change (altitude_m: a, rate: r)
  App.send(:altitude_update, meters: a, rate: r)
```

### 9.3 Firing Standard OS Events

The bridge should fire the standard events defined in `03-deck-os §5` from the appropriate OS hooks:

```c
/* Called from OS network status callback */
void on_network_changed(bool connected) {
    DeckValue* status = deck_atom(connected ? "connected" : "offline");
    const char* names[] = { "status" };
    DeckValue*  vals[]  = { status };
    deck_fire_event_map(rt, "os.network_change", names, vals, 1);
    deck_value_free(status);
}

/* Called from OS low-battery notification */
void on_low_battery(int level) {
    DeckValue* lv    = deck_int(level);
    const char* names[] = { "level" };
    DeckValue*  vals[]  = { lv };
    deck_fire_event_map(rt, "os.low_battery", names, vals, 1);
    deck_value_free(lv);
}
```

---

## 10. Registering Custom Builtins

Builtins are always in scope, require no `@use`, and normally are pure. They extend the `math`, `text`, or `time` namespaces — or create new ones.

### 10.1 Registering a Builtin Module

```c
typedef DeckValue* (*DeckBuiltinFn)(
    DeckRuntime* rt,
    DeckValue**  args,
    size_t       argc
);

void deck_register_builtin(
    DeckRuntime*  rt,
    const char*   module,      /* "math", "text", or a new name */
    const char*   fn_name,
    DeckBuiltinFn fn,
    bool          is_impure    /* true = non-deterministic; analogous to random */
);
```

Example — adding `dsp.fft`:

**In .deck-os:**
```
@builtin dsp
  fft    (samples: [float]) -> [float]
  window (samples: [float], type: atom) -> [float]
    -- type: :hanning | :hamming | :blackman | :rectangular
  rms    (samples: [float]) -> float
```

**In C:**
```c
static DeckValue* dsp_fft(DeckRuntime* rt, DeckValue** args, size_t argc) {
    size_t n = deck_list_count(args[0]);
    if (n == 0 || (n & (n - 1)) != 0) {  /* must be power of two */
        return deck_list_empty();
    }

    float* input = malloc(sizeof(float) * n);
    for (size_t i = 0; i < n; i++) {
        input[i] = (float)deck_get_float(deck_list_at(args[0], i));
    }

    float* output = malloc(sizeof(float) * n);
    kiss_fft_execute(input, output, n);

    DeckValue** results = malloc(sizeof(DeckValue*) * n);
    for (size_t i = 0; i < n; i++) {
        results[i] = deck_float(output[i]);
    }

    DeckValue* list = deck_list(results, n);
    for (size_t i = 0; i < n; i++) deck_value_free(results[i]);
    free(results); free(input); free(output);
    return list;
}

void dsp_register(DeckRuntime* rt) {
    deck_register_builtin(rt, "dsp", "fft",    dsp_fft,    false);
    deck_register_builtin(rt, "dsp", "window", dsp_window, false);
    deck_register_builtin(rt, "dsp", "rms",    dsp_rms,    false);
}
```

---

## 11. Registering Custom Types

Types declared in `.deck-os` do not need any C registration — the runtime infers their structure from the `.deck-os` file. But you can give the runtime additional metadata:

```c
/* Declare a type with validation — runtime verifies record construction */
void deck_register_type(
    DeckRuntime*  rt,
    const char*   type_name,             /* "BmpReading" */
    const char**  field_names,
    DeckType*     field_types,           /* expected types for each field */
    bool*         field_nullable,        /* true = T? */
    size_t        field_count,
    bool          allow_extra_fields     /* default false */
);

/* Optional: a custom toString for the REPL / log output */
void deck_register_type_display(
    DeckRuntime* rt,
    const char*  type_name,
    char* (*display_fn)(const DeckValue* v)   /* caller frees result */
);
```

Type registration is optional. Without it, the runtime still enforces field presence and count from the `.deck-os` declaration. With it, you also get field type checking at the C boundary.

---

## 12. The DeckRuntime Object

The `DeckRuntime*` passed to all callbacks. Never call runtime functions from a destructor, finalizer, or signal handler — only from bridge callbacks and registered task threads.

```c
/* deck_bridge.h — runtime utilities */

/* Get the app ID of the currently running app */
const char* deck_runtime_app_id(DeckRuntime* rt);

/* Get the app version */
const char* deck_runtime_app_version(DeckRuntime* rt);

/* Log from bridge code — appears in deck run output */
void deck_log(DeckRuntime* rt, const char* level, const char* msg);
void deck_logf(DeckRuntime* rt, const char* level, const char* fmt, ...);

/* Queue a callback to run on the main interpreter loop.
   Safe to call from any thread or ISR. */
void deck_dispatch_main(DeckRuntime* rt, void (*fn)(DeckRuntime*, void*), void* ctx);

/* Check if a capability is currently available for the given app */
bool deck_capability_available(DeckRuntime* rt, const char* path);

/* Allocate memory tracked by the runtime (freed if app crashes) */
void* deck_alloc(DeckRuntime* rt, size_t bytes);
void  deck_free (DeckRuntime* rt, void* ptr);

/* Get OS startup configuration */
DeckValue* deck_get_os_config(DeckRuntime* rt, const char* key);
```

---

## 13. Thread Safety

### 13.1 The Main Loop Rule

The Deck interpreter is single-threaded. The evaluator, navigation manager, scheduler, and all state machines run on a single OS task — the "Deck main loop". **Never call any runtime state-modifying function from outside this loop without going through the dispatch queue.**

Functions safe to call from any thread or ISR:
- `deck_stream_push()` — internally queues
- `deck_fire_event()` — internally queues
- `deck_dispatch_main()` — is the queue itself
- `deck_log()` — thread-safe

Functions that must only be called from the main loop (i.e., from inside a capability callback or `deck_dispatch_main` callback):
- All value constructors and destructors
- All registration functions
- `deck_capability_available()`

### 13.2 Pattern: Async Capability with Callback

For long-running OS operations (BLE scan, HTTP download) that complete asynchronously:

```c
typedef struct {
    DeckRuntime* rt;
    DeckValue*   continuation;   /* if using explicit continuations */
    /* Or: a flag + result DeckValue, checked by stream or event */
} AsyncCtx;

static void http_download_done_cb(void* ctx_ptr, uint8_t* data, size_t len, int status) {
    AsyncCtx* ctx = (AsyncCtx*)ctx_ptr;
    DeckValue* result;

    if (status == 200) {
        result = deck_ok(deck_str_n((char*)data, len));
    } else {
        result = deck_err_atom("download_failed");
    }

    /* Fire as a stream value or event, not direct state mutation */
    deck_stream_push(ctx->rt, ctx->download_sub_id, result);
    deck_value_free(result);
    free(ctx);
}
```

### 13.3 ISR-Safe Pattern

On ISR (interrupt service routine), only push pre-allocated values or atoms. Use `deck_fire_event_isr` which is ISR-safe (uses a lock-free ring buffer internally — no heap allocation, no mutex):

```c
/* deck_bridge.h */

/* ISR-safe variant of deck_fire_event. Uses a lock-free queue internally.
   payload must be a pre-allocated, retained DeckValue* (e.g. a static atom).
   The runtime releases its reference after dispatching.
   ONLY call from ISR or from any thread where malloc is not safe. */
void deck_fire_event_isr(
    DeckRuntime* rt,
    const char*  event_name,
    DeckValue*   payload     /* NULL for no-payload events; must be pre-allocated */
);
```

Usage example:

```c
static DeckValue* IRAM_ATTR s_btn_payload = NULL;  /* pre-allocated at init */

void board_init_button_events(DeckRuntime* rt) {
    /* Pre-allocate the payload value once at startup */
    const char* names[] = { "id", "action" };
    DeckValue*  vals[]  = { deck_int(0), deck_atom("press") };
    s_btn_payload = deck_variant_named("hardware.button", names, vals, 2);
    deck_value_retain(s_btn_payload);  /* kept alive across ISR calls */
}

void IRAM_ATTR gpio_isr_handler(void* arg) {
    deck_fire_event_isr(global_rt, "hardware.button", s_btn_payload);
}
```

For events with dynamic payload (e.g., variable pin or action), use `deck_dispatch_main` from the ISR to do the allocation on the main loop, then fire `deck_fire_event`.

---

## 14. Memory Management Contract

### 14.1 Ownership Rules

| Situation | Who owns the value | When freed |
|---|---|---|
| Bridge returns `DeckValue*` to runtime | Runtime | When Deck code drops it |
| Runtime passes `DeckValue*` to bridge callback (args) | Runtime | After callback returns |
| Bridge creates intermediate values | Bridge | Bridge must free before returning |
| `deck_stream_push(v)` | Caller | After `deck_stream_push` returns |
| `deck_fire_event(v)` | Caller | After `deck_fire_event` returns |
| `deck_ok(inner)` / `deck_err(inner)` | Creates new value; inner not consumed | Bridge frees inner |
| `deck_get_*` accessors | Runtime | Valid while parent DeckValue is alive |

### 14.2 Common Mistake: Double-Free

```c
/* WRONG */
DeckValue* inner = deck_float(42.0);
DeckValue* result = deck_ok(inner);
deck_value_free(inner);    /* OK so far */
deck_value_free(result);   /* deck_ok copies inner — but now inner is freed twice */

/* CORRECT */
DeckValue* inner  = deck_float(42.0);
DeckValue* result = deck_ok(inner);   /* copies inner */
deck_value_free(inner);               /* free original; result has its own copy */
return result;                        /* runtime takes ownership */
```

### 14.3 Common Mistake: Holding After Return

```c
/* WRONG — args are only valid during the callback */
static DeckValue* saved_arg = NULL;

static DeckValue* my_method(...) {
    saved_arg = args[0];    /* args[0] will be freed after return */
    ...
}

/* CORRECT — retain if you need it to outlive the callback */
static DeckValue* my_method(...) {
    saved_arg = args[0];
    deck_value_retain(saved_arg);   /* now bridge owns a reference */
    ...
}

/* Later, when done with saved_arg: */
deck_value_release(saved_arg);
saved_arg = NULL;
```

---

## 15. Error Handling Conventions

### 15.1 Return Value Contract

All capability methods must return a `DeckValue*`. They must never return `NULL`. They must never call abort/exit/panic. Hardware errors become `:err :atom_name`.

```c
/* Never: */
if (err) return NULL;   /* wrong — runtime will crash */

/* Always: */
if (err) return deck_err_atom("unavailable");
```

### 15.2 Error Atoms Must Match .deck-os @errors

The atoms in `deck_err_atom("name")` must match exactly the atoms declared in the capability's `@errors` block. The runtime warns (but does not fail) on unknown error atoms — but Deck apps cannot pattern-match them correctly.

### 15.3 Panic Recovery

If your C code can throw (C++ exceptions, setjmp/longjmp), wrap the bridge call in a recovery block. The runtime provides a hook:

```c
void deck_set_panic_handler(
    DeckRuntime* rt,
    void (*handler)(DeckRuntime*, const char* message)
);
```

The panic handler is called if a bridge function returns via an exception or segfault (detected via signal handler on POSIX systems). The handler should: log the error, return a `DeckValue*` `:err :internal` to the waiting continuation, and allow the runtime to continue.

---

## 16. The DeckRuntime Initialization API

Called by the main application before starting any app. All registrations happen here.

```c
/* deck_runtime.h */

/* Allocate and initialize the runtime */
DeckRuntime* deck_runtime_create(const DeckRuntimeConfig* cfg);

typedef struct DeckRuntimeConfig {
    /* Path to .deck-os surface file.
       Resolution order when NULL:
         1. $DECK_OS_SURFACE environment variable (if set)
         2. /etc/deck/system.deck-os (if it exists)
         3. Compiled-in default (fatal error if none of the above)
       Supply an explicit path to skip the search. */
    const char*  os_surface_path;
    const char*  app_root_path;      /* directory containing app.deck */
    size_t       heap_limit_bytes;   /* 0 = unlimited */
    int          stack_depth;        /* evaluator call stack depth; 0 = default 512 */
    const char*  log_level;          /* "debug"|"info"|"warn"|"error" */
    DeckLogFn    log_fn;             /* NULL = stderr */
    bool         enable_dap;         /* Debug Adapter Protocol server */
    uint16_t     dap_port;           /* if enable_dap */
    bool         hot_reload;         /* file watcher for deck watch mode */
    /* Hardware RNG function for seeding `random`. Called once during
       deck_runtime_create(). If NULL, the runtime uses platform entropy
       (esp_random() on ESP32, getrandom() on Linux, etc.).
       On platforms without a hardware RNG, supply a software PRNG seeded
       from boot timestamp + chip ID + ADC noise, etc. */
    uint64_t   (*rng_seed_fn)(void);
} DeckRuntimeConfig;

/* Registration (call between create and start) */
void deck_register_capability (...);
void deck_register_stream     (...);
void deck_register_builtin    (...);
void deck_register_type       (...);
void deck_register_event      (DeckRuntime* rt, const char* event_name);

/* Load, verify, and start execution */
DeckLoadResult deck_runtime_load(DeckRuntime* rt);  /* runs loader stages 1-12 */
void           deck_runtime_start(DeckRuntime* rt); /* begins main event loop; blocks */
void           deck_runtime_stop(DeckRuntime* rt);  /* signal shutdown */
void           deck_runtime_destroy(DeckRuntime* rt);

typedef struct DeckLoadResult {
    bool   success;
    int    error_count;
    int    warning_count;
    char** error_messages;   /* NULL-terminated array; free with deck_load_result_free */
    char** warning_messages;
} DeckLoadResult;

void deck_load_result_free(DeckLoadResult* r);
```

### 16.1 Minimal Startup Example

```c
#include "deck_runtime.h"
#include "bmp280_binding.h"
#include "dsp_binding.h"

void app_main(void) {
    DeckRuntimeConfig cfg = {
        .os_surface_path = "/etc/deck/myboard.deck-os",
        .app_root_path   = "/sdcard/apps/monitor",
        .heap_limit_bytes = 256 * 1024,
        .stack_depth      = 256,
        .log_level        = "warn",
    };

    DeckRuntime* rt = deck_runtime_create(&cfg);

    /* Register all native extensions */
    bmp280_register(rt);
    dsp_register(rt);

    /* Load and verify the app */
    DeckLoadResult r = deck_runtime_load(rt);
    if (!r.success) {
        for (int i = 0; r.error_messages[i]; i++) {
            printf("Error: %s\n", r.error_messages[i]);
        }
        deck_load_result_free(&r);
        deck_runtime_destroy(rt);
        return;
    }
    deck_load_result_free(&r);

    /* Run — this blocks until app exits or deck_runtime_stop() is called */
    deck_runtime_start(rt);
    deck_runtime_destroy(rt);
}
```

---

## 17. Providing a @capability That Wraps a Native Service

Some capabilities are not hardware but native service code — an LVGL rendering engine, an RTOS queue, a camera pipeline. The pattern is the same: declare in `.deck-os`, register in C.

### 17.1 Example: Native Display Backlight

**.deck-os:**
```
@capability display.backlight
  set   (level: float)  -> unit    -- 0.0..1.0
  get   ()              -> float   @pure
  pulse (duration: Duration, target: float) -> unit
  @errors
    :unavailable "Backlight hardware not available"
```

**C:**
```c
extern ledc_channel_config_t backlight_channel;  /* platform setup */

static DeckValue* bl_set(DeckRuntime* rt, DeckValue** args, size_t argc,
                          const char** nk, DeckValue** nv, size_t nc) {
    double level = deck_get_float(args[0]);
    if (level < 0.0) level = 0.0;
    if (level > 1.0) level = 1.0;

    uint32_t duty = (uint32_t)(level * 8191);  /* 13-bit LEDC */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, backlight_channel.channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, backlight_channel.channel);
    return deck_unit();
}

static DeckValue* bl_get(DeckRuntime* rt, DeckValue** args, size_t argc,
                          const char** nk, DeckValue** nv, size_t nc) {
    uint32_t duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, backlight_channel.channel);
    return deck_float((double)duty / 8191.0);
}

void display_backlight_register(DeckRuntime* rt) {
    deck_register_capability(rt, "display.backlight", "set",   bl_set,  false, NULL, NULL);
    deck_register_capability(rt, "display.backlight", "get",   bl_get,  false, NULL, NULL);
    deck_register_capability(rt, "display.backlight", "pulse", bl_pulse,false, NULL, NULL);
}
```

---

## 18. Complete Worked Example

A complete native binding for a CO₂ sensor (SCD40 over I2C), with read, stream, and calibration event.

**.deck-os additions:**
```
@type CO2Reading
  ppm       : int
  temp_c    : float
  humidity  : float
  timestamp : Timestamp

@event sensors.co2_calibrated (baseline_ppm: int)

@capability sensors.co2
  read         ()         -> Result CO2Reading sensors.co2.Error
  watch        (interval: Duration) -> Stream CO2Reading
  calibrate    ()         -> Result unit sensors.co2.Error
  @errors
    :unavailable    "SCD40 not found"
    :warming_up     "Sensor still warming up (allow 5s after power-on)"
    :calibrating    "Calibration in progress"
    :i2c_error      "I2C communication failure"
  @requires_permission
```

**C:**
```c
#include "deck_bridge.h"
#include "scd40.h"

static DeckValue* make_co2_reading(uint16_t ppm, float temp, float hum) {
    const char* names[] = { "ppm", "temp_c", "humidity", "timestamp" };
    DeckValue*  vals[4] = {
        deck_int(ppm),
        deck_float(temp),
        deck_float(hum),
        deck_timestamp(esp_timer_get_time() / 1000)  /* us → ms */
    };
    DeckValue* r = deck_record("CO2Reading", names, vals, 4);
    for (int i = 0; i < 4; i++) deck_value_free(vals[i]);
    return r;
}

static DeckValue* co2_read(DeckRuntime* rt, DeckValue** args, size_t argc,
                            const char** nk, DeckValue** nv, size_t nc) {
    uint16_t ppm; float temp, hum;
    scd40_err_t err = scd40_read_measurement(&ppm, &temp, &hum);
    if (err == SCD40_ERR_NOT_FOUND) return deck_err_atom("unavailable");
    if (err == SCD40_ERR_NOT_READY)  return deck_err_atom("warming_up");
    if (err != SCD40_OK)             return deck_err_atom("i2c_error");

    DeckValue* reading = make_co2_reading(ppm, temp, hum);
    DeckValue* result  = deck_ok(reading);
    deck_value_free(reading);
    return result;
}

typedef struct { DeckRuntime* rt; uint64_t sub_id; int64_t ms; bool active; } Co2WatchCtx;

static void co2_watch_task(void* arg) {
    Co2WatchCtx* ctx = arg;
    while (ctx->active) {
        DeckValue* v = co2_read(ctx->rt, NULL, 0, NULL, NULL, 0);
        if (deck_is_ok(v)) {
            DeckValue* reading = deck_get_ok_val(v);
            deck_stream_push(ctx->rt, ctx->sub_id, reading);
        } else {
            deck_stream_error(ctx->rt, ctx->sub_id, v);
            deck_value_free(v);
            break;
        }
        deck_value_free(v);
        vTaskDelay(pdMS_TO_TICKS(ctx->ms));
    }
    free(ctx); vTaskDelete(NULL);
}

static uint64_t co2_watch_start(DeckRuntime* rt, DeckValue** args, size_t argc,
                                  const char** nk, DeckValue** nv, size_t nc) {
    DeckValue* iv  = deck_named_arg(nk, nv, nc, "interval");
    int64_t ms     = iv ? deck_get_duration(iv) : 5000;
    uint64_t sub   = deck_new_subscription_id(rt);
    Co2WatchCtx* ctx = malloc(sizeof(Co2WatchCtx));
    *ctx = (Co2WatchCtx){ .rt = rt, .sub_id = sub, .ms = ms, .active = true };
    deck_subscription_set_ctx(rt, sub, ctx);
    xTaskCreate(co2_watch_task, "co2_watch", 4096, ctx, 5, NULL);
    return sub;
}

static void co2_watch_stop(DeckRuntime* rt, uint64_t sub) {
    Co2WatchCtx* ctx = deck_subscription_get_ctx(rt, sub);
    if (ctx) ctx->active = false;
}

static DeckValue* co2_calibrate(DeckRuntime* rt, DeckValue** args, size_t argc,
                                  const char** nk, DeckValue** nv, size_t nc) {
    uint16_t baseline;
    scd40_err_t err = scd40_perform_forced_recalibration(400, &baseline);
    if (err == SCD40_ERR_NOT_FOUND) return deck_err_atom("unavailable");
    if (err != SCD40_OK)            return deck_err_atom("calibrating");

    /* Fire event to notify all apps */
    const char* names[] = { "baseline_ppm" };
    DeckValue*  vals[]  = { deck_int(baseline) };
    deck_fire_event_map(rt, "sensors.co2_calibrated", names, vals, 1);
    deck_value_free(vals[0]);

    /* The .deck-os declares calibrate() -> Result unit sensors.co2.Error
       so we must return :ok unit, not bare unit */
    DeckValue* u      = deck_unit();
    DeckValue* result = deck_ok(u);
    deck_value_free(u);
    return result;
}

void scd40_register(DeckRuntime* rt) {
    deck_register_capability(rt, "sensors.co2", "read",      co2_read,     false, NULL, NULL);
    deck_register_stream     (rt, "sensors.co2", "watch",     co2_watch_start, co2_watch_stop);
    deck_register_capability (rt, "sensors.co2", "calibrate", co2_calibrate,false, NULL, NULL);
}
```

---

## 19. Testing Native Bindings

### 19.1 Mock Bridge

For `deck test` to work without hardware, provide mock implementations alongside the real ones. The build system selects which to link.

```c
/* sensors_co2_mock.c — compiled in test builds */
static DeckValue* co2_read_mock(DeckRuntime* rt, DeckValue** args, size_t argc,
                                  const char** nk, DeckValue** nv, size_t nc) {
    /* Return deterministic test data */
    DeckValue* reading = make_co2_reading(412, 22.5f, 55.0f);
    DeckValue* result  = deck_ok(reading);
    deck_value_free(reading);
    return result;
}
```

### 19.2 Injection via @test Context

The `deck_runtime_set_test_context()` API allows tests to override capability responses:

```c
/* Before running tests: */
deck_test_override_capability(rt, "sensors.co2", "read",
    /* return :err :unavailable for this test */
    always_err_fn, "unavailable");
```

From Deck test code:
```deck
@test "co2 view shows error when sensor unavailable"
  let ctx = simulate
    "sensors.co2": :unavailable
  assert Co2View.body_contains(ctx, banner ":danger")
```

### 19.3 Value Comparison in Tests

```c
bool deck_value_equal(const DeckValue* a, const DeckValue* b);  /* deep structural */

/* In test assertions: */
DeckValue* expected = deck_record("CO2Reading", ...);
DeckValue* actual   = co2_read_mock(rt, NULL, 0, NULL, NULL, 0);
assert(deck_value_equal(expected, deck_get_ok_val(actual)));
```

---

## 20. Reference Summary

### 20.1 Registration Functions

| Function | When to call |
|---|---|
| `deck_register_capability` | Sync method on a capability |
| `deck_register_stream` | Method returning `Stream T` |
| `deck_register_builtin` | Always-in-scope pure function |
| `deck_register_type` | Optional: validates a `.deck-os` @type |
| `deck_register_opaque_type` | Names an opaque handle type |
| `deck_register_event` | Declares a custom event (if not in .deck-os) |

### 20.2 Runtime-to-Bridge (events you fire)

| Function | Purpose |
|---|---|
| `deck_stream_push` | Push next value in a stream |
| `deck_stream_error` | Signal stream error |
| `deck_stream_end` | Signal stream completed |
| `deck_fire_event` | Fire an OS event (thread-safe, not ISR-safe) |
| `deck_fire_event_map` | Fire an OS event with named fields (thread-safe) |
| `deck_fire_event_isr` | Fire an OS event from an ISR (lock-free, pre-allocated payload only) |
| `deck_dispatch_main` | Queue a callback on the main loop (ISR-safe) |

### 20.3 Value Lifecycle

```
deck_*() constructors   → Bridge owns. Must free or return to runtime.
Return from callback    → Runtime takes ownership. Bridge must NOT free.
deck_value_retain()     → Increment refcount (bridge keeps a reference).
deck_value_release()    → Decrement; free if zero.
deck_value_free()       → Immediate free (only when refcount is 1).
get_ accessors          → Pointer into value; valid while parent lives.
```
