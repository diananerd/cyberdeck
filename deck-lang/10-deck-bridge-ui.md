# CyberDeck Bridge: UI Reference
**Version 1.0 — Board: Waveshare ESP32-S3-Touch-LCD-4.3**

---

## Modelo fundamental

**Deck apps no saben cómo se van a dibujar.**

Una app Deck declara relaciones: qué datos existen, qué estados son posibles, qué acciones puede tomar el usuario, qué información quiere mostrar. Todo esto vive en `@machine`, `@flow`, `@task`, y bloques `content =`. No hay coordenadas, no hay colores, no hay widgets, no hay "layout".

El **bridge** recibe esa descripción semántica y decide cómo materializarla para este hardware específico. Decide el layout, los widgets, los colores, las animaciones, los gestos. Decide cuándo un `DVC_CONFIRM` merece un modal en lugar de un inline button, cuándo un `DVC_TEXT` vacío debe mostrar el teclado, cuándo un `DVC_LOADING` debe bloquear toda la pantalla. El bridge infiere todo esto del contexto — la app nunca se entera.

En otro hardware (e-ink, voz, smartwatch), el mismo `.deck` corre contra un bridge diferente que toma decisiones completamente distintas para el mismo contenido semántico.

Este documento especifica las decisiones concretas del CyberDeck LVGL bridge para el Waveshare ESP32-S3-Touch-LCD-4.3.

---

Para el resumen base del bridge ver `06-deck-native §22`. Este documento lo expande en la referencia completa y autoritativa.

---

## 1. Board & Display

| Property | Value |
|---|---|
| SoC | ESP32-S3, dual-core Xtensa LX7 |
| Display | 800×480 px, RGB LCD (16-bit parallel) |
| Touch | GT911 capacitive, 5-point, I2C (SDA=GPIO8, SCL=GPIO9) |
| PSRAM | 8 MB (OPI) |
| Flash | 8 MB |
| UI library | LVGL 8.4.0 |
| UI core | Core 1 (`lvgl_task`) |
| Runtime core | Core 0 (`deck_runtime_task`) |

### 1.1 Orientation

The bridge supports two orientations. The active orientation is read from NVS via `svc_settings` at boot and stored in `app_state_get()->display_rotation`.

| Orientation | Resolution (W×H) | Navbar position | Usable content area |
|---|---|---|---|
| Portrait | 480×800 | Bottom (height 60 px) | 480×704 px |
| Landscape | 800×480 | Right edge (width 60 px) | 740×444 px |

**Touch coordinates are always passed raw to LVGL.** LVGL's `lv_indev.c` applies the rotation transform internally. The bridge `touchpad_read` callback never transforms coordinates.

Rotation is triggered by `EVT_DISPLAY_ROTATED`. The bridge calls `ui_activity_recreate_all()` — every active screen calls `on_destroy` then `on_create` in stack order. Screens reconstruct from `app_state_get()` and current machine state; `intent_data` must be treated as possibly NULL on recreate.

### 1.2 Hardware Input Constraints

- **No runtime-usable hardware buttons.** GPIO 0 is owned by the RGB LCD peripheral (DATA6) — it cannot be used as a button.
- **Navigation is touch-only.** All HOME/BACK/TASK-SWITCH events come from touch gestures or on-screen navbar buttons.
- **No Bluetooth Classic.** A2DP audio is not possible on ESP32-S3. Audio output is via an optional external BT module on UART1 (GPIO 15/16), auto-detected at boot.

### 1.3 Touch Gesture Zones

The GT911 reports raw touch points. The bridge processes gestures in a pre-pass before dispatching to LVGL:

| Gesture | Motion | Portrait | Landscape |
|---|---|---|---|
| HOME | Swipe up from bottom edge | ≥60 px upward in bottom 80 px strip | ≥60 px upward in bottom 80 px strip |
| BACK | Swipe down from top edge | ≥60 px downward in top 80 px strip | ≥60 px downward in top 80 px strip |
| TASK-SWITCH | Long press (>600 ms) on home navbar button | — | — |

Gestures that don't match are passed through to LVGL as normal touch events. The gesture detector runs at the bridge layer, not in LVGL indev.

When the nav lock is active (lockscreen), HOME and BACK gestures are swallowed by the bridge — LVGL never sees them.

---

## 2. Design Language

The terminal-aesthetic grammar is applied by the bridge. Deck never specifies colors, fonts, or layout — only semantic intent.

### 2.1 Themes

Three runtime-selectable themes, stored in `svc_settings`, applied via `ui_theme_get()`. Switching applies instantly — bridge re-renders all active screens on `EVT_SETTINGS_CHANGED (key: "theme")`.

| Field | Green (Matrix) | Amber (Retro) | Neon (Cyberpunk) |
|---|---|---|---|
| `primary` | `#00FF41` | `#FFB000` | `#FF00FF` |
| `primary_dim` | `#004D13` | `#4D3500` | `#4D004D` |
| `bg_dark` | `#000000` | `#000000` | `#000000` |
| `bg_card` | `#0A0A0A` | `#0A0A0A` | `#0A0A0A` |
| `text_dim` | `primary` at `LV_OPA_60` | `primary` at `LV_OPA_60` | `primary` at `LV_OPA_60` |
| `secondary` | — | — | `#00FFFF` |
| `accent` | — | — | `#FF0055` |
| `success` | — | — | `#39FF14` |

**Usage rules:**
- `primary` — active borders, selected items, data values, focused inputs, icon fills, CTA button text
- `primary_dim` — inactive borders, stub/disabled items, dialog title fill, inactive navbar elements
- `text_dim` — secondary labels, captions, field name prefixes, dim hints
- `bg_card` — slightly raised surface (cards, list containers) to separate from pure-black screen background
- `accent` — destructive action labels and icons (Neon theme only; other themes use `primary`)
- `success` — confirmed/done states (Neon theme only; other themes use `primary`)
- Overlay backdrops: `LV_OPA_50` (confirm dialog), `LV_OPA_70` (loading overlay)
- Button press feedback: invert fill — `bg` becomes `primary`, text becomes `bg_dark`

### 2.2 Typography

Montserrat font family. All text is **ALL CAPS** except toast messages ("Coming soon..." is the canonical exception).

| Alias | Font | Use |
|---|---|---|
| `CYBERDECK_FONT_SM` | Montserrat 18 | Statusbar labels, captions, dim field labels, list secondary line, toast text |
| `CYBERDECK_FONT_MD` | Montserrat 24 | Body text, list primary line, data values, button labels, form field text |
| `CYBERDECK_FONT_LG` | Montserrat 32 | App card icons in grid |
| `CYBERDECK_FONT_XL` | Montserrat 40 | System clock, launcher app icons, PIN dot display, loading cursor |

Field name labels always end with colon-space: `"SSID:"`, `"IP:"`, `"CHANNEL:"`. Status bar app title is always `"CYBERDECK"` (the product name) for system apps; user apps show their `app.name` ALL-CAPS.

