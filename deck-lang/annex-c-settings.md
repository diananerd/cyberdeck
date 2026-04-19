# Annex C — Settings
**Bundled OS App, Slot 2, `system.settings`**

> Settings is where the user configures the device — networks, theme, brightness, language, time, security, audio, OTA, diagnostics, app permissions, crashes. Without it, the user cannot bring up WiFi, change the PIN, or update firmware. It is the second-most essential app after the Launcher.

---

## 1. Identity

```deck
@app
  name:    "Settings"
  id:      "system.settings"
  version: "1.0.0"
  edition: 2026
  entry:   App
  icon:    "ST"
  orientation: :any
```

System-privileged. Bundled. Cannot be uninstalled.

---

## 2. Capabilities

Settings touches almost everything the OS exposes, because it surfaces almost everything to the user.

```deck
@requires
  deck_level: 3                 -- system.apps, system.crashes, ota, notifications, ble are DL3 (see 16-deck-levels §7.3)
  deck_os: ">= 1"
  runtime: ">= 1.0"
  capabilities:
    -- Privileged system access
    system.shell:    ">= 1"
    system.apps:     ">= 1"      -- per-app config schemas, app uninstall
    system.tasks:    ">= 1"      -- (read-only; see Diagnostics)
    system.security: ">= 1"      -- PIN, lockscreen, permission grants
    system.crashes:  ">= 1"      -- Diagnostics → Crashes tab

    -- User-facing surfaces
    network.wifi:    ">= 1"
    display.theme:   ">= 1"
    display.screen:  ">= 1"      -- brightness, screen timeout
    system.locale:   ">= 1"      -- timezone, language
    system.time:     ">= 1"      -- NTP, manual time set
    system.battery:  ">= 1"
    system.audio:    optional    -- volume only if BT audio module is present
    ble:             optional    -- BLE pairing UI only if BLE is available
    bt_classic:      optional    -- BT pairing UI only if external module is present

    -- Updates
    ota:             ">= 1"
    notifications:   ">= 1"      -- to receive os.notification for OTA progress

@permissions
  -- All system.* are auto-granted to system apps; no user prompt.
  -- network.wifi requires no @permissions entry on system apps.
  -- notifications: implicitly granted.
```

---

## 3. State Model

The state machine is **flat at the top level** — settings categories. Each category has its own sub-machine where complex (WiFi pairing, PIN entry, etc.).

```deck
@machine SettingsState
  state :menu                              -- top-level category list
  state :wifi          flow: WifiFlow
  state :display       flow: DisplayFlow
  state :theme         flow: ThemeFlow
  state :sound         flow: SoundFlow
  state :time          flow: TimeFlow
  state :language      flow: LanguageFlow
  state :security      flow: SecurityFlow
  state :network_other flow: NetworkOtherFlow      -- BLE, BT classic
  state :apps          flow: AppsFlow              -- per-app permissions / config / uninstall
  state :ota           flow: OtaFlow
  state :diagnostics   flow: DiagnosticsFlow       -- crashes, versions, memory, logs
  state :about         flow: AboutFlow             -- device info, credits

  initial :menu

  transition :open_category (cat: atom)
    from :menu
    to   match cat
      | :wifi          -> :wifi
      | :display       -> :display
      | :theme         -> :theme
      | :sound         -> :sound
      | :time          -> :time
      | :language      -> :language
      | :security      -> :security
      | :network_other -> :network_other
      | :apps          -> :apps
      | :ota           -> :ota
      | :diagnostics   -> :diagnostics
      | :about         -> :about

  transition :back_to_menu
    from *
    to   :menu
```

Each child flow defines its own internal navigation. Examples for WiFi (`WifiFlow`) and Security (`SecurityFlow`) below.

---

## 4. Flows (sub-flows per category)

### 4.1 WifiFlow

```deck
@flow WifiFlow
  state :scanning
  state :networks  (results: [WifiAP])
  state :prompt    (ssid: str, requires_psk: bool)
  state :connecting (ssid: str)
  state :failed    (ssid: str, reason: str)

  initial :scanning

  on enter :scanning ->
    wifi.scan_async()       -- emits os.wifi_scan_done

  transition :got_results (results: [WifiAP])
    from :scanning
    to   :networks (results: results)
    watch: networks_event

  transition :pick (ap: WifiAP)
    from :networks _
    to   :prompt (ssid: ap.ssid, requires_psk: ap.encrypted)

  transition :connect (psk: str)
    from :prompt s
    to   :connecting (ssid: s.ssid)
    before -> wifi.connect(s.ssid, psk)

  transition :connected
    from :connecting _
    to   :networks (results: scan_cache())
    watch: WifiState is :connected

  transition :failed_to_connect (reason: str)
    from :connecting s
    to   :failed (ssid: s.ssid, reason: reason)
    watch: WifiState is :failed
```

