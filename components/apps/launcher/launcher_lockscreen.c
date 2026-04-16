/*
 * CyberDeck — Lock Screen
 * Full-screen (no statusbar/navbar), 4-digit PIN entry via LVGL numpad.
 * Hides system bars on create, restores on destroy.
 */

#include "app_manager.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_engine.h"
#include "ui_statusbar.h"
#include "ui_navbar.h"
#include "svc_settings.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "lockscreen";

#define PIN_LEN 4

typedef struct {
    char      entered[PIN_LEN + 1];
    uint8_t   len;
    lv_obj_t *dots[PIN_LEN];
    lv_obj_t *status_lbl;
    lv_obj_t *ta;           /* hidden textarea driven by keyboard */
} lock_state_t;

/* ---- djb2 hash ---- */

static uint32_t djb2(const char *s)
{
    uint32_t h = 5381;
    int c;
    while ((c = (unsigned char)*s++) != '\0') {
        h = ((h << 5) + h) + c;
    }
    return h;
}

/* ---- Dot indicator refresh ---- */

static void refresh_dots(lock_state_t *s)
{
    const cyberdeck_theme_t *t = ui_theme_get();
    for (int i = 0; i < PIN_LEN; i++) {
        lv_label_set_text(s->dots[i], (i < s->len) ? LV_SYMBOL_BULLET : "-");
        lv_obj_set_style_text_color(s->dots[i],
            (i < s->len) ? t->primary : t->primary_dim, 0);
    }
}

/* ---- PIN validation ---- */

static void validate_pin(lock_state_t *s)
{
    if (s->len < PIN_LEN) {
        lv_label_set_text(s->status_lbl, "Enter all 4 digits");
        return;
    }

    uint32_t stored = 0;
    svc_settings_get_pin_hash(&stored);

    /* stored == 0: no PIN was ever set → accept anything */
    if (stored == 0 || djb2(s->entered) == stored) {
        ESP_LOGI(TAG, "PIN accepted — unlocking");
        ui_activity_pop();
    } else {
        ESP_LOGW(TAG, "Wrong PIN");
        s->len = 0;
        memset(s->entered, 0, sizeof(s->entered));
        lv_textarea_set_text(s->ta, "");
        refresh_dots(s);
        lv_label_set_text(s->status_lbl, "Incorrect PIN");
    }
}

/* ---- Keyboard event callback ---- */

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lock_state_t   *s    = (lock_state_t *)lv_event_get_user_data(e);

    if (code == LV_EVENT_READY) {
        validate_pin(s);
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED) {
        /* Sync state from the textarea the keyboard writes into */
        const char *text = lv_textarea_get_text(s->ta);
        uint8_t len = (uint8_t)strlen(text);
        if (len > PIN_LEN) len = PIN_LEN;
        s->len = len;
        strncpy(s->entered, text, PIN_LEN);
        s->entered[PIN_LEN] = '\0';
        lv_label_set_text(s->status_lbl, "");
        refresh_dots(s);
    }
}

/* ---- Minimal numpad map: digits + backspace + OK only ---- */

static const char *s_pin_map[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""
};
static const lv_btnmatrix_ctrl_t s_pin_ctrl[] = {
    1, 1, 1,
    1, 1, 1,
    1, 1, 1,
    1, 1, 1,
};

/* ---- Apply cyberdeck theme to the keyboard widget ---- */

static void style_keyboard(lv_obj_t *kb)
{
    const cyberdeck_theme_t *t = ui_theme_get();

    /* Container */
    lv_obj_set_style_bg_color(kb, t->bg_dark, 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 6, 0);
    lv_obj_set_style_pad_gap(kb, 6, 0);

    /* Button items — normal state */
    lv_obj_set_style_bg_color(kb, t->bg_card, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, t->text, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &CYBERDECK_FONT_MD, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, t->primary_dim, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 6, LV_PART_ITEMS);

    /* Button items — pressed state */
    lv_obj_set_style_bg_color(kb, t->primary, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, t->bg_dark, LV_PART_ITEMS | LV_STATE_PRESSED);
}

/* ---- Activity on_create ---- */

