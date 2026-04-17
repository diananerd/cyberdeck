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

### 6.1 Main list

```deck
content =
  match state
    | :list ->
        column
          status_bar title: "TASK MANAGER"
          group "RUNNING"
            list apps_running
              item app ->
                row
                  icon  app.icon
                  column
                    text app.name
                    text "{format_heap_kb(app.heap_kb)} KB · {format_pct(app.cpu_pct)}"  style: :dim
                  on tap -> TaskmanState.send(:open_detail, id: app.id)
          spacer
          row
            navigate "DIAGNOSTICS" -> TaskmanState.send(:open_diagnostics)
          nav_bar
```

### 6.2 Detail screen

```deck
    | :detail id ->
        let app    = find_app(apps_running, id)
        let proc   = find_proc(processes, id)
        column
          status_bar title: app.name
          data_row label: "ID:"        value: app.id
          data_row label: "VERSION:"   value: app.version
          data_row label: "STATE:"     value: state_label(proc.state)
          data_row label: "HEAP:"      value: "{proc.heap_kb} KB"
          data_row label: "CPU:"       value: "{format_pct(proc.cpu_pct)} (5s avg)"
          data_row label: "UPTIME:"    value: format_duration_ms(proc.uptime_ms)
          group "BACKGROUND TASKS"
            list filter(processes, p -> p.app_id == id and p.kind == :background)
              item bg ->
                row
                  text bg.task_name
                  spacer
                  text format_pct(bg.cpu_pct)  style: :dim
          spacer
          action_row
            trigger "FORCE KILL"  variant: :danger
              -> TaskmanState.send(:request_kill, id: id)
          nav_bar
```

### 6.3 Killing (confirm + execute)

```deck
    | :killing id ->
        confirm "Force kill {find_app(apps_running, id).name}?"
          message: "Unsaved data will be lost."
          confirm: "KILL"   variant: :danger
            -> do
                apps.kill(id)
                TaskmanState.send(:kill_done)
          cancel:  "CANCEL"
            -> TaskmanState.send(:back_to_list)
```

### 6.4 Diagnostics

```deck
    | :diagnostics ->
        column
          status_bar title: "DIAGNOSTICS"
          group "MEMORY"
            data_row label: "FREE SRAM:"    value: "{sysinfo.free_heap_internal()} bytes"
            data_row label: "FREE PSRAM:"   value: "{sysinfo.free_heap_psram()} bytes"
            data_row label: "PRESSURE:"     value: pressure_label()
          group "BATTERY"
            data_row label: "LEVEL:"        value: "{battery.level()}%"
            data_row label: "CHARGING:"     value: yesno(battery.charging())
          group "VERSIONS"
            data_row label: "RUNTIME:"      value: sysinfo.versions().runtime
            data_row label: "DECK_OS:"      value: str(sysinfo.versions().deck_os)
            data_row label: "EDITION:"      value: str(sysinfo.versions().edition_current)
            data_row label: "SDI:"          value: "{v.sdi_major}.{v.sdi_minor}"
                                              where v = sysinfo.versions()
          spacer
          nav_bar
```

`pressure_label()` reads the latest `EVT_MEMORY_PRESSURE` from `app_state` (mirrored by the bridge into the runtime).

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
