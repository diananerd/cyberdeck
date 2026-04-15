/*
 * S3 Cyber-Deck — UI effects: toast, confirm dialog, loading overlay
 * All effects render on lv_layer_top(), independent of the activity stack.
 */

#include "ui_effect.h"
#include "ui_navbar.h"
#include "ui_theme.h"
#include "ui_common.h"
#include "esp_log.h"
#include <string.h>

/* Orientation helper — true when display is taller than wide */
static bool effect_portrait(void)
{
    lv_disp_t *d = lv_disp_get_default();
    return lv_disp_get_hor_res(d) < lv_disp_get_ver_res(d);
}

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

    /* Centre toast in the content area (excluding navbar):
     * Landscape: navbar on right  → shift left by half its thickness
     * Portrait:  navbar on bottom → shift up by half its thickness */
    lv_disp_t *disp = lv_disp_get_default();
    lv_coord_t sw = lv_disp_get_hor_res(disp);
    bool portrait = effect_portrait();
    lv_coord_t x_off = portrait ? 0                   : -(UI_NAVBAR_THICK / 2);
    lv_coord_t y_off = portrait ? -(UI_NAVBAR_THICK / 2) : 0;

    /* Toast container */
    toast_obj = lv_obj_create(lv_layer_top());
    lv_obj_set_size(toast_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(toast_obj, LV_ALIGN_CENTER, x_off, y_off);
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
    /* Max width = usable content area minus some margin */
    lv_coord_t max_w = (portrait ? sw : sw - UI_NAVBAR_THICK) - 40;
    lv_obj_set_style_max_width(label, max_w, 0);

    /* Auto-dismiss timer */
    lv_timer_create(toast_timer_cb, ms, NULL);

    ESP_LOGD(TAG, "Toast: %s (%dms)", msg, ms);
}

/* ========== Confirm Dialog ========== */

/* Dialog dimensions */
#define DLG_W           380   /* dialog width in px                        */
#define DLG_TITLE_H     28    /* title polygon height                      */
#define DLG_BODY_PAD    24    /* inner body padding (2× the old 12)        */
#define DLG_ROW_GAP     16    /* gap between body elements (2× old 8)      */
#define DLG_BTN_GAP     24    /* extra top margin above button row         */

typedef struct {
    ui_confirm_cb_t  cb;
    void            *ctx;
    lv_obj_t        *dialog;    /* backdrop — deleted to dismiss              */
    lv_color_t      *title_buf; /* heap canvas buffer, freed on dismiss       */
} confirm_state_t;

static confirm_state_t confirm_st = { 0 };

static void confirm_btn_cb(lv_event_t *e)
{
    bool confirmed = (bool)(uintptr_t)lv_event_get_user_data(e);

    ui_confirm_cb_t cb  = confirm_st.cb;
    void           *ctx = confirm_st.ctx;

    /* Dismiss first so the callback can safely push new activities */
    if (confirm_st.title_buf) {
        lv_mem_free(confirm_st.title_buf);
        confirm_st.title_buf = NULL;
    }
    if (confirm_st.dialog) {
        lv_obj_del(confirm_st.dialog);
        confirm_st.dialog = NULL;
    }
    memset(&confirm_st, 0, sizeof(confirm_st));

    if (cb) cb(confirmed, ctx);
}

