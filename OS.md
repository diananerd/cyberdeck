# S3 CyberDeck — OS Design Specification

> Este documento describe el **ideal arquitectónico** del OS del CyberDeck. No es un plan de trabajo —
> es la especificación de destino. Cada decisión de diseño aquí tiene el propósito de ser correcta
> para un RTOS embebido con recursos limitados (~80 KB heap libre, PSRAM disponible, dual-core),
> extensible hacia apps dinámicas desde SD, y mantenible por una sola persona.

---

## 1. Filosofía

El OS es el intermediario entre el hardware y las apps. Su trabajo es:

- **Abstraer** todo lo que el hardware hace feo (I2C, ADC, SD mount, VSYNC, locks LVGL).
- **Exponer** APIs limpias y seguras que una app no pueda usar incorrectamente.
- **Delegar** trabajo pesado (red, disco, descargas, parseo) fuera del UI task.
- **Garantizar** que una app muerta no corrompa el estado del sistema.
- **Permitir** que en el futuro una app llegue desde la SD card igual que una compilada.

Las apps no conocen el hardware. Las apps no conocen FreeRTOS. Las apps no saben cuántos cores hay.
Una app sólo conoce las APIs que el OS le entrega.

---

## 2. Capas del Sistema

```
+-----------------------------------------------------+
|              APPS (built-in + dynamic)              |  <- solo usan OS APIs
+-----------------------------------------------------+
|                  APP FRAMEWORK                      |  <- ciclo de vida, navegacion, estado
+----------------------+------------------------------+
|     UI ENGINE        |      OS KERNEL (sys_services)|  <- LVGL + render vs. servicios
+----------------------+------------------------------+
|                   BOARD / HAL                       |  <- hardware puro
+-----------------------------------------------------+
```

Las dependencias solo fluyen **hacia abajo**. Un app no importa nada de `board/`. Una app no llama
`esp_wifi_*` directamente. El HAL no sabe que existe LVGL.

---

## 3. Modelo de App

### 3.1 Que es una App

Una **app** es una unidad de funcionalidad independiente que:

- Tiene un **manifest** (nombre, icono, ID, permisos declarados).
- Expone uno o mas **views** (pantallas LVGL con su ciclo de vida).
- Puede registrar **background workers** (tareas del OS que viven mas que un view).
- Tiene su propio **namespace de storage** en `/sdcard/apps/<app_id>/`.
- Tiene su propio **namespace de settings** en NVS bajo la clave `app.<app_id>`.
- Se comunica con el resto del sistema **exclusivamente** via event bus o OS APIs.

Una app **nunca**:
- Llama funciones de otra app directamente.
- Accede a `app_state_get()` para escribir.
- Crea tasks de FreeRTOS directamente.
- Llama `ui_lock()` / `ui_unlock()` — eso es trabajo del OS.

### 3.2 App Manifest

```c
typedef struct {
    app_id_t        id;           // uint16_t unico, 0-255 reservado para built-in
    const char     *name;         // "SETTINGS", "FILES", "NOTES"
    const char     *icon;         // LV_SYMBOL_* o char literal
    app_type_t      type;         // APP_TYPE_BUILTIN | APP_TYPE_NATIVE_PLUGIN | APP_TYPE_SCRIPT
    uint8_t         permissions;  // bitmask: PERM_WIFI | PERM_SD | PERM_NETWORK | PERM_SETTINGS
    const char     *storage_dir;  // e.g. "/sdcard/apps/notes" -- NULL si no necesita storage
} app_manifest_t;
```

### 3.3 Registro de Apps

El registro es **dinamico**: una tabla en RAM que se puede extender en runtime.

