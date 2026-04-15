# S3 CyberDeck — Refactor Roadmap

> Ruta granular desde el estado actual hasta el ideal descrito en OS.md.
> Cada tarea es lo suficientemente pequena para un solo commit o PR.
> Las tareas dentro de un mismo Track son independientes entre si y paralelizables.
> Las dependencias entre Tracks estan indicadas explicitamente.

---

## Estado Actual (Origen)

- Registro de apps: array estatico de 10 slots en `app_registry.c`
- Ciclo de vida: `activity_cbs_t` con 4 callbacks, stack de 4 entradas en `ui_activity.c`
- Navegacion: `ui_intent.c` con callback unico, `app_manager.c` como puente
- Settings: namespace NVS "cyberdeck" global, sin aislamiento por app
- Eventos: `svc_event.c` con handlers manuales, lock/unlock boilerplate en cada handler UI
- Storage: SD montada en `/sdcard`, sin estructura de directorios por app
- Tasks: 7 tasks creadas directamente con `xTaskCreate`, sin factory, sin ownership
- Workers: no existe concepto formal; cada servicio crea su propia task
- Apps dinamicas: no existe soporte

---

## Track A — OS Core: Task Factory y Ownership

**Objetivo:** Todas las tasks pasan por un factory con ownership por app.
**Prerequisito:** Ninguno. Empezar aqui.

### A1 — Definir tipos base en `os_core.h`

Crear `components/os_core/include/os_core.h` con:
- `app_id_t` (uint16_t), constantes `OS_OWNER_SYSTEM`, `APP_ID_LAUNCHER`, `APP_ID_SETTINGS`
- `OS_PRIO_LOW/MEDIUM/HIGH/REALTIME` mapeados a valores FreeRTOS
- `OS_CORE_BG(0)`, `OS_CORE_UI(1)`, `OS_CORE_ANY(-1)`
- `os_task_config_t` struct

No implementar nada todavia — solo tipos y constantes.

**Archivos:** nuevo `components/os_core/include/os_core.h`
**Tamano:** pequeno (~60 lineas)

---

### A2 — Implementar `os_task_create` / `os_task_destroy`

En `components/os_core/os_task.c`:
- `os_task_create(const os_task_config_t *cfg)` llama `xTaskCreatePinnedToCore`
- `os_task_destroy(TaskHandle_t h)` llama `vTaskDelete`
- Tabla interna `s_task_registry[OS_MAX_TASKS]` con {handle, owner, name}
- `os_task_destroy_all_for_app(app_id_t id)` itera tabla y mata las del owner

**Depende de:** A1
**Archivos:** `components/os_core/os_task.c`, actualizar CMakeLists

---

### A3 — Migrar tasks existentes al factory (un commit por servicio)

Reemplazar cada `xTaskCreate`/`xTaskCreatePinnedToCore` por `os_task_create`:

- `svc_battery.c`: battery_task
- `svc_time.c`: time_update_task
- `hal_sdcard.c`: sd_poll_task
- `svc_downloader.c`: downloader_task
- `svc_ota.c`: ota_task
- `ui_engine.c`: lvgl_task con `OS_CORE_UI`
- `svc_event.c`: event loop task

**Depende de:** A2
**Paralelizable:** Si, un commit por servicio

---

### A4 — `os_poller_register` (reemplaza tasks periodicas simples)

En `os_core/os_poller.c`: lista de pollers con {fn, arg, interval_ms, last_tick, owner}.
Un solo task `os_poller_task` que itera la lista cada 100 ms.

Migrar `battery_task` y `sd_poll_task` a pollers (ambos son while(1){work(); vTaskDelay();}).

**Depende de:** A2
**Beneficio:** Elimina 2 tasks, ahorra ~6 KB de stack

---

### A5 — `os_defer` y `os_ui_post`

En `os_core/os_defer.c`:
- `os_defer(fn, arg, delay_ms)` usa `esp_timer_create` one-shot internamente
- `os_ui_post(fn, arg)` envia mensaje a queue que el LVGL task drena en cada tick

`os_ui_post` reemplaza todos los patrones de:
```c
if (ui_lock(200)) { update_widget(); ui_unlock(); }
```

