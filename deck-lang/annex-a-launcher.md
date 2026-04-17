# Annex A — Launcher
**Bundled OS App, Slot 0, `system.launcher`**

> The Launcher is the home screen of CyberDeck. It is always running, always at the bottom of the OS app stack, and is the only app the user can never close. Every other app is reached through it.

---

## 1. Identity

```deck
@app
  name:    "Launcher"
  id:      "system.launcher"
  version: "1.0.0"
  edition: 2026
  entry:   App
  icon:    "LN"
  orientation: :any
```

System-privileged (`app.id` starts with `system.`). Bundled into the firmware image at build time; cannot be uninstalled or replaced via app OTA — only via firmware OTA.

---

## 2. Capabilities

```deck
@requires
  deck_os: ">= 1"
  runtime: ">= 1.0"
  capabilities:
    system.apps:    ">= 1"     -- list installed / launch / suspend
    system.shell:   ">= 1"     -- statusbar, navbar
    system.security: ">= 1"    -- lock, auto-lock timer
    display.theme:  ">= 1"     -- read current theme
    notifications:  ">= 1"     -- read unread counts for badges
```

`@permissions` block is empty — system apps inherit grant on `system.*` capabilities; `notifications` and `display.theme` do not require user prompts on a system app.

---

## 3. State Model

```deck
@machine LauncherState
  state :grid                              -- normal app grid
  state :search   (query: str)             -- global search overlay
  state :empty                             -- no installed apps + no SD detected

  initial :grid

  transition :open_search
    from :grid
    to   :search (query: "")

  transition :update_query (q: str)
    from :search _
    to   :search (query: q)

  transition :close_search
    from :search _
    to   :grid

  transition :sd_present
    from :empty
    to   :grid

  transition :sd_absent
    from :grid
    to   :empty
    watch: not (storage is :mounted) and apps.installed_count() == 0
```

---

## 4. Flow

```deck
@flow App
  state :home    machine: LauncherState
  initial :home
```

Single-state flow. The Launcher's complexity is content, not navigation.

---

## 5. Streams

```deck
@stream installed_apps
  source: apps.list_installed_watch()
  -- AppInfo[] from system.apps; emits on app install / uninstall / version change

@stream notif_counts
  source: apps.notif_counts_watch()
  -- (app_id, unread) pairs; the launcher overlays unread badges on grid icons
```

The Launcher does **not** poll. It subscribes to the two streams and re-renders on every emission. Both streams are restricted to `system.*` apps and unavailable to user apps.

---

## 6. Content Body

```deck
content =
  match state
    | :empty ->
        column
          spacer
          text "NO APPS INSTALLED"      style: :dim center
          text "INSERT SD CARD"         style: :dim center
          spacer
    | :grid ->
        column
          status_bar
          grid cols: orientation_grid_cols()
            for app in installed_apps
              card
                icon  app.icon
                label app.name
                badge unread_count(notif_counts, app.id)
                on tap   -> apps.launch(app.id)
                on long  -> LauncherState.send(:open_search)
          nav_bar
    | :search q ->
        column
          status_bar title: "SEARCH"
          search :query value: q
            on -> LauncherState.send(:update_query, q: event.value)
          list filtered(installed_apps, q)
            item app ->
              row
                icon  app.icon
                label app.name
                on tap -> do
                  apps.launch(app.id)
                  LauncherState.send(:close_search)
          nav_bar
```

`orientation_grid_cols()` is a small helper:

```deck
fn orientation_grid_cols () -> int =
  match orientation
    | :portrait  -> 3
    | :landscape -> 5
```

---

## 7. Lifecycle

```deck
@on launch
  -- Nothing special. Streams subscribe lazily on first content render.

@on resume
  -- Refresh badge counts in case they drifted while the launcher was suspended
  -- (rare; the launcher is rarely suspended).

@on os.theme_changed (_)
  -- Bridge re-renders automatically; no manual action needed.

@on os.storage_changed (mounted: m)
  match m
    | true  -> LauncherState.send(:sd_present)
    | false -> when apps.installed_count() == 0
                 LauncherState.send(:sd_absent)

@on back
  -- The launcher is the bottom of the stack. Back from the launcher does nothing.
  :handled
```

Note that `@on back` returning `:handled` is what makes the Launcher unswipe-from-able: the OS sees the back as consumed and does nothing further.

---

## 8. Layout Inference

| Form factor | Grid layout | Card icon font |
|---|---|---|
| Portrait (480×800) | 3 columns × N rows, scrollable | `CYBERDECK_FONT_XL` (40 px) |
| Landscape (800×480) | 5 columns × N rows, scrollable | `CYBERDECK_FONT_XL` |

Cards are `~140×140 px` in portrait, `~140×120 px` in landscape. Tap targets meet a 44 px minimum on every theme.

A "stub slot" appears for any app slot in the manifest registry that has no installed bundle (e.g. an app referenced by another app's deep link but not installed). Stubs render with `primary_dim` color and a `?` icon. Tapping a stub fires `display.notify "Coming soon..."` for 1500 ms (per `10-deck-bridge-ui §5.10`).

---

## 9. Search

The Launcher's search overlay (state `:search`) filters by:

- Substring match on `name` (case-insensitive)
- Substring match on `id`
- Tag match: `@app tags: [...]` — apps may declare hashtags for grouping

Search is **local only** — it does not query a registry. It is a quick way to launch by typing on devices with on-screen keyboards.

---

## 10. Interactions With Other Bundled Apps

- **Settings**: launched via tap on its grid card or via long-press on any other card → "App settings" → routes through `system.apps.config_schema(app_id)` to a Settings sub-screen.
- **Task Manager**: launched via the long-press on the home navbar button (gesture handled by the bridge UI service, `10-deck-bridge-ui §1.3`); the Launcher itself does not contain a Task Manager affordance.
- **Files**: launched via tap on its grid card. No special integration — the Launcher does not deep-link into Files.

---

## 11. Boot Sequence Position

Per `13-deck-cyberdeck-platform §23`, the Launcher is the **first VM created** at boot, in slot 0. The bridge:

1. Loads the embedded launcher source (`components/apps_bundled/system_launcher/`).
2. Runs the loader (~50 ms).
3. Runs `@on launch` (just stream subscription setup).
4. Emits the first DVC tree.
5. Bridge renders → the user sees the launcher (~600 ms after power-on).

The Launcher VM never enters slot 1, 2, or 3 of the runtime task pool. It is the bottom of the OS app stack and never evicted under memory pressure (`13-deck-cyberdeck-platform §9.4`).

---

## 12. Out of Scope for v1

- **Folders / categories.** The grid is flat; tagging exists for search but not visual grouping.
- **Wallpapers / customizable backgrounds.** The bg is always `bg_dark`.
- **Widgets on the home screen.** The Launcher is purely a launch target list. Widget-style behavior lives in the relevant app.
- **Drag-to-reorder.** Apps appear in install order. A future version may add a manual sort.
- **Recents / suggested.** No prediction; the user picks deliberately.

These are intentional v1 limits. The Launcher's job is to be invisible — start an app fast, get out of the way.