```c
// Built-in: en compile time
os_app_register(&launcher_manifest, &launcher_ops);
os_app_register(&settings_manifest, &settings_ops);

// Dynamic (en boot, desde /sdcard/apps/*/manifest.json o manifest.bin)
os_app_discover_sd();   // escanea SD y registra las que encuentre

// Lookup
const app_entry_t *os_app_get(app_id_t id);
void               os_app_enumerate(void (*cb)(const app_entry_t *, void *), void *ctx);
```

El launcher siempre obtiene la lista via `os_app_enumerate()` — nunca hardcodeada.

### 3.4 App Operations (vtable)

Cada app expone un struct de punteros a funcion:

```c
typedef struct {
    // Obligatorio
    esp_err_t (*on_launch   )(const app_launch_ctx_t *ctx);  // primera vez que se abre
    void      (*on_terminate)(app_id_t id);                  // sistema la mata

    // Opcional
    esp_err_t (*on_background)(app_id_t id);        // el OS la mueve a BG
    esp_err_t (*on_foreground)(app_id_t id);        // vuelve a ser visible
    esp_err_t (*on_intent    )(const intent_t *i);  // otra app la llama con datos
    size_t    (*get_mem_usage)(void);               // para el task manager
} app_ops_t;
```

---

## 4. Modelo de View (Pantalla)

Una app puede tener N views. Un view es la unidad de pantalla que el UI engine maneja.

### 4.1 View Descriptor

```c
typedef struct {
    view_id_t       id;
    const char     *debug_name;

    // Ciclo de vida
    void *(*on_create )(lv_obj_t *screen, const view_args_t *args); // retorna state*
    void  (*on_resume )(lv_obj_t *screen, void *state);
    void  (*on_pause  )(lv_obj_t *screen, void *state);
    void  (*on_destroy)(lv_obj_t *screen, void *state);             // libera state*

    // Opcional: recibir eventos mientras es el view activo
    void  (*on_event  )(view_event_t evt, const void *data, void *state);
} view_desc_t;
```

`on_create` retorna el puntero de estado propio del view — el OS lo almacena y lo pasa en los
siguientes callbacks. La firma refleja el ownership claramente: el view aloca, el view libera.

### 4.2 Maquina de Estados del View

Cada view es implicitamente una FSM. El OS garantiza estas transiciones:

```
          push()                 push otro view
  NONE ---------> CREATED -----------------------------> PAUSED
                    |                                       |
                    | resume_top()                          | pop() de lo de arriba
                    v                                       |
                 RESUMED <---------------------------------/
                    |
                    | pop()
                    v
                DESTROYED
```

Invariante: `on_destroy` **siempre** se llama antes de liberar la pantalla LVGL. Sin excepciones,
ni en rotacion, ni en pop-to-home, ni en kill de app.

### 4.3 Interaccion entre Views de la misma App

Un view puede navegar a otro view de la misma app directamente:

```c
os_view_push(APP_ID_SETTINGS, VIEW_WIFI_LIST, NULL);
os_view_push(APP_ID_SETTINGS, VIEW_WIFI_CONNECT,
             &(view_args_t){ .data = ssid, .size = 33 });
os_view_pop();
os_view_pop_to_root();   // vuelve al view[0] de la app actual
os_view_home();          // vuelve al launcher
```

`view_args_t.data` es heap del caller. El OS lo transfiere al `on_create` del view hijo y lo
libera una vez que `on_create` retorna. Si el view hijo quiere conservarlo, lo copia.

### 4.4 Stack de Views

El OS mantiene un activity stack global (max 8 niveles — extendido desde 4):

```
[0] Launcher (root, nunca se destruye)
[1] App A - View principal
[2] App A - Sub-view
[3] App B (lanzada con intent desde App A)
```

Si el stack esta lleno (8), la operacion push falla con `OS_ERR_STACK_FULL` — la app puede
manejar esto o el OS muestra un toast. No eviccion silenciosa.

---

## 5. Background Workers

Una app puede necesitar trabajo en background que sobrevive a sus views (descargas, sync
periodico, reproduccion de audio). Esto se hace via **workers registrados en el OS**, no con
tasks de FreeRTOS directas.