**Depende de:** A1

---

## Track B — OS Core: Event Bus Mejorado

**Objetivo:** Eliminar boilerplate lock/unlock en handlers UI, cleanup automatico por app.
**Prerequisito:** A1

### B1 — Extender `svc_event` con ownership por app

En `svc_event.h`, nueva firma (backward compatible):
```c
esp_err_t os_event_subscribe(app_id_t owner, cyberdeck_event_t evt,
                             event_handler_fn_t fn, void *ctx,
                             event_sub_t *out_sub);
```

Mantener `svc_event_register` como wrapper temporal.

**Archivos:** `svc_event.c/h`

---

### B2 — `os_event_subscribe_ui`

```c
event_sub_t os_event_subscribe_ui(app_id_t owner, cyberdeck_event_t evt,
                                  event_handler_fn_t fn, void *ctx);
```

Handler intermedio que llama `os_ui_post(fn_wrapper, ...)`.
`fn_wrapper` toma ui_lock, llama fn, libera lock.

**Depende de:** B1, A5

---

### B3 — `os_event_unsubscribe_all(app_id_t)`

Limpieza automatica de todas las suscripciones de una app.
Llamar desde `os_app_terminate(id)`.

**Depende de:** B1

---

### B4 — Migrar handlers de eventos en settings y launcher

Reemplazar patron manual:
```c
// Antes
static void evt_handler(void *arg, ...) {
    if (!g_scr_state) return;
    if (ui_lock(200)) {
        if (g_scr_state) update_ui(g_scr_state);
        ui_unlock();
    }
}
```

Por:
```c
// Despues
static void evt_handler(void *arg, ...) {
    my_state_t *s = arg;
    update_ui(s);  // ya con ui_lock tomado por el OS
}
// on_create: os_event_subscribe_ui(APP_ID_SETTINGS, EVT_X, evt_handler, state);
```

Archivos: `settings_wifi.c`, `settings_storage.c`, `app_launcher.c`, `main.c`

**Depende de:** B2
**Paralelizable:** Si, un archivo por commit

---

### B5 — Ampliar event IDs

En `svc_event.h`, agregar:
- `EVT_BOOT_COMPLETE`
- `EVT_MEMORY_LOW`
- `EVT_BATTERY_LOW`
- `EVT_APP_LAUNCHED`, `EVT_APP_TERMINATED`
- `EVT_SD_APP_DISCOVERED`
- `EVT_THEME_CHANGED` (separado de `EVT_SETTINGS_CHANGED`)

Publicar `EVT_BOOT_COMPLETE` al final de `app_main()`.
Publicar `EVT_THEME_CHANGED` desde `ui_theme_apply()`.

**Archivos:** `svc_event.h`, `main.c`, `ui_theme.c`

---

## Track C — App Framework: Registro Dinamico

**Objetivo:** Reemplazar array estatico de 10 slots por registro extensible en runtime.
**Prerequisito:** A1

### C1 — Definir tipos `app_manifest_t` y `app_ops_t`

En `app_framework/include/app_registry.h`:
- `app_manifest_t` con id, name, icon, type, permissions, storage_dir
- `app_ops_t` vtable: on_launch, on_terminate, on_background, on_foreground, on_intent
- `app_entry_t` = manifest + ops + estado (running/stopped/unavailable)
- `app_type_t`: BUILTIN / SCRIPT

**Archivos:** actualizar `app_registry.h`

---

### C2 — Lista dinamica en `app_registry.c`

Reemplazar `s_entries[APP_ID_COUNT]` con array de punteros con realloc:
```c
static app_entry_t **s_apps = NULL;
static uint16_t      s_app_count = 0;
static uint16_t      s_app_capacity = 0;

esp_err_t          os_app_register(const app_manifest_t *m, const app_ops_t *ops);
const app_entry_t *os_app_get(app_id_t id);
void               os_app_enumerate(void (*cb)(const app_entry_t *, void *), void *ctx);
```

IDs 0-255 reservados para built-in. Dinamicas desde 256.

**Depende de:** C1
**Archivos:** `app_registry.c`

---

### C3 — Migrar built-ins al nuevo registro

