/*
 * S3 Cyber-Deck — Settings > Security
 * PIN lock enable/disable and PIN change.
 * Uses a 2-pass 4-digit numeric entry via LVGL number keyboard.
 * Keyboard slides up from the bottom of the screen when active.
 */

#include "app_settings.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "svc_settings.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "settings_security";

#define PIN_LEN 4

typedef enum { SEC_STATE_IDLE, SEC_STATE_NEW_PIN, SEC_STATE_CONFIRM_PIN } sec_state_e;

typedef struct {
    sec_state_e  state;
    char         first_pin[PIN_LEN + 1];
    char         cur_pin[PIN_LEN + 1];
    uint8_t      cur_len;
    lv_obj_t    *dots[PIN_LEN];
    lv_obj_t    *instruction_lbl;
    lv_obj_t    *status_lbl;
    bool         pin_enabled;
    lv_obj_t    *kb;            /* LVGL number keyboard (hidden when idle) */
    lv_obj_t    *ta;            /* hidden textarea driven by keyboard */
} sec_state_t;

static uint32_t djb2(const char *s)
{
    uint32_t h = 5381;
    int c;
    while ((c = (unsigned char)*s++) != '\0') {
        h = ((h << 5) + h) + c;
    }
    return h;
}

static void refresh_dots(sec_state_t *s)
{
    const cyberdeck_theme_t *t = ui_theme_get();
    for (int i = 0; i < PIN_LEN; i++) {
        lv_label_set_text(s->dots[i], (i < (int)s->cur_len) ? LV_SYMBOL_BULLET : "-");
        lv_obj_set_style_text_color(s->dots[i],
            (i < (int)s->cur_len) ? t->primary : t->primary_dim, 0);
    }
}

static void reset_pin_entry(sec_state_t *s)
{
    s->cur_len = 0;
    memset(s->cur_pin, 0, sizeof(s->cur_pin));
    if (s->ta) lv_textarea_set_text(s->ta, "");
    refresh_dots(s);
}

/* ---- Core PIN enter logic (shared between old enter_cb and keyboard handler) ---- */

static void sec_enter_logic(sec_state_t *s)
{
    if (s->state == SEC_STATE_IDLE) return;

    if (s->cur_len < PIN_LEN) {
        lv_label_set_text(s->status_lbl, "Enter all 4 digits");
        return;
    }

    if (s->state == SEC_STATE_NEW_PIN) {
        strncpy(s->first_pin, s->cur_pin, sizeof(s->first_pin));
        s->state = SEC_STATE_CONFIRM_PIN;
        lv_label_set_text(s->instruction_lbl, "Confirm new PIN:");
        reset_pin_entry(s);

    } else if (s->state == SEC_STATE_CONFIRM_PIN) {
        if (strcmp(s->cur_pin, s->first_pin) == 0) {
            uint32_t h = djb2(s->cur_pin);
            svc_settings_set_pin_hash(h);
            svc_settings_set_pin_enabled(true);
            s->pin_enabled = true;
            s->state = SEC_STATE_IDLE;
            lv_label_set_text(s->instruction_lbl, "PIN lock enabled");
            lv_label_set_text(s->status_lbl, "PIN saved successfully");
            reset_pin_entry(s);
            /* Hide keyboard once done */
            if (s->kb) lv_obj_add_flag(s->kb, LV_OBJ_FLAG_HIDDEN);
            ui_effect_toast("PIN lock enabled", 1500);
            ESP_LOGI(TAG, "PIN set and enabled");
        } else {
            s->state = SEC_STATE_NEW_PIN;
            lv_label_set_text(s->instruction_lbl, "Enter new PIN:");
            lv_label_set_text(s->status_lbl, "PINs did not match — try again");
            reset_pin_entry(s);
        }
    }
}

/* ---- Keyboard event callback ---- */

static void sec_kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    sec_state_t    *s    = (sec_state_t *)lv_event_get_user_data(e);

    if (s->state == SEC_STATE_IDLE) return;

    if (code == LV_EVENT_READY) {
        sec_enter_logic(s);
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED) {
        /* Sync cur_pin / cur_len from what the keyboard typed into the textarea */
        const char *text = lv_textarea_get_text(s->ta);
        uint8_t len = (uint8_t)strlen(text);
        if (len > PIN_LEN) len = PIN_LEN;
        s->cur_len = len;
        strncpy(s->cur_pin, text, PIN_LEN);
        s->cur_pin[PIN_LEN] = '\0';
        lv_label_set_text(s->status_lbl, "");
        refresh_dots(s);
    }
}

/* ---- Set PIN button ---- */

