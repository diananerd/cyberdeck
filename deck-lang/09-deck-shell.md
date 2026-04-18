# Deck OS Shell Integration
**Version 2.0 — Launcher, Navigation, Lifecycle, and System Events**

> **Implementation references**: this doc describes the *abstract shell model* — what a launcher / lockscreen / task manager are, how the OS app stack behaves, how memory pressure cascades, how background tasks resume suspended VMs. The *abstract platform contract* (which services exist, their C vtables, their error vocabularies) lives in `12-deck-service-drivers.md`. The *concrete numbers and ESP-IDF bindings* (FreeRTOS task layout, memory pressure thresholds in KB, eviction algorithm, ESP-IDF event mapping, the list of svc_* modules, OTA dual track) live in `13-deck-cyberdeck-platform.md §5, §9, §10, §14`. The *runtime-internal mechanics* of snapshot/restore, mailbox IPC, panic isolation, and reactive cascade live in `11-deck-implementation.md §14, §16, §17, §19`.

---

## 1. El Modelo Fundamental

Deck no corre *sobre* un OS con su propio modelo de apps. Deck *es* el modelo de apps del OS. Esto tiene una consecuencia central:

**El launcher es una app Deck.** El task manager es una app Deck. El lock screen es una app Deck. El sistema de notificaciones es una app Deck. Todos son ciudadanos de primera clase del mismo runtime, con la única diferencia de que tienen acceso a capabilities del sistema (`system.shell`, `system.apps`) que las apps de usuario no pueden declarar en `@use`.

El runtime gestiona exactamente **una instancia de app activa en pantalla** en un momento dado, con un stack de apps suspendidas. Esto es idéntico al modelo de una sola pantalla en embedded, pero extendido a nivel del OS.

```
┌─────────────────────────────────────────────────┐
│              Display (OS Bridge)                │
├─────────────────────────────────────────────────┤
│         App activa (foreground)                 │
│         su @flow de entrada, sus @machine       │
├─────────────────────────────────────────────────┤
│  App suspendida  │  App suspendida  │   ...      │
├─────────────────────────────────────────────────┤
│   Launcher (siempre vivo, nunca destruido)      │
├─────────────────────────────────────────────────┤
│              Deck Runtime                       │
│   Un runtime, múltiples DeckVM instancias       │
│   Una por app (aisladas, ver §4)                │
└─────────────────────────────────────────────────┘
```

---

## 2. El Stack de Apps del OS

Distinto del `@flow` de entrada de una app individual, el OS mantiene su propio stack de apps:

```
OS App Stack (top = foreground):
  [3]  Notes.app         (activa, en pantalla)
  [2]  Bluesky.app       (suspendida)
  [1]  Reader.app        (suspendida)
  [0]  Launcher.app      (base, siempre presente)
```

### 2.1 Reglas del OS App Stack

- El **Launcher siempre está en la base** (índice 0). Nunca se destruye, nunca se suspende completamente — mantiene su estado aunque esté tapado.
- Cada app ocupa **exactamente una posición** en el stack. Lanzar una app que ya está suspendida la mueve al top (no duplica).
- El OS no tiene un límite fijo de apps suspendidas, pero sí un presupuesto de memoria. Cuando la presión de memoria es alta, el OS puede destruir apps suspendidas (las más antiguas primero), disparando `@on terminate` en cada una.
- **Una app no puede manipular el OS app stack directamente.** Solo puede pedir al OS que realice operaciones a través de capabilities privilegiadas o eventos declarados.

---

## 3. El Sistema de Botones y Gestos

### 3.1 Back

El botón/gesto de back tiene una semántica de dos niveles:

**Nivel 1 — Dentro de la app (primero):**
El runtime verifica si el `@flow` de entrada de la app activa tiene history disponible (ha visitado estados distintos al `initial` via transiciones `to history`). Si sí, el runtime dispara la transición `to history` — la app consume el evento, el OS nunca lo ve. El bridge restaura el contexto visual anterior.

```
@flow Nav de Notes.app — estados visitados:
  [2]  :reader    ← activo
  [1]  :editor
  [0]  :list      ← initial (root)

Back #1 → flow revierte a :editor  (to history, dentro de la app)
Back #2 → flow revierte a :list    (to history, dentro de la app)
Back #3 → OS nivel 2  (flow está en :list = initial, no hay más history)
```

**Nivel 2 — App en su root:**
Cuando la app está en el estado initial de su `@flow` de entrada, el runtime evalúa en este orden:

1. ¿La app declaró `@on back`? → ejecuta ese hook; si retorna `:handled`, el OS no hace nada más
2. ¿La app declaró `@on back` y retorna `:unhandled`? → continúa al paso 3
3. Sin `@on back`: el OS suspende la app y muestra la app anterior en el stack (o el Launcher si no hay ninguna)

```deck
-- En app.deck, declarar comportamiento del back en root:
@on back
  match unsaved_changes()
    | true  ->
        :confirm
          prompt: "¿Descartar cambios?"
          confirm: "Salir"     -> :unhandled
          cancel:  "Cancelar"  -> :handled
    | false -> :unhandled
```

`@on back` puede retornar:
- `:handled` — la app consumió el evento; el OS no hace nada
- `:unhandled` — la app delega al OS; el OS suspende la app
- `:confirm { prompt, confirm, cancel }` — el OS muestra el diálogo nativo (no la app — evita el bug de diálogos que quedan huérfanos). `confirm` y `cancel` llevan `-> atom` indicando qué retornar al OS según la elección del usuario.

### 3.2 Home

Siempre manejado por el OS. La app activa recibe `@on suspend` y el OS muestra el Launcher. No hay forma de interceptar Home desde una app — es una garantía de diseño. El usuario siempre puede salir de cualquier app.

```
Home button
  → @on suspend se dispara en la app activa
  → La app va al OS App Stack como suspendida
  → Launcher sube al foreground
  → Launcher recibe @on resume
```

### 3.3 Task Manager (App Switcher)

El Task Manager es una app Deck privilegiada (`system.shell` capability). Se activa con un gesto largo en Home o un botón dedicado, dependiendo del `.deck-os` del board.

Cuando se activa:
1. La app activa recibe `@on suspend` si no estaba ya mostrando el task manager
2. El OS captura screenshots de las apps suspendidas (via `deck_bridge_capture_thumbnail`)
3. El Task Manager recibe el control

El Task Manager puede:
- Mostrar las apps suspendidas con sus thumbnails
- Llevar una app al foreground (`system.apps.bring_to_front(app_id)`)
- Matar una app (`system.apps.kill(app_id)`)
- Lanzar el Launcher

```deck
-- Launcher.app / TaskManager.app tienen acceso a:
@use
  system.apps   as apps
  system.shell  as shell

-- system.apps API:
apps.running ()               -> [AppInfo]
apps.suspended ()             -> [AppInfo]
apps.installed ()             -> [AppInfo]
apps.search (query: str)      -> [AppInfo]   -- busca por nombre/id entre installed()
apps.bring_to_front (id: str) -> unit
apps.launch (id: str)         -> Result unit system.Error
apps.launch_url (id: str, url: str) -> Result unit system.Error
apps.kill   (id: str)         -> unit

@type AppInfo
  id          : str
  name        : str
  version     : str
  icon        : str?
  thumbnail   : [byte]?     -- PNG capturado al suspender
  suspended_at: Timestamp?
  is_launcher : bool
```

---

## 4. Aislamiento de VM por App

Cada app que se lanza obtiene su propia instancia de `DeckVM`. El runtime principal gestiona el ciclo de vida de estas instancias.

```c
/* El runtime principal mantiene: */
typedef struct DeckOSRuntime {
    DeckVMInstance*  foreground;        /* app activa */
    DeckVMInstance** suspended;         /* array de VMs suspendidas */
    size_t           suspended_count;
    DeckVMInstance*  launcher;          /* siempre vivo */
    DeckOSConfig*    config;
    DeckBridge*      bridge;
} DeckOSRuntime;

typedef struct DeckVMInstance {
    DeckVM*      vm;
    char*        app_id;
    char*        app_path;          /* directorio del app */
    DeckVMState  state;             /* RUNNING, SUSPENDED, LOADING, DEAD */
    Instr*       ip_snapshot;       /* IP guardado al suspender */
    DeckFrame*   frame_snapshot;    /* frame stack guardado */
    uint8_t*     heap_snapshot;     /* heap copiado a PSRAM al suspender */
    size_t       heap_size;
    Timestamp    suspended_at;
    uint8_t*     thumbnail;         /* PNG capturado al suspender */
    size_t       thumbnail_size;
    /* Main tasks hold their MSG_TASK_RUN messages here while suspended;
       re-enqueued to the runtime queue when the VM is next foregrounded. */
    QueueHandle_t held_task_msgs;   /* FreeRTOS queue, capacity = task count */
} DeckVMInstance;
```

### 4.1 Launch

```
deck_os_launch(rt, app_path):
  1. Crear nueva DeckVMInstance
  2. Cargar y verificar la app (Loader stages 1-12)
     → Si falla: mostrar error al usuario, no alterar el stack
  3. Suspender la app foreground actual:
     a. Capturar thumbnail del display actual
     b. Disparar @on suspend en la app
     c. Guardar heap snapshot en PSRAM
     d. Mover al suspended array
  4. La nueva instancia pasa a foreground
  5. Ejecutar @on launch de la nueva app
  6. Renderizar su root view
```

### 4.2 Suspend y Resume

```
deck_os_suspend(rt, instance):
  1. Disparar @on suspend
  2. Capturar screenshot para thumbnail
  3. Serializar heap de la VM a PSRAM
     → Solo los objetos vivos (reachable desde el value stack)
     → Objetos con refcount == 0 se descartan
  4. Guardar frame stack y IP
  5. Estado → SUSPENDED

deck_os_resume(rt, instance):
  1. Restaurar heap desde PSRAM
  2. Restaurar frame stack y IP
  3. Disparar @on resume
  4. Re-renderizar view actual (el display puede haber cambiado)
  5. Estado → RUNNING
```

### 4.3 Terminate

```
deck_os_terminate(rt, instance, reason):
  1. Disparar @on terminate (con tiempo límite: 500ms)
  2. Si reason == :memory_pressure:
     → Destruir heap snapshot en PSRAM
     → Registrar que la app fue terminada por presión de memoria
     → Próximo launch de esta app → @on launch normal (no resume)
  3. Si reason == :user_kill (desde task manager):
     → Mismo que :memory_pressure
  4. Si reason == :crash:
     → Capturar stack trace del crash
     → Reportar via system.crash_reporter si está disponible
     → OS muestra mensaje de error nativo
  5. Destruir DeckVMInstance
  6. Liberar toda la memoria asociada
```

---

## 5. Integración del @machine de Navegación con el Sistema

El `@machine` de navegación de una app y el OS App Stack son independientes pero se comunican de forma precisa.