Actualizar `app_launcher_register()` y `app_settings_register()` para usar `os_app_register()`.
Eliminar los slots hardcodeados 1-8 (los stubs "coming soon").

**Depende de:** C2
**Archivos:** `app_registry.c`, `app_launcher.c`, `app_settings.c`

---

### C4 — Actualizar launcher para usar `os_app_enumerate`

En `app_launcher.c`, reemplazar iteracion hardcodeada del grid:
```c
os_app_enumerate(build_app_card, s);
```

Grid regenerado en `on_create` y al recibir `EVT_SD_APP_DISCOVERED`.

**Depende de:** C2, B5
**Archivos:** `app_launcher.c`

---

## Track D — App Framework: Ciclo de Vida Mejorado

**Objetivo:** View descriptor con state* retornado por on_create; stack de 8; cleanup automatico.
**Prerequisito:** A1, C1

### D1 — Redefinir firma de `on_create`

Cambiar:
```c
// Actual
void (*on_create)(lv_obj_t *screen, void *intent_data);

// Nuevo
void *(*on_create)(lv_obj_t *screen, const view_args_t *args);  // retorna state*
void  (*on_resume)(lv_obj_t *screen, void *state);
void  (*on_pause )(lv_obj_t *screen, void *state);
void  (*on_destroy)(lv_obj_t *screen, void *state);             // libera state*
```

El OS almacena el state* retornado y lo pasa automaticamente.
Eliminar `ui_activity_set_state()`.

**Archivos:** `ui_activity.h`, `ui_activity.c`
**Nota:** Breaking change. Migrar todas las apps en el mismo PR o coordinar con D2.

---

### D2 — Migrar todos los views al nuevo ciclo de vida

Actualizar cada archivo:
- `on_create` retorna `malloc(sizeof(state_t))` inicializado
- `on_destroy` recibe state y hace `free(state)`
- Eliminar variables globales `g_*_scr_state`

Archivos (paralelizables):
- `settings_wifi.c`, `settings_display.c`, `settings_time.c`
- `settings_storage.c`, `settings_security.c`, `settings_audio.c`
- `settings_bluetooth.c`, `settings_about.c`
- `app_launcher.c`, `launcher_lockscreen.c`, `app_settings.c`

**Depende de:** D1
**Paralelizable:** Si, un archivo por commit

---

### D3 — Ampliar activity stack a 8

En `ui_activity.c`: `UI_ACTIVITY_MAX_STACK` de 4 a 8.
Cambiar politica de stack lleno: retornar `OS_ERR_STACK_FULL` en lugar de eviccion silenciosa.

**Depende de:** D1
**Archivos:** `ui_activity.c/h`

---

### D4 — `os_view_push` / `os_view_pop` como API publica

Crear `app_framework/include/os_nav.h`:
```c
esp_err_t os_view_push(app_id_t app, view_id_t view, const view_args_t *args);
void      os_view_pop(void);
void      os_view_pop_to_root(void);
void      os_view_home(void);
```

Internamente llaman a `ui_activity_push`/`pop`.

**Depende de:** D1
**Archivos:** nuevo `os_nav.h/c`

---

### D5 — Migrar navegacion en settings al nuevo API

Reemplazar llamadas directas a `ui_activity_push()` en `settings_*.c` por `os_view_push()`.

**Depende de:** D4, D2
**Paralelizable:** Si

---

### D6 — `view_args_t` struct consistente

Reemplazar `void *intent_data` por:
```c
typedef struct {
    void   *data;
    size_t  size;
    bool    owned;  // si true, el OS llama free(data) despues de on_create
} view_args_t;
```

**Depende de:** D1
**Paralelizable con D2**

---

## Track E — OS Settings API

**Objetivo:** Namespace de settings por app, reemplazar acceso directo a `svc_settings`.
**Prerequisito:** A1, C1

### E1 — Definir `os_settings_*` API

En `sys_services/include/os_settings.h`:
```c
esp_err_t os_settings_get_str (app_id_t id, const char *key, char *out, size_t len);
esp_err_t os_settings_set_str (app_id_t id, const char *key, const char *val);
esp_err_t os_settings_get_i32 (app_id_t id, const char *key, int32_t *out);
esp_err_t os_settings_set_i32 (app_id_t id, const char *key, int32_t val);
esp_err_t os_settings_get_bool(app_id_t id, const char *key, bool *out);
esp_err_t os_settings_set_bool(app_id_t id, const char *key, bool val);
```

