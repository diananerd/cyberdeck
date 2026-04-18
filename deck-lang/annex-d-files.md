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

Per `02-deck-app §12`, content is semantic. No `column`, `row`, `status_bar`, `nav_bar`, `action_row`, `scroll`, `center`, `menu`, `icon`, `checkbox`, `image`, `font:`, `style:`, `variant:`, `when:` (as attribute — only `when` as content-level conditional is spec'd). `menu` is not a §12 primitive; a set of secondary actions is expressed as sibling intents and the bridge groups them (`10-deck-bridge-ui §5.5 Choice Overlay Service`, `§4.3` inference for overflow menus).

### 6.1 Browser

```deck
content =
  match state
    | :browsing s ->
        when s.path != "/sdcard" and s.path != "/spiffs"
          navigate "↑ Up" -> FilesState.send(:navigate_up)

        choice :sort  value: s.sort
          options: [
            (label: "Name",     value: :name_asc),
            (label: "Size",     value: :size_desc),
            (label: "Modified", value: :mtime_desc)
          ]
          on -> FilesState.send(:set_sort, sort: event.value)

        trigger "New folder" -> begin_new_folder()
        trigger "Select"     -> FilesState.send(:start_picker, mode: :copy)

        list s.entries
          empty ->
            "FOLDER EMPTY"
          entry ->
            navigate entry.name -> FilesState.send(:open, entry: entry)
```

Each `FsEntry` is `@type`'d with name/size/mtime/kind; the bridge renders those fields with its own layout (first line, dim secondary line, size on the right — on this device). Apps do not compose rows.

### 6.2 Picker / multi-select

```deck
    | :picker s ->
        multiselect :paths  value: s.selected
          options: map(s.entries, e -> (label: e.name, value: e.path))
          on -> FilesState.send(:toggle_pick, paths: event.value)

        match s.mode
          | :copy ->
              trigger "Copy here"
                -> FilesState.send(:copy_now, dst: current_path())
          | :move ->
              trigger "Move here"
                -> FilesState.send(:move_now, dst: current_path())
          | :delete ->
              confirm "Delete selected"  message: "Delete {len(s.selected)} item(s)?"
                -> confirm_delete(s.selected)

        trigger "Cancel"
          -> FilesState.send(:loaded, entries: s.entries)
```

### 6.3 Viewer

```deck
    | :viewer v ->
        match v.kind
          | :text        -> rich_text fs.read_text(v.path)
          | :markdown    -> markdown md.parse(fs.read_text(v.path))  purpose: :reading
          | :image       -> media    fs.asset_from_path(v.path)  alt: file_basename(v.path)
          | :hex         -> rich_text format_hex(fs.read(v.path, max: 4096))
          | :unsupported ->
              "PREVIEW NOT AVAILABLE"
              format_size(file_size(v.path))

        navigate "Details" -> show_details(v.path)
        trigger  "Rename"  -> FilesState.send(:start_rename, path: v.path)
        trigger  "Move"    -> FilesState.send(:start_move,   path: v.path)
        confirm  "Delete"  message: "Delete {file_basename(v.path)}?"
          -> confirm_delete([v.path])
        trigger  "Copy"    -> FilesState.send(:start_copy,   path: v.path)
```

`rich_text` is the §12.3 primitive for multi-line text content; the bridge picks the font (on this board: `CYBERDECK_FONT_MD`; a future mono preset is a bridge choice, not an app attribute). `markdown` handles the markdown case per §12.3 with the `purpose:` hint — the bridge decides density and code-block chrome. `media` handles the image (§12.3 positional `media expr alt:`). The `:unsupported` branch is bare expressions — the bridge displays them as two stacked text lines (details like "dim styling" are bridge decisions).

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