### 4.2 SecurityFlow

```deck
@flow SecurityFlow
  state :menu
  state :set_pin     (digits: str)
  state :confirm_pin (first: str, digits: str)
  state :saved
  state :clear_confirm
  state :permissions

  initial :menu

  transition :start_set_pin
    from :menu
    to   :set_pin (digits: "")

  transition :digit (d: str)
    from :set_pin s
    to   :set_pin (digits: append(s.digits, d))

  transition :first_pin_done
    from :set_pin s
    to   :confirm_pin (first: s.digits, digits: "")
    when: text.len(s.digits) == 4

  transition :confirm_digit (d: str)
    from :confirm_pin s
    to   :confirm_pin (first: s.first, digits: append(s.digits, d))

  transition :confirm_done
    from :confirm_pin s
    to   :saved
    when: text.len(s.digits) == 4 and s.digits == s.first
    before -> security.set_pin(s.digits)

  transition :mismatch
    from :confirm_pin s
    to   :set_pin (digits: "")
    when: text.len(s.digits) == 4 and s.digits != s.first
    before -> notify.send("PINS DO NOT MATCH", :error)

  transition :clear_pin
    from :menu
    to   :clear_confirm

  transition :do_clear
    from :clear_confirm
    to   :menu
    before -> security.clear_pin()

  transition :open_permissions
    from :menu
    to   :permissions
```

The other categories (`DisplayFlow`, `ThemeFlow`, `TimeFlow`, etc.) follow the same pattern: a flat sub-flow with one screen per setting, auto-saved on change.

---

## 5. Streams

```deck
@stream wifi_status   source: wifi.status_watch()
@stream battery       source: battery.watch()
@stream theme         source: theme.watch()
```

Each settings category that displays live state subscribes to the relevant stream so toggles and indicators update without manual refresh.

---

## 6. Top-Level Menu

Per `02-deck-app §12`, content is semantic. No `column`, `status_bar`, `nav_bar`, `nav_row`, `icon:`. The top-level settings list is a sequence of `navigate` intents; the bridge lays them out as a tappable list with disclosures on this board.

```deck
content =
  match SettingsState.state
    | :menu ->
        navigate "WIFI"        -> SettingsState.send(:open_category, cat: :wifi)
        navigate "DISPLAY"     -> SettingsState.send(:open_category, cat: :display)
        navigate "THEME"       -> SettingsState.send(:open_category, cat: :theme)
        navigate "SOUND"       -> SettingsState.send(:open_category, cat: :sound)
        navigate "TIME"        -> SettingsState.send(:open_category, cat: :time)
        navigate "LANGUAGE"    -> SettingsState.send(:open_category, cat: :language)
        navigate "SECURITY"    -> SettingsState.send(:open_category, cat: :security)
        navigate "BLE / BT"    -> SettingsState.send(:open_category, cat: :network_other)
        navigate "APPS"        -> SettingsState.send(:open_category, cat: :apps)
        navigate "UPDATES"     -> SettingsState.send(:open_category, cat: :ota)
        navigate "DIAGNOSTICS" -> SettingsState.send(:open_category, cat: :diagnostics)
        navigate "ABOUT"       -> SettingsState.send(:open_category, cat: :about)

    | :wifi          -> -- WifiFlow renders its own content
    | :security      -> -- SecurityFlow renders its own
    | -- ...
```

Showing a per-row **detail summary** (like the current SSID next to "WIFI") is a presentation concern. The app exposes the data via a stream or record field; the bridge chooses whether to show it as a secondary line, an icon, or not at all, based on available space. Apps do not author the summary text placement.

---

## 7. Auto-save Pattern

Settings categories follow **auto-save with no Save button**:

- WiFi: tapping "CONNECT" triggers immediate connection attempt; the result reflects in the next state.
- Theme: tapping a theme tile immediately fires `theme.set(:matrix)` and the bridge re-renders globally.
- Brightness: slider drag fires `display.screen.set_brightness(level)` per scrub-end.
- Volume: same as brightness.
- Locale / timezone: tap-to-pick from a list; persisted on tap.

Only **destructive** actions (clear PIN, factory reset, uninstall app, force OTA rollback) require explicit confirmation via the bridge's confirm dialog.

---

## 8. App Settings (`AppsFlow`)

```deck
@flow AppsFlow
  state :list
  state :detail   (app_id: str, schema: [ConfigFieldInfo])

  initial :list

  on enter :list ->
    -- nothing; list is populated from system.apps.list_installed()

  transition :open (id: str)
    from :list
    to   :detail (app_id: id, schema: apps.config_schema(id))

content :detail s =
  group "PERMISSIONS"
    for (cap, granted) in security.permission_state_all(s.app_id)
      toggle cap  state: granted
        on -> security.permission_set(s.app_id, cap, event.value)

  group "CONFIG"
    for field in s.schema
      config_input field
        on -> apps.config_set(s.app_id, field.name, event.value)

  confirm "UNINSTALL"  prompt: "Remove {app_name(s.app_id)} and all its data?"
    -> confirm_uninstall(s.app_id)
```

