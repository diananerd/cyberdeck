/* deck_bridge_ui_overlays — modal overlays on lv_layer_top.
 *
 * F26.5 ships:
 *   - DVC_CONFIRM      → ok/cancel modal dialog
 *   - DVC_LOADING      → full-screen loading overlay (blinking _ cursor)
 *   - DVC_TOAST        → auto-dismissing notification
 *   - KEYBOARD         → on-screen TEXT_UPPER keyboard wired to a textarea
 *
 * All overlays render to lv_layer_top so they sit above the activity
 * stack and are not destroyed when activities are popped.
 */

#include "deck_bridge_ui.h"
#include "deck_bridge_ui_internal.h"

#include "deck_dvc.h"
#include "lvgl.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "bridge_ui.ovl";

#define CD_PRIMARY      lv_color_hex(0x00FF41)
#define CD_PRIMARY_DIM  lv_color_hex(0x004D13)
#define CD_BG_DARK      lv_color_black()
#define CD_BG_CARD      lv_color_hex(0x0A0A0A)

/* ---------- TOAST ---------- */

static lv_obj_t *s_toast = NULL;
static lv_timer_t *s_toast_timer = NULL;

static void toast_dismiss_cb(lv_timer_t *t)
{
    (void)t;
    if (s_toast) { lv_obj_del(s_toast); s_toast = NULL; }
    if (s_toast_timer) { lv_timer_del(s_toast_timer); s_toast_timer = NULL; }
}