static void *lockscreen_on_create(lv_obj_t *screen, const view_args_t *args, void *app_data)
{
    (void)args;
    (void)app_data;
    const cyberdeck_theme_t *t = ui_theme_get();

    lock_state_t *s = (lock_state_t *)calloc(1, sizeof(lock_state_t));
    if (!s) return NULL;

    /* Full screen: remove the padding added by the activity stack,
     * then hide system bars so we own the entire display. */
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_pad_left(screen, 0, 0);
    lv_obj_set_style_pad_right(screen, 0, 0);
    ui_statusbar_set_visible(false);
    ui_navbar_set_visible(false);

    lv_disp_t  *disp  = lv_disp_get_default();
    lv_coord_t  scr_w = lv_disp_get_hor_res(disp);
    lv_coord_t  scr_h = lv_disp_get_ver_res(disp);

    /* ---- Top half: fixed container that owns exactly the upper 50% ----
     * Flex column, centered both axes → content always vertically centered
     * within the top half regardless of display orientation. */
    lv_obj_t *top = lv_obj_create(screen);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_size(top, scr_w, scr_h / 2);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 16, 0);
    lv_obj_set_style_pad_row(top, 16, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Title */
    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, "LOCKED");
    lv_obj_set_style_text_color(title, t->primary, 0);
    lv_obj_set_style_text_font(title, &CYBERDECK_FONT_XL, 0);

    /* PIN dot row */
    lv_obj_t *dot_row = lv_obj_create(top);
    lv_obj_set_size(dot_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(dot_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dot_row, 0, 0);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dot_row, 24, 0);
    lv_obj_clear_flag(dot_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < PIN_LEN; i++) {
        s->dots[i] = lv_label_create(dot_row);
        lv_label_set_text(s->dots[i], "-");
        lv_obj_set_style_text_color(s->dots[i], t->primary_dim, 0);
        lv_obj_set_style_text_font(s->dots[i], &CYBERDECK_FONT_LG, 0);
    }

    /* Status / error label */
    s->status_lbl = lv_label_create(top);
    lv_label_set_text(s->status_lbl, "Enter PIN");
    lv_obj_set_style_text_color(s->status_lbl, t->text_dim, 0);
    lv_obj_set_style_text_font(s->status_lbl, &CYBERDECK_FONT_SM, 0);

    /* ---- Hidden textarea — keyboard writes digits here ---- */
    s->ta = lv_textarea_create(screen);
    lv_obj_add_flag(s->ta, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_max_length(s->ta, PIN_LEN);
    lv_textarea_set_one_line(s->ta, true);
    lv_textarea_set_accepted_chars(s->ta, "0123456789");

    /* ---- LVGL number keyboard — capped at 480 px wide ---- */
    lv_coord_t kb_w = scr_w < 480 ? scr_w : 480;
    lv_obj_t *kb = lv_keyboard_create(screen);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_NUMBER, s_pin_map, s_pin_ctrl);
    lv_keyboard_set_textarea(kb, s->ta);
    lv_obj_set_width(kb, kb_w);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    style_keyboard(kb);

    /* Custom event handler (fires alongside the default keyboard handler) */
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_ALL, s);

    /* (Re)apply lock so HOME/BACK gestures can't bypass this screen.
     * Called here too because recreate_all() invokes on_destroy (clear_lock)
     * followed immediately by on_create — the lock must be restored. */
    app_manager_set_lock();
    return s;
}

static void lockscreen_on_destroy(lv_obj_t *screen, void *view_state, void *app_data)
{
    (void)screen;
    (void)app_data;
    app_manager_clear_lock();
    /* Restore system bars before freeing */
    ui_statusbar_set_visible(true);
    ui_navbar_set_visible(true);
    free(view_state);
}

static const view_cbs_t s_lockscreen_cbs = {
    .on_create  = lockscreen_on_create,
    .on_resume  = NULL,
    .on_pause   = NULL,
    .on_destroy = lockscreen_on_destroy,
};

/* Called from app_launcher_register() */
void launcher_lockscreen_register(void)
{
    app_manager_set_lockscreen_cbs(&s_lockscreen_cbs);
    ESP_LOGI(TAG, "Lockscreen registered");
}
