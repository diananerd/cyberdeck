# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

S3 Cyber-Deck — ESP32-S3 firmware for the Waveshare ESP32-S3-Touch-LCD-4.3 board. A modular, OS-like UI application built with ESP-IDF (v6.0.0), LVGL 8.4.0, and FreeRTOS. 800x480 RGB LCD, GT911 touch, CH422G I/O expander, PCF85063A RTC, SD card, PSRAM, 8MB flash.

## Build & Flash Commands

```bash
# Build
idf.py build

# Flash + monitor (USB native port)
idf.py -p /dev/cu.usbmodem1101 flash monitor

# Monitor only (UART console via CH343)
idf.py -p /dev/cu.usbmodem58A60705271 monitor

# Interactive config
idf.py menuconfig

# Full clean
idf.py fullclean
```

There are no automated tests — all testing is manual on hardware.

## Flash Workflow

The board has two USB-C ports:
- **USB native** (`/dev/cu.usbmodem1101`): Flash + JTAG. **Always use this for flashing.**
- **UART** (`/dev/cu.usbmodem58A60705271`): CH343 → UART0 serial console at 115200 baud. Do NOT flash via this port (checksum errors).

Flash sequence:
1. Hold BOOT + press RESET → download mode
2. `idf.py -p /dev/cu.usbmodem1101 flash`
3. Disconnect both cables, wait a few seconds, reconnect USB native first then UART

## JTAG Debug

```bash
openocd -f board/esp32s3-builtin.cfg
xtensa-esp32s3-elf-gdb build/cyberdeck.elf
# (gdb) target remote :3333
# (gdb) mon halt
```

## Architecture

Five-layer component stack:

```
apps/launcher, apps/settings
        ↓
app_framework  (app registry, lifecycle, navigation, device state)
        ↓
ui_engine      (LVGL task, activity stack, statusbar, theme, effects)
        ↓
sys_services   (event bus, WiFi, battery, time, NVS settings, OTA)
        ↓
board          (HAL: LCD, touch, CH422G, backlight, battery ADC, RTC, SD)
```

### board/ — Hardware Abstraction Layer

All pin definitions in `include/hal_pins.h`. I2C bus (SDA=GPIO8, SCL=GPIO9) is shared by GT911 touch, CH422G expander, and PCF85063A RTC.

**CH422G OUT register** (addr `0x38`) controls key peripherals:
```
bit 1: TP_RST   bit 2: BL   bit 3: LCD_RST   bit 4: SD_CS   bit 5: USB_SEL
```
**USB_SEL (bit 5) must always stay LOW.** Setting it HIGH routes GPIO 19/20 to CAN instead of USB native, breaking flash/JTAG. Always use the named `HAL_CH422G_BIT_*` defines — never magic hex values. `hal_backlight_on()` must be called **after** `hal_lcd_init()` because touch reset overwrites the CH422G OUT register (clearing BL).

### ui_engine/ — LVGL Layer

**Thread safety:** LVGL runs on Core 1. All LVGL API calls from other tasks require `ui_lock(timeout_ms)` / `ui_unlock()`.

**Activity stack** (`ui_activity.h`): max 4 levels. Level 0 is always the launcher. Each activity has `on_create` / `on_resume` / `on_pause` / `on_destroy` callbacks. `ui_activity_push()` / `ui_activity_pop()` / `ui_activity_pop_to_home()`. Lockscreen uses `ui_activity_set_nav_lock()` to block HOME/BACK gestures.

**Rotation:** `ui_activity_recreate_all()` rebuilds all layouts. Touch coordinates are always passed raw — LVGL's `lv_indev.c` handles the rotation transform internally. Do not apply rotation in `touchpad_read`.

### sys_services/ — Background Services

Central event bus: `ESP_EVENT_DECLARE_BASE(CYBERDECK_EVENT)`. All state changes flow as events (`EVT_BATTERY_UPDATED`, `EVT_WIFI_CONNECTED`, `EVT_SETTINGS_CHANGED`, `EVT_DISPLAY_ROTATED`, etc.).