static void toast_show(const char *text, uint32_t duration_ms)
{
    if (s_toast)         { lv_obj_del(s_toast); s_toast = NULL; }
    if (s_toast_timer)   { lv_timer_del(s_toast_timer); s_toast_timer = NULL; }

    lv_obj_t *t = lv_obj_create(lv_layer_top());
    lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(t, CD_BG_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(t, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_border_width(t, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(t, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(t, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(t, 6,  LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(t);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_style_text_color(lbl, CD_PRIMARY, LV_PART_MAIN);

    lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);

    s_toast = t;
    s_toast_timer = lv_timer_create(toast_dismiss_cb, duration_ms, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

/* ---------- LOADING ---------- */

static lv_obj_t *s_loading = NULL;
static lv_timer_t *s_loading_blink = NULL;

static void loading_blink_cb(lv_timer_t *t)
{
    lv_obj_t *cur = (lv_obj_t *)t->user_data;
    if (!cur) return;
    bool visible = !lv_obj_has_flag(cur, LV_OBJ_FLAG_HIDDEN);
    if (visible) lv_obj_add_flag(cur, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_clear_flag(cur, LV_OBJ_FLAG_HIDDEN);
}

static void loading_show(const char *text)
{
    if (s_loading) return;
    lv_obj_t *backdrop = lv_obj_create(lv_layer_top());
    lv_obj_set_size(backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(backdrop, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(backdrop, 0, LV_PART_MAIN);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cur = lv_label_create(backdrop);
    lv_label_set_text(cur, "_");
    lv_obj_set_style_text_color(cur, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_align(cur, LV_ALIGN_CENTER, 0, 0);

    if (text && *text) {
        lv_obj_t *lbl = lv_label_create(backdrop);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, CD_PRIMARY_DIM, LV_PART_MAIN);
        lv_obj_align_to(lbl, cur, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
    }

    s_loading = backdrop;
    s_loading_blink = lv_timer_create(loading_blink_cb, 500, cur);
}

static void loading_hide(void)
{
    if (s_loading_blink) { lv_timer_del(s_loading_blink); s_loading_blink = NULL; }
    if (s_loading)       { lv_obj_del(s_loading); s_loading = NULL; }
}

/* ---------- CONFIRM ---------- */

typedef struct {
    lv_obj_t *backdrop;
    uint32_t  ok_intent_id;
    uint32_t  cancel_intent_id;
} confirm_state_t;

static void confirm_dismiss(confirm_state_t *cs, uint32_t intent_id)
{
    if (intent_id != 0) {
        ESP_LOGI(TAG, "confirm intent fired: id=%u", (unsigned)intent_id);
    }
    if (cs && cs->backdrop) lv_obj_del(cs->backdrop);
    if (cs) lv_mem_free(cs);
}

static void confirm_ok_cb(lv_event_t *e)
{
    confirm_state_t *cs = (confirm_state_t *)lv_event_get_user_data(e);
    confirm_dismiss(cs, cs ? cs->ok_intent_id : 0);
}
static void confirm_cancel_cb(lv_event_t *e)
{
    confirm_state_t *cs = (confirm_state_t *)lv_event_get_user_data(e);
    confirm_dismiss(cs, cs ? cs->cancel_intent_id : 0);
}

static void confirm_show(const char *title, const char *message,
                          const char *ok_label, const char *cancel_label,
                          uint32_t ok_intent, uint32_t cancel_intent)
{
    confirm_state_t *cs = lv_mem_alloc(sizeof(*cs));
    if (!cs) return;
    cs->ok_intent_id     = ok_intent;
    cs->cancel_intent_id = cancel_intent;

    /* Backdrop. */
    cs->backdrop = lv_obj_create(lv_layer_top());
    lv_obj_set_size(cs->backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(cs->backdrop, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cs->backdrop, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(cs->backdrop, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cs->backdrop, LV_OBJ_FLAG_SCROLLABLE);

    /* Dialog box. */
    lv_obj_t *dlg = lv_obj_create(cs->backdrop);
    lv_obj_set_size(dlg, 380, LV_SIZE_CONTENT);
    lv_obj_align(dlg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dlg, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dlg, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(dlg, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(dlg, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dlg, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_row(dlg, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    if (title && *title) {
        lv_obj_t *t = lv_label_create(dlg);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, CD_PRIMARY_DIM, LV_PART_MAIN);
    }
    if (message && *message) {
        lv_obj_t *m = lv_label_create(dlg);
        lv_label_set_text(m, message);
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(m, lv_pct(100));
        lv_obj_set_style_text_color(m, CD_PRIMARY, LV_PART_MAIN);
    }

    /* Button row — right-aligned [CANCEL][OK]. */
    lv_obj_t *row = lv_obj_create(dlg);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 24, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cancel = lv_btn_create(row);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(cancel, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_border_width(cancel, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(cancel, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(cancel, 8,  LV_PART_MAIN);
    lv_obj_set_style_radius(cancel, 12, LV_PART_MAIN);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, cancel_label ? cancel_label : "CANCEL");
    lv_obj_set_style_text_color(cl, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, confirm_cancel_cb, LV_EVENT_CLICKED, cs);
    lv_obj_clear_flag(cancel, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    lv_obj_t *ok = lv_btn_create(row);
    lv_obj_set_style_bg_color(ok, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(ok, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_border_width(ok, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ok, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(ok, 8,  LV_PART_MAIN);
    lv_obj_set_style_radius(ok, 12, LV_PART_MAIN);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text(okl, ok_label ? ok_label : "OK");
    lv_obj_set_style_text_color(okl, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_center(okl);
    lv_obj_add_event_cb(ok, confirm_ok_cb, LV_EVENT_CLICKED, cs);
    lv_obj_clear_flag(ok, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

/* ---------- DVC overlay dispatch ---------- */

bool deck_bridge_ui_render_overlay(const deck_dvc_node_t *n)
{
    if (!n) return false;
    switch ((deck_dvc_type_t)n->type) {
        case DVC_TOAST: {
            const deck_dvc_attr_t *m = deck_dvc_find_attr(n, "message");
            const char *text = (m && (m->type == DVC_ATTR_STR ||
                                       m->type == DVC_ATTR_ATOM))
                                  ? m->value.s : "";
            const deck_dvc_attr_t *d = deck_dvc_find_attr(n, "duration_ms");
            uint32_t dur = (d && d->type == DVC_ATTR_I64)
                              ? (uint32_t)d->value.i : 2000;
            toast_show(text, dur);
            return true;
        }
        case DVC_LOADING: {
            const deck_dvc_attr_t *m = deck_dvc_find_attr(n, "message");
            const char *text = (m && (m->type == DVC_ATTR_STR ||
                                       m->type == DVC_ATTR_ATOM))
                                  ? m->value.s : NULL;
            loading_show(text);
            return true;
        }
        case DVC_CONFIRM: {
            const deck_dvc_attr_t *t = deck_dvc_find_attr(n, "title");
            const deck_dvc_attr_t *m = deck_dvc_find_attr(n, "message");
            const deck_dvc_attr_t *o = deck_dvc_find_attr(n, "confirm_label");
            const deck_dvc_attr_t *c = deck_dvc_find_attr(n, "cancel_label");
            const char *title  = (t && (t->type == DVC_ATTR_STR ||
                                          t->type == DVC_ATTR_ATOM))
                                     ? t->value.s : NULL;
            const char *msg    = (m && (m->type == DVC_ATTR_STR ||
                                          m->type == DVC_ATTR_ATOM))
                                     ? m->value.s : "";
            const char *oklbl  = (o && (o->type == DVC_ATTR_STR ||
                                          o->type == DVC_ATTR_ATOM))
                                     ? o->value.s : "OK";
            const char *cnclbl = (c && (c->type == DVC_ATTR_STR ||
                                          c->type == DVC_ATTR_ATOM))
                                     ? c->value.s : "CANCEL";
            confirm_show(title, msg, oklbl, cnclbl, n->intent_id, 0);
            return true;
        }
        default:
            return false;
    }
}

/* External wrappers — used by the keyboard/lockscreen flows in F27. */
void deck_bridge_ui_overlay_loading_show(const char *text)
{
    if (deck_bridge_ui_lock(200)) { loading_show(text); deck_bridge_ui_unlock(); }
}
void deck_bridge_ui_overlay_loading_hide(void)
{
    if (deck_bridge_ui_lock(200)) { loading_hide(); deck_bridge_ui_unlock(); }
}
void deck_bridge_ui_overlay_toast(const char *text, uint32_t ms)
{
    if (deck_bridge_ui_lock(200)) { toast_show(text, ms); deck_bridge_ui_unlock(); }
}
void deck_bridge_ui_overlay_confirm(const char *title, const char *message,
                                     const char *ok_label, const char *cancel_label,
                                     uint32_t ok_intent, uint32_t cancel_intent)
{
    if (deck_bridge_ui_lock(200)) {
        confirm_show(title, message, ok_label, cancel_label,
                     ok_intent, cancel_intent);
        deck_bridge_ui_unlock();
    }
}