### 5.1 El Back Button — Implementación en el Bridge

```c
void deck_os_on_back_button(DeckOSRuntime* rt) {
    DeckVMInstance* app = rt->foreground;

    /* Paso 1: ¿Puede la app consumir el back internamente? */
    if (deck_vm_nav_can_go_back(app->vm)) {
        /* El @flow de entrada de la app tiene history > root */
        deck_vm_nav_go_back(app->vm);
        return;
    }

    /* Paso 2: ¿La app tiene @on back? */
    if (deck_vm_has_hook(app->vm, "back")) {
        DeckValue* result = deck_vm_fire_hook(app->vm, "back");

        if (deck_is_atom(result) && strcmp(deck_get_atom(result), "handled") == 0) {
            return;  /* app consumió el evento */
        }

        if (deck_is_record(result) &&
            strcmp(deck_record_type(result), "confirm") == 0) {
            /* El OS muestra el diálogo, no la app */
            deck_os_show_confirm_dialog(rt, result, app);
            return;
        }
        /* :unhandled → cae al paso 3 */
    }

    /* Paso 3: Suspender la app y volver a la anterior */
    deck_os_return_to_previous(rt);
}

static void deck_os_return_to_previous(DeckOSRuntime* rt) {
    DeckVMInstance* leaving = rt->foreground;
    deck_os_suspend(rt, leaving);

    /* La app anterior en el stack, o el launcher */
    DeckVMInstance* returning = rt->suspended_count > 0
        ? rt->suspended[rt->suspended_count - 1]
        : rt->launcher;

    rt->foreground = returning;
    rt->suspended_count = returning == rt->launcher
        ? 0 : rt->suspended_count - 1;

    deck_os_resume(rt, returning);
}
```

### 5.2 Estados "Modal" y el Back Button

En el modelo `@flow`, no existe la keyword `modal:`. Cualquier estado que el bridge presente visualmente como un modal (sheet, overlay, diálogo) es simplemente un estado del `@flow` de la app. La transición `to history` lo revierte igual que cualquier otro estado.

```
@flow Nav de la app — estado :compose activo (bridge lo presenta como modal):
  history: [:timeline]    ← estados visitados antes de :compose

Back → runtime emite FLOW_BACK → bridge revierte :compose a :timeline
     → no suspende la app, no va al OS
```

El runtime evalúa en orden: history del flow primero, `@on back` hook después, OS al final.

### 5.3 Deep Links

Una app puede ser lanzada desde otra app o desde el sistema con una URL:

```deck
-- En app.deck, declarar URLs que la app entiende:
@handles
  "bsky://profile/{did}"
  "bsky://post/{uri}"
  "bsky://search"

-- Handler — el bridge extrae los segmentos {…} del pattern matched
-- y los entrega en `params`. Los parámetros de query string se
-- incluyen también, pero NO se matchean contra el pattern (ver
-- 02-deck-app §20.1 para la gramática completa).
@on open_url (url: str, params: {str: str})
  match params["did"]
    | :some did      -> open_profile(did)
    | :none          ->
        match params["uri"]
          | :some uri  -> open_post(uri)
          | :none      ->
              match params["q"]
                | :some q  -> open_search(q)
                | :none    -> unit
```

```deck
-- Desde otra app, lanzar con URL:
apps.launch_url("social.bsky.app", "bsky://profile/did:plc:abc")
```

Si la app ya está suspendida, se resume y recibe `@on open_url`. Si no está instalada, `apps.launch_url` retorna `:err :not_installed`.

---

## 6. El Launcher

El Launcher es una app Deck con `app.id: "system.launcher"`. Tiene acceso a las capabilities `system.apps` y `system.shell`, que apps normales no pueden declarar.

```deck
-- launcher/app.deck

@app
  name:    "Home"
  id:      "system.launcher"
  version: "1.0.0"
  entry:   Launcher

@use
  system.apps   as apps
  system.shell  as shell
  storage.local as store
  ./flows/home_flow
  ./flows/task_switcher_flow
  ./flows/search_flow

@flow Launcher
  state :home          flow: HomeFlow
  state :task_switcher flow: TaskSwitcherFlow
  state :search        flow: SearchFlow

  initial :home

  transition :show_tasks
    from :home
    to   :task_switcher

  transition :dismiss_tasks   from :task_switcher  to :home

  transition :show_search
    from :home
    to   :search

  transition :dismiss_search  from :search  to :home

@on resume
  Launcher.send(:dismiss_tasks)
  Launcher.send(:dismiss_search)
```

The app never calls anything like `shell.set_status_bar` — the statusbar and navbar are always rendered by the bridge around every screen (see `10-deck-bridge-ui §3`). Apps only declare `content =` bodies; presentation is not theirs to control.

```deck
-- launcher/flows/home_flow.deck

-- Badge counts stream: emits [(app_id, unread)] on any change.
@stream NotifCounts
  source: apps.notif_counts_watch()

@flow HomeFlow

  step :idle (counts: [(app_id: str, unread: int)] = []) ->
    content =
      list apps.installed()
        empty ->
          "NO APPS INSTALLED"
        app ->
          trigger app.name
            badge: if unread_for(counts, app.id) > 0
                     then :some unread_for(counts, app.id)
                     else :none
            -> apps.launch(app.id)

      trigger "Apps recientes" -> Launcher.send(:show_tasks)
      trigger "Buscar"         -> Launcher.send(:show_search)

  -- Stream drives badge updates without full re-render.
  on NotifCounts counts ->
    HomeFlow.send(:refresh_counts (counts: counts))

  transition :refresh_counts (counts: [(app_id: str, unread: int)])
    from :idle _
    to   :idle (counts: counts)

fn unread_for (counts: [(app_id: str, unread: int)], app_id: str) -> int =
  match first_or(filter(counts, c -> c.app_id == app_id),
                 (app_id: app_id, unread: 0))
    | (app_id: _, unread: n) -> n
```

Nothing about the launcher mentions grid vs list, card vs row, columns, icons, or fonts. The app declares: "there is a list of installed apps; each one is a trigger that, when activated, launches it; some have unread badges." The bridge decides — for this 480×800 LVGL device — to render the list as a 3-column grid of cards with icons derived from each `@app icon:` metadata and badges drawn as top-right circles; on a voice device the same declaration would expose each app as a spoken menu option. The `.deck` is identical; the bridge chooses.

```deck
-- launcher/flows/task_switcher_flow.deck

@flow TaskSwitcherFlow

  step :visible _ ->
    content =
      list (apps.running() ++ apps.suspended() |> filter(a -> not a.is_launcher))
        empty ->
          "NO RUNNING APPS"
        app ->
          group
            when app.thumbnail is :some
              media unwrap_opt(app.thumbnail)  alt: app.name  role: :thumbnail
            navigate "Open {app.name}" -> do
                apps.bring_to_front(app.id)
                Launcher.send(:dismiss_tasks)
            confirm "Close {app.name}"  prompt: "Close {app.name}?"
              -> apps.kill(app.id)
```

```deck
-- launcher/flows/search_flow.deck

@flow SearchFlow

  step :active (query: str = "") ->
    content =
      search :query  value: query  hint: "Buscar apps"
        on -> SearchFlow.send(:update (query: event.value))

      list apps.search(query)
        empty ->
          "NO MATCHES"
        app ->
          trigger app.name -> apps.launch(app.id)

      trigger "Cancelar" -> Launcher.send(:dismiss_search)

  transition :update (query: str)
    from :active _
    to   :active (query: query)
```

---

## 6.1 El Task Manager

El Task Manager es la segunda app del sistema, con `app.id: "system.taskmanager"`. Siempre está instalada — es la única app que puede mostrar el árbol de procesos y forzar cierres. Se activa mediante un gesto largo en el botón Home o un botón dedicado según el `.deck-os` del board.

A diferencia del Launcher (que nunca se destruye), el Task Manager sí puede ser suspendido por presión de memoria — aunque el OS lo prioriza sobre apps de usuario.

```deck
-- taskmanager/app.deck

@app
  name:    "Task Manager"
  id:      "system.taskmanager"
  version: "1.0.0"
  entry:   TaskManager

@use
  system.apps   as apps
  system.tasks  as tasks
  system.shell  as shell

@stream ProcessSnapshot
  source: tasks.cpu_watch()

@flow TaskManager
  state :process_list  flow: ProcessListFlow
  state :app_detail    flow: AppDetailFlow

  initial :process_list

  transition :show_detail (app_id: str)
    from :process_list
    to   :app_detail (app_id: app_id)

  transition :back
    from :app_detail _
    to   history
```

```deck
-- taskmanager/flows/process_list_flow.deck

@flow ProcessListFlow

  step :idle _ ->
    content =
      list main_processes()
        empty ->
          "NO PROCESSES"
        p ->
          group "{p.app_id}"
            p
            list background_tasks_of(p.app_id)
              bg ->
                group
                  bg
                  confirm "Stop task"  prompt: "Cancelar {unwrap_opt(bg.task_name)}?"
                    -> tasks.kill_task(bg.app_id, unwrap_opt(bg.task_name))
            when not p.app_id == "system.launcher"
              navigate "Ver detalle" -> TaskManager.send(:show_detail, app_id: p.app_id)
              confirm "Kill {p.app_id}"  prompt: "Forzar cierre de {p.app_id}?"
                -> tasks.kill(p.app_id)

fn main_processes () =
  ProcessSnapshot.last() |> unwrap_opt_or([])
                         |> filter(p -> p.kind is :main)

fn background_tasks_of (app_id: str) =
  ProcessSnapshot.last() |> unwrap_opt_or([])
                         |> filter(bg -> bg.app_id == app_id
                                      and bg.kind is :background)
```

```deck
-- taskmanager/flows/app_detail_flow.deck

@flow AppDetailFlow

  step :idle (app_id: str) ->
    let storage = tasks.storage(app_id)
    content =
      group "{app_id}"
        group "Storage"
          storage
        list processes_of(app_id)
          p ->
            group
              p
              when p.kind is :background
                confirm "Stop {unwrap_opt(p.task_name)}"  prompt: "Cancelar este background task?"
                  -> tasks.kill_task(p.app_id, unwrap_opt(p.task_name))
        confirm "Kill app"  prompt: "Forzar cierre de {app_id}? Se perderán cambios no guardados."
          -> tasks.kill(app_id)

fn processes_of (app_id: str) =
  ProcessSnapshot.last() |> unwrap_opt_or([])
                         |> filter(p -> p.app_id == app_id)
```

**Protecciones del OS**: el Task Manager no puede matar al Launcher (`system.launcher`) ni a sí mismo. El bridge de `system.tasks.kill()` verifica el `app_id` y retorna `:unauthorized` si alguno de los dos es el objetivo.

---

## 7. System Shell Capability