`svc_settings` persists everything to NVS namespace `"cyberdeck"`. Settings changes post `EVT_SETTINGS_CHANGED`. Apps read volatile device state from `app_state_get()` (not directly from services).

### app_framework/ — App Registry & Navigation

Apps registered in `app_registry.c`: slots 0–9 (LAUNCHER, BOOKS, NOTES, TASKS, MUSIC, PODCASTS, CALC, BLUESKY, FILES, SETTINGS). Most slots beyond LAUNCHER and SETTINGS are stubs.

High-level navigation: `app_manager_launch(app_id, data, size)`, `app_manager_go_back()`, `app_manager_go_home()`, `app_manager_lock()`.

## LVGL 8.x Gotchas

- **Scrollbar styling:** Use `lv_obj_set_style_width(obj, 2, LV_PART_SCROLLBAR)` — there is no `lv_obj_set_style_scrollbar_width()` in v8.
- **No layout constant:** Use `0` instead of `LV_LAYOUT_NONE`.
- **Focused/stuck buttons:** After any `lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE)`, always add `lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE)`. Without this, the indev keeps the object in `LV_STATE_FOCUSED` (visually "stuck pressed").
- **`-Werror` is active:** All warnings are fatal. Watch for `snprintf` format-truncation on buffers too small for `uint8_t` worst case (0–255).
- **Touch + rotation:** Never transform coordinates in `touchpad_read`. Pass raw `(x, y)` — LVGL handles rotation internally.

## UI / UX Design System

Terminal-aesthetic GUI — NOT a literal terminal. Uses flex, grids, rounded cards, LVGL symbols. Black background, monochromatic themes. Outlined widgets, Montserrat fonts. The visual grammar below is authoritative; follow it in every new screen.

### Color & Themes

Three themes, each providing the same palette fields (`ui_theme_get()`):

| Field | Green (Matrix) | Amber (Retro) | Neon (Cyberpunk) |
|---|---|---|---|
| `primary` | `#00FF41` | `#FFB000` | `#FF00FF` |
| `primary_dim` | `#004D13` | `#4D3500` | dimmed magenta |
| `bg_dark` | `#000000` | `#000000` | `#000000` |
| `bg_card` | `#0A0A0A` | `#0A0A0A` | `#0A0A0A` |
| `text_dim` | 50% primary | 50% primary | 50% primary |
| `secondary` | — | — | `#00FFFF` |
| `accent` | — | — | `#FF0055` |
| `success` | — | — | `#39FF14` |

**Color intensity rules:**
- `primary` — active state, selected item, value text, widget borders in focus.
- `primary_dim` — inactive borders, disabled/stub items (e.g. unimplemented launcher apps).
- `text_dim` — secondary info (captions, sub-labels, secondary list lines). Opacity `LV_OPA_60` for inline secondary text in list rows.
- `bg_card` (`#0A0A0A`) — slightly raised surfaces (cards) to distinguish from pure black background.
- Overlays: backdrop `LV_OPA_50`, loading screen `LV_OPA_70`, press feedback `LV_OPA_20` bg fill.
- Button press state: invert — bg becomes `primary`, text becomes `bg_dark`. Inactive button: outline only, no fill.

### Typography

Font aliases defined in `ui_theme.h`:

| Alias | Font | Use |
|---|---|---|
| `CYBERDECK_FONT_SM` | Montserrat 18 | Statusbar, captions, dim labels, secondary text, toast messages |
| `CYBERDECK_FONT_MD` | Montserrat 24 | Body text, list primary items, data row values, button labels |
| `CYBERDECK_FONT_LG` | Montserrat 32 | Dialog titles, app card icons |
| `CYBERDECK_FONT_XL` | Montserrat 40 | Launcher card icons, system time display, PIN dot characters |

