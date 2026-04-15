/*
 * S3 Cyber-Deck — Cyberdeck theme system
 * Terminal-inspired monochromatic themes with outlined LVGL widgets.
 */

#include "ui_theme.h"
#include "esp_log.h"

static const char *TAG = "ui_theme";

/* ---------- Theme palettes ---------- */

static const cyberdeck_theme_t themes[THEME_COUNT] = {
    [THEME_GREEN] = {
        .primary     = { .full = 0x07E8 },  /* #00FF41 -> RGB565 */
        .primary_dim = { .full = 0x0269 },  /* #004D13 */
        .bg_dark     = { .full = 0x0000 },  /* #000000 */
        .bg_card     = { .full = 0x0841 },  /* #0A0A0A */
        .text        = { .full = 0x07E8 },  /* #00FF41 */
        .text_dim    = { .full = 0x0269 },  /* #004D13 */
        .secondary   = { .full = 0x07E8 },  /* same as primary */
        .accent      = { .full = 0x07E8 },
        .success     = { .full = 0x07E8 },
        .name        = "Green",
    },
    [THEME_AMBER] = {
        .primary     = { .full = 0xFD80 },  /* #FFB000 */
        .primary_dim = { .full = 0x49A0 },  /* #4D3500 */
        .bg_dark     = { .full = 0x0000 },
        .bg_card     = { .full = 0x0841 },
        .text        = { .full = 0xFD80 },
        .text_dim    = { .full = 0x49A0 },
        .secondary   = { .full = 0xFD80 },
        .accent      = { .full = 0xFD80 },
        .success     = { .full = 0xFD80 },
        .name        = "Amber",
    },
    [THEME_NEON] = {
        .primary     = { .full = 0xF81F },  /* #FF00FF magenta */
        .primary_dim = { .full = 0x780F },  /* dim magenta */
        .bg_dark     = { .full = 0x0000 },
        .bg_card     = { .full = 0x0841 },
        .text        = { .full = 0xF81F },
        .text_dim    = { .full = 0x780F },
        .secondary   = { .full = 0x07FF },  /* #00FFFF cyan */
        .accent      = { .full = 0xF80A },  /* #FF0055 hot pink */
        .success     = { .full = 0x3FE2 },  /* #39FF14 neon green */
        .name        = "Neon",
    },
};

static cyberdeck_theme_id_t current_theme_id = THEME_GREEN;

/* ---------- Public API ---------- */

void ui_theme_apply(cyberdeck_theme_id_t id)
{
    if (id >= THEME_COUNT) {
        id = THEME_GREEN;
    }
    current_theme_id = id;
    const cyberdeck_theme_t *t = &themes[id];

    /* Set the default display theme: black background */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, t->bg_dark, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, t->text, 0);
    lv_obj_set_style_text_font(scr, &CYBERDECK_FONT_MD, 0);

    /* Global scrollbar styling (LVGL 8.x uses width + bg_color on SCROLLBAR part) */
    lv_obj_set_style_width(scr, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(scr, t->primary_dim, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_SCROLLBAR);

    ESP_LOGI(TAG, "Theme applied: %s", t->name);
}

const cyberdeck_theme_t *ui_theme_get(void)
{
    return &themes[current_theme_id];
}

cyberdeck_theme_id_t ui_theme_get_id(void)
{
    return current_theme_id;
}

/* ---------- Style helpers ---------- */

void ui_theme_style_container(lv_obj_t *obj)
{
    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_set_style_bg_color(obj, t->bg_dark, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, t->primary_dim, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 2, 0);
    lv_obj_set_style_pad_all(obj, 4, 0);
    /* Scrollbar */
    lv_obj_set_style_width(obj, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(obj, t->primary_dim, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_SCROLLBAR);
}

void ui_theme_style_btn(lv_obj_t *btn)
{
    const cyberdeck_theme_t *t = ui_theme_get();

    /* Default state: outline only */
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(btn, t->primary, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 2, 0);
    lv_obj_set_style_text_color(btn, t->primary, 0);
    lv_obj_set_style_text_font(btn, &CYBERDECK_FONT_MD, 0);
    lv_obj_set_style_pad_ver(btn, 6, 0);
    lv_obj_set_style_pad_hor(btn, 12, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    /* Pressed state: invert (filled bg, black text) */
    lv_obj_set_style_bg_color(btn, t->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, t->bg_dark, LV_STATE_PRESSED);

    /* Focused state: primary outline (no glow) */
    lv_obj_set_style_border_color(btn, t->primary, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(btn, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(btn, t->primary, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(btn, 2, LV_STATE_FOCUSED);
}

void ui_theme_style_label(lv_obj_t *label, const lv_font_t *font)
{
    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_set_style_text_color(label, t->text, 0);
    if (font) {
        lv_obj_set_style_text_font(label, font, 0);
    }
}

void ui_theme_style_label_dim(lv_obj_t *label, const lv_font_t *font)
{
    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_set_style_text_color(label, t->text_dim, 0);
    if (font) {
        lv_obj_set_style_text_font(label, font, 0);
    }
}

void ui_theme_style_list_item(lv_obj_t *obj)
{
    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, t->primary_dim, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_ver(obj, 6, 0);
    lv_obj_set_style_pad_hor(obj, 4, 0);
}

void ui_theme_style_textarea(lv_obj_t *ta)
{
    const cyberdeck_theme_t *t = ui_theme_get();

    /* Main area */
    lv_obj_set_style_bg_color(ta, t->bg_dark, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, t->primary_dim, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 2, 0);
    lv_obj_set_style_text_color(ta, t->text, 0);
    lv_obj_set_style_text_font(ta, &CYBERDECK_FONT_MD, 0);

    /* Cursor: primary color block (terminal underscore style) */
    lv_obj_set_style_border_color(ta, t->primary, LV_PART_CURSOR);
    lv_obj_set_style_border_width(ta, 1, LV_PART_CURSOR);
    lv_obj_set_style_bg_color(ta, t->primary, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_CURSOR);

    /* Scrollbar */
    lv_obj_set_style_width(ta, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(ta, t->primary_dim, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_SCROLLBAR);
}

void ui_theme_style_scrollbar(lv_obj_t *obj)
{
    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_set_style_width(obj, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(obj, t->primary_dim, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_SCROLLBAR);
}
