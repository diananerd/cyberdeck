/*
 * CyberDeck — On-screen keyboard
 * Terminal-styled LVGL keyboard with cyberdeck theme colors.
 */

#include "ui_keyboard.h"
#include "ui_theme.h"
#include "esp_log.h"

static const char *TAG = "keyboard";

static lv_obj_t *kb_obj = NULL;

static void apply_theme(lv_obj_t *kb)
{
    const cyberdeck_theme_t *t = ui_theme_get();

    /* Main background */
    lv_obj_set_style_bg_color(kb, t->bg_dark, 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(kb, t->primary, 0);
    lv_obj_set_style_border_width(kb, 1, 0);
    lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 2, 0);
    lv_obj_set_style_pad_gap(kb, 2, 0);

    /* Key buttons (items) */
    lv_obj_set_style_bg_color(kb, t->bg_card, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, t->primary_dim, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 2, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, t->text, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &CYBERDECK_FONT_MD, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);

    /* Pressed state: invert */
    lv_obj_set_style_bg_color(kb, t->primary, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, t->bg_dark, LV_PART_ITEMS | LV_STATE_PRESSED);

    /* Checked keys (shift, special keys) */
    lv_obj_set_style_bg_color(kb, t->primary_dim, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, t->text, LV_PART_ITEMS | LV_STATE_CHECKED);
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        ui_keyboard_hide();
    }
}

void ui_keyboard_show(lv_obj_t *ta)
{
    if (!ta) return;

    if (kb_obj) {
        /* Already visible — just rebind */
        lv_keyboard_set_textarea(kb_obj, ta);
        return;
    }

    /* Create keyboard on current screen */
    lv_obj_t *scr = lv_scr_act();
    kb_obj = lv_keyboard_create(scr);
    lv_obj_set_size(kb_obj, LV_PCT(100), 200);
    lv_obj_align(kb_obj, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb_obj, ta);
    lv_obj_add_event_cb(kb_obj, kb_event_cb, LV_EVENT_ALL, NULL);

    apply_theme(kb_obj);

    ESP_LOGD(TAG, "Keyboard shown");
}

void ui_keyboard_hide(void)
{
    if (!kb_obj) return;
    lv_obj_del(kb_obj);
    kb_obj = NULL;
    ESP_LOGD(TAG, "Keyboard hidden");
}

bool ui_keyboard_is_visible(void)
{
    return kb_obj != NULL;
}

void ui_keyboard_refresh_theme(void)
{
    if (kb_obj) {
        apply_theme(kb_obj);
    }
}