**Symbol rules:** Never use raw UTF-8 codepoints for arrows, bullets, or icons — Montserrat does not include those codepoints and they render as rectangles. Use only:
- `LV_SYMBOL_*` macros for LVGL built-in symbols
- Canvas-drawn graphics for custom icons (navbar back arrow)

### 2.3 Spacing System

| Context | Value |
|---|---|
| Content area `pad_all` | 16 px |
| Flex column `pad_row` | 14 px |
| Section gap (between unrelated groups) | 18 px transparent spacer |
| Section label `pad_top` | 20 px |
| List item `pad_top/bottom` | 12 px |
| List item `pad_left/right` | 8 px |
| Grid gap (launcher, theme buttons) | 12–16 px |
| Card icon-to-name gap | 4 px |
| Dialog `pad_all` | 24 px |
| Dialog `pad_row` | 16 px |
| Dialog button gap spacer | 24 px transparent |
| Dialog button row `pad_column` | 8 px |
| Toast `pad_horizontal` | 12 px |
| Toast `pad_vertical` | 6 px |

### 2.4 Borders, Radius, and Dimensions

| Widget | Border width | Radius | Border side |
|---|---|---|---|
| Card / major container | 2 px | 12 px | all |
| Launcher app card | 2 px | 16 px | all |
| Button | 2 px | 12 px | all |
| Toast | 1 px | 2 px | all |
| Dialog | 1 px | 2 px | all |
| Text input / textarea | — | 8 px | all |
| List item row | 1 px | 0 | BOTTOM only |
| Navbar | 2 px | 0 | TOP (portrait) / LEFT (landscape) |
| Statusbar separator | 2 px height | 0 | top + bottom edges |
| Scrollbar | 2 px wide | 0 | — |
| PIN numpad button | — | 6 px | all |
| Badge circle | — | 50% (round) | all |

**Statusbar height:** 36 px. **Navbar thickness:** 60 px (bottom in portrait, right edge in landscape).

---

## 3. Screen Architecture

Every Deck `@flow` state renders to an LVGL screen. The bridge maintains a stack of LVGL screens mirroring the OS app stack.

### 3.1 Layer Stack (top → bottom in Z-order)

```
lv_layer_top()      ← UI services render here: toasts, dialogs, overlays, choice lists
                       The bridge always has access to this layer regardless of what
                       screen is active. All UI service renders are independent of
                       the activity stack.

active screen       ← Statusbar + Content Area + Navbar
                       Built per @flow state, rebuilt on state change, rotation, and
                       app resume.

(LVGL background)   ← #000000, never visible in normal operation
```

### 3.2 Screen Structure (per screen)

```
Portrait (480×800 px):
┌─────────────────────────────────────┐  ← top
│  Statusbar (36 px)                  │  fixed, docked to top
│  TIME  ●  WIFI  BT  BATTERY  TITLE │  
├─────────────────────────────────────┤
│                                     │
│  Content Area (480 × 704 px)        │  scrollable flex column
│  pad_all=16, pad_row=14             │  ui_common_content_area()
│                                     │
├─────────────────────────────────────┤
│  Navbar (60 px)                     │  fixed, docked to bottom
│  [←BACK]   [⬤ HOME]   [⊞ TASKS]   │
└─────────────────────────────────────┘  ← bottom

Landscape (800×480 px):
┌──────────────────────────────┬──────┐
│  Statusbar (36 px)           │      │
├──────────────────────────────┤      │
│                              │  N   │
│  Content Area (740 × 444 px) │  A   │
│                              │  V   │
│                              │  B   │
│                              │  A   │
│                              │  R   │
└──────────────────────────────┴──────┘
                                60 px
```

### 3.3 Statusbar Content

The statusbar is always rebuilt on `on_create` and updated via event handlers registered in `on_create`, unregistered in `on_destroy`.

| Position | Content | Source | Update |
|---|---|---|---|
| Left | Current time `"HH:MM"` | `svc_time` → `app_state_get()` | Every 60 s via `lv_timer` |
| Left+1 | WiFi indicator: SSID or `"—"` | `app_state_get()->wifi_ssid` | `EVT_WIFI_CONNECTED` / `EVT_WIFI_DISCONNECTED` |
| Left+2 | BT indicator | `app_state_get()->bt_on` | `EVT_BT_CHANGED` |
| Right | Battery `"XX%"` + charging icon | `app_state_get()->battery_level` | `EVT_BATTERY_UPDATED` |
| Center | App title (ALL CAPS) | `AppInfo.name` from app registry | Static per screen |

Event handlers run on the `esp_event_loop_task`. They must double-check-lock on a global screen pointer and call `ui_lock()` / `ui_unlock()` before touching LVGL widgets.

### 3.4 Navbar Content

| Button | Symbol | Action | Blocked by nav lock |
|---|---|---|---|
| Back | Canvas-drawn left arrow (2 px line + left-pointing triangle) | `app_manager_go_back()` | Yes |
| Home | `LV_SYMBOL_HOME` | `app_manager_go_home()` | Yes |
| Tasks | `LV_SYMBOL_LIST` (long-press for Task Manager) | `apps.bring_to_front("system.taskmanager")` | No |

The back arrow is drawn on an `lv_canvas` (not `LV_SYMBOL_*`) to achieve the terminal-aesthetic angular look. Canvas buffer: `lv_color_t[NAVBAR_ARROW_W * NAVBAR_ARROW_H]`, heap-allocated, freed on `on_destroy`.

---

## 4. Catálogo de Nodos Semánticos → Decisiones del Bridge

El runtime evalúa los bloques `content =` de la app y produce un árbol de nodos semánticos (`DeckViewContent*`). El bridge recibe ese árbol en `render_fn` y lo materializa en LVGL.

**Los nodos son el vocabulario interno del bridge**, no una API que Deck ve. Una app escribe:

```deck
content =
  list
    items: posts
    item p ->
      group
        text "title"    value: p.title
        text "author"   value: p.author
        trigger
          label: "Open"
          -> App.send(:open (id: p.id))
```

El runtime produce `DVC_LIST → DVC_GROUP → [DVC_STATUS × 2, DVC_TRIGGER]`. El bridge recibe esa estructura y decide: lista scrollable con rows separadas por borde inferior, dos data-rows apiladas, botón outline al final de cada row. La app no sabe nada de esto.

### 4.1 Nodos de Layout

#### `DVC_GROUP` — Semantic Group

```
Visual:
  [18 px transparent spacer]          ← ui_common_section_gap()
  SECTION LABEL (SM, text_dim)        ← only if group has a label
  ┌──────────────────────────────┐
  │  bg_card, 2 px primary_dim   │    ← lv_obj, radius 12, flex column
  │  border, radius 12           │
  │  [children rendered inside]  │
  └──────────────────────────────┘
```