static void set_pin_btn_cb(lv_event_t *e)
{
    sec_state_t *s = (sec_state_t *)lv_event_get_user_data(e);
    s->state = SEC_STATE_NEW_PIN;
    lv_label_set_text(s->instruction_lbl, "Enter new PIN:");
    lv_label_set_text(s->status_lbl, "");
    reset_pin_entry(s);
    /* Show the keyboard */
    if (s->kb) lv_obj_clear_flag(s->kb, LV_OBJ_FLAG_HIDDEN);
}

/* ---- Disable PIN button ---- */

static void disable_pin_btn_cb(lv_event_t *e)
{
    sec_state_t *s = (sec_state_t *)lv_event_get_user_data(e);
    svc_settings_set_pin_enabled(false);
    svc_settings_set_pin_hash(0);
    s->pin_enabled = false;
    s->state = SEC_STATE_IDLE;
    lv_label_set_text(s->instruction_lbl, "PIN lock disabled");
    lv_label_set_text(s->status_lbl, "");
    reset_pin_entry(s);
    if (s->kb) lv_obj_add_flag(s->kb, LV_OBJ_FLAG_HIDDEN);
    ui_effect_toast("PIN lock disabled", 1500);
    ESP_LOGI(TAG, "PIN disabled");
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

    lv_obj_set_style_bg_color(kb, t->bg_dark, 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 6, 0);
    lv_obj_set_style_pad_gap(kb, 6, 0);

    lv_obj_set_style_bg_color(kb, t->bg_card, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, t->text, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &CYBERDECK_FONT_SM, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, t->primary_dim, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 6, LV_PART_ITEMS);

    lv_obj_set_style_bg_color(kb, t->primary, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, t->bg_dark, LV_PART_ITEMS | LV_STATE_PRESSED);
}

/* ---- Activity ---- */

static void security_on_create(lv_obj_t *screen, void *intent_data)
{
    (void)intent_data;
    const cyberdeck_theme_t *t = ui_theme_get();

    sec_state_t *s = (sec_state_t *)calloc(1, sizeof(sec_state_t));
    if (!s) return;
    ui_activity_set_state(s);
    s->state = SEC_STATE_IDLE;

    bool pin_en = false;
    svc_settings_get_pin_enabled(&pin_en);
    s->pin_enabled = pin_en;

    ui_statusbar_set_title("SETTINGS");

    lv_obj_t *content = ui_common_content_area(screen);

    /* Current status */
    lv_obj_t *status = lv_label_create(content);
    lv_label_set_text(status, pin_en ? "PIN Lock: Enabled" : "PIN Lock: Disabled");
    ui_theme_style_label(status, &CYBERDECK_FONT_MD);

    /* Buttons to enable/disable */
    lv_obj_t *set_btn = ui_common_btn_full(content, "Set / Change PIN");
    lv_obj_add_event_cb(set_btn, set_pin_btn_cb, LV_EVENT_CLICKED, s);

    if (pin_en) {
        lv_obj_t *dis_btn = ui_common_btn_full(content, "Disable PIN Lock");
        lv_obj_add_event_cb(dis_btn, disable_pin_btn_cb, LV_EVENT_CLICKED, s);
    }

    ui_common_divider(content);

    /* Instruction label */
    s->instruction_lbl = lv_label_create(content);
    lv_label_set_text(s->instruction_lbl, "Tap 'Set / Change PIN' to begin");
    ui_theme_style_label_dim(s->instruction_lbl, &CYBERDECK_FONT_SM);

    /* PIN dot row */
    lv_obj_t *dot_row = lv_obj_create(content);
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
    s->status_lbl = lv_label_create(content);
    lv_label_set_text(s->status_lbl, "");
    ui_theme_style_label_dim(s->status_lbl, &CYBERDECK_FONT_SM);

    /* ---- Hidden textarea — keyboard writes digits here ---- */
    s->ta = lv_textarea_create(screen);
    lv_obj_add_flag(s->ta, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_max_length(s->ta, PIN_LEN);
    lv_textarea_set_one_line(s->ta, true);
    lv_textarea_set_accepted_chars(s->ta, "0123456789");

    /* ---- LVGL number keyboard — hidden until "Set PIN" is tapped ---- */
    s->kb = lv_keyboard_create(screen);
    lv_keyboard_set_mode(s->kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_map(s->kb, LV_KEYBOARD_MODE_NUMBER, s_pin_map, s_pin_ctrl);
    lv_keyboard_set_textarea(s->kb, s->ta);
    lv_obj_align(s->kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s->kb, LV_OBJ_FLAG_HIDDEN);
    style_keyboard(s->kb);

    lv_obj_add_event_cb(s->kb, sec_kb_event_cb, LV_EVENT_ALL, s);
}

static void security_on_destroy(lv_obj_t *screen, void *state)
{
    (void)screen;
    free(state);
}

const activity_cbs_t settings_security_cbs = {
    .on_create  = security_on_create,
    .on_resume  = NULL,
    .on_pause   = NULL,
    .on_destroy = security_on_destroy,
};