### 5.1 Registro de Worker

```c
typedef struct {
    const char    *name;         // para debug y task manager
    worker_fn_t    fn;           // void fn(void *arg, worker_handle_t h)
    void          *arg;
    size_t         stack_bytes;
    uint8_t        priority;     // relativa, el OS mapea a prioridades FreeRTOS
    worker_core_t  core;         // WORKER_CORE_ANY | WORKER_CORE_BG (0) | WORKER_CORE_UI (1)
    bool           auto_restart; // el OS reinicia si muere inesperadamente
} worker_config_t;

worker_handle_t os_worker_create(app_id_t owner, const worker_config_t *cfg);
void            os_worker_stop(worker_handle_t h);
void            os_worker_notify(worker_handle_t h, uint32_t value);
```

El OS lleva registro de que app owns que worker. Si la app es terminada, el OS mata sus workers.

### 5.2 Workers Predefinidos del OS

El OS trae sus propios workers que las apps pueden usar sin crearlos:

| Worker      | API                                          | Descripcion                        |
|-------------|----------------------------------------------|------------------------------------|
| Downloader  | `os_download(url, path, cb)`                 | HTTPS con resume, progress events  |
| Poller      | `os_poller_register(fn, interval_ms, arg)`   | Trabajo periodico sin task propia  |
| Deferred    | `os_defer(fn, arg, delay_ms)`                | One-shot, como setTimeout          |
| UI Updater  | `os_ui_post(fn, arg)`                        | Ejecuta fn en el LVGL task safe    |

`os_ui_post` es la forma correcta de actualizar UI desde un worker — nunca `ui_lock()` manual.

---

## 6. OS APIs Expuestas a Apps

Estas son las unicas interfaces que una app deberia necesitar. Toda comunicacion con el sistema
pasa por aqui.

### 6.1 Storage API

```c
// Path helper -- devuelve "/sdcard/apps/<app_id>/<filename>"
const char *os_storage_path(app_id_t id, const char *filename, char *buf, size_t len);

// Operaciones simples (KV sobre archivos planos)
esp_err_t os_storage_read (app_id_t id, const char *name, void *buf, size_t *len);
esp_err_t os_storage_write(app_id_t id, const char *name, const void *data, size_t len);
esp_err_t os_storage_delete(app_id_t id, const char *name);
bool      os_storage_exists(app_id_t id, const char *name);

// Directorio completo (para apps que manejan multiples archivos)
const char *os_storage_dir(app_id_t id);  // "/sdcard/apps/notes"

// Acceso a SD directo (requiere PERM_SD)
FILE *os_storage_fopen(app_id_t id, const char *rel_path, const char *mode);
```

Una app con `storage_dir = "/sdcard/apps/notes"` encuentra ahi sus archivos. El OS garantiza
que el directorio existe antes de entregar el path.

### 6.2 Settings API (por app)

```c
// Namespace automatico: "app.<app_id>"
esp_err_t os_settings_get_str (app_id_t id, const char *key, char *out, size_t len);
esp_err_t os_settings_set_str (app_id_t id, const char *key, const char *val);
esp_err_t os_settings_get_int (app_id_t id, const char *key, int32_t *out);
esp_err_t os_settings_set_int (app_id_t id, const char *key, int32_t val);
esp_err_t os_settings_get_bool(app_id_t id, const char *key, bool *out);
esp_err_t os_settings_set_bool(app_id_t id, const char *key, bool val);
esp_err_t os_settings_clear   (app_id_t id, const char *key);
```

El `svc_settings` actual pasa a ser la implementacion interna del namespace `"sys"`.

### 6.3 Event API