La capability `system.shell` está disponible únicamente para apps cuyo `app.id` comienza con `system.`. El Loader verifica esto en el Stage 4 y produce un error de carga para cualquier otra app que intente usarla.

```
@capability system.shell
  -- Barra de estado
  set_status_bar      (visible: bool)          -> unit
  set_status_bar_style(style: atom)            -> unit
  -- style: :light | :dark | :auto
  status_bar_height   ()                       -> int    @pure

  -- Barra de navegación
  set_navigation_bar  (visible: bool)          -> unit
  navigation_bar_height()                      -> int    @pure

  -- Brillo y pantalla
  set_brightness      (level: float)           -> unit   -- 0.0..1.0
  get_brightness      ()                       -> float  @pure
  set_always_on       (on: bool)               -> unit
  screen_timeout      ()                       -> Duration @pure
  set_screen_timeout  (d: Duration)            -> unit

  -- Info del sistema
  battery_level       ()                       -> int    @pure
  battery_charging    ()                       -> bool   @pure
  wifi_connected      ()                       -> bool   @pure
  wifi_ssid           ()                       -> str?   @pure
  bluetooth_on        ()                       -> bool   @pure
  storage_available   ()                       -> int    @pure  -- bytes

  -- Notificaciones del sistema
  post_notification   (opts: SysNotifOpts)     -> unit
  clear_notification  (id: str)                -> unit
  clear_all_notifications ()                   -> unit

  -- Gestión del OS app stack (solo launcher)
  push_app            (id: str)                -> unit
  pop_to_launcher     ()                       -> unit

  -- Crash reporter
  report_crash        (info: CrashInfo)        -> unit

  @errors
    :unauthorized  "Only system apps can use this capability"

@type SysNotifOpts
  id       : str
  title    : str
  message  : str
  app_id   : str
  icon     : str?
  url      : str?     -- deep link al tocar
  expires  : Duration?

@type CrashInfo
  app_id    : str
  message   : str
  stack     : str
  occurred  : Timestamp
```

### 7.1 system.apps — App Lifecycle Capability

```
@capability system.apps
  running         ()                          -> [AppInfo]
  suspended       ()                          -> [AppInfo]
  installed       ()                          -> [AppInfo]
  search          (query: str)                -> [AppInfo]
  bring_to_front  (id: str)                   -> unit
  launch          (id: str)                   -> Result unit system.Error
  launch_url      (id: str, url: str)         -> Result unit system.Error
  kill            (id: str)                   -> unit
  notif_counts_watch ()                       -> Stream [(app_id: str, unread: int)]
  -- Stream que emite un snapshot completo cada vez que cambia el unread count de cualquier app.
  -- El Launcher lo usa para renderizar badges sobre los iconos.

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
```

### 7.2 system.tasks — Process Monitor Capability

```
@capability system.tasks
  tree          ()                              -> [ProcessEntry]
  kill          (app_id: str)                   -> unit
  kill_task     (app_id: str, task_name: str)   -> unit
  storage       (app_id: str)                   -> StorageInfo
  cpu_watch     ()                              -> Stream [ProcessEntry]
  -- cpu_watch emits an updated snapshot every 5 s.

  @errors
    :not_found     "App or task not found"
    :unauthorized  "Only system apps can use this capability"

@type ProcessEntry
  id         : str          -- "bsky.app" | "bsky.app:SyncPosts"
  app_id     : str
  kind       : :main | :background
  task_name  : str?         -- nil for :main
  state      : ProcessState
  heap_kb    : int
  cpu_pct    : float        -- rolling 5 s average (0.0–100.0)
  uptime_ms  : int

@type ProcessState = :running | :suspended | :waiting_effect | :idle | :dead

@type StorageInfo
  local_kb   : int
  db_kb      : int
  nvs_bytes  : int
```

`system.tasks` is available only to apps whose `app.id` starts with `system.`. The Loader rejects any other app that declares it in `@use`.

---

## 8. Flujos Completos Sin Bugs

Estos son los flujos que históricamente producen bugs en plataformas mal diseñadas. Cómo los maneja Deck:

### 8.1 App A abre App B, usuario presiona Back en B

```
Estado inicial:
  OS stack: [Launcher, AppA(root:ListView)]

AppA lanza AppB:
  AppA recibe @on suspend
  OS stack: [Launcher, AppA(suspended), AppB(active)]
  AppB recibe @on launch, muestra su root

Back en AppB (en su root):
  AppB no tiene @on back, o retorna :unhandled
  AppB recibe @on suspend
  AppA sale de suspended, recibe @on resume
  OS stack: [Launcher, AppA(active)]
  AppA re-renderiza su view actual
  AppB sigue en el OS stack como suspendida
  (o se destruye si la presión de memoria lo requiere)
```

**Sin bug**: AppA siempre recibe `@on resume`. El estado de AppA está intacto — sus machines, su nav, su configuración. No hay posibilidad de que AppA se reinicie por accidente.

### 8.2 App tiene un modal abierto, usuario presiona Home

```
Estado: NotesApp activa, ComposeModal abierto

Home:
  NotesApp recibe @on suspend
  El estado guardado incluye: nav machine CON el modal abierto
  Launcher recibe @on resume
  OS stack: [Launcher(active), NotesApp(suspended, modal abierto)]

Usuario vuelve a NotesApp (desde launcher o task switcher):
  NotesApp recibe @on resume
  Runtime restaura el nav machine
  ComposeModal sigue abierto exactamente donde estaba
  El texto a medias en el editor sigue ahí
```

**Sin bug**: el modal no se cierra al salir y volver. El estado se preserva exactamente.

### 8.3 Deep link llega mientras otra app está en foreground

```
Estado: NotesApp en foreground

Sistema recibe deep link: "bsky://post/at://..."
  Deck OS evalúa: ¿qué app maneja "bsky://"?
  Encuentra BlueskyApp (@handles "bsky://...")
  ¿Está BlueskyApp suspendida? Sí.

  NotesApp recibe @on suspend
  BlueskyApp sale de suspended, recibe @on resume
  BlueskyApp recibe @on open_url("bsky://post/at://...")
  BlueskyApp navega al post en su nav machine

Si BlueskyApp no está en memoria:
  Se lanza normalmente (@on launch)
  Al final de launch, recibe @on open_url
```

**Sin bug**: el deep link nunca llega a una app en estado inconsistente — siempre llega después de `@on launch` o `@on resume`, nunca durante.

### 8.4 Presión de memoria mata una app suspendida

```
Estado:
  OS stack: [Launcher, ReaderApp(suspended), NotesApp(suspended), BlueskyApp(active)]
  Memoria PSRAM casi llena

OS detecta presión de memoria:
  Candidato a destruir: ReaderApp (más antigua en el stack)
  ReaderApp recibe @on terminate (500ms deadline)
    → @on terminate puede guardar estado crítico en NVS o storage.local
  ReaderApp es destruida. Su heap snapshot se libera.
  OS stack: [Launcher, NotesApp(suspended), BlueskyApp(active)]

Usuario navega al launcher y lanza ReaderApp:
  ReaderApp se lanza desde cero (@on launch, no @on resume)
  Si ReaderApp guardó su estado en @on terminate, puede restaurarlo
  Si no, empieza limpia
```

**Sin bug**: la app nunca "desaparece silenciosamente". Siempre recibe `@on terminate` con tiempo suficiente para guardar lo que necesite. El OS nunca destruye la app activa ni el Launcher.

### 8.5 App crashea en foreground

```
BlueskyApp activa. El evaluador encuentra un runtime error no manejado
(ejemplo: unwrap de :none sin verificar, stack overflow)

DeckVM entra en estado CRASHED:
  El crash es capturado por el runtime antes de tocar el OS Bridge
  Se genera un CrashInfo con stack trace de Deck (no de C)
  BlueskyApp recibe @on terminate con reason: :crash
  El OS muestra un overlay nativo: "Bluesky dejó de responder"
  El OS retorna al app anterior en el stack (o Launcher)
  BlueskyApp es removida del OS stack

La app anterior:
  Recibe @on resume normalmente
  Nunca sabe que BlueskyApp crasheó
  El runtime nunca estuvo en estado inconsistente
```

**Sin bug**: un crash en una app no puede corromper el estado de otras apps ni del Launcher porque cada app tiene su propia DeckVM. El crash es completamente contenido.

### 8.6 Back rápido múltiple (el bug del doble-pop)

```
Usuario presiona Back muy rápido dos veces mientras
la app está navegando (transición en curso)

Diseño ingenuo: dos backs se procesan, dos pops del nav machine,
resultado inesperado.

Deck OS: el bridge encolará los eventos de back en la
event queue del runtime. El runtime procesa eventos uno por
uno, en orden, en el main loop. El segundo back se procesa
solo DESPUÉS de que el primero terminó de procesar.
Si el primer back suspendió la app, el segundo back llega
al siguiente app en el stack (correcto).
Si el primer back fue un nav back dentro de la app,
el segundo es otro nav back o suspende la app (correcto).
```

**Sin bug**: la queue serial del event loop hace esto imposible.

---

## 9. Ciclo de Vida Completo de una App — Diagrama

```
                    ┌─────────────┐
                    │  INSTALLED  │
                    └──────┬──────┘
                           │ apps.launch()
                           ▼
                    ┌─────────────┐
                    │   LOADING   │ ← Loader stages 1-12
                    └──────┬──────┘
                  ┌────────┴────────┐
            error │                 │ ok
                  ▼                 ▼
           ┌──────────┐      ┌──────────────┐
           │  ERROR   │      │   LAUNCHING  │ ← @on launch
           └──────────┘      └──────┬───────┘
                                    │
                                    ▼
                             ┌────────────┐
               ┌────────────►│  RUNNING   │◄───────────┐
               │             └─────┬──────┘            │
               │ @on resume        │                   │ @on resume
               │                   │ Home / Back       │
               │                   │ otro app          │
               │                   ▼                   │
               │             ┌────────────┐            │
               │             │ SUSPENDING │            │
               │             │ @on suspend│            │
               │             └─────┬──────┘            │
               │                   │                   │
               │                   ▼                   │
               └──────────── ┌────────────┐ ───────────┘
                             │ SUSPENDED  │
                             └─────┬──────┘
                            ┌──────┴───────┐
               memory OK    │              │ memory pressure /
               user kill    │              │ user kill desde tasks
                            ▼              ▼
                    ┌─────────────┐   ┌─────────────┐
                    │ TERMINATING │   │ TERMINATING │
                    │ @on terminate│  │ @on terminate│
                    └──────┬──────┘   └──────┬──────┘
                           │                 │
                           ▼                 ▼
                    ┌─────────────┐   ┌─────────────┐
                    │    DEAD     │   │    DEAD     │
                    │ (limpio)    │   │ (por presión│
                    └─────────────┘   │ de memoria) │
                                      └─────────────┘
```

---

## 10. El .deck-os del Board Declara el Modelo de Interacción