- Nested groups increase left padding by 8 px per level.
- Groups without labels omit the section label but keep the spacer.
- `ui_common_section_gap()` is inserted **between** unrelated groups only — never between items inside a group.

#### Panel (`ui_common_panel`)

A lighter container than `DVC_GROUP` — no section label, no spacer. Used for sub-regions within a screen that need visual containment without a section heading.

```
  ┌──────────────────────────────┐
  │  bg_dark, 1 px primary_dim   │    ← lv_obj, radius 2, flex column
  │  border, radius 2            │
  │  [children]                  │
  └──────────────────────────────┘
```

Used by: Task Manager (memory bar containers), Settings (status rows). Not a `DVC_*` node — the bridge uses it internally when a `DVC_GROUP` nesting level would add too much visual weight.

#### `DVC_LIST` — Scrollable Collection

```
Visual:
  lv_obj (flex column, LV_FLEX_FLOW_COLUMN, scrollable)
  Each item:
    ┌──────────────────────────────┐
    │  pad: 12 top/bottom, 8 L/R  │  1 px primary_dim border, BOTTOM only
    │  [item content]             │
    └──────────────────────────────┘
  [if more items available]
    ┌──────────────────────────────┐
    │  "LOAD MORE"  (outlined btn) │  → intent_fn("more", deck_bool(true))
    └──────────────────────────────┘
```

- Dividers (`ui_common_divider()`) are valid **only inside list containers**, not between groups.
- Empty state: if list items = 0 and Deck provides an `empty` block, the bridge renders it as a centered `DVC_DATA` node with `text_dim` color.
- Scrollbar: 2 px wide, `text_dim` color, no radius, `ui_theme_style_scrollbar()`.

#### `DVC_FORM` — Cohesive Input Group

Same visual as `DVC_LIST` but the bridge always appends an action row at the bottom:

```
[flex-grow spacer]                 ← pushes buttons to bottom (ui_common_spacer)
[CANCEL]   [SUBMIT LABEL]          ← ui_common_action_row; SUBMIT is filled primary
```

The submit label is inferred from the first `DVC_TRIGGER` child if it has a label; otherwise `"SAVE"`. CANCEL fires `intent_fn("cancel", deck_unit())`.

#### Grid (Launcher-Specific)

Not a generic `DVC_*` node — built by the Launcher's bridge screen directly:

```
LV_FLEX_FLOW_ROW_WRAP, center-aligned on both axes
3 columns (portrait) / 5 columns (landscape)
Cell: flex column, gap 4 px
  [ICON  — CYBERDECK_FONT_LG or CYBERDECK_FONT_XL]
  [NAME  — CYBERDECK_FONT_SM, text_dim]
  [BADGE — absolute-positioned circle, top-right of icon, if unread > 0]
```

Card dimensions auto-calculated from available width / column count, with 12–16 px grid gap.

---

### 4.2 Nodos de Visualización

#### `DVC_STATUS` — Label + Value Pair

```
FIELD LABEL:          ← CYBERDECK_FONT_SM, text_dim
VALUE TEXT            ← CYBERDECK_FONT_MD, primary
```

Used by `ui_common_data_row()`. Returns the value label so the caller can update it without rebuilding the row.

#### `DVC_DATA` — Standalone Value

Single `lv_label`, `CYBERDECK_FONT_MD`, `primary` color. Used for standalone text that isn't a label+value pair.

#### `DVC_RICH_TEXT` — Multi-line Text

Multi-line `lv_label`, `CYBERDECK_FONT_MD`, `primary`. On this bridge:
- Plain text is rendered as-is.
- Markdown (bold, italic, headings, links) is rendered via the `deck_markdown` capability if available — see `08-deck-markdown.md`. If the capability is absent, raw markdown syntax is stripped and plain text is shown.
- Links are rendered as tappable spans in `primary` color; tap fires `intent_fn("open_url", deck_str(url))`.

#### `DVC_MEDIA` — Image

`lv_img`. Asset path resolved via `deck_asset_path(app_id, atom)`. If the asset is not found or fails to decode: alt-text rendered as `DVC_DATA` with `text_dim` color. Network image loading is **not supported** on this bridge — only local assets from `@assets`.

#### `DVC_PROGRESS` — Progress Bar

`lv_bar`, value range [0.0, 1.0]. `primary` fill color, `primary_dim` background. Label above shows percentage: `"XX%"`, `CYBERDECK_FONT_SM`, `text_dim`.

#### `DVC_LOADING` — Loading Indicator

See **UI Service §5.3** — this triggers the Loading Overlay Service. The bridge does not render an inline widget; it shows a full-screen overlay on `lv_layer_top()`.

#### `DVC_ERROR` — Error State

Centered `lv_label`, `CYBERDECK_FONT_MD`, `accent` color (or `primary` on non-Neon themes). Used for error states in the content area — not a UI service overlay.

#### `DVC_CHART` — Chart / Graph

**Not supported on this bridge.** The node is skipped and a single `"[CHART: UNSUPPORTED]"` label in `text_dim` is rendered in its place. Logged once per view at `WARN` level.

---

### 4.3 Nodos de Interacción

Deck declara **intent de interacción** — "el usuario puede activar esto", "el usuario puede ajustar este valor", "el usuario puede confirmar esta acción destructiva". El bridge elige el widget y el patrón de presentación adecuados para ese intent en este hardware.

Invariante de clickables en LVGL 8: todo `lv_obj` con `LV_OBJ_FLAG_CLICKABLE` debe además tener `lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE)` — sin esto el indev deja el objeto en `LV_STATE_FOCUSED` visualmente (stuck-pressed).
1. `lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE)`
2. `lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE)` — **mandatory**, prevents stuck-pressed visual

#### `DVC_TRIGGER` — Action Button

```
Outline button (ui_common_btn):
  2 px primary_dim border, radius 12, no fill
  Label: CYBERDECK_FONT_MD, primary

Press state: invert
  bg → primary fill
  label → bg_dark

Promoted (primary CTA — bridge decides):
  bg → primary fill (default)
  label → bg_dark
  (ui_common_btn_style_primary)
```

**Layout inference:**
- Single `DVC_TRIGGER` in a `DVC_FORM` → promoted (filled primary), full-width
- Multiple `DVC_TRIGGER` siblings → outlined, inline in `LV_FLEX_FLOW_ROW` row, right-aligned (`LV_FLEX_ALIGN_END`)
- Last trigger in a form's action row → promoted to filled primary (bridge heuristic: rightmost = primary CTA)

**Badge field:** if the trigger node carries `badge: :some n`, the bridge renders a small badge circle (see §5.9 Badge Service) at the top-right of the button.