```c
// Suscripcion -- el OS registra por app_id para cleanup automatico
event_sub_t os_event_subscribe(app_id_t owner, cyberdeck_event_t evt,
                               event_handler_fn_t fn, void *ctx);
void        os_event_unsubscribe(event_sub_t sub);
esp_err_t   os_event_post(cyberdeck_event_t evt, const void *data, size_t data_len);

// Helper: el handler se ejecuta ya con ui_lock tomado
event_sub_t os_event_subscribe_ui(app_id_t owner, cyberdeck_event_t evt,
                                  event_handler_fn_t fn, void *ctx);
```

`os_event_subscribe_ui` elimina el patron boilerplate de lock/unlock en cada handler de UI.
Cuando la app termina, `os_event_unsubscribe_all(app_id)` limpia todo automaticamente.

### 6.4 Network API

```c
// Estado (no bloquea)
bool        os_net_is_connected(void);
const char *os_net_ssid(void);
int8_t      os_net_rssi(void);

// HTTP (requiere PERM_NETWORK)
esp_err_t os_http_get (const char *url, os_http_response_t *resp);
esp_err_t os_http_post(const char *url, const void *body, size_t len,
                       os_http_response_t *resp);
void      os_http_response_free(os_http_response_t *resp);
```

### 6.5 Display / UI API

```c
// El view recibe su screen como argumento en on_create -- no necesita hacer nada mas.
// Para interactuar con statusbar/navbar desde el view:
void os_ui_set_title  (const char *title);
void os_ui_show_toast (const char *msg, uint32_t duration_ms);
void os_ui_show_confirm(const char *title, const char *msg,
                        confirm_cb_t cb, void *ctx);
void os_ui_show_loading(bool show);

// Actualizar UI desde un worker (thread-safe):
void os_ui_post(os_ui_fn_t fn, void *arg);
```

### 6.6 Time API

```c
time_t   os_time_now(void);
bool     os_time_is_synced(void);
esp_err_t os_time_format(time_t t, const char *fmt, char *buf, size_t len);
```

### 6.7 Intent API (comunicacion entre apps)

```c
typedef struct {
    app_id_t    target_app;
    const char *action;       // "VIEW", "PICK_FILE", "SHARE"
    const char *mime_type;    // "text/plain", "image/*"
    const void *data;
    size_t      data_len;
    intent_cb_t result_cb;    // NULL si fire-and-forget
    void       *result_ctx;
} intent_t;

esp_err_t os_intent_send(const intent_t *intent);
esp_err_t os_intent_register_handler(app_id_t id, const char *action,
                                     intent_handler_fn_t fn);
```

---

## 7. Sistema de Storage

### 7.1 Estructura de Directorios en SD

```
/sdcard/
+-- apps/
|   +-- notes/
|   |   +-- notes.csv          <- datos de la app Notes
|   |   +-- attachments/
|   +-- tasks/
|   |   +-- tasks.db           <- SQLite
|   +-- books/
|   |   +-- *.epub / *.txt
|   +-- <script_app_id>/       <- app cargada dinamicamente
|       +-- manifest.json
|       +-- main.py            <- MicroPython / Lua / DSL
|       +-- data/
+-- media/
|   +-- music/
|   +-- podcasts/
+-- downloads/
|   +-- *.download             <- metadata de descargas en progreso
+-- .cyberdeck/
    +-- crash.log              <- log de panics y errores fatales
```

### 7.2 Bases de Datos Ligeras

El OS ofrece tres niveles de persistencia para apps:

| Nivel       | Mecanismo                        | Cuando usar                           |
|-------------|----------------------------------|---------------------------------------|
| Clave-valor | NVS via `os_settings_*`          | Configuracion, preferencias           |
| Tabla simple| CSV en SD via `os_storage_*`     | Listas, notas, tareas simples         |
| Relacional  | SQLite via `os_db_*` (PERM_SD)   | Datos estructurados, busqueda         |

