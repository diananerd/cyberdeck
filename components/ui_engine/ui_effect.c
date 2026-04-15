/*
 * S3 Cyber-Deck — UI effects: toast, confirm dialog, loading overlay
 * All effects render on lv_layer_top(), independent of the activity stack.
 */

#include "ui_effect.h"
#include "ui_navbar.h"
#include "ui_theme.h"
#include "esp_log.h"
#include <string.h>

/* Horizontal offset to center elements within the non-navbar content area */
#define CONTENT_OFFSET_X   (-(UI_NAVBAR_THICK / 2))

static const char *TAG = "ui_effect";

/* ========== Toast ========== */

static lv_obj_t *toast_obj = NULL;

static void toast_timer_cb(lv_timer_t *timer)
{
    if (toast_obj) {
        lv_obj_del(toast_obj);
        toast_obj = NULL;
    }
    lv_timer_del(timer);
}

void ui_effect_toast(const char *msg, uint16_t ms)
{
    if (!msg) return;
    if (ms == 0) ms = 2000;

    /* Remove existing toast if any */
    if (toast_obj) {
        lv_obj_del(toast_obj);
        toast_obj = NULL;
    }

    const cyberdeck_theme_t *t = ui_theme_get();

    /* Toast container */
    toast_obj = lv_obj_create(lv_layer_top());
    lv_obj_set_size(toast_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(toast_obj, LV_ALIGN_BOTTOM_MID, CONTENT_OFFSET_X, -30);
    lv_obj_set_style_bg_color(toast_obj, t->bg_card, 0);
    lv_obj_set_style_bg_opa(toast_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(toast_obj, t->primary, 0);
    lv_obj_set_style_border_width(toast_obj, 1, 0);
    lv_obj_set_style_radius(toast_obj, 2, 0);
    lv_obj_set_style_pad_hor(toast_obj, 12, 0);
    lv_obj_set_style_pad_ver(toast_obj, 6, 0);
    lv_obj_clear_flag(toast_obj, LV_OBJ_FLAG_SCROLLABLE);

    /* Toast label */
    lv_obj_t *label = lv_label_create(toast_obj);
    lv_label_set_text(label, msg);
    lv_obj_set_style_text_color(label, t->text, 0);
    lv_obj_set_style_text_font(label, &CYBERDECK_FONT_SM, 0);
    /* Keep within content area (screen width minus navbar) */
    lv_disp_t *disp = lv_disp_get_default();
    lv_coord_t max_w = lv_disp_get_hor_res(disp) - UI_NAVBAR_THICK - 40;
    lv_obj_set_style_max_width(label, max_w, 0);

    /* Auto-dismiss timer */
    lv_timer_create(toast_timer_cb, ms, NULL);

    ESP_LOGD(TAG, "Toast: %s (%dms)", msg, ms);
}

/* ========== Confirm Dialog ========== */

typedef struct {
    ui_confirm_cb_t cb;
    void *ctx;
    lv_obj_t *dialog;
} confirm_state_t;

static confirm_state_t confirm_st = { 0 };

static void confirm_btn_cb(lv_event_t *e)
{
    bool confirmed = (bool)(uintptr_t)lv_event_get_user_data(e);

    if (confirm_st.cb) {
        confirm_st.cb(confirmed, confirm_st.ctx);
    }

    if (confirm_st.dialog) {
        lv_obj_del(confirm_st.dialog);
        confirm_st.dialog = NULL;
    }
    memset(&confirm_st, 0, sizeof(confirm_st));
}

void ui_effect_confirm(const char *title, const char *msg,
                       ui_confirm_cb_t cb, void *ctx)
{
    /* Remove existing dialog */
    if (confirm_st.dialog) {
        lv_obj_del(confirm_st.dialog);
        confirm_st.dialog = NULL;
    }

    const cyberdeck_theme_t *t = ui_theme_get();

    /* Semi-transparent backdrop */
    lv_obj_t *backdrop = lv_obj_create(lv_layer_top());
    lv_obj_set_size(backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_align(backdrop, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_set_style_radius(backdrop, 0, 0);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);

    /* Dialog box */
    lv_obj_t *dialog = lv_obj_create(backdrop);
    lv_obj_set_size(dialog, 350, LV_SIZE_CONTENT);
    lv_obj_align(dialog, LV_ALIGN_CENTER, CONTENT_OFFSET_X, 0);
    lv_obj_set_style_bg_color(dialog, t->bg_dark, 0);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dialog, t->primary, 0);
    lv_obj_set_style_border_width(dialog, 1, 0);
    lv_obj_set_style_radius(dialog, 2, 0);
    lv_obj_set_style_pad_all(dialog, 12, 0);
    lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dialog, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(dialog, 8, 0);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    if (title && title[0]) {
        lv_obj_t *lbl_title = lv_label_create(dialog);
        lv_label_set_text(lbl_title, title);
        lv_obj_set_style_text_color(lbl_title, t->text, 0);
        lv_obj_set_style_text_font(lbl_title, &CYBERDECK_FONT_LG, 0);
    }

    /* Message */
    if (msg && msg[0]) {
        lv_obj_t *lbl_msg = lv_label_create(dialog);
        lv_label_set_text(lbl_msg, msg);
        lv_obj_set_style_text_color(lbl_msg, t->text_dim, 0);
        lv_obj_set_style_text_font(lbl_msg, &CYBERDECK_FONT_MD, 0);
        lv_obj_set_width(lbl_msg, LV_PCT(100));
    }

    /* Button row */
    lv_obj_t *btn_row = lv_obj_create(dialog);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 8, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Cancel button */
    lv_obj_t *btn_cancel = lv_btn_create(btn_row);
    ui_theme_style_btn(btn_cancel);
    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "CANCEL");
    lv_obj_add_event_cb(btn_cancel, confirm_btn_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)false);

    /* OK button */
    lv_obj_t *btn_ok = lv_btn_create(btn_row);
    ui_theme_style_btn(btn_ok);
    lv_obj_t *lbl_ok = lv_label_create(btn_ok);
    lv_label_set_text(lbl_ok, "OK");
    lv_obj_add_event_cb(btn_ok, confirm_btn_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)true);

    /* Store state */
    confirm_st.cb = cb;
    confirm_st.ctx = ctx;
    confirm_st.dialog = backdrop;  /* Delete the whole backdrop to dismiss */

    ESP_LOGD(TAG, "Confirm dialog: %s", title ? title : "(no title)");
}