**Acción destructiva:** en Deck, cualquier acción irreversible se declara con `confirm` (no `trigger`) — la distinción semántica ya está en el nodo. El bridge recibe `VCConfirm` → siempre muestra el Confirm Dialog Service (§5.2). En el diálogo, el botón [OK] recibe styling rojo fijo `#FF3333` cuando el contexto es una acción de eliminación/kill (inferido por el label — "KILL", "DELETE", "REMOVE"). Este color es constante, independiente del tema activo — no proviene de `theme.accent`.

#### `DVC_NAVIGATE` — Navigation Row

```
Tappable list row:
  LABEL (CYBERDECK_FONT_MD, primary)   [LV_SYMBOL_RIGHT]
  1 px primary_dim border, BOTTOM only
  pad: 12/8
```

On tap → `intent_fn("navigate", deck_atom(route))`. The bridge does not push a new screen — the runtime handles the navigation by sending a state transition to the machine, which results in a new `render_fn` call with the new state.

**Badge field:** same as `DVC_TRIGGER` — rendered top-right of the row if `badge: :some n`.

#### `DVC_CONFIRM` — Destructive Action Button

Visually identical to `DVC_TRIGGER` (outline button). On tap → triggers **Confirm Dialog Service** (§5.2). The dialog receives the trigger's label as its title and the `message` field as the dialog body. On user confirmation → `intent_fn(name, deck_bool(true))`. On cancel → no intent dispatched.

#### `DVC_CREATE` — Create Row

```
  [LV_SYMBOL_PLUS]  CREATE LABEL (CYBERDECK_FONT_MD, primary)
  1 px primary_dim border, BOTTOM only
```

Semantically a "new item" row. On tap → `intent_fn("create", deck_unit())`.

#### `DVC_TOGGLE` — Boolean Switch

```
lv_switch
  ON:  thumb → bg_dark, track → primary
  OFF: thumb → primary_dim, track → bg_dark

Label left:  FIELD NAME (CYBERDECK_FONT_MD, text_dim)
Switch right
```

On change → `intent_fn(name, deck_bool(new_state))` immediately. No Save button — auto-commit.

#### `DVC_RANGE` — Numeric Slider

```
FIELD NAME: VALUE          ← label above, CYBERDECK_FONT_SM, text_dim; value shows current
[━━━━━━━━━●──────────────] ← lv_slider, primary indicator, primary_dim track
  min                max    ← CYBERDECK_FONT_SM, text_dim, below slider
```

On release (not on drag) → `intent_fn(name, deck_float(value))`. Auto-commit on release.

#### `DVC_CHOICE` — Single Selection

```
Tappable row:
  FIELD NAME: CURRENT VALUE  [LV_SYMBOL_RIGHT]
```

On tap → triggers **Choice Overlay Service** (§5.5): full-screen list of options on `lv_layer_top()`. On select → `intent_fn(name, deck_atom(selected))` + dismiss overlay. Auto-commit on selection.

#### `DVC_MULTISELECT` — Multi Selection

Same as `DVC_CHOICE` but the overlay shows checkmarks next to selected items. On confirm → `intent_fn(name, deck_list(selected_atoms))`.

#### `DVC_TEXT` — Text Input

```
FIELD NAME                ← CYBERDECK_FONT_SM, text_dim, above textarea
┌──────────────────────┐
│  textarea             │  lv_textarea, CYBERDECK_FONT_MD, radius 8
│  blinking cursor ▏   │  cursor color: primary
└──────────────────────┘
```

Keyboard shown in `on_resume` — **never in `on_create`**. On keyboard confirm (`LV_SYMBOL_OK`) → `intent_fn(name, deck_str(text))`. On keyboard dismiss without confirm → no intent.

#### `DVC_PASSWORD` — Password Input

Identical to `DVC_TEXT` but `lv_textarea_set_password_mode(ta, true)`. Bullets replace characters. Keyboard has no autocomplete.

#### `DVC_PIN` — PIN Entry

```
Display:  ● ● — —   (LV_SYMBOL_BULLET filled / "-" empty)
          CYBERDECK_FONT_XL, centered, primary color

Numpad: lv_btnmatrix with custom key map
  ["1"] ["2"] ["3"]
  ["4"] ["5"] ["6"]
  ["7"] ["8"] ["9"]
  [LV_SYMBOL_BACKSPACE] ["0"] [LV_SYMBOL_OK]
```

Implementado con **`lv_btnmatrix_create()`**, no con `lv_btn` individuales — permite styling uniforme y manejo de eventos centralizado. El mapa de teclas es un `const char*[]` con `"\n"` como separador de filas.

PIN length defined by node metadata (default 4, max 8). On complete (length reached and `LV_SYMBOL_OK` tapped) → `intent_fn(name, deck_str(pin))`. El bridge no envía PINs parciales.

`lv_btnmatrix` part styling:
- `LV_PART_ITEMS`: bg `bg_dark`, border none, radius 6 px, text `CYBERDECK_FONT_MD` `primary`
- `LV_STATE_PRESSED`: bg fill `primary`, text `bg_dark`

#### `DVC_DATE` — Date Picker

```
Tappable row:
  FIELD NAME: YYYY-MM-DD  [LV_SYMBOL_RIGHT]
```

On tap → triggers **Date Picker Service** (§5.6): overlay with month/year selectors and day grid. On confirm → `intent_fn(name, deck_str("YYYY-MM-DD"))`.

#### `DVC_SEARCH` — Search Input

```
[LV_SYMBOL_SEARCH]  SEARCH...
───────────────────────────────
[result list below]
```

Keyboard shown on `on_resume`. On every keystroke → `intent_fn(name, deck_str(query))` — fires per character, not on confirm. The runtime debounces or filters via stream if needed. Results populate via a subsequent `render_fn` call.

#### `DVC_SHARE` — Share Action

```
Tappable row:
  [LV_SYMBOL_UPLOAD]  SHARE LABEL
```

On tap → triggers **Share Sheet Service** (§5.11). Available share targets on this board depend on what's installed and what the OS supports. The intent result is `intent_fn("share", deck_atom(:done | :cancelled))`.

#### `DVC_FLOW` — Inline Sub-Flow

Renders the active step of a named `@machine` or `@flow` inline within the parent content. The bridge receives the current step's children and renders them as if they were direct children of the parent. The bridge caches the `machine_name + state_name` pair for diffing — only rebuilds the sub-tree if the state changed.

---

## 5. UI Services — Mecanismos Autónomos del Bridge

Los UI services son subsistemas C autónomos dentro del bridge. **Deck no los conoce, no los llama, no sabe que están activos.** El bridge los activa basándose en nodos recibidos, patrones de interacción, o condiciones del sistema.

Desde la perspectiva del runtime: un UI service puede entregar resultados al runtime vía `intent_fn`, igual que cualquier tap. El runtime no distingue si el intent vino de un tap directo o de un UI service que medió la interacción. El runtime nunca sabe qué servicio estuvo activo.

Todos los UI services renderizan en `lv_layer_top()` — encima de todo el contenido de la app. Son independientemente dismissibles y no dependen del activity stack.