```c
// SQLite wrapper (componente idf-sqlite3)
os_db_t   *os_db_open(app_id_t id, const char *filename);
esp_err_t  os_db_exec(os_db_t *db, const char *sql,
                      os_db_row_cb_t cb, void *ctx);
esp_err_t  os_db_close(os_db_t *db);
```

El componente `idf-sqlite3` es compatible con ESP-IDF y funciona bien con PSRAM habilitado.
La habilitacion es opt-in por app (requiere PERM_SD en el manifest).

### 7.3 Montaje y Desmontaje

- Al montar: crea `/sdcard/apps/` si no existe, verifica espacio libre.
- Apps con `storage_dir != NULL`: el OS crea el directorio en el primer launch.
- Al desmontar: publica `EVT_SDCARD_UNMOUNTED`. Apps con archivos abiertos deben escuchar
  este evento y cerrar sus handles en `on_event`.
- Apps dinamicas: marcadas como unavailable si la SD se desmonta (launcher las muestra dim).

---

## 8. Apps Dinamicas desde SD

### 8.1 Tipos de App

```c
typedef enum {
    APP_TYPE_BUILTIN,        // compilada en firmware, siempre disponible
    APP_TYPE_NATIVE_PLUGIN,  // codigo nativo (futuro, requiere linker dinamico)
    APP_TYPE_SCRIPT,         // interpretada por un runtime incluido en firmware
} app_type_t;
```

### 8.2 Runtime de Scripts

El firmware puede incluir uno o mas runtimes de script. El OS los registra:

```c
typedef struct {
    const char *name;          // "micropython", "lua", "cyberdeck-dsl"
    const char *file_ext;      // ".py", ".lua", ".cd"
    esp_err_t (*load  )(const char *path, script_handle_t *out);
    esp_err_t (*call  )(script_handle_t h, const char *fn,
                        const script_args_t *args);
    void      (*unload)(script_handle_t h);
} script_runtime_t;

void os_script_register_runtime(const script_runtime_t *rt);
```

Una app de tipo `APP_TYPE_SCRIPT` indica en su manifest el runtime a usar.
El OS busca el runtime correcto y delega la ejecucion.

### 8.3 Manifest de App Dinamica (JSON)

```json
{
  "id": 256,
  "name": "MY APP",
  "icon": "A",
  "type": "script",
  "runtime": "micropython",
  "entry": "main.py",
  "permissions": ["sd", "network"],
  "version": "1.0.0",
  "min_os_api": "1.0.0"
}
```

### 8.4 APIs Expuestas al Script (MicroPython ejemplo)

```python
import cyberdeck

# UI
cyberdeck.ui.set_title("MY APP")
cyberdeck.ui.show_toast("Hello!", 1500)
cyberdeck.ui.show_confirm("SURE?", "This will delete X", callback)

# Storage
cyberdeck.storage.write("data.txt", "hello world")
data = cyberdeck.storage.read("data.txt")
f = cyberdeck.storage.open("notes.csv", "r")

# Network
resp = cyberdeck.net.get("https://api.example.com/data")

# Settings
cyberdeck.settings.set("theme", "dark")
val = cyberdeck.settings.get("theme")

# Events
cyberdeck.events.on("wifi_connected", my_handler)
```

La API del script es un subconjunto deliberadamente restringido — el runtime hace de sandbox.

### 8.5 Descubrimiento

```c
void os_app_discover_sd(void) {
    // Lee /sdcard/apps/*/manifest.json
    // Para cada manifest valido: os_app_register(manifest, &script_ops)
    // Si min_os_api > os_api_version: registrar como incompatible (no lanzable)
    // Publicar EVT_SD_APP_DISCOVERED por cada app nueva
}
```

Se llama en boot (despues de montar SD) y al recibir `EVT_SDCARD_MOUNTED`.

---

## 9. Modelo de Procesos y Memoria

### 9.1 Task Factory

Todas las tasks del OS se crean a traves de un factory unificado:

