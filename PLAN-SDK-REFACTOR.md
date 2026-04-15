# CyberDeck — Plan de Refactor SDK de Apps

> **Documento vivo.** Actualizar al completar cada ítem.  
> Última actualización: 2026-04-15  
> Estado global: **Fase 6 DONE (build limpio) — Fase 7 en diseño**

---

## Índice

1. [Motivación y problema raíz](#1-motivación-y-problema-raíz)
2. [Arquitectura target](#2-arquitectura-target)
3. [Tres capas de estado](#3-tres-capas-de-estado)
4. [Ciclo de vida completo](#4-ciclo-de-vida-completo)
5. [API del SDK](#5-api-del-sdk)
6. [Process Monitor Service](#6-process-monitor-service)
7. [Service Registry](#7-service-registry)
8. [Plan de implementación por fases](#8-plan-de-implementación-por-fases)
9. [Estructura de archivos target](#9-estructura-de-archivos-target)
10. [Invariantes del SDK](#10-invariantes-del-sdk-reglas-para-app-authors)
11. [Ejemplo completo — app Notes](#11-ejemplo-completo--app-notes)
12. [Progreso detallado](#12-progreso-detallado)

---

## 1. Motivación y problema raíz

### Síntoma

El Task Manager no puede matar apps correctamente: al cerrar la actividad LVGL, los FreeRTOS tasks del app siguen corriendo. La memoria no se libera completamente.

### Causa raíz — tres acoplamientos incorrectos

**Acoplamiento 1: `state*` ligado al ciclo de vida LVGL**

El `state*` retornado por `on_create` mezcla tres cosas distintas:
- Handles de widgets LVGL → deberían vivir en `view_state`
- Datos del app → deberían vivir en `app_data`, fuera del render
- Estado persistente → debería ir a NVS o DB

Consecuencia: en `recreate_all()` por rotación, `on_destroy` destruye todo y `on_create` tiene que releer NVS para reconstruir estado. El scroll offset, el ítem seleccionado, cualquier resultado de scan — todo se pierde.

**Acoplamiento 2: sin modelo de proceso OS-level**

No existe un concepto de "app instance" independiente del stack LVGL:
- No se puede saber si una app "está corriendo" sin mirar el stack LVGL
- Los FreeRTOS tasks no se matan al cerrar la activity
- Servicios de core no tienen representación en el modelo de proceso
- El Task Manager mostraba raw FreeRTOS tasks — nivel de abstracción incorrecto

**Acoplamiento 3: sin storage SDK**

Cada app implementa su propio acceso a SD y SQLite ad-hoc. No hay sandbox por app, no hay lifecycle de DB gestionado por el OS, no hay migrations estándar.

---

## 2. Arquitectura target

```
┌─────────────────────────────────────────────────────────────────┐
│                        APP (unidad conceptual)                  │
│                                                                 │
│   MANIFEST         PROCESS          VIEWS          STORAGE      │
│   (identidad)      (OS instance)    (render LVGL)  (FS+SQLite)  │
│                                                                 │
│   id               proc_state_t     view_cbs_t     os_app_      │
│   name             app_data*        on_create       storage_t   │
│   icon             bg_tasks[]       on_resume       .db         │
│   permissions      heap_delta       on_pause        .base_path  │
│   type             launched_ms      on_destroy      .files_path │
│   storage_dir      view_count                       .cache_path │
└─────────────────────────────────────────────────────────────────┘

Stack de capas:

  ┌──────────────────────────────────────────────┐
  │  Apps: launcher, settings, notes, tasks, ...  │  components/apps/
  ├──────────────────────────────────────────────┤
  │  App Framework: registry, manager, nav        │  app_framework/
  ├──────────────────────────────────────────────┤
  │  UI Engine: LVGL, activity stack, statusbar   │  ui_engine/
  ├──────────────────────────────────────────────┤
  │  System Services: monitor, wifi, battery...   │  sys_services/
  ├──────────────────────────────────────────────┤
  │  OS Core: process, service, task, storage,    │  os_core/
  │           settings, event, defer, poller      │
  ├──────────────────────────────────────────────┤
  │  Board HAL: LCD, touch, CH422G, RTC, SD       │  board/
  └──────────────────────────────────────────────┘
```

---

## 3. Tres capas de estado

Esta es **la** decisión de diseño central del SDK.

| Capa | Nombre | Dónde vive | Lifetime | Quién la gestiona |
|------|--------|-----------|----------|-------------------|
| **L1** | App Data | `os_process_t.app_data*` | `on_launch` → `on_terminate` | OS (proceso) |
| **L2** | View State | `activity_t.view_state*` | `on_create` → `on_destroy` | LVGL stack |
| **L3** | Persistent | NVS / SQLite via `os_settings` | Survives reboot | OS (storage) |

### Regla de oro

> `view_state*` contiene **solo handles de widgets LVGL** (`lv_obj_t*`, `lv_timer_t*`).
> Ningún dato de app. Nada de scroll offset, selección, resultados de queries.

### Consecuencia directa

En `recreate_all()` (rotación):
- `on_destroy` libera **solo widgets** → `view_state` freed
- `app_data` **sigue intacto** en `os_process_t`
- `on_create(screen, NULL, app_data)` rehidrata UI desde L1
- **Sin lecturas de NVS. Sin queries a DB. Instantáneo.**

### Qué va en cada capa

```
L1 (app_data)             L2 (view_state)          L3 (NVS / SQLite)
─────────────────         ───────────────          ─────────────────
scroll_pos                list_widget*              saved_theme
selected_idx              header_label*             saved_ssid
wifi_ap_list[]            refresh_timer*            pin_hash
notes_cache[]             keyboard_obj*             volume_level
db_connection             status_bar_label*         boot_count
os_app_storage_t          confirm_dialog*           rotation
dirty (flag)              progress_bar*
```

---

## 4. Ciclo de vida completo

```
LAUNCH (tap en launcher / intent)
   │
   ├─[1] os_process_start(app_id, args)
   │      ¿proceso ya existe? → raise (os_process_raise)
   │                             on_foreground(app_id, app_data)
   │                             ui_activity_raise(app_id) — return
   │
   ├─[2] os_app_storage_open(id, manifest, migrations)
   │      mkdir /sdcard/apps/{name}/, /files/, /cache/
   │      sqlite3_open → db
   │      os_db_migrate(db, migrations, count)
   │      → os_app_storage_t lista
   │
   ├─[3] app_ops.on_launch(id, args, &storage)
   │      calloc(app_data)
   │      app_data->storage = storage
   │      carga caché inicial desde DB
   │      retorna app_data*                ← L1 nace aquí
   │
   ├─[4] ui_activity_push → on_create(screen, args, app_data)
   │      construye widgets LVGL desde app_data   ← L2 nace aquí
   │      svc_event_register(EVT_X, handler, app_data)
   │      retorna view_state*
   │
   │  ┌── [ROTACIÓN / recreate_all] ────────────────────────────┐
   │  │   on_destroy(scr, view_state, app_data)                  │
   │  │     lv_timer_del; svc_event_unregister; free(view_state) │
   │  │     app_data intacto — NO liberar                        │
   │  │   on_create(scr, NULL, app_data)                         │
   │  │     rebuild desde app_data (sin NVS, sin DB)             │
   │  └──────────────────────────────────────────────────────────┘
   │
   │  ┌── [HOME gesture] ──────────────────────────────────────┐
   │  │   on_pause(scr, view_state, app_data)                   │
   │  │   lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN)              │
   │  │   proceso → PROC_STATE_SUSPENDED                        │
   │  │   app_data vivo, bg tasks corriendo                     │
   │  └────────────────────────────────────────────────────────┘
   │
   │  ┌── [RAISE desde suspended] ──────────────────────────────┐
   │  │   lv_obj_clear_flag(scr, LV_OBJ_FLAG_HIDDEN)             │
   │  │   on_resume(scr, view_state, app_data)                   │
   │  │     refresh UI si app_data cambió (bg task escribió)     │
   │  └──────────────────────────────────────────────────────────┘
   │
TERMINATE (Processes app KILL / os_process_terminate)
   │
   ├─[5] on_destroy(scr, view_state, app_data)
   │      libera widgets, timers, cancela eventos
   │      free(view_state)     ← L2 muere
   │
   ├─[6] app_ops.on_terminate(id, app_data)
   │      flush dirty data a DB
   │      free(app_data)       ← L1 muere
   │
   ├─[7] os_task_destroy_all_for_app(id)  ← via close hook en app_manager
   │
   └─[8] os_app_storage_close(&storage)
          sqlite3_close_v2 + WAL checkpoint
          dirs en SD se conservan
```

---

## 5. API del SDK

### 5.1 Manifest + Ops

```c
/* components/app_framework/include/app_registry.h */

typedef struct {
    app_id_t        id;
    const char     *name;         /* "Notes", "Settings" */
    const char     *icon;         /* "Nt", "St" — 2 chars para launcher card */
    app_type_t      type;         /* APP_TYPE_BUILTIN | APP_TYPE_SCRIPT */
    uint8_t         permissions;  /* bitmask: APP_PERM_WIFI, SD, NETWORK, ... */
    const char     *storage_dir;  /* "notes" → /sdcard/apps/notes/; NULL = sin storage */
} app_manifest_t;

typedef struct {
    /* [1] Antes de pushear ninguna view. Retorna app_data* (L1).
     * Si retorna NULL → launch abortado.
     * Si storage->db_ready==false → SD offline, continuar sin DB. */
    void *(*on_launch)(app_id_t id,
                       const view_args_t *args,
                       os_app_storage_t  *storage);

    /* [6] Tras destruir todas las views. Flush a DB, free(app_data).
     * El OS cierra storage DESPUÉS de este call.
     * El OS ya mató bg tasks ANTES de este call. */
    void  (*on_terminate)(app_id_t id, void *app_data);

    /* Opcional: app suspendida al home. Pausar audio, reducir polling. */
    void  (*on_suspend)(app_id_t id, void *app_data);

    /* Opcional: app vuelve al foreground. Reanudar polling. */
    void  (*on_foreground)(app_id_t id, void *app_data);
} app_ops_t;
```

### 5.2 View Callbacks

```c
/* components/ui_engine/include/ui_activity.h */

typedef struct {
    /* Construye UI en screen. app_data (L1) es fuente de verdad.
     * args==NULL → rotación; rebuild desde app_data sin NVS ni DB.
     * Retorna view_state* (solo widgets LVGL). NULL → push abortado. */
    void *(*on_create)(lv_obj_t          *screen,
                       const view_args_t *args,
                       void              *app_data);

    /* View vuelve a ser visible. Refrescar UI si app_data fue modificado
     * por bg tasks. No recrear widgets — solo actualizar texto/color. */
    void  (*on_resume)(lv_obj_t *screen,
                       void     *view_state,
                       void     *app_data);

    /* Se pushea otra view encima. Raramente necesario. NO hace cleanup. */
    void  (*on_pause)(lv_obj_t *screen,
                      void     *view_state,
                      void     *app_data);

    /* Libera view_state: widgets, lv_timers, event handlers.
     * NO libera app_data. NO accede a DB. */
    void  (*on_destroy)(lv_obj_t *screen,
                        void     *view_state,
                        void     *app_data);
} view_cbs_t;
```

### 5.3 Storage per-app

```c
/* components/os_core/include/os_app_storage.h */

typedef struct {
    char     base_path[64];   /* /sdcard/apps/notes                */
    char     files_path[72];  /* /sdcard/apps/notes/files          */
    char     cache_path[72];  /* /sdcard/apps/notes/cache          */
    sqlite3 *db;              /* NULL si SD offline                */
    bool     db_ready;        /* true si migrations corridas OK    */
} os_app_storage_t;

/* Llamado por el OS antes de app_ops.on_launch().
 * Crea dirs, abre DB, corre migrations.
 * Si SD offline → ESP_OK con db=NULL, db_ready=false. */
esp_err_t os_app_storage_open(app_id_t              id,
                               const app_manifest_t *manifest,
                               const db_migration_t *migrations,
                               int                   migration_count,
                               os_app_storage_t     *out);

/* Llamado por el OS DESPUÉS de app_ops.on_terminate().
 * sqlite3_close_v2 + WAL checkpoint. */
void os_app_storage_close(os_app_storage_t *storage);

/* Construye path absoluto en el sandbox.
 * os_app_storage_path(&st, "files/doc.txt", buf, sizeof(buf))
 * → "/sdcard/apps/notes/files/doc.txt" */
const char *os_app_storage_path(const os_app_storage_t *storage,
                                 const char             *rel,
                                 char *buf, size_t buflen);

/* Borra /cache/ del app. No toca /files/ ni la DB. */
esp_err_t os_app_storage_clear_cache(os_app_storage_t *storage);

/* Re-abre storage si SD fue insertada mientras el proceso corría.
 * Llamar tras EVT_SDCARD_MOUNTED si !storage->db_ready. */
esp_err_t os_app_storage_reopen(os_app_storage_t     *storage,
                                 const app_manifest_t *manifest,
                                 const db_migration_t *migrations,
                                 int count);
```

### 5.4 SQLite Migrations

```c
/* components/os_core/include/os_db.h */

typedef struct {
    int         version;  /* monótonamente creciente: 1, 2, 3... */
    const char *up_sql;   /* DDL/DML para subir a este version */
} db_migration_t;

/* Corre migrations pendientes en orden ascendente.
 * Lee/escribe versión en tabla _schema_version (autocreada).
 * Cada migration en su propia transacción — fallo → rollback de esa migration. */
esp_err_t os_db_migrate(sqlite3              *db,
                          const db_migration_t *migrations,
                          int                   count);

/* Helpers para queries simples */
esp_err_t os_db_exec(sqlite3 *db, const char *sql);
int64_t   os_db_last_insert_rowid(sqlite3 *db);
int       os_db_get_int(sqlite3 *db, const char *sql, int default_val);
```

**Convenciones de schema:**
- `id INTEGER PRIMARY KEY` en todas las tablas
- Timestamps: `INTEGER` Unix seconds
- Texto: `TEXT NOT NULL DEFAULT ''`
- Booleans: `INTEGER NOT NULL DEFAULT 0`
- La tabla `_schema_version` la gestiona el OS — no tocar

### 5.5 Background Tasks

```c
/* Siempre usar os_task_create — NUNCA xTaskCreate directamente */
os_task_config_t cfg = {
    .fn             = my_sync_fn,
    .name           = "notes_sync",
    .stack_size     = 4096,
    .priority       = 3,
    .core           = 0,
    .owner          = APP_ID_NOTES,  /* CRÍTICO: limpieza automática en terminate */
    .stack_in_psram = false,
};
os_task_create(&cfg, &app_data->sync_handle);
```

**Reglas:**
1. Crear en `on_launch` (no en `on_create`) — persisten entre rotaciones
2. Nunca tocar LVGL directamente — comunicar via `svc_event_post()`
3. Event handlers en la view hacen el bridge con `ui_lock()`
4. El OS mata las bg tasks en terminate — no hacerlo en `on_terminate`

**Patrón de comunicación bg_task → view:**
```c
/* bg task */
svc_event_post(EVT_NOTES_SYNC_DONE, &result, sizeof(result));

/* view — puntero global para event handler */
static notes_view_t *g_view = NULL;

static void sync_done_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (!g_view) return;                    /* view destruida — ignorar */
    notes_app_data_t *d = (notes_app_data_t *)arg;
    d->last_sync = *(sync_result_t *)data;  /* actualizar L1 */
    if (ui_lock(200)) {
        if (!g_view) { ui_unlock(); return; }  /* double-check locking */
        lv_label_set_text(g_view->status_lbl, d->last_sync.message);
        ui_unlock();
    }
}

/* on_create: */
g_view = v;
svc_event_register(EVT_NOTES_SYNC_DONE, sync_done_handler, app_data);

/* on_destroy: g_view = NULL PRIMERO para evitar race */
g_view = NULL;
svc_event_unregister(EVT_NOTES_SYNC_DONE, sync_done_handler);
```

### 5.6 Navegación

```c
/* Push sub-screen dentro del mismo app */
os_view_push(app_id, screen_id, &cbs, &args);
os_view_pop();           /* BACK */
os_view_pop_to_root();   /* pop hasta screen_id==0 del app actual */
os_view_home();          /* HOME — suspend, proceso sigue */

/* Lanzar otro app */
app_manager_launch(app_id, data, data_size);
```

**Convención de screen_id:**
- `0` → pantalla principal
- `1..99` → sub-screens del app
- `100..199` → sub-screens de detalle (e.g., `WIFI_SCR + 100` = connect screen)

### 5.7 Eventos del SDK

```c
/* Ciclo de vida de proceso */
EVT_APP_LAUNCHED          /* data: app_id_t */
EVT_APP_TERMINATED        /* data: app_id_t */

/* Monitor */
EVT_MONITOR_UPDATED       /* snapshot nuevo en svc_monitor_get() */
EVT_MEMORY_LOW            /* data: size_t* free_bytes */
EVT_SERVICE_CHANGED       /* algún servicio cambió estado */

/* Storage (existentes) */
EVT_SDCARD_MOUNTED
EVT_SDCARD_UNMOUNTED

/* Sistema (existentes) */
EVT_DISPLAY_ROTATED
EVT_SETTINGS_CHANGED
EVT_BATTERY_UPDATED
EVT_WIFI_CONNECTED
EVT_WIFI_DISCONNECTED
EVT_GESTURE_HOME
EVT_GESTURE_BACK
EVT_NAV_PROCESSES
```

---

## 6. Process Monitor Service

### 6.1 Diseño del servicio

**Responsabilidades únicas:**
1. Agrega estado de múltiples fuentes en un snapshot consistente
2. Publica ese snapshot vía evento — clientes suscriben, no pollan
3. Detecta cambios importantes y emite eventos específicos
4. Operación de background — sin UI propia

**No hace:** matar procesos (`os_process_terminate`), mostrar UI (`app_processes`), gestionar lifecycle (`app_manager`).

**Fuentes:**
```
os_process_registry  → apps corriendo: estado, heap, view_count, tasks
os_service_registry  → servicios: nombre, estado, status_text
os_task (dev mode)   → FreeRTOS tasks: stack HWM, priority, core, state
heap_caps            → memoria libre: internal, PSRAM
esp_timer / tick     → uptime
```

**Sincronización — doble buffer ping-pong:**
```
buffers[0], buffers[1]  ← dos snapshots estáticos, sin malloc en runtime
write_idx (atómico)     ← el monitor escribe aquí
read_idx  (atómico)     ← el cliente lee aquí

Monitor task:
  1. Toma snapshot → escribe en buffers[write_idx]
  2. Intercambia atómicamente write_idx ↔ read_idx
  3. Publica EVT_MONITOR_UPDATED

Cliente (LVGL task):
  1. svc_monitor_get() → &buffers[read_idx]
  2. Siempre coherente: nunca lee el buffer que el monitor escribe
```

### 6.2 Data model

```c
/* components/sys_services/include/svc_monitor.h */

#define MON_MAX_APPS      12
#define MON_MAX_SERVICES  12

typedef struct {
    app_id_t     app_id;
    char         name[24];
    proc_state_t state;          /* RUNNING | SUSPENDED | BACKGROUND */
    uint8_t      view_count;
    uint8_t      bg_task_count;
    size_t       heap_delta;
    uint32_t     uptime_s;
} mon_app_entry_t;

typedef struct {
    char        name[24];
    svc_state_t state;           /* RUNNING | IDLE | ERROR | OFFLINE */
    char        status_text[32]; /* "192.168.1.5", "78%", "NTP ok" */
} mon_service_entry_t;

typedef struct {
    char       name[OS_TASK_NAME_LEN];
    app_id_t   owner;
    uint32_t   stack_hwm;
    uint8_t    priority;
    uint8_t    core;
    eTaskState rtos_state;
} mon_task_entry_t;

typedef struct {
    size_t   heap_internal_free;
    size_t   heap_internal_total;
    size_t   heap_psram_free;
    size_t   heap_psram_total;

    mon_app_entry_t     apps[MON_MAX_APPS];
    uint8_t             app_count;

    mon_service_entry_t services[MON_MAX_SERVICES];
    uint8_t             service_count;

    /* Solo con CONFIG_CYBERDECK_MONITOR_DEV_MODE=y */
    mon_task_entry_t    tasks[OS_MAX_TASKS];
    uint8_t             task_count;   /* 0 en producción */

    uint32_t uptime_s;
    uint32_t snapshot_tick;
    uint16_t refresh_count;
} sys_snapshot_t;

/* API */
esp_err_t             svc_monitor_init(uint32_t refresh_interval_ms);
const sys_snapshot_t *svc_monitor_get_snapshot(void);
void                  svc_monitor_force_refresh(void);
```

### 6.3 App Processes — tres pantallas

**Pantalla 0: Overview** (main, event-driven)
```
PROCESSES

  MEMORY
  INT  ████████░░░░  58%   142K / 340K
  PSRM ████░░░░░░░░  32%   1.2M / 4M

  RUNNING APPS
  SETTINGS   SUSPENDED  2v ~18K  [KILL]   ← tap → App Detail
  PROCESSES  RUNNING    1v ~12K           ← self: sin KILL

  SERVICES
  ● svc_wifi        192.168.1.5
  ● svc_battery     78%
  ● svc_time        NTP synced
  ○ svc_ota         idle
  ○ svc_downloader  idle
```

Actualización: suscribe `EVT_MONITOR_UPDATED` → `lv_label_set_text` en widgets existentes.
Si `app_count` o `service_count` cambia → reconstruir solo esa sección.

**Pantalla 1: App Detail** (push desde tap en app row)
```
SETTINGS

  Estado:          SUSPENDED (HOME)
  Lanzada:         hace 1m 23s
  Views en stack:  2  (main + WiFi list)
  Heap delta:      ~18K
  BG tasks:        0
  Storage:         /sdcard/apps/settings/

           [RAISE TO FRONT]   [KILL]
```

**Pantalla 2: Sys View** (dev mode, long press en header de memoria)
```
SYSTEM TASKS

  Tick: 45,231ms   Tasks: 12

  NAME              PRIO CORE  STK    STATE
  lvgl_task           5    1   256W   RUNNING
  os_poller           3    0   128W   BLOCKED
  svc_battery_bg      3    0   512W   BLOCKED
  notes_sync          3    0   1.2K   BLOCKED
  esp_timer          22    0    —     BLOCKED
  IDLE0               0    0    —     RUNNING
  IDLE1               0    1    —     RUNNING
```

Solo visible con `CONFIG_CYBERDECK_MONITOR_DEV_MODE=y`.

---

## 7. Service Registry

```c
/* components/os_core/include/os_service.h */

typedef enum {
    SVC_STATE_INIT    = 0,
    SVC_STATE_RUNNING = 1,
    SVC_STATE_IDLE    = 2,
    SVC_STATE_ERROR   = 3,
    SVC_STATE_OFFLINE = 4,
} svc_state_t;

void    os_service_register(const char *name);
void    os_service_update(const char *name, svc_state_t state,
                           const char *status_text);
uint8_t os_service_list(mon_service_entry_t *buf, uint8_t max);
```

**Servicios que se auto-registran:**

| Servicio | Nombre | Estado inicial | Actualizaciones |
|----------|--------|----------------|-----------------|
| `svc_wifi` | `"svc_wifi"` | INIT | RUNNING(ssid) / OFFLINE |
| `svc_battery` | `"svc_battery"` | INIT | RUNNING("78%") |
| `svc_time` | `"svc_time"` | INIT | RUNNING("NTP ok") / IDLE("RTC only") |
| `svc_ota` | `"svc_ota"` | IDLE | RUNNING durante check/download |
| `svc_downloader` | `"svc_downloader"` | IDLE | RUNNING durante descarga |
| `os_poller` | `"os_poller"` | INIT | RUNNING al iniciar |

---

## 8. Plan de implementación por fases

### ✅ Fases 1–6 — DONE (build limpio 2026-04-15)

Ver historial de commits. Fase 6 = close hook + Task Manager unificado.
Pendiente: verificación en hardware de Fase 6.

---

### 🔲 Fase 7a — Infraestructura OS (no disruptiva)

No rompe nada existente. Infraestructura nueva en paralelo.

| ID | Descripción | Archivo nuevo |
|----|-------------|---------------|
| I1 | `os_process.h/.c` — registry de procesos activos | `os_core/` |
| I2 | `os_service.h/.c` — registry de servicios de core | `os_core/` |
| I3 | `svc_monitor.h/.c` — snapshot + doble buffer + eventos | `sys_services/` |
| I4 | Servicios auto-registran en `os_service`: wifi, battery, time, ota, downloader, poller | múltiples |
| I5 | Nuevos `EVT_APP_LAUNCHED`, `EVT_APP_TERMINATED`, `EVT_MONITOR_UPDATED` en `os_core.h` | `os_core.h` |
| I6 | `svc_monitor_init(2000)` en `app_main()` | `main.c` |

**I1 — `os_process.h` draft:**

```c
typedef enum {
    PROC_STATE_STOPPED    = 0,
    PROC_STATE_RUNNING    = 1,
    PROC_STATE_SUSPENDED  = 2,   /* HOME, screen hidden */
    PROC_STATE_BACKGROUND = 3,   /* sin screen, solo bg tasks */
} proc_state_t;

typedef struct {
    app_id_t      app_id;
    proc_state_t  state;
    void         *app_data;
    uint32_t      launched_ms;
    uint8_t       view_count;
    uint8_t       task_count;
    size_t        heap_before;   /* snapshot antes de on_launch */
} os_process_t;

void          os_process_init(void);
esp_err_t     os_process_start(app_id_t id, void *app_data, size_t heap_before);
void          os_process_stop(app_id_t id);
os_process_t *os_process_get(app_id_t id);
bool          os_process_is_running(app_id_t id);
void          os_process_set_state(app_id_t id, proc_state_t state);
void          os_process_update_counts(app_id_t id, uint8_t views, uint8_t tasks);
uint8_t       os_process_list(os_process_t *buf, uint8_t max);
```

---

### 🔲 Fase 7b — Migración de firma (ruptura total)

> **Sesión dedicada.** Cambiar firma en TODO de una vez — nunca gradualmente.
> Target: 0 errores, 0 warnings antes de commitear.

| ID | Descripción | Archivos afectados |
|----|-------------|--------------------|
| J1 | `os_app_storage.h/.c` — sandbox FS | `os_core/` |
| J2 | `os_db.h/.c` — SQLite migrations | `os_core/` |
| J3 | Cambiar `view_cbs_t`: add `void *app_data` en 4 callbacks | `ui_activity.h` |
| J4 | Cambiar `app_ops_t`: `on_launch(id, args, storage*)` retorna `app_data*` | `app_registry.h` |
| J5 | `ui_activity.c`: pasar `app_data` del proceso a cada view callback | `ui_activity.c` |
| J6 | `app_manager.c`: `os_process_start` en navigate_fn, `os_process_stop` en close hook | `app_manager.c` |
| J7 | Migrar Launcher | `app_launcher.c` |
| J8 | Migrar Lockscreen | `launcher_lockscreen.c` |
| J9 | Migrar Settings main | `app_settings.c` |
| J10 | Migrar sub-screens de Settings (8 pantallas) | `settings_*.c` |
| J11 | Migrar Task Manager | `app_taskman.c` |
| J12 | Build clean — 0 errores, 0 warnings | — |

Orden sugerido J7→J11: empezar por las apps más simples (Launcher, TaskMan) antes de Settings.

---

### 🔲 Fase 7c — Processes app completa + servicios

| ID | Descripción |
|----|-------------|
| K1 | App Processes: pantalla Overview (event-driven, sin polling timer) |
| K2 | App Processes: pantalla App Detail (raise/kill) |
| K3 | App Processes: pantalla Sys View (dev mode) |
| K4 | Renombrar `app_taskman` → `app_processes`, actualizar registry |
| K5 | Verificar en hardware: kill app → tasks muertos, memoria liberada |

---

### 🔲 Fase 7d — Apps con storage real

| ID | App | DB Schema |
|----|-----|-----------|
| L1 | Notes | `notes(id, title, body, created, updated, pinned)` |
| L2 | Tasks | `tasks(id, title, done, due_date, priority, created)` |
| L3 | Calc | `history(id, expression, result, created)` |
| L4 | Files | Browser /sdcard/ — sin DB propia |

---

## 9. Estructura de archivos target

```
components/
├── os_core/
│   ├── include/
│   │   ├── os_core.h           ← existente (app_id_t, EVT_*, OS_OWNER_SYSTEM)
│   │   ├── os_task.h           ← existente
│   │   ├── os_process.h        ← NEW I1
│   │   ├── os_service.h        ← NEW I2
│   │   ├── os_app_storage.h    ← NEW J1
│   │   ├── os_db.h             ← NEW J2
│   │   ├── os_defer.h          ← existente
│   │   ├── os_event.h          ← existente
│   │   ├── os_poller.h         ← existente
│   │   └── os_settings.h       ← existente (Fase 5)
│   └── *.c
│
├── sys_services/
│   ├── include/
│   │   ├── svc_monitor.h       ← NEW I3
│   │   ├── svc_wifi.h
│   │   ├── svc_battery.h
│   │   ├── svc_time.h
│   │   ├── svc_ota.h
│   │   └── svc_downloader.h
│   └── *.c
│
├── ui_engine/
│   ├── include/
│   │   ├── ui_activity.h       ← MODIFIED J3 (view_cbs_t new signature)
│   │   └── ...
│   └── *.c
│
├── app_framework/
│   ├── include/
│   │   ├── app_registry.h      ← MODIFIED J4 (app_ops_t new signature)
│   │   ├── app_manager.h
│   │   └── os_nav.h
│   └── *.c
│
└── apps/
    ├── launcher/               ← MODIFIED J7, J8
    ├── settings/               ← MODIFIED J9, J10
    ├── processes/              ← REWRITTEN K1-K4
    │   ├── include/app_processes.h
    │   ├── processes_overview.c
    │   ├── processes_app_detail.c
    │   └── processes_sysview.c
    ├── notes/                  ← NEW L1
    ├── tasks_app/              ← NEW L2  (no confundir con os_task)
    ├── calc/                   ← NEW L3
    └── files/                  ← NEW L4
```

**Storage en SD:**
```
/sdcard/apps/
├── notes/
│   ├── notes.db
│   ├── files/
│   └── cache/
├── tasks/
│   ├── tasks.db
│   └── attachments/
├── calc/
│   └── calc.db
└── bluesky/
    ├── bluesky.db
    └── media/
```

---

## 10. Invariantes del SDK (reglas para app authors)

### Estado
1. `view_state*` contiene **solo** `lv_obj_t*`, `lv_timer_t*` y handles de widgets LVGL
2. `app_data*` contiene datos de app — allocado en `on_launch`, liberado en `on_terminate`
3. `on_create(args=NULL)` = rotación → reconstruir UI desde `app_data` sin NVS ni DB
4. NVS solo se lee en `on_launch` si no está ya en `app_data`; se escribe en `on_terminate` o en callbacks de usuario

### Background Tasks
5. Crear con `os_task_create()` y `owner = APP_ID_*`, nunca `xTaskCreate` directamente
6. Crear en `on_launch` (no en `on_create`) — persisten entre rotaciones
7. Comunicar a la view via `svc_event_post()` — nunca tocar LVGL directamente
8. El OS mata las bg tasks en terminate — no hacerlo manualmente en `on_terminate`

### Event Handlers
9. Registrar en `on_create`, cancelar en `on_destroy`
10. En `on_destroy`: `g_view_state = NULL` **antes** de `svc_event_unregister` (previene race)
11. En el handler: `if (!g_view) return;` → `ui_lock()` → `if (!g_view) { ui_unlock(); return; }` (double-check)

### Storage
12. Si `storage->db_ready == false` → modo degradado sin DB (SD no disponible)
13. Al recibir `EVT_SDCARD_MOUNTED`: llamar `os_app_storage_reopen()` si `!storage->db_ready`
14. Flush a DB en `on_terminate`, no en `on_destroy` (storage puede estar cerrado antes)
15. Una sola conexión SQLite por app, provista por el OS en `storage->db`

### Navegación
16. `on_resume` puede encontrar `app_data` modificado por bg tasks — refrescar UI si `dirty`
17. Push de sub-screens: `os_view_push(app_id, screen_id, &cbs, &args)`
18. `view_args_t.owned=true` → el OS libera `args->data` tras `on_create`

---

## 11. Ejemplo completo — app Notes

```c
/* ---- DB Schema ---- */
static const db_migration_t NOTES_DB[] = {
    { .version = 1, .up_sql =
        "CREATE TABLE notes ("
        "  id      INTEGER PRIMARY KEY,"
        "  title   TEXT NOT NULL DEFAULT '',"
        "  body    TEXT NOT NULL DEFAULT '',"
        "  created INTEGER NOT NULL,"
        "  updated INTEGER NOT NULL,"
        "  pinned  INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX idx_updated ON notes(updated DESC);"
    },
    { .version = 2, .up_sql =
        "ALTER TABLE notes ADD COLUMN color INTEGER NOT NULL DEFAULT 0;"
    },
};

/* ---- L1: App Data (proceso, sobrevive rotación) ---- */
typedef struct {
    os_app_storage_t storage;    /* FS + DB — inyectado por OS */
    int64_t  *note_ids;          /* caché de IDs desde DB */
    char    **note_titles;       /* caché de títulos */
    int       note_count;
    int       selected_idx;      /* sobrevive rotación */
    int       scroll_pos;        /* sobrevive rotación */
    bool      dirty;             /* hay cambios de bg task sin aplicar en UI */
} notes_app_data_t;

/* ---- L2: View State (solo widgets) ---- */
typedef struct {
    lv_obj_t   *list;
    lv_obj_t   *empty_label;
    lv_timer_t *autosave_timer;
} notes_view_t;

static notes_view_t *g_view = NULL;  /* para event handlers */

/* ---- App Ops ---- */
static void *notes_on_launch(app_id_t id, const view_args_t *args,
                              os_app_storage_t *storage)
{
    notes_app_data_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->storage = *storage;   /* adoptar storage del OS */
    notes_load_cache(d);     /* SELECT id, title FROM notes ORDER BY... */
    return d;
}

static void notes_on_terminate(app_id_t id, void *app_data)
{
    notes_app_data_t *d = app_data;
    for (int i = 0; i < d->note_count; i++) free(d->note_titles[i]);
    free(d->note_ids);
    free(d->note_titles);
    free(d);
    /* El OS llama os_app_storage_close() DESPUÉS de aquí */
}

/* ---- View Callbacks ---- */
static void *notes_on_create(lv_obj_t *screen, const view_args_t *args,
                              void *app_data)
{
    notes_app_data_t *d = app_data;   /* L1: fuente de verdad */
    notes_view_t *v = calloc(1, sizeof(*v));

    ui_statusbar_set_title("NOTES");
    lv_obj_t *content = ui_common_content_area(screen);
    v->list = ui_common_list(content);

    for (int i = 0; i < d->note_count; i++)
        ui_common_list_add(v->list, d->note_titles[i], i, item_cb, d);

    /* Restaurar scroll desde L1 — funciona en rotación sin consultar DB */
    lv_obj_scroll_to_y(v->list, d->scroll_pos, LV_ANIM_OFF);

    g_view = v;
    svc_event_register(EVT_SDCARD_MOUNTED, sd_mount_handler, d);
    return v;
}

static void notes_on_destroy(lv_obj_t *screen, void *view_state, void *app_data)
{
    notes_view_t     *v = view_state;
    notes_app_data_t *d = app_data;

    /* Guardar scroll en L1 ANTES de destruir el widget */
    if (v->list) d->scroll_pos = lv_obj_get_scroll_y(v->list);

    /* g_view = NULL PRIMERO — handler puede estar corriendo concurrentemente */
    g_view = NULL;
    svc_event_unregister(EVT_SDCARD_MOUNTED, sd_mount_handler);
    if (v->autosave_timer) lv_timer_del(v->autosave_timer);
    free(v);
    /* NO free(d) — el OS lo hace en on_terminate */
}

static void notes_on_resume(lv_obj_t *screen, void *view_state, void *app_data)
{
    notes_view_t     *v = view_state;
    notes_app_data_t *d = app_data;
    ui_statusbar_set_title("NOTES");
    if (d->dirty) {          /* bg task actualizó datos mientras estaba oculta */
        notes_load_cache(d);
        notes_rebuild_list(v, d);
        d->dirty = false;
    }
}

/* ---- Registro ---- */
esp_err_t app_notes_register(void)
{
    static const app_manifest_t manifest = {
        .id          = APP_ID_NOTES,
        .name        = "Notes",
        .icon        = "Nt",
        .type        = APP_TYPE_BUILTIN,
        .permissions = APP_PERM_SD,
        .storage_dir = "notes",
    };
    static const app_ops_t ops = {
        .on_launch    = notes_on_launch,
        .on_terminate = notes_on_terminate,
    };
    static const view_cbs_t main_view = {
        .on_create  = notes_on_create,
        .on_resume  = notes_on_resume,
        .on_destroy = notes_on_destroy,
    };
    os_app_register(&manifest, &ops, &main_view);
    os_app_register_db_migrations(APP_ID_NOTES, NOTES_DB,
                                   sizeof(NOTES_DB) / sizeof(*NOTES_DB));
    return ESP_OK;
}
```

---

## 12. Progreso detallado

### Completado

| Fecha | Commit | Descripción |
|-------|--------|-------------|
| 2026-04-15 | `aebbbe7` | Fase 4 — storage, crash log, dynamic SD apps |
| 2026-04-15 | `02190cb` | Fase 5 — os_settings cache API + app Task Manager |
| 2026-04-15 | WIP | Fase 6 — H1/H2/H3/H4: close hook, lifecycle integrity, Task Manager unificado |

### Fase 6 — pendiente hardware

- [ ] Flash + verificar que KILL en Processes mata tasks y libera memoria
- [ ] Commit de Fase 6

### Fase 7a — infraestructura OS

- [ ] I1: `os_process.h/.c`
- [ ] I2: `os_service.h/.c`
- [ ] I3: `svc_monitor.h/.c` + doble buffer + eventos
- [ ] I4: auto-registro de servicios (wifi, battery, time, ota, downloader, poller)
- [ ] I5: nuevos eventos en `os_core.h`
- [ ] I6: `svc_monitor_init()` en `app_main()`
- [ ] Build clean, commit

### Fase 7b — migración de firma

- [ ] J1: `os_app_storage.h/.c`
- [ ] J2: `os_db.h/.c`
- [ ] J3–J4: cambiar firmas `view_cbs_t` + `app_ops_t`
- [ ] J5–J6: `ui_activity.c` + `app_manager.c`
- [ ] J7–J11: migrar todas las apps (Launcher, Lockscreen, Settings x8, TaskMan)
- [ ] J12: build clean — 0 errores, 0 warnings
- [ ] Commit

### Fase 7c — Processes app completa

- [ ] K1: Overview screen (event-driven)
- [ ] K2: App Detail screen
- [ ] K3: Sys View (dev mode)
- [ ] K4: renombrar taskman → processes
- [ ] K5: verificar en hardware
- [ ] Commit

### Fase 7d — apps con storage

- [ ] L1: Notes
- [ ] L2: Tasks
- [ ] L3: Calc
- [ ] L4: Files

---

*Fin del documento. Actualizar sección 12 al completar cada ítem.*