**Formato de cada servicio:**
- **Trigger conditions:** qué hace que el bridge lo active
- **Visual:** cómo se ve en pantalla
- **Resultado al runtime:** qué `intent_fn` llama (si alguna)
- **Ciclo de vida:** cómo y cuándo se descarta

---

### 5.1 Toast Service

**Trigger conditions:**
- Bridge explicitly calls `deck_bridge_show_toast(msg, duration_ms)` — used by other services and bridge internals
- OS posts `shell.post_notification()` for a lightweight in-app notification
- App stub loaded with no matching `@on launch` → "Coming soon..." toast (see §5.10)

**Visual:**
```
                  ┌───────────────────────┐
                  │  MESSAGE TEXT         │  CYBERDECK_FONT_SM, text color
                  └───────────────────────┘
                       bg_card fill, 1 px primary border, radius 2
                       pad: 12 horizontal / 6 vertical
```

Centered in the content area. In portrait: shifted up by `UI_NAVBAR_THICK / 2` (30 px) to clear the navbar. In landscape: shifted left by `UI_NAVBAR_THICK / 2`.

**Duration rules:**
| Type | Duration |
|---|---|
| Success / confirmation | 1200–1500 ms |
| Informational (default) | 2000 ms |
| "Coming soon..." stub | 1500 ms |
| Error | 2500 ms |

**Queue behavior:** if a toast is already visible and a new one arrives, the existing toast is dismissed immediately and the new one is shown. No queue — always shows the most recent.

**Implementation:** `lv_timer` for auto-dismiss. Fade-in: `lv_anim` over 150 ms (alpha 0 → 255). Fade-out: immediate on timer expiry (no fade-out animation — avoids the visual complexity of animating on `lv_layer_top()`).

---

### 5.2 Confirm Dialog Service

**Trigger conditions:**
- `DVC_CONFIRM` node tapped
- Bridge detects back navigation on a dirty form (see §6.2)
- `system.tasks.kill()` confirmation (Task Manager)
- Bridge internally for any destructive OS action

**Visual:**
```
┌─────────────────────────────────────────────────────────┐  semi-transparent black
│  ┌─────────────────────────────────────────────────┐    │  backdrop LV_OPA_50
│  │▓▓▓▓▓  DIALOG TITLE  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓/  │  28 px parallelogram
│  │  Dialog body text — CYBERDECK_FONT_MD, primary  │  pad_all=24
│  │  Longer description of what will happen          │  pad_row=16
│  │                                                  │
│  │  [24 px transparent spacer]                      │
│  │                                     [CANCEL] [OK]│  action row, right-aligned
│  └─────────────────────────────────────────────────┘
│     380 px wide, bg_dark, 1 px primary_dim border, radius 2
└─────────────────────────────────────────────────────────┘
```

**Title polygon geometry:** `lv_canvas`, 380 × 28 px buffer (`lv_color_t*`, `lv_mem_alloc`). Parallelogram: `A(0,0) ─ B(379,0) ─ C(351,27) ─ D(0,27)`. Fill: `primary_dim`. Title text: `CYBERDECK_FONT_SM`, `text` color, bold-by-double-draw (render at (x,y) then again at (x+1,y)), ALL CAPS.

**Dismiss sequence (order matters):**
1. Free canvas buffer (`lv_mem_free(state->title_buf)`)
2. Delete dialog backdrop (`lv_obj_del(state->backdrop)`)
3. Invoke result callback (`cb(confirmed, user_data)`)

Step 3 must be last — the callback may push a new activity, and the activity push calls `lv_scr_load`. If the backdrop were still alive, it would interfere.

**CANCEL:** calls `cb(false, user_data)`, no intent sent to runtime.
**OK:** calls `cb(true, user_data)` → bridge calls `intent_fn(node_name, deck_bool(true))`.

---

### 5.3 Loading Overlay Service

**Trigger conditions:**
- `DVC_LOADING` node emitted by the runtime
- Bridge auto-shows during `@on launch` window (from VM load until first non-loading render)
- Any long-running effect where the runtime emits `VCLoading {}`

**Visual:**
```
Full screen semi-transparent black (LV_OPA_70)

                        _             ← blinking cursor
               CYBERDECK_FONT_XL, primary color
               centered in content area (adjusted for navbar)
```

Blinks at 500 ms interval via `lv_timer` (toggle visibility). 

**Dismissal:** automatically dismissed when the runtime calls `render_fn` with a non-loading content tree. The bridge detects this by checking if the root node is no longer `DVC_LOADING`.

---

### 5.4 Keyboard Service

**Trigger conditions:**
- `DVC_TEXT` or `DVC_PASSWORD` node is in the active screen → keyboard shown on `on_resume` (never `on_create`)
- `DVC_SEARCH` node → keyboard shown on `on_resume`
- `DVC_PIN` node → custom numpad is rendered inline (not the keyboard service — see §4.3)

**Implementation:** `lv_keyboard` docked to bottom of screen. The associated `lv_textarea` scrolls to stay visible above the keyboard. Height: ~40% of screen height.

**Keyboard modes:**
| Node | `lv_keyboard_mode_t` |
|---|---|
| `DVC_TEXT` | `LV_KEYBOARD_MODE_TEXT_UPPER` |
| `DVC_PASSWORD` | `LV_KEYBOARD_MODE_TEXT_UPPER` (no autocomplete) |
| `DVC_SEARCH` | `LV_KEYBOARD_MODE_TEXT_LOWER` |

**Confirm:** `LV_SYMBOL_OK` key → `intent_fn(name, deck_str(text))` → keyboard dismissed.
**Dismiss without confirm:** tapping outside the keyboard area → keyboard hidden, no intent. The textarea retains its current value.
**Navigation away:** if the runtime sends a new `render_fn` call while keyboard is visible, the keyboard is dismissed before the new screen is built.

---

### 5.5 Choice Overlay Service

**Trigger conditions:**
- `DVC_CHOICE` node tapped
- `DVC_MULTISELECT` node tapped

**Visual:**
```
┌─────────────────────────────────────────────────────────┐  semi-transparent black
│  ┌─────────────────────────────────────────────────┐    │  backdrop LV_OPA_50
│  │  FIELD NAME                                      │  title row, SM, text_dim
│  ├─────────────────────────────────────────────────┤
│  │  ○  Option One                                   │  list rows, MD, primary
│  │  ●  Option Two (selected)                        │  LV_SYMBOL_BULLET for selected
│  │  ○  Option Three                                 │  "-" for unselected
│  │  ...                                             │  scrollable
│  ├─────────────────────────────────────────────────┤
│  │  [CANCEL]                          [CONFIRM]     │  only for multiselect
│  └─────────────────────────────────────────────────┘
│     640 px wide, bg_dark, 1 px primary_dim border, radius 2
└─────────────────────────────────────────────────────────┘
```

