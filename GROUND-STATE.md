# GROUND-STATE.md — CyberDeck App Platform Audit

Estado actual de toda la arquitectura relacionada a apps, servicios del OS, componentes UI,
ciclo de vida, storage, eventos, interacción, y APIs pendientes de construir.
Sirve como base de diseño para el micro-intérprete / DSL de apps en SD Card.

---

## Índice

1. [Estado general por área](#1-estado-general-por-área)
2. [Ciclo de vida de una app](#2-ciclo-de-vida-de-una-app)
3. [Procesos en segundo plano](#3-procesos-en-segundo-plano)
4. [Sistema de eventos](#4-sistema-de-eventos)
5. [Storage por app](#5-storage-por-app)
6. [Componentes UI nativos](#6-componentes-ui-nativos)
7. [Sistema de navegación e interacción](#7-sistema-de-navegación-e-interacción)
8. [Datos en tiempo real](#8-datos-en-tiempo-real)
9. [SD Card app discovery y registro dinámico](#9-sd-card-app-discovery-y-registro-dinámico)
10. [HTTP/HTTPS API Client](#10-httphttps-api-client)
11. [NVS privado por app](#11-nvs-privado-por-app)
12. [API surface: limpia vs gotchas vs faltante](#12-api-surface-limpia-vs-gotchas-vs-faltante)
13. [Hoja de ruta para el micro-intérprete](#13-hoja-de-ruta-para-el-micro-intérprete)

---

## 1. Estado general por área

| Área | Estado | Calidad | Archivo(s) clave |
|---|---|---|---|
| Ciclo de vida de app | ✅ Implementado | Production-ready | `app_manager.c`, `ui_activity.c`, `app_registry.c` |
| Procesos en background | ✅ Implementado | Production-ready | `os_process.c`, `os_task.c`, `os_poller.c` |
| Sistema de eventos | ✅ Implementado | Production-ready | `os_event.c`, `svc_event.c` |
| Storage por app (SD + SQLite) | ✅ Implementado | Production-ready | `os_app_storage.c`, `os_storage.c`, `os_db.c` |
| Componentes UI nativos | ⚠️ Parcial | Works but rough | `ui_common.c`, `ui_theme.c`, `ui_effect.c` |
| Navegación y gestos | ✅ Implementado | Production-ready | `ui_activity.c`, `ui_navbar.c`, `hal_gesture.c` |
| Datos en tiempo real | ⚠️ Parcial | Works but rough | `os_event.c`, `os_poller.c` |
| SD App discovery + registro dinámico | ✅ Implementado | Production-ready | `os_app_discover.c`, `os_manifest.c`, `app_registry.c` |
| HTTP/HTTPS API Client (general) | ❌ No existe | — | Solo `svc_downloader.c` (archivos) |
| NVS privado por app | ❌ No implementado | — | Solo NVS global en `svc_settings.c` |
| Script runtime vtable | ✅ Definido | Stub | `os_script.h` — ningún intérprete conectado |

---

## 2. Ciclo de vida de una app

### Dos capas de state independientes

```
L1: app_data — persiste mientras el proceso está vivo
    on_launch(app_id, app_data)       → alloca L1, abre storage, inicia servicios
    on_suspend(app_id, app_data)      → pausa background tasks al ir a home
    on_foreground(app_id, app_data)   → reanuda al volver al frente
    on_terminate(app_id, app_data)    → free L1, flush DB, cleanup total

L2: view_state — scoped por pantalla, destruido al hacer pop
    on_create(screen, args, app_data)          → alloca L2, crea widgets, registra eventos
    on_resume(screen, view_state, app_data)    → reinicia timers, muestra keyboard
    on_pause(screen, view_state, app_data)     → pausa timers
    on_destroy(screen, view_state, app_data)   → free L2, cancela subs, oculta keyboard
```

### Flujo de registro (boot)

```c
// 1. En el inicializador de la app (llamado desde main.c):
app_manifest_t manifest = {
    .id          = APP_ID_MYAPP,
    .name        = "My App",
    .icon        = "MA",
    .type        = APP_TYPE_BUILTIN,
    .permissions = APP_PERM_WIFI | APP_PERM_SD,
    .storage_dir = "myapp",       // /sdcard/apps/myapp/
};
app_ops_t ops = {
    .on_launch     = myapp_launch,
    .on_terminate  = myapp_terminate,
    .on_suspend    = NULL,
    .on_foreground = NULL,
};
view_cbs_t cbs = {
    .on_create  = myapp_create,
    .on_resume  = myapp_resume,
    .on_pause   = NULL,
    .on_destroy = myapp_destroy,
};
os_app_register(&manifest, &ops, &cbs);

// 2. app_manager_init() conecta el sistema de intent al registry
// 3. La app aparece en el launcher automáticamente
```

### Limpieza automática al cerrar app

`on_app_closed` (app_manager) llama en orden:
1. `on_terminate(app_id, app_data)` — el app hace su cleanup
2. `os_task_destroy_all_for_app(app_id)` — mata todos los FreeRTOS tasks del app
3. `os_event_unsubscribe_all(app_id)` — cancela todas las subscripciones de eventos
4. `os_poller_remove_all_for_app(app_id)` — cancela todos los pollers

**Cero leaks si el app registra sus recursos con su `app_id` como owner.**

### Tipos de parámetros en callbacks (gotchas para el intérprete)

| Parámetro | Tipo C | Visible al app | Nota |
|---|---|---|---|
| `screen` | `lv_obj_t *` | Sí — LVGL directo | El intérprete necesita handle opaco |
| `args->data` | `void *` | Sí | Raw bytes — el intérprete define el formato |
| `args->owned` | `bool` | Sí | Si `true`, el OS llama `free(data)` post-`on_create` |
| `view_state` | `void *` | Sí | Retornado por `on_create`, opaco para el OS |
| `app_data` | `void *` | Sí | Retornado por `on_launch`, opaco para el OS |

### Intent data: convenciones

```c
// Caller heap-alloca y transfiere ownership:
char *ssid = malloc(33);
strncpy(ssid, ap_ssid, 32);
view_args_t args = { .data = ssid, .size = 33, .owned = true };
os_view_push(APP_ID_SETTINGS, SCREEN_WIFI_CONNECT, &cbs, &args);

// on_create del destino:
char *ssid = (char *)args->data;   // ya fue copiado
// NO llamar free() — owned=true lo hace el OS
```

Regla: heap only (no stack pointers). `NULL` es válido (sin datos). El caller y callee acuerdan la estructura fuera de banda.

---

## 3. Procesos en segundo plano

### os_task — Factory de FreeRTOS tasks con ownership

```c
os_task_config_t cfg = {
    .name         = "myapp_worker",
    .fn           = worker_fn,
    .arg          = ctx,
    .stack_size   = 4096,
    .priority     = OS_PRIO_MEDIUM,    // 5 — no expone UBaseType_t
    .core         = OS_CORE_BG,        // Core 0
    .owner        = APP_ID_MYAPP,
    .stack_in_psram = true,            // stack en PSRAM (ahorra SRAM)
};
TaskHandle_t handle;
os_task_create(&cfg, &handle);

// Cleanup automático al cerrar app:
os_task_destroy_all_for_app(APP_ID_MYAPP);  // llamado por framework
```

Registro interno: 32 slots. Tags por `app_id`. `os_task_list()` devuelve snapshot para el task manager.

### os_poller — Polling consolidado (1 task para N apps)

```c
// En lugar de crear un task dedicado de 3KB para cada poll:
os_poller_register("myapp_sensor", sensor_poll_fn, ctx, 500, APP_ID_MYAPP);
//                  nombre         callback          arg  ms   owner

// El OS tiene 1 task (Core 0) que llama todos los pollers registrados.
// Máx 16 pollers. Ahorra ~3KB de stack por app que lo usa.
// Cleanup automático: os_poller_remove_all_for_app(app_id)
```

### os_defer — One-shot timer

```c
os_defer(my_fn, ctx, 1500);    // llama my_fn(ctx) después de 1500ms
os_ui_post(my_fn, ctx);        // ejecuta my_fn en el LVGL task, con mutex held
```

### os_process — Registro de estado de proceso

```c
// El framework lo maneja — las apps no llaman esto directamente.
// Estado visible vía svc_monitor_get_snapshot() para el task manager.
typedef enum { PROC_STOPPED, PROC_RUNNING, PROC_SUSPENDED, PROC_BACKGROUND } proc_state_t;
```

---

## 4. Sistema de eventos

### 27 eventos definidos

| Evento | Payload | Productor | Uso típico |
|---|---|---|---|
| `EVT_WIFI_CONNECTED` | — | `svc_wifi` | Actualizar statusbar, habilitar network ops |
| `EVT_WIFI_DISCONNECTED` | — | `svc_wifi` | Mostrar error, cancelar requests |
| `EVT_WIFI_SCAN_DONE` | — | `svc_wifi` | Poblar lista de APs |
| `EVT_TIME_SYNCED` | — | `svc_time` | Actualizar display de hora |
| `EVT_BATTERY_UPDATED` | `uint8_t *pct` | `svc_battery` | Actualizar indicador de batería |
| `EVT_BATTERY_LOW` | — | `svc_battery` | Alertar al usuario, reducir consumo |
| `EVT_AUDIO_STATE_CHANGED` | — | audio service | Actualizar controles de audio |
| `EVT_AUDIO_NO_OUTPUT` | — | audio service | Alertar falta de salida de audio |
| `EVT_DOWNLOAD_PROGRESS` | `svc_download_progress_t *` | `svc_downloader` | Barra de progreso |
| `EVT_DOWNLOAD_COMPLETE` | `svc_download_progress_t *` | `svc_downloader` | Notificar al usuario |
| `EVT_DOWNLOAD_ERROR` | `svc_download_progress_t *` | `svc_downloader` | Mostrar error |
| `EVT_OTA_STARTED` | — | `svc_ota` | Bloquear navegación |
| `EVT_OTA_PROGRESS` | `uint8_t *pct` | `svc_ota` | Barra de progreso OTA |
| `EVT_OTA_COMPLETE` | — | `svc_ota` | Notificar reboot |
| `EVT_OTA_ERROR` | — | `svc_ota` | Mostrar error |
| `EVT_SDCARD_MOUNTED` | — | SD service | Reabrir storage, redescubrir apps |
| `EVT_SDCARD_UNMOUNTED` | — | SD service | Suspender operaciones de archivo |
| `EVT_SETTINGS_CHANGED` | `const char *key` | `os_settings_set_*` | Refresh de UI según setting cambiado |
| `EVT_GESTURE_HOME` | — | `hal_gesture` | Navegar a home |
| `EVT_GESTURE_BACK` | — | `hal_gesture` | Navegar atrás |
| `EVT_DISPLAY_ROTATED` | `uint8_t *rotation` | `ui_engine_set_rotation` | Reconstruir layouts |
| `EVT_NAV_PROCESSES` | — | navbar | Abrir task manager |
| `EVT_BOOT_COMPLETE` | — | OS boot | Inicialización post-boot |
| `EVT_MEMORY_LOW` | — | memory monitor | Liberar caches |
| `EVT_APP_LAUNCHED` | — | `app_manager` | Monitor de procesos |
| `EVT_APP_TERMINATED` | `uint8_t *app_id` | `os_process_stop` | Limpiar referencias |
| `EVT_SD_APP_DISCOVERED` | — | `os_app_discover` | Actualizar grid del launcher |
| `EVT_THEME_CHANGED` | — | `ui_theme_apply` | Refresh de todos los widgets |
| `EVT_MONITOR_UPDATED` | — | `svc_monitor` | Actualizar task manager |

### API de subscripción

```c
// Variante UI-safe (handler corre en LVGL task, mutex ya held):
event_sub_t sub = os_event_subscribe_ui(APP_ID_MYAPP, EVT_WIFI_SCAN_DONE,
                                         on_scan_done, my_state);

// Variante background (handler corre en event-loop task — NO tocar LVGL):
event_sub_t sub;
os_event_subscribe(APP_ID_MYAPP, EVT_BATTERY_UPDATED, on_battery, ctx, &sub);

// Cancelar una subscripción:
os_event_unsubscribe(sub);

// Cancelar todas las del app (llamado automáticamente por el framework al cerrar):
os_event_unsubscribe_all(APP_ID_MYAPP);

// Postear un evento:
svc_event_post(EVT_SETTINGS_CHANGED, "theme", strlen("theme") + 1);
```

### Patrón de handler UI-safe

```c
// Handler registrado con os_event_subscribe_ui — ya está en LVGL task:
static void on_scan_done(void *arg, esp_event_base_t base, int32_t id, void *data) {
    wifi_scr_state_t *s = arg;
    // No necesita ui_lock() — ya está en LVGL task
    populate_scan_results(s);
}
```

### Patrón de handler background con guard manual

```c
// Handler registrado con os_event_subscribe — corre en event-loop task:
static wifi_scr_state_t *g_wifi_state = NULL;  // seteado en on_create, NULL en on_destroy

static void on_scan_done(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (!g_wifi_state) return;          // guard: view ya destruida
    if (ui_lock(200)) {
        if (g_wifi_state)               // double-checked locking
            populate_scan_results(g_wifi_state);
        ui_unlock();
    }
}
```

**Gap para el intérprete:** Este guard manual debe ser automático — el framework debería cancelar el callback si la view fue destruida, sin que el script lo gestione.

### Implementación interna

- `svc_event.c`: Event loop en Core 0, queue 16 slots, 4KB stack, priority 4.
- `os_event.c`: 64 slots de subscripción, 4 simultáneas en runtime.
- Data de eventos es **copiada** antes del dispatch al LVGL task — sin use-after-free.
- `os_event_subscribe_ui` usa `os_ui_post()` como shim para reenviar al LVGL task.

---

## 5. Storage por app

### Estructura en SD Card

```
/sdcard/apps/{storage_dir}/
    files/              → datos persistentes de la app
    cache/              → datos descartables (limpiables en eviction)
    {storage_dir}.db    → SQLite database (WAL mode)
```

### API de storage

```c
// Abrir storage (en on_launch):
os_app_storage_t storage;
const db_migration_t migrations[] = {
    { .version = 1, .up_sql = "CREATE TABLE items (id INTEGER PRIMARY KEY, text TEXT);" },
    { .version = 2, .up_sql = "ALTER TABLE items ADD COLUMN ts INTEGER;" },
};
os_app_storage_open(APP_ID_MYAPP, "myapp", migrations, 2, &storage);
// storage.db_ready == false si SD no montada — app maneja modo offline

// Paths:
char path[128];
os_app_storage_path(&storage, "config.json", path, sizeof(path));
// → "/sdcard/apps/myapp/files/config.json"

// File I/O:
FILE *f = os_storage_fopen(APP_ID_MYAPP, "data.bin", "rb");
size_t n = os_storage_read(APP_ID_MYAPP, "config.json", buf, sizeof(buf));
os_storage_write(APP_ID_MYAPP, "cache/img.jpg", data, len);
bool exists = os_storage_exists(APP_ID_MYAPP, "config.json");
os_storage_delete(APP_ID_MYAPP, "cache/old.tmp");

// SQLite:
os_db_run(storage.db, "INSERT INTO items (text, ts) VALUES ('hello', 1700000000)");
int count = os_db_get_int(storage.db, "SELECT COUNT(*) FROM items", 0);
int64_t last_id = os_db_last_rowid(storage.db);

// Cerrar (en on_terminate):
os_app_storage_close(&storage);

// Re-abrir tras montar SD (en handler de EVT_SDCARD_MOUNTED):
os_app_storage_reopen(&storage);

// Limpiar cache:
os_app_storage_clear_cache(&storage);
```

### Degradación graceful

Si la SD no está montada al llamar `os_app_storage_open`, retorna `ESP_OK` con `storage.db_ready = false`. El app verifica este flag y opera en modo offline (usa solo RAM o NVS). Al recibir `EVT_SDCARD_MOUNTED`, llama `os_app_storage_reopen`.

### Gap — NVS privado por app

**No existe.** El NVS global (`svc_settings`) es compartido entre todos. No hay namespace por `app_id`.

Apps que necesitan persistencia sin SD card (tokens, preferencias pequeñas) no tienen solución limpia hoy. Ver sección 11 para la propuesta.

---

## 6. Componentes UI nativos

### Implementados

```c
// ── Contenedores ──────────────────────────────────────────────────────────

lv_obj_t *ui_common_content_area(lv_obj_t *screen);
// Área de contenido debajo del statusbar: flex-column, pad 16px, scrollable

lv_obj_t *ui_common_panel(lv_obj_t *parent);
// Card con borde primario y radius 12px

lv_obj_t *ui_common_card(lv_obj_t *parent, const char *title, lv_coord_t w, lv_coord_t h);
// Card con título en borde superior

// ── Listas ────────────────────────────────────────────────────────────────

lv_obj_t *ui_common_list(lv_obj_t *parent);
// Contenedor flex-column para items de lista

typedef void (*ui_list_cb_t)(uint32_t index, void *data);

void ui_common_list_add(lv_obj_t *list, const char *text, uint32_t idx,
                        ui_list_cb_t cb, void *data);
// Item de una línea, tappable, borde inferior

void ui_common_list_add_two_line(lv_obj_t *list, const char *primary,
                                  const char *secondary, uint32_t idx,
                                  ui_list_cb_t cb, void *data);
// Item de dos líneas: primary MD + secondary SM dim

// ── Grids ─────────────────────────────────────────────────────────────────

lv_obj_t *ui_common_grid(lv_obj_t *parent, uint8_t cols, lv_coord_t row_h);
// Grid flex-wrap con N columnas y altura de fila fija

void ui_common_grid_cell(lv_obj_t *grid, const char *icon, const char *label,
                          uint8_t col, uint8_t row);
// Celda de grid: icono (texto) arriba + label abajo

// ── Botones ───────────────────────────────────────────────────────────────

lv_obj_t *ui_common_btn(lv_obj_t *parent, const char *text);
// Botón outline (borde primary, sin fill). Press: invierte colores.

lv_obj_t *ui_common_btn_full(lv_obj_t *parent, const char *text);
// Igual pero full-width

void ui_common_btn_style_primary(lv_obj_t *btn);
// Convierte btn outline en CTA: bg primary, text bg_dark

// ── Layout helpers ────────────────────────────────────────────────────────

lv_obj_t *ui_common_action_row(lv_obj_t *parent);
// Fila flex-row END para par [CANCEL][OK]

void ui_common_data_row(lv_obj_t *parent, const char *label, const char *value);
// Etiqueta dim SM arriba + valor primary MD abajo

void ui_common_section_gap(lv_obj_t *parent);
// Spacer de 18px entre grupos de contenido

void ui_common_spacer(lv_obj_t *parent);
// Flex-grow spacer (empuja siguientes hijos al fondo)

void ui_common_divider(lv_obj_t *parent);
// Línea horizontal 1px (solo dentro de listas)

// ── Feedback ─────────────────────────────────────────────────────────────

void ui_effect_toast(const char *msg, uint16_t duration_ms);
// Toast centrado (corregido por navbar), borde primary, SM font

typedef void (*ui_confirm_cb_t)(bool confirmed, void *ctx);

void ui_effect_confirm(const char *title, const char *msg,
                       ui_confirm_cb_t cb, void *ctx);
// Modal: backdrop 50%, caja 380px, título parallelogram, [CANCEL][OK]

void ui_effect_loading(bool show);
// Overlay 70% con cursor "_" parpadeante, 500ms interval

void ui_effect_progress_show(const char *msg, bool cancellable,
                              ui_confirm_cb_t cancel_cb, void *ctx);
void ui_effect_progress_set(int8_t pct);    // -1 = indeterminate
void ui_effect_progress_hide(void);

// ── Keyboard ─────────────────────────────────────────────────────────────

void ui_keyboard_show(lv_obj_t *textarea);  // Llamar solo desde on_resume
void ui_keyboard_hide(void);
bool ui_keyboard_is_visible(void);

// ── Theme ─────────────────────────────────────────────────────────────────

const cyberdeck_theme_t *ui_theme_get(void);
cyberdeck_theme_id_t     ui_theme_get_id(void);
void                     ui_theme_apply(cyberdeck_theme_id_t id);

// Styling helpers:
void ui_theme_style_container(lv_obj_t *obj);   // bg_dark, borde dim, radius 2
void ui_theme_style_btn(lv_obj_t *btn);          // outline + press-fill
void ui_theme_style_label(lv_obj_t *lbl, const lv_font_t *font);
void ui_theme_style_label_dim(lv_obj_t *lbl, const lv_font_t *font);
void ui_theme_style_list_item(lv_obj_t *obj);    // borde inferior, pad 12/8
void ui_theme_style_textarea(lv_obj_t *ta);
void ui_theme_style_scrollbar(lv_obj_t *obj);    // 2px, dim, radius 0

// Fuentes:
// CYBERDECK_FONT_SM  → Montserrat 18 (statusbar, captions)
// CYBERDECK_FONT_MD  → Montserrat 24 (body, lista, botones)
// CYBERDECK_FONT_LG  → Montserrat 32 (iconos de card)
// CYBERDECK_FONT_XL  → Montserrat 40 (tiempo, PIN, launcher)
```

### Paleta de temas

| Campo | Green (Matrix) | Amber (Retro) | Neon (Cyberpunk) |
|---|---|---|---|
| `primary` | `#00FF41` | `#FFB000` | `#FF00FF` |
| `primary_dim` | `#004D13` | `#4D3500` | magenta dim |
| `bg_dark` | `#000000` | `#000000` | `#000000` |
| `bg_card` | `#0A0A0A` | `#0A0A0A` | `#0A0A0A` |
| `text_dim` | 50% primary | 50% primary | 50% primary |
| `secondary` | — | — | `#00FFFF` |
| `accent` | — | — | `#FF0055` |
| `success` | — | — | `#39FF14` |

### Widgets faltantes (necesarios para apps reales)

```c
// ── Controles interactivos — NO EXISTEN ──────────────────────────────────
ui_common_slider(parent, min, max, val, on_change)
ui_common_checkbox(parent, label, checked, on_change)
ui_common_switch(parent, state, on_change)           // toggle ON/OFF
ui_common_dropdown(parent, options[], n, sel, on_change)
ui_common_radio_group(parent, options[], n, sel, on_change)
ui_common_spinner(parent, min, max, step, val, on_change)

// ── Display de datos — NO EXISTEN ─────────────────────────────────────────
ui_common_progress_bar(parent, pct)       // display sin cancelar
ui_common_badge(parent, text)             // contador o estado pequeño
ui_common_tag(parent, text)               // pill/chip de categoría

// ── Inputs — NO EXISTEN (apps crean lv_textarea directo) ─────────────────
ui_common_textarea(parent, placeholder, max_len)   // multi-line
ui_common_input(parent, placeholder, max_len)      // single-line

// ── Ítems de lista enriquecidos — NO EXISTEN ─────────────────────────────
ui_common_list_add_icon(list, icon, text, idx, cb, data)
ui_common_list_add_value(list, label, value, idx, cb, data)  // item + valor derecha
ui_common_list_add_toggle(list, label, state, on_change)     // item con switch

// ── Navegación compuesta — NO EXISTEN ────────────────────────────────────
ui_common_tabs(parent, labels[], n, on_tab)
ui_common_segmented(parent, labels[], n, sel, cb)

// ── Datos visuales — NO EXISTEN ──────────────────────────────────────────
ui_common_chart_line(parent, data[], n, label)
ui_common_meter(parent, pct, label)                // gauge circular
ui_common_sparkline(parent, data[], n)             // mini chart inline
```

### Reglas de diseño (para consistencia al implementar faltantes)

- Todos los textos en ALL CAPS excepto mensajes de toast.
- Botones: 2px border, radius 12px. Press: bg=primary, text=bg_dark.
- Inputs: radius 8px, sin border explícito (usa theme_style_textarea).
- Listas: borde inferior 1px, sin radius, pad 12px vertical / 8px horizontal.
- Nunca usar Unicode arrows/bullets — usar `LV_SYMBOL_*` o canvas.
- Después de `lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE)` siempre:
  `lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE)` para evitar stuck-pressed.

---

## 7. Sistema de navegación e interacción

### Stack de actividades

```
Máx 8 niveles. Slot 0 = Launcher (nunca se destruye).

[0] Launcher  [1] Settings  [2] WiFi List  [3] WiFi Connect
                                            ↑ current (depth=4)
```

### API de navegación para apps

```c
// Desde una app:
os_view_push(app_id, screen_id, &cbs, &args);  // push sub-screen
os_view_pop();                                  // volver un nivel
os_view_pop_to_root();                          // pop hasta screen_id=0 (raíz del app)
os_view_home();                                 // suspend to home (app sigue viva en background)

// Desde app_manager (cross-app):
app_manager_launch(app_id, data, data_size);   // lanza otra app
app_manager_go_back();                          // pop top
app_manager_go_home();                          // pop all to launcher
app_manager_lock();                             // push lockscreen + nav_lock

// Intent (desde launcher):
intent_t intent = { .app_id = APP_ID_SETTINGS, .screen_id = 0, .data = NULL };
ui_intent_navigate(&intent);
```

### Singleton enforcement

Si el app ya está en el stack, `ui_activity_raise(app_id)` lo trae al frente en vez de duplicarlo. El `app_manager` verifica esto antes de hacer push.

### Gestos de borde (hal_gesture.c)

```
Swipe down desde borde superior (y < EDGE_TOP_PX)          → EVT_GESTURE_HOME
Swipe derecha desde borde izquierdo (x < EDGE_LEFT_PX,
                                      dx > 40px)            → EVT_GESTURE_BACK
```

Dos franjas transparentes en `lv_layer_top()`, recreadas en rotación. No requieren código en las apps — el OS las maneja globalmente.

### Navbar (ui_navbar.c)

```
Portrait:  docked al fondo, height=72px, borde superior 2px
Landscape: docked a la derecha, width=72px, borde izquierdo 2px

Botones:
  [◀] → ui_activity_pop()                    (canvas 2px, triángulo izquierda)
  [○] → ui_activity_suspend_to_home()         (círculo outline)
  [□] → svc_event_post(EVT_NAV_PROCESSES)    (cuadrado outline → abre taskman)
```

### Lockscreen

- `app_manager_lock()` → push lockscreen + `ui_activity_set_nav_lock(true)`
- Con nav_lock activo, BACK, HOME y RAISE están bloqueados
- `app_manager_clear_lock()` → desbloquea

### Reglas de timing de keyboard

```c
// INCORRECTO — on_create, screen no está activo aún:
static void *myapp_create(lv_obj_t *screen, ...) {
    ui_keyboard_show(ta);   // ← nunca aquí
}

// CORRECTO — on_resume, screen ya es el activo:
static void myapp_resume(lv_obj_t *screen, void *view_state, void *app_data) {
    myapp_state_t *s = view_state;
    ui_keyboard_show(s->input_ta);   // ← aquí
}
```

### Secuencia completa de navegación (referencia)

```
Boot
  └─ push Launcher (depth=1) → on_create: grid de apps

Tap "Settings"
  └─ ui_intent_navigate → push Settings/main (depth=2) → on_create: lista de categorías

Tap "WiFi"
  └─ os_view_push(SETTINGS, SCREEN_WIFI, ...) (depth=3)
       on_create: alloca state, suscribe EVT_WIFI_SCAN_DONE, construye lista, inicia scan

[EVT_WIFI_SCAN_DONE]
  └─ on_scan_done (LVGL task): popula lista de APs

Tap AP
  └─ os_view_push(SETTINGS, SCREEN_WIFI+100, ...) (depth=4)
       on_create: copia SSID del intent, construye form de password
       on_resume: ui_keyboard_show(password_input)

Tap "CONNECT"
  └─ guarda creds en NVS, svc_wifi_connect(), os_view_pop()
       → WiFi list on_resume: reinicia scan (depth=3)
       → WiFi connect on_destroy: ui_keyboard_hide(), free state

[EVT_WIFI_CONNECTED]
  └─ statusbar actualiza icono de WiFi

Gesto BACK ×2
  └─ pop WiFi (depth=2): on_destroy: cancela subs, borra timer
  └─ pop Settings (depth=1): Launcher on_resume

Gesto HOME
  └─ ui_activity_suspend_to_home(): Launcher visible, Settings en background (no destruido)
```

---

## 8. Datos en tiempo real

### Mecanismo actual

1. **Eventos** (`os_event_subscribe_ui`): Push desde servicios → LVGL task → widget update. El caso más común.
2. **Poller** (`os_poller_register`): Pull periódico (ms interval) en background → puede postear evento o actualizar state directamente.
3. **app_state snapshot** (`app_state_get()`): Snapshot read-only del dispositivo. Actualizado por handlers de eventos internos del OS.

```c
// Ejemplo: actualizar un label de batería en tiempo real
static void *battery_create(lv_obj_t *screen, ...) {
    state_t *s = calloc(1, sizeof(*s));
    s->lbl = lv_label_create(ui_common_content_area(screen));

    // Mostrar valor actual inmediatamente:
    s->lbl_text = ui_common_data_row(content, "BATTERY:", "");
    lv_label_set_text_fmt(s->lbl_text, "%d%%", app_state_get()->battery_pct);

    // Suscribir para updates futuros (handler en LVGL task):
    s->sub = os_event_subscribe_ui(APP_ID_MYAPP, EVT_BATTERY_UPDATED, on_battery, s);
    return s;
}

static void on_battery(void *arg, esp_event_base_t base, int32_t id, void *data) {
    state_t *s = arg;
    lv_label_set_text_fmt(s->lbl_text, "%d%%", *(uint8_t*)data);
}

static void battery_destroy(lv_obj_t *screen, void *vs, void *ad) {
    state_t *s = vs;
    os_event_unsubscribe(s->sub);
    free(s);
}
```

### Gaps

- **No hay reactive bindings:** Las apps cablean manualmente evento → update de widget. Verbose.
- **No hay debounce/throttle helpers:** Si un evento llega 60 veces por segundo, el widget se actualiza 60 veces.
- **No hay diff en listas:** Cualquier cambio requiere reconstruir la lista completa.

### Propuesta para el intérprete: bindings declarativos

```c
// Binding simple: widget se actualiza automáticamente en cada evento
os_bind_label(label, EVT_BATTERY_UPDATED, fmt_battery_pct, NULL);

// Binding extractor: saca un campo del payload
os_bind_label_extract(label, EVT_DOWNLOAD_PROGRESS,
                      offsetof(svc_download_progress_t, bytes_received),
                      OS_BIND_FMT_U32, NULL);
```

O en el DSL del intérprete:
```
val battery = bind(BATTERY_PCT)      -- auto-refresh en EVT_BATTERY_UPDATED
label.text = "{battery}%"            -- reactivo
slider.value = bind(VOLUME, rw)      -- two-way binding
```

---

## 9. SD Card app discovery y registro dinámico

### Estructura de una SD app

```
/sdcard/apps/myapp/
    manifest.json       → metadata de la app
    main.lua            → entry point (o main.js, main.py, etc.)
    icon.bin            → (futuro) icono bitmap
    files/              → storage sandbox (creado automáticamente)
    cache/              → cache sandbox
    myapp.db            → SQLite DB (creado automáticamente)
```

### Formato de manifest.json

```json
{
    "name":        "My App",
    "icon":        "MA",
    "type":        "SCRIPT",
    "runtime":     "lua",
    "entry":       "main.lua",
    "version":     "1.0.0",
    "min_os_api":  1,
    "permissions": "WIFI,SD,NETWORK",
    "storage_dir": "myapp"
}
```

### Flujo de discovery

```
EVT_SDCARD_MOUNTED
  └─ os_app_discover_sd()
       ├─ Escanea /sdcard/apps/ buscando subdirectorios con manifest.json
       ├─ os_manifest_parse(path, &manifest)  → cJSON, max 4KB
       ├─ Asigna ID dinámico desde APP_ID_DYNAMIC_BASE (256) en adelante
       ├─ os_app_register(&manifest, NULL, NULL)  → cbs=NULL para scripts
       └─ svc_event_post(EVT_SD_APP_DISCOVERED)
            └─ Launcher actualiza grid en caliente (sin reboot)
```

### Registro dinámico en runtime

```c
// Interno al OS — no llamado por apps directamente.
// app_registry.c usa realloc dinámico (crece de a REGISTRY_GROW_BY=8).
// Lookup O(n) — aceptable para <32 apps.
// app_registry_get_raw: retorna entradas con available=false (para mostrar stubs).
```

### Script runtime vtable

```c
// Definido en os_script.h — ningún intérprete conectado todavía:
typedef struct {
    const char *name;       // "lua", "quickjs", "micropython"
    const char *file_ext;   // ".lua", ".js", ".py"
    esp_err_t (*load)(void *script_data, size_t size);
    esp_err_t (*call)(const char *fn_name, ...);
    void      (*unload)(void);
} script_runtime_t;

esp_err_t os_script_register_runtime(const script_runtime_t *rt);
const script_runtime_t *os_script_get_runtime(const char *name);
uint8_t os_script_runtime_count(void);
```

Este es el punto de extensión exacto para el micro-intérprete. El vtable define el contrato: el OS llama `load` con el script compilado o fuente, `call` para cada callback del ciclo de vida, y `unload` al destruir.

---

## 10. HTTP/HTTPS API Client

### Estado actual

Solo existe `svc_downloader` para descarga de archivos a SD con resume. No hay HTTP client de propósito general para apps.

Apps que necesitan hacer requests (Bluesky, APIs REST, etc.) deben usar `esp_http_client` de ESP-IDF directamente — tipo no expuesto en el API de app.

### Propuesta: svc_http — HTTP Client como servicio compartido

Servicio instanciable por apps vía **session context** que encapsula base URL, auth, cookies, y retry policy.

#### Tipos

```c
typedef struct svc_http_session svc_http_session_t;  // opaque

typedef enum {
    HTTP_AUTH_NONE,
    HTTP_AUTH_BEARER,     // Authorization: Bearer {token}
    HTTP_AUTH_BASIC,      // Authorization: Basic base64({user}:{pass})
    HTTP_AUTH_API_KEY,    // header configurable + valor
    HTTP_AUTH_CUSTOM,     // header_name + header_value libre
} http_auth_type_t;

typedef struct {
    int      status;           // HTTP status code
    bool     ok;               // status 200-299
    char    *body;             // heap-allocated, NULL-terminated, owned por response
    size_t   body_len;
    cJSON   *json;             // parsed si Content-Type: application/json, owned por response
    char     content_type[64];
    char     error_msg[128];   // descripción de error de red o HTTP
} http_response_t;

typedef void (*http_response_cb_t)(const http_response_t *r, void *ctx);

typedef enum {
    HTTP_ERR_OK = 0,
    HTTP_ERR_NO_WIFI,       // WiFi no conectado
    HTTP_ERR_DNS,           // fallo DNS
    HTTP_ERR_CONNECT,       // TCP refused / timeout
    HTTP_ERR_TLS,           // TLS handshake failed
    HTTP_ERR_TIMEOUT,       // response timeout
    HTTP_ERR_STATUS_4XX,    // error del cliente (r->status disponible)
    HTTP_ERR_STATUS_5XX,    // error del servidor
    HTTP_ERR_PARSE,         // JSON no parseable
    HTTP_ERR_OOM,           // out of memory al leer body
} http_err_t;

typedef struct {
    uint8_t  request_id;
    uint32_t bytes_received;
    uint32_t bytes_total;
    bool     complete;
    bool     error;
    char     dest_path[128];
} http_download_progress_t;
```

#### Lifecycle de session

```c
// Crear (en on_launch):
svc_http_session_t *svc_http_session_create(app_id_t owner, const char *base_url);
// Auto-destruida (requests cancelados, memoria liberada) al cerrar la app.
// base_url: "https://bsky.social" — sin trailing slash

void svc_http_session_destroy(svc_http_session_t *s);
```

#### Autenticación

```c
void svc_http_session_set_auth_bearer(svc_http_session_t *s, const char *token);
void svc_http_session_set_auth_basic(svc_http_session_t *s, const char *user, const char *pass);
void svc_http_session_set_auth_api_key(svc_http_session_t *s,
                                        const char *header_name, const char *key);
void svc_http_session_set_header(svc_http_session_t *s, const char *name, const char *value);

// Hook para refresh automático de token en respuesta 401:
typedef esp_err_t (*http_refresh_fn_t)(svc_http_session_t *s, void *ctx);
void svc_http_session_set_refresh_hook(svc_http_session_t *s, http_refresh_fn_t fn, void *ctx);
```

#### Requests síncronos (desde task de background — NO desde LVGL task)

```c
esp_err_t svc_http_get(svc_http_session_t *s, const char *path,
                        const char *query,   // "limit=20&cursor=abc" o NULL
                        http_response_t *out);

esp_err_t svc_http_post_json(svc_http_session_t *s, const char *path,
                              cJSON *body, http_response_t *out);

esp_err_t svc_http_post_form(svc_http_session_t *s, const char *path,
                              const char *form_body,   // "key=val&key2=val2"
                              http_response_t *out);

void svc_http_response_free(http_response_t *r);    // free(body) + cJSON_Delete(json)

http_err_t  svc_http_last_error(svc_http_session_t *s);
const char *svc_http_err_str(http_err_t err);
```

#### Requests asíncronos (callback en LVGL task — seguro para actualizar widgets)

```c
uint8_t svc_http_get_async(svc_http_session_t *s, const char *path,
                            const char *query,
                            http_response_cb_t cb, void *ctx);

uint8_t svc_http_post_json_async(svc_http_session_t *s, const char *path,
                                  cJSON *body,       // COPIADO internamente
                                  http_response_cb_t cb, void *ctx);

void svc_http_cancel(uint8_t request_id);
```

#### Descarga progresiva a SD Card

```c
uint8_t svc_http_download(svc_http_session_t *s, const char *path,
                           const char *dest_path,   // "/sdcard/apps/myapp/files/img.jpg"
                           bool resume);             // resume si existe parcial
// Progress: EVT_DOWNLOAD_PROGRESS (http_download_progress_t payload)
// Complete: EVT_DOWNLOAD_COMPLETE
// Error:    EVT_DOWNLOAD_ERROR
```

#### Parsing tipado de JSON response

```c
// Navega el JSON de la response por path "user.profile.handle":
bool    svc_http_json_str(const http_response_t *r, const char *path,
                          char *buf, size_t len);
int64_t svc_http_json_int(const http_response_t *r, const char *path, int64_t def);
double  svc_http_json_float(const http_response_t *r, const char *path, double def);
bool    svc_http_json_bool(const http_response_t *r, const char *path, bool def);
cJSON  *svc_http_json_array(const http_response_t *r, const char *path);  // no owned
```

#### Ejemplo de uso completo (app Bluesky)

```c
// on_launch:
static void bsky_on_launch(app_id_t id, void *data) {
    bsky_state_t *s = calloc(1, sizeof(*s));
    s->session = svc_http_session_create(id, "https://bsky.social");

    char token[256] = {0};
    os_app_nvs_get_str(id, "access_token", token, sizeof(token));
    if (token[0]) svc_http_session_set_auth_bearer(s->session, token);
    svc_http_session_set_refresh_hook(s->session, bsky_refresh_token, s);
    // ...
}

// on_create de feed screen:
static void *bsky_feed_create(lv_obj_t *screen, const view_args_t *args, void *app_data) {
    bsky_state_t *s = app_data;
    feed_view_t *v = calloc(1, sizeof(*v));
    v->list = ui_common_list(ui_common_content_area(screen));

    svc_http_get_async(s->session, "/xrpc/app.bsky.feed.getTimeline",
                       "limit=20", on_feed_response, v);
    ui_effect_loading(true);
    return v;
}

// Callback en LVGL task — seguro para widgets:
static void on_feed_response(const http_response_t *r, void *ctx) {
    feed_view_t *v = ctx;
    ui_effect_loading(false);

    if (!r->ok) {
        ui_effect_toast(r->error_msg, 2000);
        return;
    }

    cJSON *feed = svc_http_json_array(r, "feed");
    cJSON *item;
    cJSON_ArrayForEach(item, feed) {
        char author[64], text[256];
        // (navegar item directamente con cJSON aquí, no por path global)
        ui_common_list_add_two_line(v->list, author, text, v->count++, on_tap, NULL);
    }
}

// on_terminate:
static void bsky_on_terminate(app_id_t id, void *app_data) {
    bsky_state_t *s = app_data;
    svc_http_session_destroy(s->session);   // cancela requests pendientes
    free(s);
}
```

#### Notas de implementación interna

```
Worker tasks:    2-4 tasks en Core 0 (pool compartido entre todas las sessions)
Queue:           xQueue de requests (url, method, body_copy, session*, cb, ctx)
TLS:             esp_http_client con CONFIG_ESP_TLS_USING_MBEDTLS
Body buffering:  PSRAM para bodies >8KB, stack interno para pequeños
Cookie jar:      Tabla hash por session (name → value + expiry)
Rate limiting:   Por session, configurable (req/s), cola interna
Cancellation:    Flag por request_id, chequeado entre chunks
Auto-cleanup:    svc_http_cancel_all_for_app(app_id) en on_app_closed
```

---

## 11. NVS privado por app

### Estado actual

No existe. El único NVS es el global `svc_settings` (namespace `"cyberdeck"`), compartido entre el OS y todas las apps. No hay aislamiento por `app_id`.

### Propuesta: os_app_nvs

NVS privado por app usando namespace `"a{id}"` (ej: `"a256"`, `"a9"`). Límite de NVS: namespace máx 15 chars, key máx 15 chars.

```c
// Abrir (llamado automáticamente al primer uso, no requiere init explícito):
esp_err_t os_app_nvs_get_str(app_id_t id, const char *key, char *buf, size_t len);
esp_err_t os_app_nvs_set_str(app_id_t id, const char *key, const char *val);

esp_err_t os_app_nvs_get_i32(app_id_t id, const char *key, int32_t *out);
esp_err_t os_app_nvs_set_i32(app_id_t id, const char *key, int32_t val);

esp_err_t os_app_nvs_get_blob(app_id_t id, const char *key, void *buf, size_t *len);
esp_err_t os_app_nvs_set_blob(app_id_t id, const char *key, const void *data, size_t len);

esp_err_t os_app_nvs_erase_key(app_id_t id, const char *key);
esp_err_t os_app_nvs_erase_all(app_id_t id);   // al desinstalar la app
```

### Casos de uso que requieren NVS privado

- Access tokens y refresh tokens de APIs (no deben estar en SD — SD puede desmontarse)
- Preferencias pequeñas de la app (último tab visto, orden de lista, etc.)
- Último cursor/paginación de un feed
- Estado de onboarding (¿ya mostró el tutorial?)

### Alternativa si NVS es insuficiente

Para apps que necesitan storage estructurado sin SD, el OS podría reservar una partición NVS extra (`nvs_apps`) de 256KB con namespaces por app. Requiere modificación del `partitions.csv`.

---

## 12. API surface: limpia vs gotchas vs faltante

### Ya es API limpia — el intérprete puede bindear directamente

```c
// Lifecycle:
os_app_register(manifest, ops, cbs)
os_view_push / os_view_pop / os_view_pop_to_root / os_view_home

// Eventos:
os_event_subscribe_ui(owner, evt_id, handler, ctx) → event_sub_t
os_event_unsubscribe(sub)

// Background:
os_task_create(cfg, &handle)
os_defer(fn, arg, delay_ms)
os_poller_register(name, fn, arg, interval_ms, owner)
os_ui_post(fn, arg)

// Estado del dispositivo:
app_state_get()   // → cyberdeck_state_t* (WiFi, batería, audio, SD, BT)

// WiFi (operaciones):
svc_wifi_connect(ssid, pass)
svc_wifi_disconnect()
svc_wifi_start_scan()
svc_wifi_is_connected()

// OTA:
svc_ota_start(url)

// UI feedback:
ui_effect_toast(msg, ms)
ui_effect_confirm(title, msg, cb, ctx)
ui_effect_loading(show)
ui_effect_progress_show / set / hide

// Widgets:
ui_common_content_area / panel / card
ui_common_list / list_add / list_add_two_line
ui_common_grid / grid_cell
ui_common_btn / btn_full / btn_style_primary
ui_common_action_row / data_row / section_gap / spacer / divider
ui_keyboard_show / hide

// Theme:
ui_theme_get() / ui_theme_apply(id)

// Storage:
os_app_storage_open / close / reopen / clear_cache
os_app_storage_path
os_storage_read / write / fopen / exists / delete

// SQLite:
os_db_run / os_db_get_int / os_db_last_rowid

// Settings del OS:
os_settings_get()
os_settings_set_brightness / theme / rotation / volume / etc.
```

### Requieren wrappers antes de exponer al intérprete

| Gotcha | Problema | Solución |
|---|---|---|
| `wifi_ap_record_t` (ESP-IDF struct) | Apps reciben array de tipo ESP-IDF con campos `.ssid`, `.authmode`, `.rssi` | Wrap en `cyber_ap_t { char ssid[33]; int8_t rssi; uint8_t channel; bool secured; }` |
| IP address | No está en `app_state_get()` | Añadir `wifi_ip[16]` al snapshot `cyberdeck_state_t` |
| `esp_event_base_t` en handler signature | Tipo ESP-IDF visible en la firma del callback | `typedef void (*cyber_event_handler_t)(int32_t evt_id, void *data, void *ctx)` |
| Settings dual-path | `os_settings_set_*` vs `svc_settings_*` vs `svc_event_post` manual | Unificar todo bajo `os_settings_set_*` con post automático de eventos |
| `lv_obj_t *` en callbacks | Tipo LVGL expuesto en `on_create`, `on_resume`, etc. | Handle opaco `cyber_screen_t` que el intérprete nunca dereference |
| `view_args_t.owned` flag | El intérprete no puede gestionar si llama `free()` o no | Framework siempre libera `data` — el intérprete nunca llama `free` en intent data |
| `wifi_auth_mode_t` → string | Apps convierten enum ESP-IDF a string manualmente | `svc_wifi_auth_str(mode)` → `"WPA2"` etc. |
| `lv_mem_alloc` para ctx de callback | Apps usan allocator LVGL para guardar contexto por item | `ui_common_list_add` ya acepta `void *data` — el intérprete pasa un handle numérico |
| Guard async manual (global `*state`) | Race condition entre evento queued y view destruida | Guard automático en `os_event_subscribe_ui`: cancelar callback si la view fue destruida |
| `configMAX_TASK_NAME_LEN` / `UBaseType_t` | Constantes y tipos FreeRTOS en `os_task_config_t` | Macros `OS_PRIO_*` ya existen — eliminar raw types del struct público |

### No existe — debe construirse

| Qué | Por qué es crítico | Prioridad |
|---|---|---|
| `svc_http` (session + auth + async + JSON) | Toda app de red moderna lo necesita | 🔴 Alta |
| `os_app_nvs` (NVS privado por app) | Tokens y prefs sin depender de SD | 🔴 Alta |
| Wrappers WiFi scan + IP en snapshot | Toda app de red necesita IP y lista de APs | 🔴 Alta |
| Guard automático en `subscribe_ui` | Sin esto el intérprete tiene race conditions por default | 🔴 Alta |
| `ui_common_slider / checkbox / switch / dropdown` | Sin controles interactivos las settings screens son imposibles | 🟡 Media |
| `ui_common_input / textarea` | Apps crean `lv_textarea` directo hoy | 🟡 Media |
| `ui_common_tabs / segmented` | Navegación compuesta en apps de contenido | 🟡 Media |
| Unificación de settings setters | Consistencia del API | 🟡 Media |
| Reactive bindings (`os_bind_label`) | Reduce 80% del boilerplate de update UI | 🟢 Baja |
| `ui_common_chart / sparkline / meter` | Visualización de datos en tiempo real | 🟢 Baja |
| Script runtime implementado (Lua/QuickJS) | El intérprete en sí | 🔵 Siguiente fase |

---

## 13. Hoja de ruta para el micro-intérprete

### Capa de abstracción objetivo

```
┌─────────────────────────────────────────────────────────────────┐
│            INTÉRPRETE / DSL  (Lua / QuickJS / propio)           │
│            apps en /sdcard/apps/{id}/main.{ext}                 │
└──────────────────────────────────────────┬──────────────────────┘
                                           │ bindings C
┌──────────────────────────────────────────▼──────────────────────┐
│                     CYBER SDK  (capa a completar)               │
│                                                                 │
│  cyber_wifi_t      — sin wifi_ap_record_t                      │
│  cyber_ui_*        — sin lv_obj_t directo                       │
│  cyber_event_t     — sin esp_event_base_t                       │
│  cyber_state_t     — snapshot completo (+ IP)                   │
│  cyber_http_t      — svc_http sessions                          │
│  cyber_db_*        — SQLite + query builder                     │
│  cyber_nvs_*       — os_app_nvs                                 │
│  cyber_task_t      — sin tipos FreeRTOS                         │
│  os_settings_*     — solo os_settings_set_* unificado           │
└──────────────────────────────────────────┬──────────────────────┘
                                           │ ya existe
┌──────────────────────────────────────────▼──────────────────────┐
│   app_framework / ui_engine / os_core / sys_services / board    │
│         [No tocar desde apps — solo vía Cyber SDK]              │
└─────────────────────────────────────────────────────────────────┘
```

### Fases de construcción

**Fase 1 — Cerrar gotchas del API existente**
1. Wrapper `cyber_ap_t` para WiFi scan results
2. Añadir `wifi_ip[16]` a `cyberdeck_state_t`
3. Typedef `cyber_event_handler_t` sin `esp_event_base_t`
4. Unificar settings bajo `os_settings_set_*`
5. Guard automático en `os_event_subscribe_ui`

**Fase 2 — Nuevos servicios**
6. `os_app_nvs` — NVS privado por app
7. `svc_http` — HTTP client con session/auth/async/JSON/download

**Fase 3 — Completar UI**
8. `ui_common_slider / checkbox / switch / dropdown`
9. `ui_common_input / textarea` (reemplaza `lv_textarea` directo)
10. `ui_common_tabs / segmented`

**Fase 4 — Intérprete**
11. Implementar `script_runtime_t` para Lua o QuickJS
12. Bindear Cyber SDK al runtime
13. Conectar vtable en `os_script_register_runtime`
14. Loader: `on_launch` → `rt->load(script_data, size)`, `on_create` → `rt->call("on_create", screen_handle, args)`, etc.

**Fase 5 — DX del DSL**
15. Reactive bindings (`os_bind_label`, `os_bind_value`)
16. Form builder declarativo
17. Hot-reload de scripts desde SD sin reboot