`config_input` is an app-defined helper that dispatches to the right §12.4 intent based on `field.type`: `range` for `:int`/`:float`, `toggle` for `:bool`, `choice` for `:atom` with options, `text` for `:str`. All four are spec primitives; the helper just picks which one for each schema field. There is no "danger zone" grouping declared by the app — `confirm` is semantically the destructive action and the bridge renders it accordingly (`10-deck-bridge-ui §4.3` destructive-action styling).

System apps (Launcher, TaskMan, Settings, Files) appear in the list but UNINSTALL is dimmed and tapping it shows a toast "CANNOT UNINSTALL SYSTEM APP".

---

## 9. Diagnostics (Crash Reporter inside Settings)

```deck
@flow DiagnosticsFlow
  state :menu
  state :crashes      (entries: [CrashInfo])
  state :crash_detail (info: CrashInfo)
  state :memory
  state :logs
  state :versions

  initial :menu

  on enter :crashes ->
    -- Read crash log via system.crashes
    let entries = crashes.list()
    -- transition to :crashes with entries

  transition :open_crash (info: CrashInfo)
    from :crashes _
    to   :crash_detail (info: info)
```

The Crash Reporter `@on crash_report` hook lives in this app:

```deck
@on crash_report (info: CrashInfo)
  -- Fired when ANY app on the device panics. The OS already persisted info via
  -- system.crashes. We surface it as a notification.
  notif.post_local(
    title:   "{info.app_id} crashed",
    body:    info.message,
    url:     :some "settings://diagnostics/crashes/{info.app_id}"
  )
```

Per `13-deck-cyberdeck-platform §7.4`, the Loader rule that restricts `@on crash_report` to `system.crash_reporter` is amended to also accept `system.settings`.

---

## 10. Updates (`OtaFlow`)

```deck
@flow OtaFlow
  state :idle
  state :checking
  state :available  (info: OtaInfo)
  state :downloading (info: OtaInfo, progress: float)
  state :ready     (info: OtaInfo)
  state :validation_window (deadline: Timestamp)
  state :failed    (reason: str)

  initial :idle

  transition :check
    from :idle
    to   :checking
    before -> ota.check_async()

  transition :got_check (info: OtaInfo?)
    from :checking
    to   match info
      | :some i  -> :available (info: i)
      | :none    -> :idle
    watch: ota_check_event

  transition :download
    from :available i
    to   :downloading (info: i.info, progress: 0.0)
    before -> ota.download(i.info.url)

  transition :progress (pct: float)
    from :downloading s
    to   :downloading (info: s.info, progress: pct)
    watch: ota_progress_event

  transition :downloaded
    from :downloading s
    to   :ready (info: s.info)
    when: pct >= 1.0

  transition :apply
    from :ready _
    to   :idle
    before -> ota.apply()       -- triggers reboot
```

After reboot, the new firmware enters the validation window (per `13-deck-cyberdeck-platform §13.4`); user interaction commits.

---

## 11. Lifecycle

```deck
@on launch
  -- Subscribe to streams; populate menu

@on resume
  -- Re-fetch any data that may have changed while suspended

@on os.config_change (field: _, value: _)
  -- Some other system change happened (e.g., another setting was changed via
  -- system.shell). Re-render to reflect.

@on back
  match SettingsState.state
    | :menu        -> :unhandled
    | _            -> do
                       SettingsState.send(:back_to_menu)
                       :handled
```

---

## 12. Boot-Time Recovery Mode

If the device boots into recovery mode (3 consecutive boot failures, per `13-deck-cyberdeck-platform §12.2`), the bootloader presents a minimal Settings UI with only:

- Network → connect to WiFi
- Updates → force a firmware OTA
- Diagnostics → view the most recent crash

This recovery Settings is the **same Deck source** as the normal Settings, with a runtime feature flag that hides categories that depend on a working SD card.

---

## 13. Out of Scope for v1

- **Profile / multi-user.** Single user.
- **Backup & restore.** No cloud sync; the user copies `/sdcard/system/` manually.
- **Developer mode toggles** (USB MSC, hot reload). Hidden behind a tap-version-7-times easter egg in About; deferred.
- **Quick-toggles in statusbar** (drag-down panel). Out of scope; everything is in Settings.
- **Per-app battery usage charts.** Diagnostics shows totals only.

The Settings app's v1 job is to surface every setting clearly and make destructive ones explicit. Polish lives elsewhere.