**Single select (`DVC_CHOICE`):** tapping a row immediately fires `intent_fn` and dismisses. No CONFIRM button.
**Multi select (`DVC_MULTISELECT`):** tapping rows toggles selection. CONFIRM fires `intent_fn(name, deck_list(selected))` and dismisses. CANCEL dismisses with no intent.

---

### 5.6 Date Picker Service

**Trigger conditions:**
- `DVC_DATE` node tapped

**Visual:** overlay on `lv_layer_top()` with:
- Month/year selector (left/right arrows, `CYBERDECK_FONT_MD`, `primary`)
- 7-column day grid (Mon–Sun headers `SM text_dim`, day numbers `MD primary`)
- Selected day: filled `primary`, text `bg_dark`
- [CANCEL] outline / [CONFIRM] filled primary

On CONFIRM → `intent_fn(name, deck_str("YYYY-MM-DD"))`.

---

### 5.7 Statusbar Service

The statusbar is not a global autonomous service — it is rebuilt per screen in `on_create` and uses event handlers for live updates. However, it behaves like a service from the app's perspective: it always shows system info regardless of what the app renders.

**Layout (left → right):**

```
┌────────────────────────────────────────────────────────────────────────┐  36 px
│  /TITLE PARALLELOGRAM\   TIME   BT   SD   AUDIO   WIFI BARS   BAT %   │
└────────────────────────────────────────────────────────────────────────┘
  ↑ canvas-drawn shape                                          ↑ outline box
```

**Indicators — detail:**

| Indicator | Visual | Source | Update trigger |
|---|---|---|---|
| Title | Canvas parallelogram `primary_dim` fill, `bg_dark` text, `CYBERDECK_FONT_MD`, ALL CAPS | App `name` from registry | Static per screen |
| Time | `"HH:MM"`, `CYBERDECK_FONT_SM`, `primary` | `svc_time` | `lv_timer` every 60 s |
| Bluetooth | `LV_SYMBOL_BLUETOOTH`, hidden if off | `app_state_get()->bt_on` | `EVT_BT_CHANGED` |
| SD card | `LV_SYMBOL_SD_CARD`, hidden if not mounted | SD mount state | `EVT_SD_CHANGED` |
| Audio | Speaker icon (custom canvas or `LV_SYMBOL_AUDIO`), hidden if no audio output | External BT module state | At boot, `EVT_BT_CHANGED` |
| WiFi | 4 bars of increasing height, colored by signal: 6 / 10 / 16 / 20 px tall. Active bars: `primary`. Inactive: `primary_dim`. All hidden if disconnected. | `app_state_get()->wifi_rssi` | `EVT_WIFI_SCAN_DONE`, `EVT_WIFI_CONNECTED` |
| Battery | Outlined box with inner fill bar (`lv_bar`, `primary` fill), `"XX%"` label `CYBERDECK_FONT_SM`. `LV_SYMBOL_CHARGE` visible when charging. | `app_state_get()->battery_level` | `EVT_BATTERY_UPDATED` |

**Border:**
- Portrait: 2 px `primary_dim` border on bottom edge only
- Landscape: 2 px `primary_dim` border on right edge, plus bottom accent line

Each screen registers these handlers in `on_create` and unregisters them in `on_destroy`. The handler always double-checks-locks on a global screen state pointer — if `NULL`, the screen was destroyed, return immediately.

---

### 5.8 Navbar Service

The navbar renders three touch zones using `lv_obj` shapes — **not** `LV_SYMBOL_*` icons. The terminal aesthetic uses geometric primitives: triangle, circle, square.

**Icon implementations (all drawn via `lv_obj` or `lv_canvas`):**

| Button | Icon | Implementation | Action |
|---|---|---|---|
| Back | Left-pointing triangle outline | `lv_canvas` with 4-point line path (2 px, `primary`) | `app_manager_go_back()` |
| Home | Filled circle | `lv_obj_create()` + `lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0)`, `primary` fill, no border | `app_manager_go_home()` |
| Tasks | Filled square | `lv_obj_create()` + `radius = 0`, `primary` fill, no border | Task Switcher or Task Manager |

All three zones are equal-width (flex-grow evenly distributed). Press state for all: `LV_OPA_20` primary fill overlay behind the icon.

**Back button:**
- Normal: `app_manager_go_back()` → `ui_activity_pop()`
- If `s_nav_lock == true`: visually dimmed (`primary_dim` icon color), touch event swallowed

**Home button:**
- Short tap: `ui_activity_suspend_to_home()` — hides all apps above launcher **preserving state** (apps remain in memory, resume on return)
- `app_manager_go_home()` (pop_to_home variant) — destroys all above launcher; used when OS needs to free memory

