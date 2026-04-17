# Annex D — Files
**Bundled OS App, Slot 3, `system.files`**

> Files lets the user browse, move, copy, rename, and delete contents of the SD card and the system flash partition. It is the recovery tool when an SD goes wrong, the staging area for moving media around, and the inspector for what's actually on the device. It is bundled (not SD-resident) because it must work when the SD itself is the problem.

---

## 1. Identity

```deck
@app
  name:    "Files"
  id:      "system.files"
  version: "1.0.0"
  edition: 2026
  entry:   App
  icon:    "FL"
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
    fs:              ">= 1"     -- file system browse / read / write
    storage.local:   ">= 1"     -- per-app storage inspector
    system.shell:    ">= 1"     -- statusbar, navbar
    system.apps:     ">= 1"     -- show "owned by app X" labels per directory
    system.tasks:    ">= 1"     -- per-app storage usage
    display.theme:   ">= 1"
    display.notify:  ">= 1"     -- toast on copy/move/delete
```

`fs` is a regular permission for user apps but auto-granted for `system.*`. Files reads and writes **outside** the per-app sandbox via a privileged variant of the `fs` capability — the SDI driver checks the caller's `app.id` and lifts the sandbox restriction for `system.files` only.

This privilege is special-cased in the SDI: `cap_fs.read("/sdcard/social.bsky.app/files/cache.db")` works for `system.files` but fails with `:permission` for any other app, including other `system.*` apps. The reasoning: cross-app data inspection is a privileged operation that should be locked to one well-audited app.

---

## 3. State Model

```deck
@machine FilesState
  state :browsing  (path: str, entries: [FsEntry], sort: SortMode)
  state :viewer    (path: str, kind: ViewerKind)        -- preview a file
  state :copying   (src: str, dst: str, progress: float, total_bytes: int)
  state :moving    (src: str, dst: str)
  state :deleting  (paths: [str])
  state :renaming  (path: str, new_name: str)
  state :picker    (mode: PickerMode, selected: [str])  -- multi-select for batch ops
  state :error     (message: str, retry_path: str?)

  initial :browsing (path: "/sdcard", entries: [], sort: :name_asc)

  on enter :browsing s ->
    -- Populate entries by listing path
    let result = fs.list(s.path)
    match result
      | :ok es  -> -- replace entries via :loaded
      | :err e  -> FilesState.send(:fs_error, message: error_msg(e))

  transition :loaded (entries: [FsEntry])
    from :browsing s
    to   :browsing (path: s.path, entries: sort_entries(entries, s.sort), sort: s.sort)

  transition :navigate (path: str)
    from :browsing _
    to   :browsing (path: path, entries: [], sort: :name_asc)

  transition :navigate_up
    from :browsing s
    to   :browsing (path: parent(s.path), entries: [], sort: s.sort)

  transition :open (entry: FsEntry)
    from :browsing s
    to   match entry.kind
      | :directory -> :browsing (path: entry.path, entries: [], sort: s.sort)
      | :file      -> :viewer (path: entry.path, kind: detect_kind(entry.name))

  transition :start_picker (mode: PickerMode)
    from :browsing _
    to   :picker (mode: mode, selected: [])

  transition :toggle_pick (path: str)
    from :picker s
    to   :picker (mode: s.mode, selected: toggle_in(s.selected, path))

  transition :copy_now (dst: str)
    from :picker s
    to   :copying (src: head(s.selected), dst: dst, progress: 0.0, total_bytes: 0)
    when: s.mode == :copy
    -- (multi-file copy iterates here; details elided)

  -- ... move / delete / rename mirror copy
```

`SortMode` = `:name_asc | :name_desc | :size_asc | :size_desc | :mtime_asc | :mtime_desc`.

`ViewerKind` = `:text | :image | :markdown | :hex | :unsupported`.

`PickerMode` = `:copy | :move | :delete`.

---

## 4. Flow

```deck
@flow App
  state :main  machine: FilesState
  initial :main
```

Single-state flow; the `FilesState` machine carries everything.

---

## 5. Streams

Files does not use streams — its data is request/response from `fs.list()`, `fs.stat()`, `fs.read()`. A filesystem watch capability is not in the v1 surface; if a user adds files via SD swap, Files re-lists on the next navigate or pull-to-refresh.