```c
typedef struct {
    const char    *name;
    size_t         stack_bytes;
    uint8_t        priority;   // OS_PRIO_LOW / MEDIUM / HIGH / REALTIME
    int            core;       // OS_CORE_BG(0) / OS_CORE_UI(1) / OS_CORE_ANY
    app_id_t       owner;      // OS_OWNER_SYSTEM para tasks del propio OS
    TaskFunction_t fn;
    void          *arg;
} os_task_config_t;

TaskHandle_t os_task_create(const os_task_config_t *cfg);
void         os_task_destroy(TaskHandle_t h);
```

El OS lleva contabilidad de tasks por app. Si una app se termina, todas sus tasks mueren.

### 9.2 Niveles de Prioridad

| Nivel OS            | Prioridad FreeRTOS | Quien lo usa                         |
|---------------------|--------------------|--------------------------------------|
| `OS_PRIO_LOW`       | 1                  | Battery poll, SD poll                |
| `OS_PRIO_MEDIUM`    | 2                  | Time update, workers de apps         |
| `OS_PRIO_HIGH`      | 3                  | LVGL task, event bus                 |
| `OS_PRIO_REALTIME`  | 5                  | ISR callbacks, gesture detection     |

### 9.3 Arenas de Memoria

```
Flash (8 MB):
  - Firmware:        ~3 MB
  - LVGL assets:     ~1 MB
  - OTA partition:   ~2 MB
  - NVS:             ~0.5 MB
  - Reserva:         ~0.5 MB

IRAM (interno, ~512 KB):
  - FreeRTOS kernel
  - ISR handlers, LVGL flush callback

DRAM (interno, ~320 KB usable):
  - Heap primario
  - Task stacks (~45 KB actualmente)
  - App states, intent data, estructuras pequenas

PSRAM (externo, ~8 MB):
  - Framebuffers RGB (~2 MB para triple buffer 800x480x2)
  - Heap para allocations grandes (SQLite pages, downloader, bitmaps)
```

El OS declara que usa PSRAM:

```c
void *os_malloc_psram(size_t size);   // para buffers > 8 KB
void  os_free_psram(void *ptr);
```

---

## 10. Bus de Eventos

### 10.1 Taxonomia Completa de Eventos

```c
// Sistema
EVT_BOOT_COMPLETE
EVT_SHUTDOWN_REQUESTED
EVT_MEMORY_LOW              // heap libre < threshold (20 KB)

// Hardware
EVT_BATTERY_UPDATED         // data: uint8_t percentage
EVT_BATTERY_LOW             // data: NULL (threshold 15%)
EVT_SDCARD_MOUNTED          // data: sd_info_t
EVT_SDCARD_UNMOUNTED        // data: NULL
EVT_DISPLAY_ROTATED         // data: uint8_t orientation

// Conectividad
EVT_WIFI_CONNECTED          // data: wifi_info_t
EVT_WIFI_DISCONNECTED       // data: NULL
EVT_WIFI_SCAN_DONE          // data: ap_list_t*

// Tiempo
EVT_TIME_SYNCED             // data: time_t
EVT_RTC_UPDATED             // data: time_t

// Navegacion
EVT_GESTURE_HOME
EVT_GESTURE_BACK
EVT_NAV_PROCESSES           // abre task manager

// Apps
EVT_APP_LAUNCHED            // data: app_id_t
EVT_APP_TERMINATED          // data: app_id_t
EVT_SD_APP_DISCOVERED       // data: app_id_t

// Descargas / Red
EVT_DL_STARTED              // data: dl_info_t
EVT_DL_PROGRESS             // data: dl_progress_t
EVT_DL_COMPLETE             // data: dl_info_t
EVT_DL_ERROR                // data: dl_error_t

// OTA
EVT_OTA_STARTED
EVT_OTA_PROGRESS            // data: uint8_t percentage
EVT_OTA_COMPLETE
EVT_OTA_ERROR               // data: const char* message

// Settings
EVT_SETTINGS_CHANGED        // data: settings_key_t
EVT_THEME_CHANGED           // data: ui_theme_id_t
```