Namespace NVS como `"app%d"` con snprintf. El namespace del OS es `"sys"`.

**Archivos:** `svc_settings.h/c`

---

### E2 — Migrar Settings app al nuevo API

Reemplazar en `settings_*.c` las llamadas a `svc_settings_*` especificas:
```c
// Antes: svc_settings_get_wifi_ssid(0, ssid, sizeof(ssid));
// Despues: os_settings_get_str(APP_ID_SETTINGS, "wifi_ssid_0", ssid, sizeof(ssid));
```

**Depende de:** E1
**Paralelizable:** Si, un archivo por commit

---

### E3 — Migrar settings del sistema a namespace "sys"

Settings del OS (theme, rotation, brightness, PIN, timezone) usan `OS_OWNER_SYSTEM`, namespace `"sys"`.

**Depende de:** E1
**Archivos:** `svc_settings.c`, `settings_display.c`, `settings_security.c`, `settings_time.c`

---

## Track F — OS Storage API

**Objetivo:** Estructura de directorios por app en SD, API limpia para archivos.
**Prerequisito:** Ninguno (independiente)

### F1 — Crear estructura de directorios en SD mount

En `hal_sdcard.c`, despues del mount exitoso:
```c
mkdir("/sdcard/apps", 0755);
mkdir("/sdcard/media", 0755);
mkdir("/sdcard/downloads", 0755);
mkdir("/sdcard/.cyberdeck", 0755);
```

**Archivos:** `hal_sdcard.c`

---

### F2 — Implementar `os_storage_*` API

En `sys_services/os_storage.c`:
```c
const char *os_storage_dir(app_id_t id);
const char *os_storage_path(app_id_t id, const char *name, char *buf, size_t len);
FILE       *os_storage_fopen(app_id_t id, const char *rel, const char *mode);
esp_err_t   os_storage_read(app_id_t id, const char *name, void *buf, size_t *len);
esp_err_t   os_storage_write(app_id_t id, const char *name, const void *data, size_t len);
bool        os_storage_exists(app_id_t id, const char *name);
esp_err_t   os_storage_delete(app_id_t id, const char *name);
```

`os_storage_dir` crea el directorio si no existe.

**Depende de:** F1
**Archivos:** nuevo `os_storage.c/h`

---

### F3 — Integrar `idf-sqlite3` como componente opcional

Agregar `idf-sqlite3` a `idf_component.yml`.
Crear `os_db.c/h` con wrapper:
```c
os_db_t   *os_db_open(app_id_t id, const char *filename);
esp_err_t  os_db_exec(os_db_t *db, const char *sql, os_db_row_cb_t cb, void *ctx);
esp_err_t  os_db_close(os_db_t *db);
```

Habilitado via Kconfig: `CONFIG_OS_ENABLE_SQLITE`. Requiere PSRAM para page cache.

**Depende de:** F2
**Archivos:** nuevo `os_db.c/h`, `idf_component.yml`, `Kconfig.projbuild`

---

### F4 — Crash log en SD

Registrar handler de shutdown de ESP-IDF que escribe timestamp + reason en
`/sdcard/.cyberdeck/crash.log`.

**Depende de:** F1
**Archivos:** `main.c`, nuevo `os_crash.c/h`

---

## Track G — Apps Dinamicas desde SD

**Objetivo:** Descubrir y lanzar apps con manifest.json desde /sdcard/apps/.
**Prerequisito:** C2 (registro dinamico), F2 (storage API), B5 (EVT_SD_APP_DISCOVERED)

### G1 — Parser de manifest.json

```c
esp_err_t os_manifest_parse(const char *path, app_manifest_t *out);
```

Lee: id, name, icon, type, runtime, entry, permissions, version, min_os_api.
Usar `cJSON` (disponible en ESP-IDF como componente built-in).

**Archivos:** nuevo `os_manifest.c/h`

---

### G2 — `os_app_discover_sd`