---

## 6. Content Body

### 6.1 Browser

```deck
content =
  match state
    | :browsing s ->
        column
          status_bar title: short_path(s.path)
          row
            navigate "↑"  on tap -> FilesState.send(:navigate_up)
              when: s.path != "/sdcard" and s.path != "/spiffs"
            spacer
            menu
              item "SORT BY NAME"     -> set_sort(:name_asc)
              item "SORT BY SIZE"     -> set_sort(:size_desc)
              item "SORT BY MODIFIED" -> set_sort(:mtime_desc)
              item "NEW FOLDER"       -> begin_new_folder()
              item "SELECT"           -> FilesState.send(:start_picker, mode: :copy)
          list s.entries
            item entry ->
              row
                icon  entry_icon(entry)
                column
                  text entry.name
                  text entry_subtitle(entry)  style: :dim
                spacer
                text format_size(entry.size)  style: :dim
                on tap   -> FilesState.send(:open, entry: entry)
                on long  -> FilesState.send(:start_picker, mode: :copy)
          nav_bar
```

`entry_subtitle` shows last modified time + (for top-level `apps/` subdirs) the app name owning that directory:

```deck
fn entry_subtitle (e: FsEntry) -> str =
  match owner_of(e.path)
    | :some app -> "{format_time(e.mtime)} · {app.name}"
    | :none     -> format_time(e.mtime)
```

### 6.2 Picker / multi-select

```deck
    | :picker s ->
        column
          status_bar title: "{len(s.selected)} SELECTED"
          list s.entries
            item entry ->
              row
                checkbox state: contains(s.selected, entry.path)
                  on -> FilesState.send(:toggle_pick, path: entry.path)
                column
                  text entry.name
                  text format_size(entry.size)  style: :dim
          spacer
          action_row
            trigger "CANCEL"
              -> FilesState.send(:loaded, entries: s.entries)
            trigger "COPY HERE"  variant: :primary  when: s.mode == :copy
              -> FilesState.send(:copy_now, dst: current_path())
            trigger "MOVE HERE"  variant: :primary  when: s.mode == :move
              -> FilesState.send(:move_now, dst: current_path())
            trigger "DELETE"     variant: :danger   when: s.mode == :delete
              -> confirm_delete(s.selected)
          nav_bar
```

### 6.3 Viewer

```deck
    | :viewer v ->
        column
          status_bar title: file_basename(v.path)
          match v.kind
            | :text ->
                scroll
                  text fs.read_text(v.path)  font: :mono  style: :small
            | :markdown ->
                scroll
                  markdown md.parse(fs.read_text(v.path))
            | :image ->
                center
                  image src: fs.asset_from_path(v.path)
            | :hex ->
                scroll
                  text format_hex(fs.read(v.path, max: 4096))  font: :mono  style: :small
            | :unsupported ->
                center
                  text "PREVIEW NOT AVAILABLE"  style: :dim
                  text format_size(file_size(v.path))  style: :dim
          row
            navigate "DETAILS"  -> show_details(v.path)
            spacer
            menu
              item "RENAME"  -> FilesState.send(:start_rename, path: v.path)
              item "MOVE"    -> FilesState.send(:start_move, path: v.path)
              item "DELETE"  -> confirm_delete([v.path])
              item "COPY"    -> FilesState.send(:start_copy, path: v.path)
          nav_bar
```

The `markdown` and `image` viewer modes depend on the corresponding capabilities being available; if not, the viewer falls back to `:text` or `:hex`.

### 6.4 Copy / move progress

```deck
    | :copying s ->
        progress
          title: "COPYING {file_basename(s.src)}"
          value: s.progress
          cancellable: true
          on cancel -> abort_copy()
```

The bridge auto-renders the Progress Overlay (`10-deck-bridge-ui §5.11`) for this state.

---

## 7. Mount Roots

The Files app exposes two top-level roots in the browser:

```
/sdcard/                   -- user-facing SD card (apps, system, tmp, media, trash)
/spiffs/                   -- system flash partition (snapshots, crashes, ota, assets)
```