**Copy rules:**
- All labels, titles, and button text are **ALL CAPS** (`"SETTINGS"`, `"MASTER VOLUME:"`, `"CANCEL"`, `"OK"`).
- Field name labels end with a colon+space (`"SSID:"`, `"IP:"`, `"CHANNEL:"`).
- Status bar title: `"S3 CYBERDECK"` (the product name, always that exact string).
- Stub/coming-soon toast: `"Coming soon..."` (only exception to all-caps, it's a toast message).

### Spacing System

Content flows inside `ui_common_content_area()` which sets `pad_all=16`, `pad_row=14` on the flex column.

| Context | Value |
|---|---|
| Content area pad | 16 px all sides |
| Row spacing in flex column | 14 px |
| Section gap (between unrelated groups) | 18 px via `ui_common_section_gap()` |
| Section label `pad_top` | 20 px (additional separation before a group label) |
| List item pad vertical/horizontal | 12 / 8 px |
| Grid gap (launcher, theme buttons) | 12–16 px |
| Card icon-to-name gap | 4 px |
| Dialog button row `pad_column` | 8 px |
| Toast pad | 12 px horizontal, 6 px vertical |
| Dialog pad | 12 px all |

**Visual grouping rule:** Use `ui_common_section_gap()` between unrelated content groups — never dividers outside list views. `ui_common_divider()` is only valid inside a list container.

### Borders & Radius

| Widget | Border width | Radius | Border side |
|---|---|---|---|
| Container / card (major) | 2 px | 12 px (container) / 16 px (launcher card) | all |
| Button | 2 px | 12 px | all |
| Toast | 1 px | 2 px | all |
| Dialog | 1 px | 2 px | all |
| Text input | — | 8 px | all |
| List item row | 1 px | 0 | BOTTOM only |
| Navbar | 2 px | 0 | TOP (portrait) / LEFT (landscape) |
| Statusbar separator | 2 px height | 0 | top + bottom |
| Scrollbar | 2 px wide | 0 | — |
| PIN numpad button | — | 6 px | all |

### Layout Patterns

**Screen structure** (top → bottom in portrait):
1. Statusbar — `lv_obj_t` docked to top, fixed height 36 px.
2. Content area — `ui_common_content_area(screen)` fills remaining height, scrollable flex column.
3. Navbar — docked to bottom (portrait) or right (landscape), fixed width/height.

In landscape the navbar shifts to the right edge; statusbar gains a right-side border.

**Flex conventions:**
- Content column: `LV_FLEX_FLOW_COLUMN`, `LV_FLEX_ALIGN_START` — items stack top-down.
- Launcher grid: `LV_FLEX_FLOW_ROW_WRAP`, center-aligned on all axes. 5 columns (landscape) / 3 columns (portrait).
- Card content (icon+name): `LV_FLEX_FLOW_COLUMN`, center on all axes.
- Dialog: `LV_FLEX_FLOW_COLUMN`, `START` alignment.
- Button row (action_row): `LV_FLEX_FLOW_ROW`, `END` (right-aligned), `CENTER` cross.
- Icon row in statusbar: `LV_FLEX_FLOW_ROW`, `END`.

**Bottom action pattern** — screens with a primary CTA:
```
ui_common_spacer(content)       // absorbs remaining space, pins buttons to bottom
lv_obj_t *row = ui_common_action_row(content);
lv_obj_t *sec = ui_common_btn(row, "CANCEL");        // outline, left
lv_obj_t *pri = ui_common_btn(row, "SAVE");          // promote to filled:
ui_common_btn_style_primary(pri);                    // right, Fitts-compliant
```

**Data display pattern** — technical info screens (About, WiFi connected details):
```
ui_common_data_row(parent, "SSID:", ssid);    // dim SM label stacked above primary MD value
ui_common_data_row(parent, "IP:", ip);
ui_common_data_row(parent, "CHANNEL:", ch);
```
Returns the value label — caller can call `lv_label_set_text()` to update it.

### Symbols & Characters

**Prefer safe ASCII / `LV_SYMBOL_*` over raw UTF-8.** The font only includes Montserrat glyphs + LVGL symbol range. Unknown codepoints render as rectangles.

| Use case | Character |
|---|---|
| PIN filled dot | `LV_SYMBOL_BULLET` |
| PIN empty slot | `"-"` (hyphen) |
| Keyboard backspace | `LV_SYMBOL_BACKSPACE` |
| Keyboard confirm | `LV_SYMBOL_OK` |
| Battery charging | `LV_SYMBOL_CHARGE` |
| Loading cursor blink | `"_"` (underscore) |
| Missing app icon fallback | `"?"` |
| Current WiFi network marker | `"*"` prefix |
| Navbar back arrow | Custom LVGL canvas (2 px line, left-pointing triangle) |

Do not use Unicode arrows (→, ◀, ●) directly as string literals — use the `LV_SYMBOL_*` macros or draw them via canvas.

### Interaction & Behavior Patterns

**Clickable objects:** After `lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE)`, always call `lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE)` to prevent the stuck-pressed visual.

**Auto-load, no Save buttons** unless the action is destructive:
- WiFi screen: auto-scans on `on_create` + every 12 s timer.
- Audio screen: slider change auto-saves to NVS immediately.
- Time screen: auto-syncs via SNTP on create if WiFi is connected.
- Display settings: theme/rotation applied instantly on tap.
- OTA: explicit "CHECK FOR UPDATE" button — destructive, keep the button.

**One screen = one context.** Split when a screen would mix two distinct contexts; merge when two screens share the same context. Settings screens follow this rule strictly (Audio = just volume slider; Security = just PIN state; About = device info + OTA).

**Toast durations:**
- Confirmation/success: 1 200–1 500 ms.
- Default informational: 2 000 ms.
- "Coming soon": 1 500 ms.

**Transitions:** No slide/fade animations between activities — instant screen swap. Only toasts and the loading overlay have animations (fade-in + timer dismiss).

### ui_common Helper Reference

| Function | Purpose |
|---|---|
| `ui_common_content_area(screen)` | Scrollable flex-column content area below statusbar |
| `ui_common_list(parent)` | Vertical flex list container |
| `ui_common_list_add(list, text, idx, cb, data)` | Single-line tappable list row |
| `ui_common_list_add_two_line(list, primary, secondary, idx, cb, data)` | Two-line row (MD primary + SM dim secondary) |
| `ui_common_grid(parent, cols, row_h)` | Icon/card grid container |
| `ui_common_grid_cell(grid, icon, label, col, row)` | Grid cell with icon string + label |
| `ui_common_card(parent, title, w, h)` | Bordered card with optional title |
| `ui_common_btn(parent, text)` | Outline button (default style) |
| `ui_common_btn_full(parent, text)` | Full-width outline button |
| `ui_common_btn_style_primary(btn)` | Promote button to filled primary CTA |
| `ui_common_data_row(parent, label, value)` | Stacked dim-label / primary-value pair; returns value label |
| `ui_common_section_gap(parent)` | 18 px transparent spacer between content groups |
| `ui_common_spacer(parent)` | Flex-grow spacer (pushes following children to bottom) |
| `ui_common_action_row(parent)` | Right-aligned row for [secondary][primary] button pair |
| `ui_common_divider(parent)` | Horizontal rule — only inside list containers |

### ui_theme Style Helper Reference

| Function | Purpose |
|---|---|
| `ui_theme_style_container(obj)` | Black bg, 1 px dim border, radius 2 |
| `ui_theme_style_btn(btn)` | Outline style, press = color inversion |
| `ui_theme_style_label(lbl, font)` | Primary color + given font |
| `ui_theme_style_label_dim(lbl, font)` | Dim color + given font |
| `ui_theme_style_list_item(obj)` | Bottom border only, no radius, pad 12/8 |
| `ui_theme_style_textarea(ta)` | Black bg, primary border, blinking cursor |
| `ui_theme_style_scrollbar(obj)` | 2 px wide, dim color, radius 0 |

## Hardware Notes

- **No Bluetooth Classic on ESP32-S3.** A2DP is impossible. Audio uses an optional external BT module on UART1 (GPIO 15/16), auto-detected at boot.
- **GPIO 0 (BOOT)** is owned by the RGB LCD peripheral as DATA6 — it cannot be used as a runtime button. Navigation uses touch gestures (swipe up = HOME, swipe down = BACK).
- **Waveshare example code** reference: `~/Downloads/ESP32-S3-Touch-LCD-4.3-Demo 2/ESP-IDF/`