Cada board define qué botones físicos mapean a qué eventos del sistema. El modelo de interacción no está hardcodeado en el runtime — está declarado en el `.deck-os`.

```
-- myboard.deck-os

@os
  name: "MyBoard S3"

-- Mapeo de botones físicos a eventos del sistema
@system_buttons
  back:       hardware.button (id: 0, action: :press)
  home:       hardware.button (id: 1, action: :press)
  task_view:  hardware.button (id: 1, action: :long_press)
  volume_up:  hardware.button (id: 2, action: :press)
  volume_down:hardware.button (id: 3, action: :press)

-- Gestos en pantalla táctil (si disponible)
@system_gestures
  back:       swipe_from_left_edge
  task_view:  swipe_from_bottom
  home:       swipe_from_bottom_center

-- Qué app es el launcher
@launcher_app "system.launcher"

-- Qué app IDs son del sistema (privilegiadas)
@system_apps
  "system.launcher"
  "system.settings"
  "system.crash_reporter"
```

El runtime lee `@system_buttons` y `@system_gestures` al iniciar y registra los handlers correspondientes con el bridge. Las apps nunca ven botones de "back" o "home" — solo ven sus `@on back` hooks y los eventos de hardware no-sistema que declaren explícitamente.

---

## 11. Settings App

El OS incluye una Settings app (`system.settings`) que accede a la configuración de todas las apps instaladas. Puede leer y modificar los valores de `@config` de cualquier app a través de la API `system.apps`:

```deck
-- En la settings app:
let all_apps = apps.installed()
for app in all_apps
  let config_schema = apps.config_schema(app.id)
  -- config_schema: [{name, type, default, range?, options?, unit?, current_value}]
  -- Renderiza un formulario nativo por app
```

Desde la perspectiva de las apps de usuario, sus `@config` nunca cambian mid-execution sin que lo sepa la app. Cuando el usuario cambia un valor en Settings, la app recibe un evento `os.config_change (field: str, value: any)` (ver §13 — este evento se añade al catálogo estándar de `03-deck-os`).

---

## 12. Crash Reporter

The crash log itself is persisted by the OS in a system-level ring buffer (last 50 crashes by default). The Crash Reporter app does not need its own database — it consumes the `system.crashes` read API (`03-deck-os §4.11`). When the runtime catches an unrecoverable error in any app VM, the OS:

1. Captures `CrashInfo { app_id, message, stack, occurred }`.
2. Appends it to the ring buffer.
3. Fires `@on crash_report` to `system.crash_reporter` (resuming it from suspension if needed).

The Crash Reporter is therefore stateless — it reacts to the new crash, posts a system notification, and exposes the buffer to the user.

```deck
-- system/crash_reporter/app.deck

@app
  name: "Crash Reporter"
  id:   "system.crash_reporter"

@use
  system.shell    as shell
  system.crashes  as crashes

@on launch
  let pending = crashes.list()
  when len(pending) > 0
    show_crash_overlay(head(pending))

@on crash_report (info: CrashInfo)
  -- The OS has already persisted `info`; we only need to surface it.
  show_crash_overlay(info)

fn show_crash_overlay (info: CrashInfo) -> unit !shell =
  shell.post_notification(SysNotifOpts {
    id:      "crash_{info.app_id}",
    title:   "{info.app_id} dejó de responder",
    message: "Toca para ver detalles",
    app_id:  "system.crash_reporter",
    icon:    :none,
    url:     :some "crash://detail/{info.app_id}",
    expires: :none
  })
```

---

## 13. Resumen de Integración con Specs Hermanos

Las afirmaciones de las secciones 1–12 dependen de eventos, hooks, capabilities y anotaciones declarados en los otros specs. Esta sección los enumera y apunta a la definición canónica para que el modelo de shell quede acoplado al lenguaje sin duplicación.

### Eventos declarados en `03-deck-os §5`

| Evento | Notas |
|---|---|
| `os.config_change (field: str, value: any)` | Solo recibido por la app cuyo `@config` cambió |
| `os.app_launched (app_id: str)` | Solo entregado a apps con `@use system.apps` |
| `os.app_suspended (app_id: str)` | Solo entregado a apps con `@use system.apps` |
| `os.memory_pressure (level: atom)` | `:low` \| `:critical`. Entregado a todas las apps vivas |

### Hooks declarados en `02-deck-app §11`

| Hook | Restricción |
|---|---|
| `@on back` → `:handled` \| `:unhandled` \| `:confirm { … }` | Disponible para cualquier app |
| `@on open_url (url: str, params: {str: str})` | Requiere `@handles` declarado |
| `@on crash_report (info: CrashInfo)` | Solo permitido en `system.crash_reporter`; load-error en cualquier otra app |

### Capabilities declaradas en `03-deck-os §4.11`

| Capability | Quién |
|---|---|
| `system.apps`, `system.tasks`, `system.shell`, `system.security` | Apps cuyo `app.id` empieza por `system.` |
| `system.crashes` | Solo `system.crash_reporter` |

### Anotación `@handles` en `02-deck-app §20`

`@handles` es top-level en `app.deck` (no anida en `@app`). Declara patrones de URL que activan `@on open_url`. El Loader los registra en el OS en Stage 4. La gramática completa (segmentos `{name}` / `{name:rest}`, query string, resolución cuando varias apps matchean) vive en `02-deck-app §20.1–§20.6`.

---

## 14. Por Qué Esta Arquitectura Es Correcta

El error que cometen la mayoría de plataformas es mezclar el modelo de navegación de la app con el modelo de lifecycle del OS. Android lo hizo durante años: el back button podía destruir Activities de maneras que los developers no esperaban, y el resultado fue años de bugs donde "Back mata mi modal pero no debería".

Deck separa los dos modelos limpiamente:

- El `@machine` de navegación es **estado de la app**, propiedad de la app, nunca tocado por el OS sin consentimiento
- El OS App Stack es **estado del sistema**, propiedad del OS, nunca tocado por la app directamente
- La comunicación entre los dos es exclusivamente a través de hooks declarados (`@on back`, `@on suspend`, `@on resume`) con semántica exacta
- El OS nunca destruye estado de la app silenciosamente — siempre dispara `@on terminate` primero
- Los crashes están contenidos por el aislamiento de VMs — nunca pueden corromper el OS ni otras apps

El resultado es que los bugs del tipo "el back button dejó mi app en un estado imposible" son estructuralmente imposibles, no solo improbables.

---

## 15. El Runtime Determina el Cómo

Los specs anteriores definen **qué** hace Deck: qué declara una app, qué efectos puede usar, qué lifecycles existen. Esta sección define **cómo** el runtime ejecuta esos qués — las decisiones de implementación que el lenguaje no prescribe pero que el runtime toma de forma consistente.

### 15.1 La Distinción Fundamental

| Nivel | Quién decide | Qué decide |
|---|---|---|
| Deck lang | El developer | Qué estado existe, qué transiciones hay, qué efectos se piden |
| Runtime | El intérprete | Cuándo evalúa, en qué tarea de OS, cómo serializa efectos, cuándo renderiza |
| Bridge | La plataforma C | Cómo mapea efectos a hardware, cómo escribe en pantalla, cómo mide memoria |

El runtime es el mediador. Una app Deck no puede observar ni controlar en qué hilo corre, cuándo se flusheó su render al display, o si su `@task` fue retrasado por presión de batería. El runtime decide todo eso.

### 15.2 Las Tareas Permanentes del OS Bridge

El bridge implementa un conjunto fijo de tareas del OS que existen desde el boot hasta el apagado. Son del sistema — no pertenecen a ninguna app:

```
┌────────────────────────────────────────────────────────────────────┐
│  Core 0                        │  Core 1                          │
│  ─────────────────────         │  ─────────────────────           │
│  deck_runtime_task             │  lvgl_task (ui_engine)           │
│    └─ evaluador principal      │    └─ LVGL tick, flush, touch    │
│    └─ effect dispatcher        │    └─ deck_bridge_render()       │
│    └─ scheduler (tasks/pollers)│    └─ lv_async_call queue        │
│                                │                                  │
│  esp_event_loop_task           │  (Core 1 es single-tenant        │
│    └─ bus de eventos del OS    │   de LVGL — sin runtime aquí)    │
└────────────────────────────────────────────────────────────────────┘
```

**`deck_runtime_task`** (Core 0): El loop principal del intérprete. Única tarea que ejecuta el evaluador, el effect dispatcher y el scheduler. **No hay paralelismo en la evaluación de Deck.** Toda evaluación de una app ocurre en esta tarea, serializada.

**`lvgl_task`** (Core 1): El loop de LVGL. Es el único contexto donde se puede llamar a la API de LVGL. `deck_bridge_render()` se llama desde aquí — el runtime hace `lv_async_call` y `deck_bridge_render()` corre en Core 1. El mutex LVGL (`ui_lock`) protege cualquier acceso cruzado.

**`esp_event_loop_task`**: El bus de eventos del OS. Servicios del sistema (WiFi, batería, tiempo) postean eventos aquí. Los handlers registrados por el bridge son notificados aquí y encolan mensajes al `deck_runtime_task` vía una queue.

Ninguna de estas tareas pertenece a ninguna app. No se destruyen cuando una app termina.

### 15.3 El Main Loop del Runtime

`deck_runtime_task` ejecuta este loop:

```c
void deck_runtime_task(void *arg) {
    DeckOSRuntime *rt = (DeckOSRuntime *)arg;

    for (;;) {
        /* Bloquear hasta que haya trabajo, máximo 100ms para health checks */
        DeckRuntimeMsg msg;
        if (xQueueReceive(rt->msg_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            deck_runtime_dispatch(rt, &msg);
        }

        /* Scheduler: @task timers y watch: conditions */
        deck_scheduler_tick(rt->scheduler);

        /* Memory pressure check cada N ticks */
        if (deck_runtime_should_check_memory(rt)) {
            deck_memory_pressure_check(rt);
        }
    }
}
```

Los mensajes (`DeckRuntimeMsg`) son:

| Tipo | Origen | Qué dispara |
|---|---|---|
| `MSG_EVENT` | OS event bus | `@on` hook en la VM foreground |
| `MSG_INTENT` | Bridge (toque del usuario) | `on ->` handler del intent en la view activa |
| `MSG_BACK` / `MSG_HOME` | Bridge (gesto) | Lógica de navegación del §3 |
| `MSG_DEEP_LINK` | Bridge (URL recibida) | Dispatch a la app registrada en `@handles` |
| `MSG_APP_LAUNCH` / `MSG_APP_KILL` | `system.apps` capability | Ciclo de launch / terminate |
| `MSG_STREAM_VALUE` | Bridge (callback de stream) | Scheduler actualiza buffer, posible re-render |
| `MSG_EFFECT_DONE` | Bridge (I/O completado) | Effect dispatcher reanuda continuación suspendida |
| `MSG_TASK_RUN` | Scheduler timer | Evaluador ejecuta body de `@task` |