Navigating up from `/sdcard` shows a chooser between the two. `/spiffs` is read-only by default (Files presents a banner: "READ-ONLY — system files; use only for inspection") with an admin toggle in Settings → Diagnostics → Allow system writes.

---

## 8. Lifecycle

```deck
@on launch
  -- nothing; first browse populates on enter

@on resume
  -- Re-list current directory in case files changed while suspended
  FilesState.send(:refresh)

@on os.storage_changed (mounted: m)
  match m
    | true  -> FilesState.send(:refresh)
    | false -> FilesState.send(:fs_error, message: "SD card removed", retry_path: :none)

@on back
  match state
    | :browsing s ->
        match s.path
          | "/sdcard" -> :unhandled                 -- back exits to Launcher
          | _         -> do
                          FilesState.send(:navigate_up)
                          :handled
    | _           -> do
                      FilesState.send(:back_to_browsing)
                      :handled
```

---

## 9. Operations

### 9.1 Copy

Implemented via streamed read + write through the `fs` capability:

```deck
fn do_copy (src: str, dst: str) -> Result unit fs.Error !fs =
  let total = fs.size(src)
  let writer = fs.open_writer(dst)
  let result = fs.read_chunked(src, 4096, chunk -> do
    fs.write(writer, chunk)
    let pct = float(bytes_so_far) / float(total)
    FilesState.send(:progress, pct: pct)
  )
  fs.close_writer(writer)
  result
```

Copy across mount points (e.g. SD → /spiffs) is allowed but rate-limited (write to `/spiffs` is slower; user is warned).

### 9.2 Move

Within the same filesystem: `fs.rename(src, dst)` (atomic). Across filesystems: copy + delete.

### 9.3 Delete

Moves the file to `/sdcard/trash/{date}/{original_path}` instead of unlinking. The trash is wiped weekly by `svc_storage` (per `13-deck-cyberdeck-platform §7.5`). Holding "DELETE" for 1 s offers a "permanent delete" option that bypasses the trash.

Files in `/spiffs/` cannot be moved to trash (no trash partition). `/spiffs/` deletes are immediate and permanent — confirm dialog is more emphatic.

### 9.4 Rename

`fs.rename(path, new_path)` within the same directory. The new name is validated for `/`, `..`, and reserved characters (FAT FS rules).

---

## 10. Per-App Storage Inspector

Tapping a top-level subdirectory under `/sdcard/apps/` reveals a small per-app summary header:

```
{app_name}
{app_id}     installed v1.0.0     32 KB total
files/       12 KB                 -- per-app FS sandbox
cache/       8 KB                  -- per-app cache
{app_id}.db  12 KB                 -- per-app SQLite
```

A "MANAGE APP" button jumps directly to Settings → Apps → {app_id} for permissions, config, and uninstall. A "CLEAR CACHE" button calls `cache.clear(app_id)` after a confirm dialog.

---

## 11. Edge Cases

| Situation | Behavior |
|---|---|
| SD removed mid-browse | Toast "SD CARD REMOVED"; state moves to `:error`; back returns to `/spiffs` root |
| File too large to preview | Viewer shows `:unsupported` placeholder with size |
| Path with non-ASCII characters | FAT FS supports LFN with UTF-16; rendering uses Montserrat (no full Unicode) — non-Latin chars render as `?` |
| Symlinks | Not supported on FAT FS; `fs.list` does not return any |
| Permissions denied (`/spiffs/` write while admin off) | Toast "READ-ONLY MODE"; action disabled |
| Free space < 64 KB | Banner at top of every screen: "LOW DISK SPACE — {free} KB free" |

---

## 12. Out of Scope for v1

- **Background/queued operations.** All copies run inline with progress; closing Files cancels.
- **Network mounts.** No SMB / WebDAV.
- **Archive support.** No tar/zip create/extract; viewer shows them as `:hex`.
- **Search.** No filesystem search; locate-style indexing is too costly for the SD bandwidth.
- **Permissions / chmod UI.** FAT FS has no permissions to show.
- **File sharing via Share Sheet.** Out of scope; the user copies via SD swap.

The Files app's v1 job is to make the on-device storage **inspectable and recoverable**. Power-user features are deferred.
