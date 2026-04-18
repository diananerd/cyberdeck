# Annex B — Task Manager
**Bundled OS App, Slot 1, `system.taskman`**

> The Task Manager shows what's running, lets the user switch between active apps, and lets them force-kill a misbehaving one. It is the app you reach when something is stuck — therefore it must keep working when other apps cannot.

---

## 1. Identity

```deck
@app
  name:    "Task Manager"
  id:      "system.taskman"
  version: "1.0.0"
  edition: 2026
  entry:   App
  icon:    "TM"
  orientation: :any
```

System-privileged. Bundled. Cannot be uninstalled.

---

## 2. Capabilities

```deck
@requires
  deck_os: ">= 1"
  runtime: ">= 1.0"
  capabilities:
    system.apps:    ">= 1"     -- list running / suspended / kill
    system.tasks:   ">= 1"     -- per-process CPU, heap, uptime
    system.shell:   ">= 1"     -- statusbar, navbar
    display.theme:  ">= 1"
```

No user-prompt permissions. Uses only `system.*` and read-only display theme.

---

## 3. State Model

```deck
@machine TaskmanState
  state :list                              -- main list of running/suspended apps
  state :detail   (app_id: str)            -- per-app detail screen with kill button
  state :killing  (app_id: str)            -- transient: confirm dialog + kill in flight
  state :diagnostics                        -- system-wide stats screen

  initial :list

  transition :open_detail (id: str)
    from :list
    to   :detail (app_id: id)

  transition :back_to_list
    from :detail _
    to   :list
    from :diagnostics
    to   :list

  transition :request_kill (id: str)
    from :detail s
    to   :killing (app_id: id)

  transition :kill_done
    from :killing _
    to   :list

  transition :open_diagnostics
    from :list
    to   :diagnostics
```

---

## 4. Flow

```deck
@flow App
  state :main  machine: TaskmanState
  initial :main
```

---

## 5. Streams

```deck
@stream processes
  source: tasks.cpu_watch()
  -- ProcessEntry[] every 5s; CPU%, heap, uptime per VM and per @task

@stream apps_running
  source: apps.list_running_watch()
  -- AppInfo[] for currently-foreground + suspended apps
```

The CPU watch emits a fresh snapshot every 5 s. The Task Manager re-renders at most that often; the user perceives it as live.

---

## 6. Content Body

Per `02-deck-app §12`, content bodies declare semantic intent only. No layout primitives (`column`, `row`, `card`, `grid`, `status_bar`, `nav_bar`, `data_row`, `action_row`, `spacer`) — those are bridge decisions. `style: :dim`, `variant: :danger` are presentation hints the app must not carry. The statusbar and navbar are rendered by the bridge around every screen (`10-deck-bridge-ui §3`).

### 6.1 Main list

```deck
content =
  match state
    | :list ->
        group "RUNNING"
          list apps_running
            empty ->
              "NO APPS RUNNING"
            app ->
              navigate app.name -> TaskmanState.send(:open_detail, id: app.id)

        navigate "DIAGNOSTICS" -> TaskmanState.send(:open_diagnostics)
```

Each running app is a `navigate` — a semantic "go into" affordance that the bridge renders as a tappable list row with a disclosure arrow on this device, or as a spoken menu option on a voice bridge. `app.name` is the label; supplementary stats (`heap_kb`, `cpu_pct`, etc.) travel with the app record and are auto-formatted by the bridge when it renders the navigate row for an `@type` with those fields. Apps do not compose dim secondary lines themselves.

### 6.2 Detail screen

```deck
    | :detail id ->
        let app  = find_app(apps_running, id)
        let proc = find_proc(processes, id)

        group "{app.name}"
          app
          proc

          group "BACKGROUND TASKS"
            list filter(processes, p -> p.app_id == id and p.kind == :background)
              empty ->
                "NO BACKGROUND TASKS"
              bg ->
                bg

          confirm "FORCE KILL"  message: "Unsaved data will be lost."
            -> TaskmanState.send(:request_kill, id: id)
```