### 10.2 Reglas del Bus

- Handlers que tocan LVGL usan `os_event_subscribe_ui()` — el OS hace lock/unlock.
- Los datos del evento son propiedad del bus mientras el handler corre. Copiar antes de retornar.
- Queue size: 32 (vs 16 actual) — la perdida silenciosa es peor que el delay.
- Nunca postear desde un ISR sin `xQueueSendFromISR`.

---

## 11. App: Settings (Referencia de Implementacion)

Settings es la app 0 (ID `APP_ID_SETTINGS`). Built-in y monolitica — vive en firmware.
Sin embargo, debe comportarse exactamente como cualquier otra app: usa OS APIs exclusivamente.

### 11.1 View Map de Settings

```
VIEW_SETTINGS_MAIN          <- lista de categorias
+-- VIEW_SETTINGS_DISPLAY
+-- VIEW_SETTINGS_WIFI_LIST
|   +-- VIEW_SETTINGS_WIFI_CONNECT    <- recibe ssid via view_args
+-- VIEW_SETTINGS_AUDIO
+-- VIEW_SETTINGS_TIME
+-- VIEW_SETTINGS_STORAGE
+-- VIEW_SETTINGS_SECURITY
+-- VIEW_SETTINGS_BLUETOOTH
+-- VIEW_SETTINGS_ABOUT
    +-- VIEW_SETTINGS_OTA             <- lanzado desde About
```

### 11.2 Patron Correcto de Sub-View

```c
// En settings_main, handler de tap en "WiFi":
static void wifi_item_tap(lv_event_t *e) {
    os_view_push(APP_ID_SETTINGS, VIEW_SETTINGS_WIFI_LIST, NULL);
}

// En settings_wifi, para ir al sub-view de conexion:
static void ap_tap(lv_event_t *e) {
    char *ssid = strdup(ap->ssid);
    os_view_push(APP_ID_SETTINGS, VIEW_SETTINGS_WIFI_CONNECT,
                 &(view_args_t){ .data = ssid, .size = strlen(ssid)+1,
                                 .owned = true });
}
```

### 11.3 Uso de Storage en Settings

Settings NO usa `/sdcard/` — su persistence es NVS via `os_settings_*`:

```c
char ssid[33];
os_settings_get_str(APP_ID_SETTINGS, "wifi_ssid_0", ssid, sizeof(ssid));
os_settings_set_str(APP_ID_SETTINGS, "wifi_ssid_0", ssid);
```

---

## 12. App: Launcher (Referencia de Implementacion)

El Launcher es el root de la activity stack (index 0, nunca se destruye).
No es "una app" que el usuario abre, pero implementa el mismo modelo de view.

### 12.1 Responsabilidades

- Listar todas las apps registradas via `os_app_enumerate()`.
- Mostrar apps dinamicas de SD con distincion visual (icono distinto, dim si unavailable).
- Lanzar apps via `os_intent_send()`.
- Gestionar lockscreen como view especial (`VIEW_LAUNCHER_LOCK`).
- Actualizar grid en `EVT_SD_APP_DISCOVERED` y `EVT_SDCARD_UNMOUNTED`.

### 12.2 App Cards Dinamicas

```c
os_app_enumerate(build_app_card, launcher_state);

static void build_app_card(const app_entry_t *app, void *ctx) {
    launcher_state_t *s = ctx;
    bool is_dynamic   = (app->manifest.type != APP_TYPE_BUILTIN);
    bool is_available = app->available;  // false si SD desmontada
    // crear card con estilo dim si !is_available
    // icono diferente para apps dinamicas
}
```

---

## 13. Consideraciones de RTOS

### 13.1 Watchdog por App