void ui_effect_confirm(const char *title, const char *msg,
                       ui_confirm_cb_t cb, void *ctx)
{
    /* Dismiss any existing dialog */
    if (confirm_st.dialog) {
        if (confirm_st.title_buf) {
            lv_mem_free(confirm_st.title_buf);
            confirm_st.title_buf = NULL;
        }
        lv_obj_del(confirm_st.dialog);
        confirm_st.dialog = NULL;
    }

    const cyberdeck_theme_t *t = ui_theme_get();

    /* ---- Semi-transparent backdrop ---- */
    lv_obj_t *backdrop = lv_obj_create(lv_layer_top());
    lv_obj_set_size(backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_align(backdrop, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_set_style_radius(backdrop, 0, 0);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- Dialog box — no padding, title canvas goes edge-to-edge ---- */
    lv_coord_t dlg_x = effect_portrait() ? 0 : -(UI_NAVBAR_THICK / 2);
    lv_obj_t *dialog = lv_obj_create(backdrop);
    lv_obj_set_size(dialog, DLG_W, LV_SIZE_CONTENT);
    lv_obj_align(dialog, LV_ALIGN_CENTER, dlg_x, 0);
    lv_obj_set_style_bg_color(dialog, t->bg_dark, 0);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dialog, t->primary_dim, 0);
    lv_obj_set_style_border_width(dialog, 1, 0);
    lv_obj_set_style_radius(dialog, 2, 0);
    lv_obj_set_style_pad_all(dialog, 0, 0);
    lv_obj_set_style_pad_row(dialog, 0, 0);
    lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dialog, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- Title polygon (only when title provided) ---- */
    if (title && title[0]) {
        /* Measure text so the polygon is only as wide as needed.
         * Canvas width = left_pad + text + right_pad + DLG_TITLE_H (diagonal) */
        const lv_coord_t title_pad = 12;
        lv_point_t txt_size = {0, 0};
        lv_coord_t max_txt_w = DLG_W - DLG_TITLE_H - title_pad * 2;
        lv_txt_get_size(&txt_size, title, &CYBERDECK_FONT_SM, 0, 0,
                        max_txt_w, LV_TEXT_FLAG_NONE);
        lv_coord_t tc_w = txt_size.x + title_pad * 2 + DLG_TITLE_H;
        if (tc_w > DLG_W) tc_w = DLG_W;

        lv_color_t *tbuf = (lv_color_t *)lv_mem_alloc(
                               tc_w * DLG_TITLE_H * sizeof(lv_color_t));
        if (tbuf) {
            confirm_st.title_buf = tbuf;

            lv_obj_t *tc = lv_canvas_create(dialog);
            lv_canvas_set_buffer(tc, tbuf, tc_w, DLG_TITLE_H,
                                 LV_IMG_CF_TRUE_COLOR);
            lv_obj_set_size(tc, tc_w, DLG_TITLE_H);
            lv_obj_clear_flag(tc, LV_OBJ_FLAG_CLICKABLE);

            /* Clear to dialog background */
            lv_canvas_fill_bg(tc, t->bg_dark, LV_OPA_COVER);

            /* Parallelogram — left edge vertical, right edge 45° ascending:
             *   A(0,0) ─────────── B(W-1, 0)
             *   |                 ╱  45° diagonal
             *   D(0,H-1) ── C(W-H, H-1)          */
            lv_point_t pts[4] = {
                {0,              0              },
                {tc_w - 1,       0              },
                {tc_w - DLG_TITLE_H, DLG_TITLE_H - 1},
                {0,              DLG_TITLE_H - 1},
            };
            lv_draw_rect_dsc_t poly_dsc;
            lv_draw_rect_dsc_init(&poly_dsc);
            poly_dsc.bg_color     = t->primary_dim;
            poly_dsc.bg_opa       = LV_OPA_COVER;
            poly_dsc.border_width = 0;
            poly_dsc.radius       = 0;
            lv_canvas_draw_polygon(tc, pts, 4, &poly_dsc);

            /* Title text: FONT_SM, bold-by-offset (+1 px right) */
            lv_draw_label_dsc_t txt_dsc;
            lv_draw_label_dsc_init(&txt_dsc);
            txt_dsc.color = t->text;
            txt_dsc.font  = &CYBERDECK_FONT_SM;
            lv_coord_t ty = (DLG_TITLE_H -
                             (lv_coord_t)CYBERDECK_FONT_SM.line_height) / 2;
            if (ty < 0) ty = 0;
            lv_canvas_draw_text(tc, title_pad,     ty, txt_size.x + 2, &txt_dsc, title);
            lv_canvas_draw_text(tc, title_pad + 1, ty, txt_size.x + 1, &txt_dsc, title);
        }
    }

    /* ---- Body container — padded, description + buttons ---- */
    lv_obj_t *body = lv_obj_create(dialog);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, DLG_BODY_PAD, 0);
    lv_obj_set_style_pad_row(body, DLG_ROW_GAP, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    /* Description — protagonist: primary color, FONT_MD */
    if (msg && msg[0]) {
        lv_obj_t *lbl_msg = lv_label_create(body);
        lv_label_set_text(lbl_msg, msg);
        lv_label_set_long_mode(lbl_msg, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl_msg, LV_PCT(100));
        ui_theme_style_label(lbl_msg, &CYBERDECK_FONT_MD);
    }

    /* Extra gap above buttons */
    lv_obj_t *gap = lv_obj_create(body);
    lv_obj_set_size(gap, LV_PCT(100), DLG_BTN_GAP);
    lv_obj_set_style_bg_opa(gap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gap, 0, 0);
    lv_obj_set_style_pad_all(gap, 0, 0);
    lv_obj_clear_flag(gap, LV_OBJ_FLAG_SCROLLABLE);

    /* Button row — [CANCEL] secondary, [OK] primary */
    lv_obj_t *btn_row = ui_common_action_row(body);

    lv_obj_t *btn_cancel = ui_common_btn(btn_row, "CANCEL");
    lv_obj_add_event_cb(btn_cancel, confirm_btn_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)false);

    lv_obj_t *btn_ok = ui_common_btn(btn_row, "OK");
    ui_common_btn_style_primary(btn_ok);
    lv_obj_add_event_cb(btn_ok, confirm_btn_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)true);

    confirm_st.cb     = cb;
    confirm_st.ctx    = ctx;
    confirm_st.dialog = backdrop;

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
    lv_coord_t cur_x = effect_portrait() ? 0 : -(UI_NAVBAR_THICK / 2);
    lv_obj_align(cursor, LV_ALIGN_CENTER, cur_x, 0);

    /* Blink at 500ms interval */
    loading_timer = lv_timer_create(loading_blink_cb, 500, cursor);

    ESP_LOGD(TAG, "Loading overlay shown");
}