Escanea `/sdcard/apps/`, lee cada `manifest.json`, registra apps validas.
Marca incompatibles si `min_os_api` > version actual.
Publica `EVT_SD_APP_DISCOVERED` por cada app nueva.

Llamar en boot despues de montar SD y en `EVT_SDCARD_MOUNTED`.

**Depende de:** G1, C2, F1
**Archivos:** nuevo `os_app_discover.c`

---

### G3 — Script app stub (sin runtime todavia)

`app_ops_t` para `APP_TYPE_SCRIPT` que muestra toast "Script runtime not available".
Permite que el launcher muestre apps de SD aunque no haya runtime integrado.

**Depende de:** G2
**Archivos:** nuevo `os_script_app.c`

---

### G4 — Interface de Script Runtime

En `os_script.h`:
```c
typedef struct {
    const char *name;
    const char *file_ext;
    esp_err_t (*load  )(const char *path, script_handle_t *out);
    esp_err_t (*call  )(script_handle_t h, const char *fn,
                        const script_args_t *args);
    void      (*unload)(script_handle_t h);
} script_runtime_t;

void                   os_script_register_runtime(const script_runtime_t *rt);
const script_runtime_t *os_script_get_runtime(const char *name);
```

Preparacion para MicroPython u otro runtime futuro.

**Depende de:** G3
**Archivos:** nuevo `os_script.h/c`

---

## Track H — Task Manager (Vista de Procesos)

**Objetivo:** Pantalla de procesos activos accesible desde navbar.
**Prerequisito:** A2, C2

### H1 — `os_process_list` API

```c
typedef struct {
    char         name[configMAX_TASK_NAME_LEN];
    TaskHandle_t handle;
    app_id_t     owner;
    uint32_t     stack_high_water;
    uint8_t      priority;
    uint8_t      core;
} os_process_info_t;

uint8_t os_process_list(os_process_info_t *buf, uint8_t max);
size_t  os_heap_free(void);
size_t  os_heap_free_psram(void);
```

**Depende de:** A2
**Archivos:** `os_core.h/c`

---

### H2 — View del Task Manager

Nueva pantalla `components/apps/settings/settings_processes.c`:
- Lista de tasks con nombre, owner app, stack usage.
- Heap libre DRAM / PSRAM.
- Uptime y boot count.
- Boton KILL para apps que no sean LAUNCHER/SETTINGS.
- Integrado en el menu de Settings (VIEW_SETTINGS_PROCESSES).
- Tambien alcanzable via `EVT_NAV_PROCESSES` desde navbar.

**Depende de:** H1, D4
**Archivos:** nuevo `settings_processes.c`

---

## Track I — Limpieza y Calidad (Paralelo a todo)

**Prerequisito:** Ninguno para I1/I2/I4. I3 depende de D2.

### I1 — Macro `UI_LOCKED_SECTION`

```c
// En ui_engine.h
#define UI_LOCKED_SECTION(timeout_ms, code) \
    do { if (ui_lock(timeout_ms)) { code; ui_unlock(); } } while(0)
```

Reemplazar todos los `if (ui_lock(...)) { ... ui_unlock(); }` en handlers de eventos.

**Archivos:** `ui_engine.h`, luego cada handler

---

### I2 — `settings_common.h`

Crear `components/apps/settings/settings_common.h` con los includes y tipos compartidos
entre todos los `settings_*.c`.

**Archivos:** nuevo `settings_common.h`, cada `settings_*.c`

---

### I3 — Eliminar variables globales de state en settings

Post-D2: verificar que no queden `static my_state_t *g_*_state` en ningun `settings_*.c`.
Si quedan, completar la migracion.

**Depende de:** D2

---

### I4 — Documentar headers publicos

Para cada `.h` nuevo en los tracks anteriores, agregar un comentario de una linea por funcion.
Solo en headers publicos. Sin exagerar.

**Independiente de todo**

---

## Orden de Ejecucion Recomendado

### Fase 1 — Fundamentos sin riesgo (empezar aqui, todo paralelo)

**COMPLETADA** (2026-04-15) — build limpio, 0 warnings.

