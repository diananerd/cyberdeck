#pragma once

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Font aliases (fallback to Montserrat 14 until Roboto Mono generated) ---------- */

/* Generate with: lv_font_conv --bpp 4 --size <N> --font RobotoMono-Regular.ttf -r 0x20-0x7F --format lvgl -o font_roboto_mono_<N>.c */
/* Once generated, place in components/ui_engine/fonts/ and replace these externs */

#ifndef CYBERDECK_FONT_SM
#define CYBERDECK_FONT_SM      lv_font_montserrat_14   /* target: Roboto Mono 12 */
#endif
#ifndef CYBERDECK_FONT_MD
#define CYBERDECK_FONT_MD      lv_font_montserrat_14   /* target: Roboto Mono 14 */
#endif
#ifndef CYBERDECK_FONT_LG
#define CYBERDECK_FONT_LG      lv_font_montserrat_14   /* target: Roboto Mono 18 */
#endif
#ifndef CYBERDECK_FONT_XL
#define CYBERDECK_FONT_XL      lv_font_montserrat_14   /* target: Roboto Mono 24 */
#endif

/* ---------- Theme IDs ---------- */

typedef enum {
    THEME_GREEN = 0,    /* Matrix */
    THEME_AMBER = 1,    /* Retro terminal */
    THEME_NEON  = 2,    /* Cyberpunk multi-color */
    THEME_COUNT
} cyberdeck_theme_id_t;

/* ---------- Theme palette ---------- */

typedef struct {
    lv_color_t primary;         /* Main outline/text color */
    lv_color_t primary_dim;     /* Dimmed borders, inactive */
    lv_color_t bg_dark;         /* Always #000000 */
    lv_color_t bg_card;         /* Slightly raised: #0A0A0A */
    lv_color_t text;            /* Same as primary */
    lv_color_t text_dim;        /* 50% primary */
    /* Neon extras (ignored by green/amber) */
    lv_color_t secondary;       /* Cyan (#00FFFF) for neon */
    lv_color_t accent;          /* Hot pink (#FF0055) for neon */
    lv_color_t success;         /* Neon green (#39FF14) for neon */
    const char *name;
} cyberdeck_theme_t;

/* ---------- Public API ---------- */

/**
 * @brief Apply a cyberdeck theme globally. Call after ui_engine_init().
 *        Must be called with the LVGL mutex held.
 * @param id Theme to apply
 */
void ui_theme_apply(cyberdeck_theme_id_t id);

/**
 * @brief Get the currently active theme palette.
 */
const cyberdeck_theme_t *ui_theme_get(void);

/**
 * @brief Get current theme ID.
 */
cyberdeck_theme_id_t ui_theme_get_id(void);

/* ---------- Style helpers (apply cyberdeck styling to widgets) ---------- */

/** Style a container: black bg, 1px dim border, radius 2 */
void ui_theme_style_container(lv_obj_t *obj);

/** Style a button: outline only, inverts on press */
void ui_theme_style_btn(lv_obj_t *btn);

/** Style a label with primary color and given font */
void ui_theme_style_label(lv_obj_t *label, const lv_font_t *font);

/** Style a label with dimmed color */
void ui_theme_style_label_dim(lv_obj_t *label, const lv_font_t *font);

/** Style a list item row: bottom border only */
void ui_theme_style_list_item(lv_obj_t *obj);

/** Style a text input: black bg, primary border, blinking cursor color */
void ui_theme_style_textarea(lv_obj_t *ta);

/** Style a scrollbar: 2px wide, dim color */
void ui_theme_style_scrollbar(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