**Task-switcher button:**
- Short tap: activates Task Switcher overlay (handled by Launcher's `TaskSwitcherFlow`)
- Long press (>600 ms): `app_manager_launch("system.taskmanager")` — launches Task Manager

**Nav lock** is set by `ui_activity_set_nav_lock(true)` — used exclusively by the lockscreen. Cleared on successful PIN entry.

---

### 5.9 Badge Service

Renders unread notification counts on app triggers and launcher grid icons.

**Data source:** `notif_counts_watch()` stream from `system.apps` capability, subscribed by the Launcher. Stream emits `[(app_id: str, unread: int)]` snapshot on any count change.

**Visual:**
```
         ┌──┐
         │ 3│    ← small circle, top-right of icon/button
         └──┘
  [APP ICON]
   APP NAME
```

Badge circle: `primary` fill, `bg_dark` text, `CYBERDECK_FONT_SM`, radius = 50% (round). Width auto-sized to digit count (min 20 px). Positioned absolute, z-order on top of icon.

**Bridge rendering:**
- For `DVC_TRIGGER` and `DVC_NAVIGATE` nodes: `badge?: Expr` is evaluated by the runtime; the bridge receives it as an optional int in the node metadata
- For Launcher grid cells: bridge reads unread count from `notif_counts_watch()` stream value by matching `app_id`
- Badge = 0 or absent → no badge rendered (node removed if was present, no rebuild needed)
- Badge ≥ 100 → displayed as `"99+"` 

---

### 5.10 "Coming Soon" Stub Service

**Trigger conditions:**
- Launcher launches an app that is in the registry but has no `.deck` bundle installed (stub slot)

**Behavior:**
- Bridge does not attempt to load or render anything
- Toast displayed: `"Coming soon..."` — 1500 ms duration
- This is the **only** string in the entire UI that is NOT all-caps (toast style exception, per design)
- No confirm dialog, no navigation change — Launcher stays in foreground

---

### 5.12 Progress Overlay Service

**Distinto del Loading Overlay (§5.3).** El Loading Overlay indica "algo está pasando, no sé cuánto falta". El Progress Overlay indica un avance medible — descarga de OTA, copia de archivos, sync inicial.

**Trigger conditions:**
- La app emite un `DVC_PROGRESS` con `value: float` (0.0–1.0) y `cancellable: bool`
- El bridge detecta que el valor progresa en el tiempo — activa automáticamente el overlay en lugar de un inline `lv_bar` cuando el `DVC_PROGRESS` es el nodo raíz del contenido

**Modos:**

| Modo | `value` | Visual |
|---|---|---|
| Determinado | 0.0–1.0 | `lv_bar` con fill `primary`, `"XX%"` encima, `CYBERDECK_FONT_SM` |
| Indeterminado | −1 | Animación de franjas diagonales 45°, moviéndose hacia la izquierda en loop |

**Visual (ambos modos):**
```
Full screen semi-transparent black (LV_OPA_70)

  OPERATION TITLE          ← CYBERDECK_FONT_MD, primary, centered
  [████████░░░░░░░░░░]      ← lv_bar (determinado) o franjas animadas (indeterminado)
  XX%                      ← solo en modo determinado

  [CANCEL]                 ← solo si cancellable: true
```

**Animación de franjas (indeterminado):**
- Patrón de líneas diagonales a 45° dibujado en canvas sobre el bar background
- El canvas se desplaza 2 px hacia la izquierda cada 16 ms via `lv_timer` — efecto de movimiento
- Colores: franjas alternas `primary_dim` / `bg_dark`

**Ciclo de vida:**
- Mostrado vía `ui_effect_progress_show(title, cancellable)`
- Actualizado vía `ui_effect_progress_set(percent)` — −1 activa indeterminado
- Descartado vía `ui_effect_progress_hide()` o por CANCEL
- CANCEL → `intent_fn("progress_cancel", deck_bool(true))` → la app decide si abortar

---

### 5.11 Share Sheet Service

**Trigger conditions:**
- `DVC_SHARE` node tapped

**Available targets on this board:**
| Target | Condition |
|---|---|
| Copy to clipboard | Always available (ESP32 in-memory clipboard, cleared on reboot) |
| Save to file | If `fs` capability is available to the requesting app |
| Share via Bluesky | If `social.bsky.app` is installed and user is authenticated |

Share sheet renders as a `DVC_CHOICE`-style overlay with available targets. On selection → performs the share action and fires `intent_fn("share", deck_atom(:done))`. On cancel → `intent_fn("share", deck_atom(:cancelled))`.

---

## 6. UX Patterns and Flows

### 6.1 App Launch Flow

```
1. Bridge loads VM (deck_os_launch)
   → Loading Overlay Service auto-shows (§5.3)

2. @on launch executes (NVS reads, DB schema, registrations)
   → Loading overlay stays visible

3. Runtime emits first non-loading render_fn call
   → Bridge dismisses loading overlay
   → First content renders
   → If @assets has :splash → splash shown briefly before first render
      (bridge holds the splash for max(500ms, @on launch duration))

4. Bridge calls on_resume (keyboard shows if DVC_TEXT is in initial state)
```

### 6.2 Back Navigation Flow

```
User presses back (gesture or navbar button)
  ↓
Is nav lock active?
  YES → swallow, nothing happens
  NO  ↓
  
Does active @flow have history (visited states ≠ initial)?
  YES → runtime sends to history transition
        bridge calls nav_back_fn → BridgeActivity pops
        on_destroy on popped screen, on_resume on new top
  NO  ↓
  
Does the active screen have a dirty form? (bridge tracks pending DVC_TEXT edits)
  YES → Confirm Dialog Service: "DISCARD CHANGES?"
        CANCEL: no action
        OK: continue to OS back
  NO  ↓
  
Does the app have @on back hook?
  YES → runtime executes @on back
        :handled → stop
        :unhandled → continue
  NO  ↓
  
OS suspends app — dos variantes según presión de memoria:
  → suspend_to_home: apps quedan en memoria (PSRAM), se restauran en resume
  → pop_to_home:    apps se destruyen (on_destroy + lv_obj_del), launcher recupera memoria
  → previous app's on_resume fires
```

**`suspend_to_home` vs `pop_to_home`:**
- `suspend_to_home()` — el bridge preserva las apps en el activity stack (estado intacto en heap). La app vuelve exactamente donde estaba en `on_resume`. Comportamiento default del botón Home.
- `pop_to_home()` — destruye todas las apps encima del launcher. Úsado cuando el OS necesita recuperar memoria, o cuando la navegación back debe destruir state (e.g., flujo completado). El launcher recibe `on_resume` pero las apps destruidas no pueden volver — el usuario debe relanzarlas.

### 6.3 Rotation Flow

```
EVT_DISPLAY_ROTATED received by bridge handler
  ↓
app_state_update_rotation(new_rotation)
  ↓
ui_activity_recreate_all()
  For each screen in stack (bottom to top):
    on_destroy(screen, state)    ← unregisters event handlers, deletes timers
    rebuild screen from scratch
    on_create(screen, NULL)      ← NULL intent_data — screen reads from app_state_get()
    ↓
lv_scr_load(top screen)
  ↓
top screen's on_resume fires    ← keyboard re-shown if applicable
```

Touch coordinates continue to be passed raw — LVGL reapplies its internal rotation matrix automatically. No coordinate transforms needed in touch callbacks.

### 6.4 Lockscreen Flow

```
app_manager_lock() called (timer, low battery, explicit lock action)
  ↓
Bridge pushes lockscreen activity (APP_ID_LAUNCHER, screen_id=1)
  ↓
ui_activity_set_nav_lock(true)
  ↓
Lockscreen shows PIN numpad (DVC_PIN, §4.3)
  ↓
Correct PIN entered → intent_fn("unlock", deck_str(pin))
  ↓
Runtime validates → if correct:
  ui_activity_set_nav_lock(false)
  ui_activity_pop()              ← lockscreen removed
  on_resume on underlying screen
  ↓
Wrong PIN → Toast Service: "INCORRECT PIN" (2000 ms), numpad cleared
```

### 6.5 Permission Dialog Flow

```
App loads → Loader stage (deck_os_launch)
  ↓
Loader finds @permissions entries requiring @requires_permission
  ↓
Bridge calls request_perms_fn(caps[], n, on_done_cb, ctx)
  ↓
Bridge shows permission list as a full-screen confirm-style dialog:
  Title: "PERMISSIONS"
  Body: bulleted list of [capability → reason] pairs
  Buttons: [DENY] [ALLOW]
  ↓
User taps ALLOW → on_done_cb(all_granted=true) → app loading continues
User taps DENY  → on_done_cb(all_granted=false)
                  → denied capabilities behave as optional-absent
                  → app may still launch (runtime degrades gracefully)
```

### 6.6 Auto-Save vs Explicit Save

The bridge determines save behavior based on node type — not instruction from Deck:

| Node type | Bridge behavior | Save button? |
|---|---|---|
| `DVC_TOGGLE` | `intent_fn` on every change | No |
| `DVC_RANGE` | `intent_fn` on release | No |
| `DVC_CHOICE` | `intent_fn` on selection | No |
| `DVC_TEXT` / `DVC_PASSWORD` | `intent_fn` on keyboard confirm only | No — keyboard confirm is the action |
| `DVC_CONFIRM` | Requires dialog confirmation | Dialog is the save gate |
| `DVC_FORM` | Bridge appends [CANCEL] [SAVE] row | Yes — explicit SAVE |

The bridge tracks whether a `DVC_TEXT` field has been edited (current value ≠ last committed value). If the user navigates back from a screen with unsaved text, the **Confirm Dialog Service** fires: `"DISCARD CHANGES?"`.

---

## 7. Reglas de Inferencia del Bridge

El bridge infiere decisiones de presentación del contexto semántico. Estas reglas definen exactamente cómo el bridge convierte "qué" en "cómo" sin instrucciones explícitas de la app.

### 7.1 Presentación de Botones y Acciones

| Contexto semántico | Decisión del bridge |
|---|---|
| Un solo `DVC_TRIGGER` en un `DVC_FORM` | Filled primary, full-width, al fondo del form |
| Múltiples `DVC_TRIGGER` hermanos | Todos outline; el último (rightmost/bottommost) promovido a filled primary |
| `DVC_TRIGGER` aislado fuera de form | Outline, ancho natural |
| `DVC_CONFIRM` (acción destructiva) | Route al **Confirm Dialog Service** — nunca intent directo |
| `badge: :some n` en cualquier trigger o navigate | Badge circle top-right sobre el widget |
| `badge: :none` o ausente | Sin badge |

### 7.2 Layout y Estructura

| Contexto semántico | Decisión del bridge |
|---|---|
| `DVC_GROUP` sin label | Spacer + card container, sin label row |
| `DVC_GROUP` con label | Spacer + dim label + card container |
| `DVC_LIST` con `more: true` | "LOAD MORE" button al final de la lista |
| `DVC_LIST` vacía con bloque `empty` | Renderiza el bloque `empty` como contenido centrado |
| `DVC_DATA` con texto largo (>3 líneas estimadas) | `lv_label` con long mode wrap; container scrollable |
| `DVC_STATUS` items en fila horizontal | Bridge apila vertical (portrait) o muestra en grid 2-col (landscape) según espacio disponible |

### 7.3 Inputs y Teclado

| Contexto semántico | Decisión del bridge |
|---|---|
| `DVC_TEXT` / `DVC_PASSWORD` / `DVC_SEARCH` en estado inicial | NO mostrar teclado en `on_create`; esperar `on_resume` |
| `DVC_TEXT` con cambios no committed al navegar back | Interceptar; Confirm Dialog Service: "DISCARD CHANGES?" |
| `DVC_CHOICE` tapeado | Route al **Choice Overlay Service** — nunca inline select |
| `DVC_DATE` tapeado | Route al **Date Picker Service** |

### 7.4 Overlays y Feedback

| Contexto semántico | Decisión del bridge |
|---|---|
| `DVC_LOADING` como nodo raíz | Full-screen Loading Overlay Service — no inline widget |
| `DVC_LOADING` durante `@on launch` | Bridge auto-activa Loading Overlay desde VM load; descarta en primer render no-loading |
| `DVC_CHART` | Skip + `WARN` log una vez por view; render placeholder text |
| Notificación OS llega con app en foreground | Toast Service — no interrumpe el render principal |

### 7.5 Navegación y Lifecycle

| Contexto semántico | Decisión del bridge |
|---|---|
| App stub sin bundle `.deck` | "Coming soon..." Toast Service — sin navegación |
| @flow en `initial` state + back gesture | Back pasa al OS (suspensión de app) |
| @flow con history + back gesture | Bridge hace `nav_back_fn` — runtime ya sabe que es `to history` |
| Rotación del display | Rebuild total de todas las pantallas activas; `intent_data = NULL` en `on_create` |
| App toma el foreground (resume) | Bridge llama `on_resume`; teclado se muestra si aplica |

---

## 8. Diffing and Rebuild Strategy

The bridge receives the full `DeckViewContent*` tree on every `render_fn` call. It must decide whether to rebuild the screen or patch it.

**Current strategy:** full rebuild on any state change. The bridge compares `(app_id, screen_id, state_name)` against the current top of its activity stack:

- Same triple → **patch**: update `lv_label` texts and `lv_bar`/`lv_slider` values in place if only leaf data changed. Full rebuild only if tree structure changed.
- Different triple → **push or replace**: push new `BridgeActivity`, call `lv_scr_load`.

**Stream-driven updates** (e.g., statusbar time, badge counts): these are handled directly by bridge event handlers — they do not go through `render_fn`. The runtime does not re-render on stream ticks unless the stream value is bound to machine state that triggers a state transition.

---

## 9. Theme Switching Implementation

Theme switch is triggered by `EVT_SETTINGS_CHANGED (key: "theme")`:

```
1. svc_settings saves new theme atom to NVS
2. EVT_SETTINGS_CHANGED fires on event loop
3. Bridge handler:
   a. app_state_update_theme(new_theme)
   b. ui_theme_set(new_theme)            ← updates ui_theme_get() return value globally
   c. ui_activity_recreate_all()         ← full rebuild of all active screens
4. All screens reconstruct with new theme colors
```

The theme is applied at widget creation time (in `on_create`), not retroactively — hence the full rebuild. LVGL styles are not globally patched because they're applied per-widget at creation with explicit color values from `ui_theme_get()`.

---

## 10. LVGL Gotchas Reference

Issues specific to LVGL 8.4 on this bridge:

| Issue | Rule |
|---|---|
| Scrollbar styling | Use `lv_obj_set_style_width(obj, 2, LV_PART_SCROLLBAR)` — no `lv_obj_set_style_scrollbar_width()` in v8 |
| No layout constant | Use `0` instead of `LV_LAYOUT_NONE` |
| Stuck-pressed buttons | Always `lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE)` after any `lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE)` |
| `-Werror` active | All warnings are fatal. Watch `snprintf` format-truncation on buffers smaller than `uint8_t` worst-case (0–255) |
| Touch + rotation | Never transform coordinates in `touchpad_read`; LVGL handles rotation internally |
| Canvas buffer ownership | `lv_color_t*` canvas buffers are NOT owned by LVGL — allocate with `lv_mem_alloc`, free manually in dismiss/destroy |
| `lv_scr_load` + `lv_obj_del` order | Always `lv_scr_load` (new screen) BEFORE `lv_obj_del` (old screen) to prevent dangling `disp->act_scr` |
| Event handlers after destroy | Unregister all LVGL event handlers in `on_destroy`; freed objects still fire callbacks if not removed |
| `lv_timer` after object destroy | Always delete `lv_timer` in `on_destroy`; a timer firing against a deleted object crashes |