Todo el trabajo del evaluador ocurre en `deck_runtime_dispatch`. Los mensajes se procesan uno a la vez, en orden FIFO. Esto es lo que garantiza la ausencia del bug del doble-pop y la ausencia de race conditions en el estado de los machines.

---

## 16. Modelo de Memoria

### 16.1 Regiones de Heap en Embedded

En un ESP32-S3 con PSRAM:

```
┌──────────────────────────────────────────────────────┐
│  SRAM interna  (~520 KB total)                       │
│  ┣─ runtime structs (DeckOSRuntime, VMs, queues)     │
│  ┣─ atom intern table (compartida entre todas las VMs│
│  ┣─ value stack y frame stack del VM foreground      │
│  ┣─ LVGL draw buffers (doble buffer)                 │
│  └─ stacks de FreeRTOS tasks permanentes             │
│                                                      │
│  PSRAM externa  (~8 MB)                              │
│  ┣─ heap snapshots de VMs suspendidas                │
│  ┣─ stream buffers de apps suspendidas               │
│  ┣─ thumbnails PNG de apps suspendidas               │
│  ┣─ stacks de tasks pesadas (downloader, OTA, SQLite)│
│  └─ assets grandes (imágenes, fuentes adicionales)   │
└──────────────────────────────────────────────────────┘
```

La regla: **los objetos vivos del VM foreground viven en SRAM** (acceso rápido para el evaluador). Los objetos de VMs suspendidas se mueven a PSRAM para liberar SRAM.

### 16.2 Layout de Memoria por VM

Cada `DeckVMInstance` mantiene:

```c
typedef struct DeckVMHeap {
    uint8_t*   base;          /* inicio del heap de valores */
    size_t     capacity;      /* tamaño total asignado */
    size_t     used;          /* bytes actualmente en uso */
    uint8_t    region;        /* HEAP_SRAM | HEAP_PSRAM */
    DeckValue* value_stack;   /* stack del evaluador (512 entradas) */
    DeckFrame* frame_stack;   /* frames de funciones (512 entradas) */
    DeckEnv*   global_env;    /* bindings globales del módulo */
} DeckVMHeap;
```

Al lanzar una app: el heap se asigna en SRAM (`MALLOC_CAP_INTERNAL`).
Al suspender: el heap se serializa a PSRAM (`MALLOC_CAP_SPIRAM`), la SRAM se libera.
Al resumir: el heap se deserializa de vuelta a SRAM, el snapshot en PSRAM se libera.

### 16.3 Snapshots de VMs Suspendidas

`deck_os_suspend()` no copia raw bytes del heap — serializa el grafo de objetos vivos:

```
deck_vm_serialize(vm) → byte array (PSRAM):
  1. Trazar desde value_stack (raíces del evaluador)
  2. Trazar desde global_env (bindings del módulo)
  3. Trazar desde todos los @machine instances (estado actual)
  4. Trazar desde los stream buffers de esta VM
  5. Por cada objeto alcanzado: serializar en orden topológico
  6. Objetos no alcanzados (refcount == 0): descartar sin serializar

El snapshot incluye:
  - @machine instances con sus estados actuales y payloads
  - nav history stack (el historial de navegación dentro de la app)
  - stream buffers (últimos N valores de cada @stream suscrito)

El snapshot NO incluye:
  - El AST y el programa compilado (vive en flash/ROM, nunca se libera)
  - Los DeckViewContent* (son efímeros, se regeneran en resume)
  - Atoms internados (la intern table es global y permanente)
```

### 16.4 Conteo de Referencias

El runtime usa reference counting con estas propiedades:

- Todo `DeckValue*` tiene un campo `refcount` (uint16_t).
- Se incrementa al asignar a una variable o pasar como argumento.
- Se decrementa al salir de scope (frame destruido) o al reemplazar una binding.
- `refcount == 0` → liberar inmediatamente. Sin colector diferido, sin pausas.

**No hay ciclos posibles en el modelo de valores de Deck:**
- Closures capturan un snapshot del `Env` en el momento de creación. El Env no referencia la closure de vuelta.
- Los `@machine` instances son structs en el heap del VM, fuera del sistema de refcount. Las transiciones reemplazan el valor de estado; el viejo valor pierde su referencia y se libera.
- Los records son datos inmutables — ningún campo puede referenciar el record que lo contiene.

No se necesita cycle detection. No hay GC. Las pausas de GC son imposibles.

### 16.5 Ciclo de Vida de DeckViewContent

```
Trigger de re-render
  → Navigation Manager evalúa content= del estado activo (Core 0)
  → Serializa VFragment → DeckViewContent* (SRAM, allocado en Core 0)
  → Encola MSG_RENDER al LVGL task

  → lvgl_task (Core 1) recibe MSG_RENDER
  → deck_bridge_render(view_name, content) llamado en Core 1
  → Bridge lee DeckViewContent*, actualiza widgets LVGL
  → DeckViewContent* liberado al retornar de bridge_render
```

**Un render = una allocation + un free.** No hay cache del content tree. El bridge hace el diffing — si un widget no cambió, no lo re-crea ni lo re-dibuja. El runtime no sabe de widgets; el bridge no sabe de Deck expressions.

---

## 17. Background Tasks: de @task a Realidad

### 17.1 Cómo @task Mapea al OS

Un `@task` es una declaración. El Scheduler lo convierte en trabajo real:

```deck
@task sync_posts
  every: 5m
  when: network is :connected and battery > 20%
  priority: :background
  battery: :normal
  run: sync_logic()
```

El Scheduler mantiene un `TaskEntry` por cada `@task` declarado:

```c
typedef struct {
    char               name[32];
    uint32_t           interval_ms;    /* 0 si no tiene every: */
    Expr*              when_exprs;     /* evaluados por ConditionTracker */
    Expr*              run_body;
    TaskPriority       priority;       /* :urgent | :normal | :background | :low */
    TaskBattery        battery;        /* :any | :normal | :charging */
    uint32_t           last_run_ms;
    esp_timer_handle_t timer;          /* NULL si no tiene every: */
    bool               cond_satisfied; /* última evaluación de when: */
    bool               running;        /* true mientras el body se ejecuta */
    bool               is_background;  /* @task background: true — sobrevive suspension */
} TaskEntry;
```

Para `every: 5m`: el Scheduler crea un timer one-shot de 5 minutos. Al disparar:
1. El callback encola `MSG_TASK_RUN` a la queue de `deck_runtime_task`.
2. El runtime task lo recibe, evalúa el `when:`, y si es true ejecuta el body en el evaluador.
3. Si el body hace efectos (`!net`, `!db`), son dispatched por el effect dispatcher.
4. Al terminar, el timer se re-arma para los próximos 5 minutos.

**El body de un @task corre en `deck_runtime_task`, no en una tarea separada.** El evaluador es single-threaded. Si un @task hace una operación larga (un download), el efecto subyacente delega el trabajo I/O a una task del bridge y retorna una continuación al evaluador. El evaluador suspende esa VM y puede procesar otros mensajes mientras espera — ver §17.3.

### 17.2 Política de Prioridad y Batería

```
priority: :urgent      → ejecutar incluso si hay otros mensajes en queue
priority: :normal      → cola normal (default)
priority: :background  → ejecutar solo si la queue está vacía
priority: :low         → ejecutar solo si queue vacía y battery > 50%

battery: :any          → ejecutar en cualquier nivel de batería (default)
battery: :normal       → no ejecutar si battery < 15%
battery: :charging     → ejecutar solo si está cargando
```

Estas son políticas del Scheduler de Deck, no prioridades de FreeRTOS. Todos los bodies de @task corren en el mismo `deck_runtime_task`. La prioridad determina cuándo el Scheduler elige despachar el task dentro del main loop.

### 17.3 Efectos de Larga Duración: El Modelo de Continuación

Cuando cualquier código Deck llama a un efecto que toma tiempo (p.ej. `net.get(url)`):

```
Evaluador: encuentra EffectRequest { alias: "net", method: "fetch", args: [url] }
  → suspende la VM: guarda continuación, marca VM como "awaiting effect"
  → devuelve EffectRequest al effect dispatcher

Effect dispatcher:
  → llama deck_bridge_call("net", "fetch", [url])
  → el bridge lanza una task separada para el HTTP request
  → retorna inmediatamente (no bloquea deck_runtime_task)
  → la VM está suspendida pero el runtime task sigue libre

deck_runtime_task:
  → procesa otros MSGs mientras la VM espera
  → puede procesar streams, render de otras VMs, intents de usuario

HTTP request completa (en task del bridge):
  → bridge encola MSG_EFFECT_DONE { vm_id, result }
  → deck_runtime_task lo recibe
  → effect dispatcher reanuda la continuación con el resultado
  → evaluador continúa desde donde fue suspendido
```

Este modelo permite que las VMs esperen I/O sin bloquear el runtime para otros mensajes. El runtime es cooperativo a nivel de evaluador Deck, pero nunca bloquea el OS thread.

### 17.4 Main Task vs Background Task — Ciclo de Vida en el Runtime

El campo `background: bool` de `@task` cambia cómo el Scheduler maneja la tarea cuando la VM no está en foreground.

**App en foreground**: ambos tipos ejecutan normalmente en `deck_runtime_task` cuando sus condiciones se cumplen.

**App suspendida** — comportamiento diverge:

*Main task (`is_background = false`):*
```
timer dispara → callback encola MSG_TASK_RUN { vm_id, task_name }
  → deck_runtime_task lo recibe
  → ¿VM suspendida? → sí
  → mueve el mensaje a DeckVMInstance.held_task_msgs
  → timer se re-arma para el próximo intervalo (el reloj sigue corriendo)

App vuelve al foreground (deck_os_resume):
  → se vacía held_task_msgs → mensajes re-encolados en runtime queue
  → tasks se ejecutan en orden de llegada (lo antes posible)
```

*Background task (`is_background = true`):*
```
timer dispara → callback encola MSG_TASK_BG_RUN { vm_id, task_name }
  → deck_runtime_task lo recibe
  → ¿VM suspendida? → sí; no importa — el background task ejecuta igual
  → restaura VM de PSRAM a SRAM (heap_snapshot → vm->heap)
  → evalúa when: conditions en la VM restaurada
  → si true: ejecuta el run_body en el evaluador
     - efectos (!db, !net, !nvs) funcionan normalmente
     - Machine.send() actualiza machine state en la VM
     - NO se llama deck_bridge_render() — no hay UI mientras está suspendida
  → re-serializa VM a PSRAM
  → timer se re-arma

App vuelve al foreground (deck_os_resume):
  → Navigation Manager ve el machine state actualizado
  → llama deck_bridge_render() con el contenido del nuevo estado
  → el bridge muestra la UI actualizada sin que el dev escriba código de restauración
```