El OS puede registrar watchdog timers por app activa. Si una app no hace `os_watchdog_feed()`
en N ms (configurable, default 5000), el OS termina la app, muestra toast, vuelve al launcher.

### 13.2 Stack Overflow Detection

Todas las tasks via `os_task_create()` tienen stack canary habilitado. En overflow, el OS
guarda el crash en `/sdcard/.cyberdeck/crash.log` antes de reboot.

### 13.3 Memoria Baja

Cuando heap libre < 20 KB:
1. Publica `EVT_MEMORY_LOW`.
2. Llama `on_background()` en todas las apps no activas.
3. Apps en background liberan caches y buffers opcionales.

### 13.4 Afinidad de Cores

```
Core 0 (Background):
  - Event bus task
  - Battery, SD poll, Time update
  - Downloader, OTA
  - Background workers de apps
  - WiFi callbacks (ESP-IDF los fija en Core 0)

Core 1 (UI):
  - LVGL task
  - Gesture detection
  - UI deferred callbacks (os_ui_post)
```

Las apps nunca especifican core directamente — usan `WORKER_CORE_BG` o `WORKER_CORE_UI`.

---

## 14. Seguridad y Permisos

```c
typedef enum {
    PERM_WIFI     = BIT(0),  // usar red WiFi
    PERM_SD       = BIT(1),  // acceder a /sdcard
    PERM_NETWORK  = BIT(2),  // hacer HTTP requests
    PERM_SETTINGS = BIT(3),  // leer/escribir settings del sistema
    PERM_OVERLAY  = BIT(4),  // mostrar overlays sobre otras apps
    PERM_AUTORUN  = BIT(5),  // ejecutar en boot sin interaccion del usuario
} app_permission_t;
```

Si una app script llama una API que requiere un permiso no declarado, el OS devuelve error
(no crash, no excepcion silenciosa). Apps built-in declaran permisos en su manifest igualmente
(documentacion + enforcement consistente).

---

## 15. Task Manager (Vista de Procesos)

Accesible via swipe desde la navbar (EVT_NAV_PROCESSES). Muestra:

- Todas las apps registradas: estado (running / background / stopped / unavailable).
- Workers activos por app con stack usage.
- Heap libre global y por arena (DRAM vs PSRAM).
- Uptime, boot count, ultimo crash.
- Para apps dinamicas: runtime, version, path en SD.

El task manager puede terminar apps (excepto Launcher y Settings con PIN activo).

---

## 16. OTA y Versionado

```c
typedef struct {
    char firmware_version[16]; // semver "0.4.0"
    char os_api_version[16];   // "1.0.0" -- incrementa en breaking changes de API
    char build_date[20];
    char git_commit[12];
} os_version_t;

const os_version_t *os_get_version(void);
```

Los manifests de apps dinamicas declaran `"min_os_api"`. Si la version de OS API es menor,
la app no carga y muestra error explicativo en el launcher.

OTA actualiza firmware + runtimes de script. Apps en SD se actualizan copiando archivos
(no via OTA del firmware).

---

## Apendice: Glosario

| Termino        | Definicion                                                              |
|----------------|-------------------------------------------------------------------------|
| App            | Unidad de funcionalidad con manifest, views, y storage propio           |
| View           | Pantalla LVGL con ciclo de vida (create/resume/pause/destroy)           |
| Worker         | Task de FreeRTOS gestionada por el OS, con owner app                   |
| Intent         | Mensaje inter-app con accion, tipo MIME, y datos                        |
| Manifest       | Descriptor estatico de una app (ID, nombre, permisos)                  |
| Built-in       | App compilada en el firmware                                            |
| Dynamic        | App cargada desde SD card, interpretada por un runtime                 |
| Activity Stack | Pila global de views activos (max 8)                                    |
| OS API         | Funcion del OS expuesta a apps; unica forma valida de interactuar      |
| Runtime        | Motor de interpretacion de scripts (MicroPython, Lua, DSL custom)      |