```
A1  — tipos base os_core.h                ✅  components/os_core/include/os_core.h
B5  — nuevos event IDs                    ✅  svc_event.h (+8 IDs nuevos)
F1  — dirs en SD mount                    ✅  hal_sdcard_mount() crea /apps /media /downloads /.cyberdeck
I1  — macro UI_LOCKED_SECTION             ✅  ui_engine.h
I2  — settings_common.h                   ✅  components/apps/settings/settings_common.h
```

**Notas:**
- `os_core` es un componente header-only: `CMakeLists.txt` con `SRCS` vacío, solo `INCLUDE_DIRS`.
- Los dirs de SD mount (F1) son distintos a los que crea `hal_sdcard_format()` (/apps /books /music /system). mount crea los del OS; format recrea los de la app. Podría unificarse en Fase 4 (F2).
- `settings_common.h` incluye todos los headers UI+services; cada settings_*.c puede incluirlo en lugar de 8-10 includes individuales. Migración de callers queda para I2 follow-up en Fase 5.

### Fase 2 — Task Factory y Event Bus  ← SIGUIENTE

```
A2  — os_task_create
A5  — os_defer / os_ui_post
B1  — event subscribe con owner
```

Luego (paralelo entre si, dependen de Fase 2):
```
A3  — migrar tasks al factory (un commit por servicio)
A4  — os_poller
B2  — os_event_subscribe_ui
B3  — os_event_unsubscribe_all
```

### Fase 3 — Registro Dinamico y Ciclo de Vida (breaking change grande)

```
C1  — tipos manifest/ops
D1  — nueva firma on_create
```

Luego (paralelo):
```
C2  — lista dinamica
D2  — migrar views (11 archivos)
D6  — view_args_t
```

Luego:
```
C3, C4  — migrar builtin + launcher
D3, D4, D5  — stack x8, os_view API, migrar navegacion
E1, E2, E3  — os_settings API y migracion
B4  — migrar handlers de eventos
```

### Fase 4 — Storage y Apps Dinamicas

```
F2  — os_storage API
F3  — SQLite (opcional)
F4  — crash log
G1  — parser manifest.json
G2  — os_app_discover_sd
G3  — script app stub
G4  — runtime interface
```

### Fase 5 — Task Manager y Pulido

```
H1  — os_process_list
H2  — procesos view
I3  — eliminar globales restantes
I4  — documentacion headers
```

---

## Tabla de Dependencias

```
A1 <-- A2 <-- A3, A4
A1 <-- A5
A1 <-- B1 <-- B2 <-- B4
              B1 <-- B3
A1 <-- C1 <-- C2 <-- C3, C4
C1, A1 <-- D1 <-- D2, D3, D6
            D1 <-- D4 <-- D5
A1 <-- E1 <-- E2, E3
F1 <-- F2 <-- F3, F4
C2, F1, B5 <-- G1 <-- G2 <-- G3 <-- G4
A2 <-- H1 <-- H2
A5, B1 <-- B2
D1 <-- I3
```

---

## Notas de Implementacion

### Backward compatibility durante la migracion

Las APIs antiguas (`svc_event_register`, `ui_activity_push`, etc.) se mantienen como wrappers
hasta que todos los callers esten migrados. Estrategia: agregar nuevo -> migrar callers ->
eliminar viejo. PRs separados para cada paso.

### Sobre el runtime de scripts

No implementar MicroPython todavia. La Fase 4 llega hasta la interfaz (`script_runtime_t`).
El runtime es trabajo futuro. Lo importante es que la arquitectura lo soporte sin cambios en el OS.

### Sobre SQLite

`idf-sqlite3` requiere ~300 KB de flash adicional. Opt-in via Kconfig.
Primera app candidata: Tasks (cuando exista) con `tasks.db` en `/sdcard/apps/tasks/`.

### Sobre stack size del activity stack

Ampliar de 4 a 8 es seguro — cada entrada solo almacena punteros y un uint8_t.
El costo real es el heap de los screen objects LVGL, que ya existia igual.

### Sobre PSRAM para workers pesados

SQLite, downloader, y OTA deben tener sus stacks en PSRAM para no presionar el DRAM heap.
Usar `os_task_config_t.stack_in_psram = true` -> `xTaskCreatePinnedToCoreWithCaps` con
`MALLOC_CAP_SPIRAM`.