**Navegación intra-app no afecta tasks.** Cambiar de `:timeline` a `:compose` vía `to history` o cualquier transición del `@flow` no pausa ni detiene ningún `@task`. Los tasks corren a nivel de VM, no de flow state. El Condition Tracker sí re-evalúa condiciones `when:` cuando la machine cambia de estado — eso es lo único que la navegación afecta en el scheduler.

**App terminada** (`@on terminate`): todos los timers se cancelan con `esp_timer_stop()` + `esp_timer_delete()`. El held_task_msgs se vacía y destruye. La VM no ejecuta más trabajo.

---

## 18. Flujo Completo de Eventos

### 18.1 De Hardware a Deck

```
Hardware (sensor, radio, RTC)
  → Driver ISR / callback del OS
  → svc_* service (svc_wifi, svc_battery, svc_time)
  → esp_event_post (CYBERDECK_EVENT, EVT_WIFI_CONNECTED, data)
  → esp_event_loop_task ejecuta el handler del bridge
  → deck_bridge_on_os_event(event_id, data) — el bridge notifica al runtime
  → runtime encola MSG_EVENT { event_id, payload }

  → deck_runtime_task desencola
  → Effect dispatcher localiza el @on hook del event_id en la VM foreground
  → Evaluador ejecuta el body del @on hook
  → El hook puede hacer send() → machine state change → re-render pipeline
  → El hook puede hacer efectos → ciclo de continuación del §17.3
```

**Los eventos del OS solo llegan a la VM foreground.** Las VMs suspendidas no reciben eventos. Excepción: el Scheduler mantiene activos los timers de `@task every:` de las VMs suspendidas — al disparar, el mensaje se encola pero el body no corre hasta que la app esté en foreground (o hasta que el Scheduler decida ejecutarlo en background según su `priority:`).

### 18.2 Intents del Usuario (Touch → Deck)

```
Usuario toca un toggle en pantalla
  → LVGL detecta el toque (lvgl_task, Core 1)
  → Bridge identifica el intent: view "settings", intent name: :wifi_enabled, value: true
  → Bridge encola MSG_INTENT { view_name, intent_name, payload } al runtime

  → deck_runtime_task desencola
  → Navigation Manager localiza el on -> handler para :wifi_enabled en el content actual
  → Evaluador ejecuta el handler con (event.value = true) en scope
  → El handler hace send(:toggle_wifi, enabled: true) → machine state change
  → Re-render pipeline (§18.3)
  → El handler hace network.wifi.connect(ssid, pwd) → effect dispatched (§17.3)
```

### 18.3 Pipeline de Re-render

```
send() produce machine state change
  → MACHINE_STATE_CHANGED emitido internamente al Navigation Manager
  → Navigation Manager evalúa content= del nuevo estado activo (Core 0)
  → Serializa a DeckViewContent*
  → Encola MSG_RENDER al lvgl_task via lv_async_call

  → lvgl_task (Core 1) ejecuta deck_bridge_render(view_name, content)
  → Bridge diff: compara el nuevo DeckViewContent* con widgets actuales
  → Actualiza solo los widgets que cambiaron
  → Libera DeckViewContent* al retornar
```

**Batching de renders**: si múltiples `send()` ocurren en el mismo `deck_runtime_dispatch()` (p.ej. un `on ->` handler que hace varios sends encadenados), el Navigation Manager acumula todos los `MACHINE_STATE_CHANGED` y produce **un único re-render** al final. No hay renders intermedios por cada send individual.

### 18.4 La Garantía de Serialización

Toda interacción del usuario, todo evento del OS y todo timer de @task llega como un mensaje a la queue del `deck_runtime_task`. La queue es FIFO. El dispatch es serial. Esto garantiza:

1. **No hay race conditions en el estado de los machines.** Dos transiciones no pueden ocurrir simultáneamente.
2. **No hay double-pop.** Dos backs rápidos producen dos MSGs en orden; el segundo se procesa solo después de que el primero terminó completamente.
3. **No hay evento que llegue a una app en estado inconsistente.** Un `@on resume` siempre llega después de que `@on launch` (o el resume anterior) terminó por completo.
4. **No hay render parcial.** Un render siempre refleja un estado coherente de todos los machines, nunca un estado intermedio de una transición en curso.

---

## 19. Presión de Memoria: Detección y Respuesta

### 19.1 El Memory Monitor

El `deck_runtime_task` evalúa la presión de memoria periódicamente (cada ~10 segundos, configurable en el `.deck-os`):

```c
void deck_memory_pressure_check(DeckOSRuntime *rt) {
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    MemPressureLevel level;
    if (free_internal < rt->config->mem_critical_internal ||
        free_psram    < rt->config->mem_critical_psram)
        level = MEM_CRITICAL;
    else if (free_internal < rt->config->mem_low_internal ||
             free_psram    < rt->config->mem_low_psram)
        level = MEM_LOW;
    else
        level = MEM_OK;

    if (level != MEM_OK) {
        /* Notificar a las apps vía el evento estándar del §13 */
        deck_os_emit_event("os.memory_pressure", level_to_atom(level));
    }

    if (level == MEM_CRITICAL) {
        deck_evict_oldest_suspended(rt);
    }
}
```

Umbrales de referencia (configurables en el `.deck-os`):

| Umbral | SRAM | PSRAM | Nivel |
|---|---|---|---|
| `mem_low_*` | 64 KB libre | 512 KB libre | `:low` — se emite el evento |
| `mem_critical_*` | 32 KB libre | 256 KB libre | `:critical` — eviction inmediata |

### 19.2 Política de Eviction

El OS **nunca** destruye:
- La VM foreground (la app activa)
- La VM del Launcher
- Las VMs de apps del sistema (`system.*`)

Candidatos para eviction, en orden de prioridad (mayor = evictar primero):
1. La VM suspendida más antigua (`suspended_at` más bajo)
2. Entre iguales: la de snapshot más grande en PSRAM (libera más memoria)
3. Solo apps de usuario; nunca apps del sistema

El OS destruye de a una VM por ciclo de check. Si la presión sigue siendo `:critical` en el próximo tick (100ms), destruye la siguiente. No hace bulk eviction — preserva responsividad del sistema.

### 19.3 El Protocolo de Eviction

```c
void deck_evict_oldest_suspended(DeckOSRuntime *rt) {
    DeckVMInstance *victim = deck_find_eviction_candidate(rt);
    if (!victim) return;  /* nada evictable */

    /* 1. @on terminate con timeout de 500ms — tiempo para guardar estado */
    deck_vm_fire_hook_with_timeout(victim->vm, "terminate",
                                   deck_value_atom(":memory_pressure"),
                                   500 /* ms */);

    /* 2. Cancelar efectos en vuelo, desregistrar streams y eventos */
    deck_vm_cancel_pending_effects(victim->vm);
    deck_vm_unsubscribe_all_streams(victim->vm);
    deck_vm_unregister_all_events(victim->vm);

    /* 3. Liberar snapshot PSRAM, thumbnail, VM */
    heap_caps_free(victim->heap_snapshot);
    heap_caps_free(victim->thumbnail);
    deck_vm_destroy(victim->vm);

    /* 4. Remover del suspended array, registrar terminación por presión */
    deck_os_remove_from_suspended(rt, victim);
    deck_os_record_terminated_by_pressure(rt, victim->app_id);

    /* 5. Emitir evento estándar (§13) */
    deck_os_emit_event("os.app_terminated",
        deck_value_record("AppTerminatedInfo",
            "app_id", deck_value_str(victim->app_id),
            "reason", deck_value_atom(":memory_pressure")));

    free(victim);
}
```

### 19.4 El Contrato de @on terminate

`@on terminate` tiene 500ms para ejecutarse — inviolables. El runtime usa un timer; si el hook no retorna, la destrucción continúa de todos modos. Dentro de esos 500ms la app puede:

- Guardar estado crítico en `store.set()` o `nvs.set()` — efectos síncronos y rápidos
- Cerrar conexiones (`ws.close()`, `ble.disconnect()`)

**No puede** hacer efectos de larga duración como `net.get(url)` — estos serían cancelados al destruir la VM. El `@on terminate` es un callback de limpieza, no una oportunidad de I/O.

La próxima vez que el usuario lance la app que fue terminada por presión: recibirá `@on launch` (no `@on resume`). Si guardó estado en `@on terminate`, puede restaurarlo en `@on launch`. Si no guardó nada, empieza limpia.

---

## 20. Ciclo de Vida de una Vista

### 20.1 Primera Aparición

```
VM lanzada (deck_os_launch)
  → @on launch ejecutado (puede hacer efectos, cargar datos)
  → Durante @on launch: bridge muestra VCLoading {} automáticamente
    (el runtime emite un loading placeholder hasta que @on launch termina)
  → @on launch retorna
  → @flow de entrada inicializado en su initial state
  → Navigation Manager evalúa content= del initial state
  → deck_bridge_render() con el primer DeckViewContent*
  → Bridge crea widgets, muestra la pantalla
  → @on resume ejecutado (primera vez, mismo frame)
```

El placeholder `VCLoading {}` previene que el usuario vea pantalla en blanco mientras la app carga datos iniciales. El bridge lo renderiza como un spinner o cursor parpadeante — el estilo exacto es decisión del bridge.

### 20.2 Re-renders

Los triggers de re-render son:

| Trigger | Origen |
|---|---|
| `send()` exitoso en cualquier `@machine` referenciado por el content= activo | Evaluador |
| Nuevo valor en un `@stream` leído en el content= actual | Scheduler → stream buffer |
| `@on resume` recibido (el display pudo haber rotado o cambiado) | OS lifecycle |
| `os.config_change` event (el usuario cambió un @config) | OS event bus |

El Navigation Manager agrupa todos los triggers que ocurren en el mismo `deck_runtime_dispatch()` en un único re-render. No hay renders parciales.

**Filosofía del diffing**: el runtime siempre produce el `DeckViewContent*` completo del estado activo. El bridge decide qué actualizar comparando el nuevo tree con los widgets existentes. Esta separación es intencional: el runtime no sabe de widgets, el bridge no sabe de expresiones Deck.

### 20.3 Suspensión: El Último Frame

```
Home gesture → deck_os_suspend(foreground_vm)
  1. @on suspend ejecutado
     → Si @on suspend hace send() → un último re-render ocurre
  2. Bridge captura screenshot del display actual → thumbnail PNG (PSRAM)
  3. VM heap serializado a PSRAM
  4. @machine instances y nav history guardados en snapshot
  5. Pantalla no se limpia aún — los widgets LVGL quedan visibles
  6. deck_os_resume(launcher) comienza
  7. deck_bridge_render(launcher_content) → la pantalla ahora muestra el Launcher
  8. Los widgets de la app suspendida se destruyen DESPUÉS de que el Launcher es visible
```