/* ========== Loading overlay ========== */

static lv_obj_t *loading_obj = NULL;
static lv_timer_t *loading_timer = NULL;

static void loading_blink_cb(lv_timer_t *timer)
{
    lv_obj_t *cursor = (lv_obj_t *)timer->user_data;
    if (!cursor) return;
    bool hidden = lv_obj_has_flag(cursor, LV_OBJ_FLAG_HIDDEN);
    if (hidden) {
        lv_obj_clear_flag(cursor, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(cursor, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_effect_loading(bool show)
{
    if (!show) {
        if (loading_timer) {
            lv_timer_del(loading_timer);
            loading_timer = NULL;
        }
        if (loading_obj) {
            lv_obj_del(loading_obj);
            loading_obj = NULL;
        }
        return;
    }

    if (loading_obj) return;  /* Already showing */

    const cyberdeck_theme_t *t = ui_theme_get();

    /* Semi-transparent overlay */
    loading_obj = lv_obj_create(lv_layer_top());
    lv_obj_set_size(loading_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_align(loading_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(loading_obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(loading_obj, LV_OPA_70, 0);
    lv_obj_set_style_border_width(loading_obj, 0, 0);
    lv_obj_set_style_radius(loading_obj, 0, 0);
    lv_obj_clear_flag(loading_obj, LV_OBJ_FLAG_SCROLLABLE);

    /* Blinking cursor label */
    lv_obj_t *cursor = lv_label_create(loading_obj);
    lv_label_set_text(cursor, "_");
    lv_obj_set_style_text_color(cursor, t->primary, 0);
    lv_obj_set_style_text_font(cursor, &CYBERDECK_FONT_XL, 0);
    lv_obj_align(cursor, LV_ALIGN_CENTER, CONTENT_OFFSET_X, 0);

    /* Blink at 500ms interval */
    loading_timer = lv_timer_create(loading_blink_cb, 500, cursor);

    ESP_LOGD(TAG, "Loading overlay shown");
}