The detail view hands the bridge an `@type AppInfo` record (`app`) and an `@type ProcessInfo` record (`proc`), and the bridge renders their fields with labels derived from field names. There is no app-side label formatting (`"ID:"`, `"VERSION:"`, `"HEAP:"`). The `confirm` is a single semantic intent — the bridge decides when and how to present the confirmation dialog (`10-deck-bridge-ui §5.2`).

### 6.3 Killing

```deck
    | :killing id ->
        loading
```

The `confirm` above is a single intent; its interaction is the bridge's Confirm Dialog Service (`10-deck-bridge-ui §5.2`) — the app does not declare two separate labels for OK/CANCEL or a second `confirm` for the negative path. The `:killing` state shows a `loading` marker while `apps.kill(id)` completes; the machine transitions on completion.

### 6.4 Diagnostics

```deck
    | :diagnostics ->
        group "MEMORY"
          sysinfo.free_heap_internal()  -- labelled "FREE HEAP INTERNAL" by bridge
          sysinfo.free_heap_psram()
          pressure_label()

        group "BATTERY"
          battery.level()
          battery.charging()

        group "VERSIONS"
          sysinfo.versions()             -- bridge renders the record
```

`pressure_label()` returns an atom describing pressure; `battery.level()` returns a `float` (0..1) which the bridge formats as a percentage; `battery.charging()` returns a `bool` which the bridge renders as a yes/no indicator. No app-side formatting — the bridge owns presentation.

---

## 7. Lifecycle

```deck
@on launch
  -- nothing; streams attach lazily

@on resume
  -- nothing; streams remain subscribed across suspend/resume

@on os.app_launched (app_id: _)
  -- list updates automatically via apps_running stream

@on os.app_suspended (app_id: _)
  -- same

@on back
  match TaskmanState.state
    | :list         -> :unhandled         -- back exits TaskMan to whoever invoked it
    | :detail _     -> do
                        TaskmanState.send(:back_to_list)
                        :handled
    | :diagnostics  -> do
                        TaskmanState.send(:back_to_list)
                        :handled
    | :killing _    -> :handled           -- swallow back during kill confirmation
```

---

## 8. How the User Reaches Task Manager

Per `09-deck-shell §3.3` and `10-deck-bridge-ui §1.3`:

- **Long press on the home navbar button** (>600 ms) — the bridge intercepts, raises the Task Manager.
- The Task Manager is also tappable from the Launcher grid like any other app.

When raised via long-press, the Task Manager pushes onto the OS app stack on top of whatever was foreground. Back from `:list` returns to the previous app. Back from the Launcher grid still raises Task Manager normally.

---

## 9. Behavior While Itself the Foreground

- The Task Manager's CPU watch on its own VM produces a tiny self-loop in the data — that's expected and not filtered out (transparency over cleanliness).
- If the user tries to kill `system.taskman` from its own detail screen, the action is **rejected** with a toast "CANNOT KILL ACTIVE APP" — the kill button is disabled when `state = :detail "system.taskman"`.
- If the user tries to kill `system.launcher`, the bridge rejects it server-side (`apps.kill` returns `:err :unauthorized` for system apps that are protected). The Task Manager surfaces the error as a toast.

---

## 10. Memory Budget

The Task Manager's heap usage is small (~16 KB) — it holds at most:

- A list of `AppInfo` (one per running app, max ~32 entries × ~200 bytes)
- A list of `ProcessEntry` (typically < 50 entries × ~80 bytes)

It's never under memory pressure itself; in eviction order (`13-deck-cyberdeck-platform §9.4`), it's exempt as a system app.

---

## 11. Out of Scope for v1

- **Per-app network / disk I/O graphs.** Only CPU and heap are shown.
- **Time-series charts.** Single point-in-time snapshots.
- **Killing individual `@task`s within an app.** v1 only kills the whole app; per-task kill exists in `system.tasks.kill_task()` but is not exposed through TaskMan UI yet.
- **Process tree visualization.** Flat list only.
- **Notifications about high-CPU apps.** No proactive nagging.

These are roadmap. The Task Manager's v1 job is "let me see and kill what's running" — the absolute minimum for recovery.