La secuencia "resume primero, destroy después" evita el flash de pantalla negra entre apps. El usuario siempre ve una pantalla coherente.

### 20.4 Resume: Estado Exacto Preservado

```
Usuario vuelve a la app (desde launcher o task switcher)
  1. Deserializar heap snapshot de PSRAM → SRAM
  2. Restaurar @machine instances con sus estados exactos
  3. Restaurar nav history stack (historial de navegación interno)
  4. Re-registrar @stream subscriptions
     (valores producidos durante la suspensión se descartan —
      si la app necesita datos frescos, los pide en @on resume)
  5. @on resume ejecutado
  6. Navigation Manager re-evalúa content= del estado actual
  7. deck_bridge_render() con el estado restaurado
```

El `@on resume` recibe la app en el **estado exacto** donde fue suspendida — el mismo estado de machines, los mismos valores en scope, el mismo punto en el nav history. Es como si el tiempo no hubiese pasado para la app.

Única excepción: los streams pueden haber perdido valores intermedios. La app debe re-fetchear si necesita datos frescos en `@on resume`.

---

## 21. Ciclo de Vida del Sistema: Boot

### 21.1 Secuencia de Arranque

```
1. Hardware init (HAL, C puro — sin Deck)
   → LCD, touch, I2C expander, backlight, SD card, RTC, batería ADC

2. OS Core init
   → os_task_init()
   → os_process_init()
   → os_service_init()
   → os_event_init() / svc_event_init()

3. System services init
   → svc_settings      (NVS — configuración persistente)
   → svc_battery       (ADC — nivel de batería)
   → svc_time          (RTC + SNTP — hora del sistema)
   → svc_wifi          (gestión de red)
   → svc_notifications (SQLite history + NVS source config; inicia polling tasks
                        solo cuando hay fuentes registradas)

4. UI Engine init
   → LVGL init, display driver, touch driver
   → lvgl_task creada en Core 1 (comienza a tickear)

5. Deck Bridge init (deck_bridge_init())
   → Leer y parsear el .deck-os (desde SD card o flash)
   → Registrar capabilities en el runtime
   → Inicializar effect dispatchers para cada capability

6. Deck Runtime init (deck_runtime_create())
   → Crear DeckOSRuntime y msg_queue
   → Crear deck_runtime_task en Core 0
   → Inicializar Scheduler, ConditionTracker, atom intern table
   → Seed random desde hardware entropy (ADC noise, chip ID)

7. Lanzar Launcher (primer app Deck)
   → deck_os_launch(rt, LAUNCHER_APP_PATH)
   → Loader stages 1–12 sobre launcher/app.deck
   → @on launch del Launcher corre
   → Primer deck_bridge_render() — el usuario ve la pantalla por primera vez
```

El usuario no ve nada útil hasta el paso 7. Desde el paso 4 el backlight puede estar encendido (pantalla negra o splash en C). El paso 7 es el momento en que Deck toma el control de la experiencia del usuario.

### 21.2 El Bridge como Única Interfaz al Hardware

El bridge implementa la interfaz `DeckBridgeInterface`. El runtime nunca llama a drivers ni a ESP-IDF directamente:

```c
typedef struct DeckBridgeInterface {
    /* Render (se llama desde lvgl_task, Core 1) */
    void   (*render)        (const char *view, DeckViewContent *c, void *ctx);
    void   (*capture_thumb) (uint8_t **out_png, size_t *out_sz, void *ctx);

    /* Capabilities — dispatch de efectos */
    DeckValue* (*call)      (const char *cap, const char *method,
                             DeckValue **args, int argc, void *ctx);
    void  (*subscribe)      (const char *cap, const char *method,
                             DeckValue **args, int argc,
                             deck_stream_cb_t on_value, void *cb_ctx, void *ctx);

    /* Permisos */
    void  (*request_perms)  (const char **caps, int n,
                             deck_perm_result_cb_t on_done, void *ctx);

    /* Navegación hacia atrás (para to history — el bridge mantiene el back stack) */
    void  (*nav_back)       (const char *view_name, void *ctx);

    /* Eventos del OS hacia el runtime */
    void  (*on_event_ready) (deck_event_handler_t fn, void *ctx);

    /* Memoria (para presión de memoria y snapshots PSRAM) */
    size_t (*free_heap)     (uint8_t region, void *ctx);
    void*  (*alloc_psram)   (size_t size, void *ctx);
    void   (*free_psram)    (void *ptr, void *ctx);

    void  *ctx;
} DeckBridgeInterface;
```

Esta separación hace al runtime completamente portable. El mismo runtime puede correr en un host Linux con un bridge mock para testing — sin hardware, sin LVGL, sin ESP-IDF.

---

## 22. Procesos Internos del OS+Bridge

### 22.1 Tareas Permanentes del Sistema

Estas tareas existen desde el boot hasta el apagado. No pertenecen a ninguna app:

| Tarea | Core | Stack | Propósito |
|---|---|---|---|
| `deck_runtime_task` | 0 | 8 KB SRAM | Main loop del evaluador Deck |
| `lvgl_task` | 1 | 8 KB SRAM | LVGL tick, flush, touch input |
| `esp_event_loop_task` | 0 | 4 KB SRAM | Bus de eventos del OS |
| `os_poller_task` | 0 | 2 KB SRAM | Pollers de servicios (resolución 100ms) |
| `svc_wifi_task` | 0 | 8 KB PSRAM | Gestión WiFi (esp_netif) |
| `svc_notif_poll_{n}` | 0 | 4 KB PSRAM | Una task por fuente HTTP activa; se crea al registrar y muere al eliminar la fuente |
| `svc_notif_mqtt` | 0 | 6 KB PSRAM | Cliente MQTT permanente para todas las fuentes MQTT registradas (única task compartida) |

### 22.2 Tareas por App (Lifetime = Efecto en Vuelo)

Cuando una app usa efectos de larga duración, el bridge puede crear tareas temporales propiedad del app:

| Efecto | Tarea creada | Cuándo muere |
|---|---|---|
| `net.get()` / `net.post()` | HTTP client task (stack PSRAM) | Al completar el fetch |
| `net.download()` | Downloader task (stack PSRAM) | Al completar el download |
| `ws.connect()` | WebSocket recv task (stack PSRAM) | Al `ws.close()` o app termination |
| `ble.scan()` | BLE scan task | Al `ble.stop_scan()` |
| `db.query()` | SQLite query task (stack PSRAM) | Al completar la query |

Todas las tareas creadas por el bridge para una app se registran en el `os_task` registry bajo el `app_id` de esa app. `os_task_destroy_all_for_app(app_id)` las mata en el teardown — garantizando que no queden tasks zombie.

### 22.3 Servicios del Sistema como Capabilities

Los servicios del sistema no son apps Deck — son módulos C permanentes que el bridge expone como capabilities. Dos niveles:
- **OS surface capabilities** (declaradas en `03-deck-os.md`): primitivas de hardware y OS — red, almacenamiento, sensores, BLE, crypto, OTA.
- **High-level service capabilities** (declaradas en `05-deck-os-api.md`): construidas sobre las anteriores — `api_client` (sobre `network.http`), `cache` (memoria del VM, sin backing C permanente).

```
-- OS surface capabilities (03-deck-os.md)
svc_wifi          → network.http, network.ws, network.socket → @use network.http as net
svc_battery       → (parte de system.shell)                  → shell.battery_level()
svc_time          → @builtin time (siempre en scope)         → time.now(), time.format(), …
svc_db            → capability "db"                          → db.query(), db.exec(sql)
svc_nvs           → capability "nvs"                         → nvs.get(), nvs.set()
svc_storage       → capability "storage.local"               → store.get(), store.set()
svc_fs            → capability "fs"                          → fs.read(), fs.write()
svc_mqtt          → capability "mqtt"                        → mqtt.subscribe(), mqtt.publish()
svc_ble           → capability "ble"                         → ble.scan(), ble.connect()
svc_crypto        → capability "crypto.aes"                  → crypto.aes.encrypt(), decrypt()
svc_bgfetch       → capability "background_fetch"            → bg.register()
svc_ota           → capability "ota"                         → ota.check(), ota.download()
svc_notifications → capability "notifications"               → notif.list(), notif.register_source(), …

-- High-level service capabilities (05-deck-os-api.md) — sin svc_* propio
(sobre network.http)   → api_client   → api.get(), api.post(), …
(memoria del VM)       → cache        → cache.get(), cache.set()
```

Estos servicios existen independientemente de cualquier app. Cuando una app termina, su VM se destruye pero los servicios siguen corriendo — solo se cancelan las suscripciones y los efectos pendientes de esa VM.

### 22.4 Orden de Teardown al Terminar una App

El teardown siempre ocurre en este orden exacto:

```
1. deck_vm_fire_hook("terminate", reason)    ← @on terminate, 500ms max
2. deck_vm_cancel_pending_effects(vm)        ← abortar efectos en vuelo
3. deck_vm_unsubscribe_all_streams(vm)       ← cancelar @stream subscriptions
4. deck_vm_unregister_all_events(vm)         ← cancelar @on hooks del OS event bus
5. deck_scheduler_remove_all_tasks(vm)       ← cancelar @task timers
6. os_task_destroy_all_for_app(app_id)       ← matar FreeRTOS tasks del bridge
7. deck_vm_destroy(vm)                       ← liberar heap, frames, value stack
8. os_process_stop(app_id)                   ← limpiar del process registry
```

El orden importa: primero se desregistra todo (pasos 2–5) para que no lleguen más eventos o callbacks mientras la VM se destruye en el paso 7. El destroy de la VM es siempre lo último porque los pasos 2–5 necesitan acceder a ella para cancelar sus suscripciones activas.

### 22.5 Arquitectura de Servicios del OS

**Principio fundamental:** los servicios del OS no son wrappers delgados sobre ESP-IDF. Son módulos C autónomos con sus propios recursos, ciclo de vida, y acceso directo a infraestructura del sistema. El `app_id` es la clave de aislamiento universal — todo dato per-app se almacena o consulta bajo ese key, sea main task o background task.

#### Recursos por servicio

| Servicio | Capability | NVS namespace | SQLite / FS | WiFi/HTTP | Timers | Notas |
|---|---|---|---|---|---|---|
| `svc_settings` | — (interno) | `"cyberdeck"` | — | — | — | Configuración global del OS; no expuesta directamente como capability |
| `svc_battery` | `system.battery` | — | — | — | esp_timer 30 s | ADC polling; métodos básicos también en `system.shell` |
| `svc_time` | `@builtin time` | `"time"` | — | SNTP | esp_timer | Builtin, siempre en scope; re-sync periódico vía NTP |
| `svc_wifi` | `network.http/ws/socket` | `"wifi"` | — | esp_netif | — | Gestión de red; tarea permanente `svc_wifi_task` |
| `svc_db` | `db` | — | `/sdcard/data/{app_id}/db.sqlite` | — | — | Una DB SQLite por app; acceso sandboxeado |
| `svc_nvs` | `nvs` | `app_id` | — | — | — | Namespace NVS por app; atómico, sobrevive corrupción de FS |
| `svc_storage` | `storage.local` | — | `/sdcard/data/{app_id}/local/` | — | — | KV store de strings, file-backed; más lento que NVS |
| `svc_fs` | `fs` | — | `/sdcard/{app_id}/` | — | — | FS completo sandboxeado; paths relativos al root del app |
| `svc_mqtt` | `mqtt` | — | — | MQTT (esp-mqtt) | esp_timer (keepalive) | Una conexión por app; broker, QoS, y client ID por app |
| `svc_ble` | `ble` | — | — | BLE (nimble) | — | Scan y GATT client; una conexión activa por app |
| `svc_crypto` | `crypto.aes` | — | — | — | — | Stateless; usa hardware AES del ESP32 (AES-CBC) |
| `svc_bgfetch` | `background_fetch` | — | — | — | esp_timer (min_interval) | Despierta VM suspendida periódicamente |
| `svc_ota` | `ota` | `"ota"` | — | HTTP | — | Descarga y validación de firmware; partición OTA en flash |
| `svc_notifications` | `notifications` | `"notif_src"` | `/sdcard/system/notifications.db` | HTTP (por fuente) | esp_timer (TTL, poll) | Historia global; fuentes registradas en NVS del servicio |

#### Reglas de aislamiento

```
app_id → clave de aislamiento universal
  svc_nvs:     nvs_open(app_id, NVS_READWRITE)
  svc_db:      "/sdcard/data/{app_id}/db.sqlite"
  svc_notif:   SELECT * FROM notifications WHERE app_id = ?
  svc_ota:     registro de versión en NVS bajo app_id
```

- Un background task del mismo `app_id` accede exactamente a los mismos datos que el main task. No hay separación dentro del mismo `app_id`.
- Los servicios nunca exponen datos de un `app_id` a otro. El bridge verifica el `app_id` en cada llamada de capability antes de delegarla al servicio.
- `svc_notifications` es el único servicio que lee datos NVS de otra app (`app_id` del registrante) para recuperar tokens de autenticación — esto es por diseño y está documentado en `05-deck-os-api §10.2`.

#### Ciclo de vida de servicios vs ciclo de vida de apps

```
Boot:   svc_notifications init → leer fuentes del NVS → lanzar poll tasks activas
                                                              ↓
App lanza:    register_source(opts) → svc_notifications guarda en NVS, lanza poll task
                                                              ↓
App suspende: poll task sigue corriendo — el servicio no sabe ni le importa que la app esté suspendida
                                                              ↓
App termina:  svc_notifications cancela subscripción del @on os.notification de esa VM.
              Las fuentes registradas SIGUEN ACTIVAS — su config está en NVS del servicio,
              no en la VM. Las notificaciones se almacenan en SQLite normalmente.
              El Launcher puede seguir mostrando el badge unread count.
                                                              ↓
App re-lanza: svc_notifications re-entrega el unread count via notif_counts_watch().
              La app puede llamar notif.list() para obtener las notificaciones acumuladas.
```

Esta separación es lo que hace posible que `svc_notifications` funcione como sistema autónomo: el config persiste en el NVS del servicio (no en la app), el historial persiste en SQLite, y el badge del Launcher refleja la realidad aunque la app esté muerta.

---

## 23. Modelo de Procesos: Task Manager

### 23.1 Árbol de Procesos

Cada app en el OS app stack se representa como un árbol de procesos. El root es el proceso main; los background tasks son sus hijos. Comparten `app_id` como namespace; kill del root mata todo el árbol.

```
OS Process Tree:

social.bsky.app          kind: :main        [RUNNING]   heap: 128 KB  cpu: 2.3%
  └── SyncPosts          kind: :background  [IDLE]      heap:   8 KB  cpu: 0.0%
  └── PollNotifs         kind: :background  [WAITING]   heap:   6 KB  cpu: 0.1%

reader.app               kind: :main        [SUSPENDED] heap:  64 KB  cpu: 0.0%

system.launcher          kind: :main        [RUNNING]   heap:  32 KB  cpu: 0.1%
```

**IDs:**
- Proceso main: `app_id` (e.g., `"social.bsky.app"`)
- Background task: `app_id + ":" + task_name` (e.g., `"social.bsky.app:SyncPosts"`)

El `kind: :main` agrupa toda la evaluación del `@flow` de entrada, los main tasks (`background: false`), y el effect context de la VM. Los background tasks son nodos independientes que comparten el mismo heap físico pero tienen su propio ciclo de vida.

### 23.2 Contabilidad de CPU en el Runtime

El evaluador incrementa un contador por cada instrucción despachada. Se añade `eval_cycles` y campos de ventana temporal a `DeckVMInstance`:

```c
typedef struct DeckVMInstance {
    /* ... campos existentes ... */
    uint64_t  eval_cycles_total;   /* ciclos acumulados desde el launch */
    uint64_t  eval_cycles_window;  /* ciclos en la ventana de 5 s actual */
    uint32_t  window_start_ms;     /* timestamp de inicio de la ventana */
} DeckVMInstance;
```

Un `esp_timer` periódico de 5 s en el OS runtime calcula el porcentaje de CPU:

```c
static void cpu_accounting_timer_cb(void* arg) {
    DeckOSRuntime* rt = (DeckOSRuntime*)arg;
    uint32_t now_ms   = esp_timer_get_time() / 1000;

    /* Actualizar stats de la VM foreground y todas las suspendidas */
    for each vm in (rt->foreground, rt->suspended[], rt->launcher) {
        uint32_t elapsed = now_ms - vm->window_start_ms;
        if (elapsed == 0) continue;

        /* Porcentaje = ciclos en la ventana / (elapsed_ms * ciclos_por_ms_max) */
        float pct = (float)vm->eval_cycles_window
                  / (float)(elapsed * DECK_MAX_CYCLES_PER_MS) * 100.0f;
        vm->cpu_pct_snapshot = fminf(pct, 100.0f);

        vm->eval_cycles_window = 0;
        vm->window_start_ms    = now_ms;
    }
}
```

`DECK_MAX_CYCLES_PER_MS` es calibrado al iniciar el runtime (primer boot: benchmark de ciclos vacíos por ms en el hardware actual).

### 23.3 Implementación de system.tasks.tree()

El bridge implementa `tree()` como una lectura sincrónica del OS runtime state. Puede ser llamada desde el `deck_runtime_task` con `ui_lock` no necesario (no toca LVGL):

```c
DeckValue* deck_cap_tasks_tree(DeckRuntime* rt, DeckValue** args, int nargs) {
    DeckOSRuntime* os = (DeckOSRuntime*)rt->os_ctx;
    DeckValueList* list = deck_list_new();

    /* Iterar todas las VMs: foreground, suspended, launcher */
    DeckVMInstance* all_vms[] = { os->foreground, os->launcher };
    /* + os->suspended[0..suspended_count-1] */

    for each vm in all_vms {
        /* Proceso main */
        deck_list_push(list, make_process_entry(vm, NULL));

        /* Background tasks */
        DeckScheduler* sched = vm->vm->scheduler;
        for (int i = 0; i < sched->task_count; i++) {
            TaskEntry* t = &sched->tasks[i];
            if (t->is_background && !t->dead)
                deck_list_push(list, make_process_entry(vm, t));
        }
    }

    return deck_list_to_value(list);
}

static DeckValue* make_process_entry(DeckVMInstance* vm, TaskEntry* task) {
    bool is_bg = (task != NULL);
    char id[96];
    if (is_bg) snprintf(id, sizeof(id), "%s:%s", vm->app_id, task->name);
    else        strncpy(id, vm->app_id, sizeof(id));

    ProcessState state = is_bg
        ? (task->running ? PROC_RUNNING : task->cond_satisfied ? PROC_IDLE : PROC_WAITING)
        : vm_state_to_proc_state(vm->state);

    return deck_record_new("ProcessEntry",
        "id",        deck_str(id),
        "app_id",    deck_str(vm->app_id),
        "kind",      is_bg ? deck_atom("background") : deck_atom("main"),
        "task_name", is_bg ? deck_str(task->name) : deck_none(),
        "state",     deck_atom(proc_state_name(state)),
        "heap_kb",   deck_int(is_bg ? task->heap_bytes / 1024
                                    : (int)(vm->heap_size / 1024)),
        "cpu_pct",   deck_float(vm->cpu_pct_snapshot),  /* bg tasks share VM cpu */
        "uptime_ms", deck_int((int)(esp_timer_get_time()/1000 - vm->launch_time_ms)),
        NULL);
}
```

### 23.4 Kill Semántica

```
system.tasks.kill(app_id)
  → deck_os_terminate(rt, vm)          ← secuencia completa de teardown (§22.4)
  → aplica a main + todos los bg tasks
  → bg tasks quedan dead en la misma llamada (sus timers se cancelan en paso 5)

system.tasks.kill_task(app_id, task_name)
  → find TaskEntry en vm->scheduler->tasks[] por nombre
  → esp_timer_stop(entry->timer)
  → esp_timer_delete(entry->timer)
  → entry->dead = true; entry->running = false
  → la VM sigue viva; el @flow de entrada sigue en pantalla
  → no dispara @on terminate de la app
  → el Condition Tracker elimina las when: conditions del task muerto
```

El task manager UI puede usar `system.tasks.kill_task()` para cancelar un sync que lleva demasiado tiempo sin matar la app entera. El runtime no dispara ningún hook en el task muerto — la limpieza es responsabilidad del bridge (cancelar el timer es suficiente; si el task estaba en mitad de un efecto, el effect dispatcher recibe un `MSG_EFFECT_DONE` que el runtime descarta al ver el task marcado como dead).

### 23.5 cpu_watch: Stream de Actualizaciones

`system.tasks.cpu_watch()` retorna un `Stream [ProcessEntry]` que emite cada 5 s (sincronizado con el timer de contabilidad de CPU). El task manager app puede declarar:

```deck
@stream ProcessSnapshot
  source: tasks.cpu_watch()

step :active _ ->
  content =
    list (ProcessSnapshot.recent(1) |> unwrap_opt_or([]))
      p ->
        group
          p
          when p.kind is :main
            confirm "Kill {p.app_id}"  prompt: "Forzar cierre de {p.app_id}?"
              -> tasks.kill(p.app_id)
          when p.kind is :background
            confirm "Stop {p.task_name}"  prompt: "Cancelar background task {p.task_name}?"
              -> tasks.kill_task(p.app_id, unwrap_opt(p.task_name))
```

El stream no emite más frecuente que 5 s aunque el OS internamente muestree más rápido. La throttle está en el bridge que implementa la capability.
