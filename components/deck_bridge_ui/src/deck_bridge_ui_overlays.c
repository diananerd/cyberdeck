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
    /* Optional callbacks (mutually exclusive with intent_id usage). */
    deck_bridge_ui_overlay_cb_t on_ok;
    deck_bridge_ui_overlay_cb_t on_cancel;
    void                        *user_data;
} confirm_state_t;

static void confirm_dismiss(confirm_state_t *cs, bool is_ok)
{
    if (!cs) return;
    /* Copy out everything we need before freeing — the callback may
     * re-enter (push activity, pop, etc.) and we must not deref cs
     * after we've freed it. */
    uint32_t intent_id = is_ok ? cs->ok_intent_id : cs->cancel_intent_id;
    deck_bridge_ui_overlay_cb_t cb = is_ok ? cs->on_ok : cs->on_cancel;
    void *ud = cs->user_data;

    /* Tear down the dialog BEFORE the callback runs, so a callback that
     * pushes/pops activities doesn't interact with the dialog's widgets. */
    if (cs->backdrop) lv_obj_del(cs->backdrop);
    lv_mem_free(cs);

    if (intent_id != 0) {
        ESP_LOGI(TAG, "confirm intent fired: id=%u", (unsigned)intent_id);
    }
    if (cb) cb(ud);
}

static void confirm_ok_cb(lv_event_t *e)
{
    confirm_state_t *cs = (confirm_state_t *)lv_event_get_user_data(e);
    confirm_dismiss(cs, true);
}
static void confirm_cancel_cb(lv_event_t *e)
{
    confirm_state_t *cs = (confirm_state_t *)lv_event_get_user_data(e);
    confirm_dismiss(cs, false);
}

static void confirm_show(const char *title, const char *message,
                          const char *ok_label, const char *cancel_label,
                          uint32_t ok_intent, uint32_t cancel_intent,
                          deck_bridge_ui_overlay_cb_t on_ok,
                          deck_bridge_ui_overlay_cb_t on_cancel,
                          void *user_data)
{
    confirm_state_t *cs = lv_mem_alloc(sizeof(*cs));
    if (!cs) return;
    memset(cs, 0, sizeof(*cs));
    cs->ok_intent_id     = ok_intent;
    cs->cancel_intent_id = cancel_intent;
    cs->on_ok            = on_ok;
    cs->on_cancel        = on_cancel;
    cs->user_data        = user_data;

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
            confirm_show(title, msg, oklbl, cnclbl,
                         n->intent_id, 0, NULL, NULL, NULL);
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
                     ok_intent, cancel_intent, NULL, NULL, NULL);
        deck_bridge_ui_unlock();
    }
}

void deck_bridge_ui_overlay_confirm_cb(const char *title, const char *message,
                                        const char *ok_label, const char *cancel_label,
                                        deck_bridge_ui_overlay_cb_t on_ok,
                                        deck_bridge_ui_overlay_cb_t on_cancel,
                                        void *user_data)
{
    if (deck_bridge_ui_lock(200)) {
        confirm_show(title, message, ok_label, cancel_label,
                     0, 0, on_ok, on_cancel, user_data);
        deck_bridge_ui_unlock();
    }
}

/* ---------- PROGRESS ---------- */

static lv_obj_t *s_progress_backdrop = NULL;
static lv_obj_t *s_progress_bar      = NULL;
static lv_timer_t *s_progress_anim   = NULL;

static void progress_indet_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_progress_bar) return;
    int32_t v = lv_bar_get_value(s_progress_bar);
    v = (v + 30) % 1001;
    lv_bar_set_value(s_progress_bar, v, LV_ANIM_OFF);
}

static void progress_show_impl(const char *label)
{
    if (s_progress_backdrop) {
        /* Update label only if one exists as a second child. */
        return;
    }
    lv_obj_t *bd = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bd, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(bd, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bd, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(bd, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bd, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_bar_create(bd);
    lv_obj_set_size(bar, 300, 8);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 0);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, CD_BG_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, CD_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);

    if (label && *label) {
        lv_obj_t *l = lv_label_create(bd);
        lv_label_set_text(l, label);
        lv_obj_set_style_text_color(l, CD_PRIMARY, LV_PART_MAIN);
        lv_obj_align_to(l, bar, LV_ALIGN_OUT_TOP_MID, 0, -16);
    }

    s_progress_backdrop = bd;
    s_progress_bar      = bar;
}

static void progress_set_impl(float pct)
{
    if (!s_progress_bar) return;
    if (pct < 0.0f) {
        /* Indeterminate — start a marquee tick if not already running. */
        if (!s_progress_anim) {
            s_progress_anim = lv_timer_create(progress_indet_tick_cb, 80, NULL);
        }
    } else {
        if (s_progress_anim) { lv_timer_del(s_progress_anim); s_progress_anim = NULL; }
        int32_t v = (int32_t)(pct * 1000.0f + 0.5f);
        if (v < 0)    v = 0;
        if (v > 1000) v = 1000;
        lv_bar_set_value(s_progress_bar, v, LV_ANIM_OFF);
    }
}

static void progress_hide_impl(void)
{
    if (s_progress_anim) { lv_timer_del(s_progress_anim); s_progress_anim = NULL; }
    if (s_progress_backdrop) { lv_obj_del(s_progress_backdrop); s_progress_backdrop = NULL; }
    s_progress_bar = NULL;
}

void deck_bridge_ui_overlay_progress_show(const char *label)
{
    if (deck_bridge_ui_lock(200)) { progress_show_impl(label); deck_bridge_ui_unlock(); }
}
void deck_bridge_ui_overlay_progress_set(float pct)
{
    if (deck_bridge_ui_lock(200)) { progress_set_impl(pct); deck_bridge_ui_unlock(); }
}
void deck_bridge_ui_overlay_progress_hide(void)
{
    if (deck_bridge_ui_lock(200)) { progress_hide_impl(); deck_bridge_ui_unlock(); }
}

/* ---------- CHOICE ---------- */

typedef struct {
    lv_obj_t *backdrop;
    deck_bridge_ui_overlay_choice_cb_t cb;
    void     *user_data;
    bool      fired;
} choice_state_t;

static void choice_dismiss(choice_state_t *cs, int idx)
{
    if (!cs || cs->fired) return;
    cs->fired = true;
    deck_bridge_ui_overlay_choice_cb_t cb = cs->cb;
    void *ud = cs->user_data;
    lv_obj_t *bd = cs->backdrop;
    cs->backdrop = NULL;
    lv_mem_free(cs);
    if (bd) lv_obj_del(bd);
    if (cb) cb(ud, idx);
}

static void choice_row_cb(lv_event_t *e)
{
    choice_state_t *cs = (choice_state_t *)lv_event_get_user_data(e);
    lv_obj_t *row = lv_event_get_current_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(row);
    choice_dismiss(cs, idx);
}

static void choice_cancel_cb(lv_event_t *e)
{
    choice_state_t *cs = (choice_state_t *)lv_event_get_user_data(e);
    choice_dismiss(cs, -1);
}

static void choice_show_impl(const char *title,
                              const char *const *options, uint16_t n,
                              deck_bridge_ui_overlay_choice_cb_t cb,
                              void *user_data)
{
    choice_state_t *cs = lv_mem_alloc(sizeof(*cs));
    if (!cs) return;
    memset(cs, 0, sizeof(*cs));
    cs->cb = cb;
    cs->user_data = user_data;

    cs->backdrop = lv_obj_create(lv_layer_top());
    lv_obj_set_size(cs->backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(cs->backdrop, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cs->backdrop, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(cs->backdrop, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cs->backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cs->backdrop, choice_cancel_cb, LV_EVENT_CLICKED, cs);

    lv_obj_t *dlg = lv_obj_create(cs->backdrop);
    lv_obj_set_size(dlg, 420, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(dlg, lv_pct(80), LV_PART_MAIN);
    lv_obj_align(dlg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dlg, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dlg, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(dlg, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(dlg, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dlg, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(dlg, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    if (title && *title) {
        lv_obj_t *t = lv_label_create(dlg);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, CD_PRIMARY_DIM, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(t, 8, LV_PART_MAIN);
    }

    for (uint16_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(dlg);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, CD_PRIMARY_DIM, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(row, 12, LV_PART_MAIN);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_event_cb(row, choice_row_cb, LV_EVENT_CLICKED, cs);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, (options && options[i]) ? options[i] : "");
        lv_obj_set_style_text_color(lbl, CD_PRIMARY, LV_PART_MAIN);
    }
}

void deck_bridge_ui_overlay_choice_show(const char *title,
                                         const char *const *options,
                                         uint16_t n_options,
                                         deck_bridge_ui_overlay_choice_cb_t on_pick,
                                         void *user_data)
{
    if (deck_bridge_ui_lock(200)) {
        choice_show_impl(title, options, n_options, on_pick, user_data);
        deck_bridge_ui_unlock();
    }
}

/* ---------- MULTISELECT ---------- */

typedef struct {
    lv_obj_t *backdrop;
    deck_bridge_ui_overlay_multiselect_cb_t cb;
    void     *user_data;
    bool     *selected;      /* len = n */
    uint16_t  n;
    bool      fired;
} mselect_state_t;

static void mselect_dismiss(mselect_state_t *s, bool commit)
{
    if (!s || s->fired) return;
    s->fired = true;
    deck_bridge_ui_overlay_multiselect_cb_t cb = s->cb;
    void *ud = s->user_data;
    lv_obj_t *bd = s->backdrop;
    bool *selected = s->selected;
    uint16_t n = s->n;
    s->backdrop = NULL;
    s->selected = NULL;
    lv_mem_free(s);
    if (bd) lv_obj_del(bd);
    if (cb) cb(ud, commit ? selected : NULL, commit ? n : 0);
    if (selected) lv_mem_free(selected);
}

static void mselect_row_cb(lv_event_t *e)
{
    mselect_state_t *s = (mselect_state_t *)lv_event_get_user_data(e);
    lv_obj_t *row = lv_event_get_current_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(row);
    if (!s || idx < 0 || idx >= s->n || !s->selected) return;
    s->selected[idx] = !s->selected[idx];
    /* Toggle visual marker (second child of row is the checkmark label). */
    uint32_t n_children = lv_obj_get_child_cnt(row);
    if (n_children >= 2) {
        lv_obj_t *mark = lv_obj_get_child(row, 0);
        lv_label_set_text(mark, s->selected[idx] ? LV_SYMBOL_OK : " ");
    }
}

static void mselect_done_cb(lv_event_t *e)
{
    mselect_state_t *s = (mselect_state_t *)lv_event_get_user_data(e);
    mselect_dismiss(s, true);
}
static void mselect_cancel_cb(lv_event_t *e)
{
    mselect_state_t *s = (mselect_state_t *)lv_event_get_user_data(e);
    mselect_dismiss(s, false);
}

static void mselect_show_impl(const char *title,
                               const char *const *options, uint16_t n,
                               const bool *initially,
                               deck_bridge_ui_overlay_multiselect_cb_t cb,
                               void *user_data)
{
    mselect_state_t *st = lv_mem_alloc(sizeof(*st));
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->cb = cb;
    st->user_data = user_data;
    st->n = n;
    if (n > 0) {
        st->selected = lv_mem_alloc(sizeof(bool) * n);
        if (!st->selected) { lv_mem_free(st); return; }
        for (uint16_t i = 0; i < n; i++) {
            st->selected[i] = initially ? initially[i] : false;
        }
    }

    st->backdrop = lv_obj_create(lv_layer_top());
    lv_obj_set_size(st->backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(st->backdrop, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(st->backdrop, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(st->backdrop, 0, LV_PART_MAIN);
    lv_obj_clear_flag(st->backdrop, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dlg = lv_obj_create(st->backdrop);
    lv_obj_set_size(dlg, 420, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(dlg, lv_pct(80), LV_PART_MAIN);
    lv_obj_align(dlg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dlg, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dlg, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(dlg, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(dlg, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dlg, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(dlg, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    if (title && *title) {
        lv_obj_t *t = lv_label_create(dlg);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, CD_PRIMARY_DIM, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(t, 8, LV_PART_MAIN);
    }

    for (uint16_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(dlg);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, CD_PRIMARY_DIM, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(row, 10, LV_PART_MAIN);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                               LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_event_cb(row, mselect_row_cb, LV_EVENT_CLICKED, st);

        lv_obj_t *mark = lv_label_create(row);
        lv_label_set_text(mark, st->selected[i] ? LV_SYMBOL_OK : " ");
        lv_obj_set_style_text_color(mark, CD_PRIMARY, LV_PART_MAIN);
        lv_obj_set_width(mark, 20);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, (options && options[i]) ? options[i] : "");
        lv_obj_set_style_text_color(lbl, CD_PRIMARY, LV_PART_MAIN);
    }

    /* Button row — CANCEL + DONE. */
    lv_obj_t *btnrow = lv_obj_create(dlg);
    lv_obj_set_size(btnrow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btnrow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btnrow, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btnrow, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btnrow, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_column(btnrow, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnrow, LV_FLEX_ALIGN_END,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cancel = lv_btn_create(btnrow);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(cancel, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_border_width(cancel, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(cancel, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(cancel, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(cancel, 12, LV_PART_MAIN);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "CANCEL");
    lv_obj_set_style_text_color(cl, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, mselect_cancel_cb, LV_EVENT_CLICKED, st);
    lv_obj_clear_flag(cancel, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    lv_obj_t *done = lv_btn_create(btnrow);
    lv_obj_set_style_bg_color(done, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(done, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(done, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_border_width(done, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(done, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(done, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(done, 12, LV_PART_MAIN);
    lv_obj_t *dl = lv_label_create(done);
    lv_label_set_text(dl, "DONE");
    lv_obj_set_style_text_color(dl, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_center(dl);
    lv_obj_add_event_cb(done, mselect_done_cb, LV_EVENT_CLICKED, st);
    lv_obj_clear_flag(done, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

void deck_bridge_ui_overlay_multiselect_show(const char *title,
                                              const char *const *options,
                                              uint16_t n_options,
                                              const bool *initially_selected,
                                              deck_bridge_ui_overlay_multiselect_cb_t on_done,
                                              void *user_data)
{
    if (deck_bridge_ui_lock(200)) {
        mselect_show_impl(title, options, n_options, initially_selected,
                           on_done, user_data);
        deck_bridge_ui_unlock();
    }
}

/* ---------- KEYBOARD ---------- */

static lv_obj_t *s_kb_backdrop = NULL;
static lv_obj_t *s_kb          = NULL;
static lv_obj_t *s_kb_ta       = NULL;

static void kb_dismiss_cb(lv_event_t *e)
{
    (void)e;
    deck_bridge_ui_overlay_keyboard_hide();
}

static void keyboard_show_impl(const char *kind_atom)
{
    if (s_kb_backdrop) return;   /* already up */

    lv_obj_t *bd = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bd, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(bd, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(bd, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bd, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ta = lv_textarea_create(bd);
    lv_obj_set_size(ta, lv_pct(80), 48);
    lv_obj_align(ta, LV_ALIGN_BOTTOM_MID, 0, -260);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_style_bg_color(ta, CD_BG_CARD, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_border_width(ta, 2, LV_PART_MAIN);

    lv_obj_t *kb = lv_keyboard_create(bd);
    lv_obj_set_size(kb, lv_pct(100), 240);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ta);

    lv_keyboard_mode_t mode = LV_KEYBOARD_MODE_TEXT_UPPER;
    if (kind_atom) {
        if      (!strcmp(kind_atom, "text") ||
                 !strcmp(kind_atom, "text_lower")) mode = LV_KEYBOARD_MODE_TEXT_LOWER;
        else if (!strcmp(kind_atom, "number") ||
                 !strcmp(kind_atom, "pin"))        mode = LV_KEYBOARD_MODE_NUMBER;
        else if (!strcmp(kind_atom, "special"))    mode = LV_KEYBOARD_MODE_SPECIAL;
    }
    lv_keyboard_set_mode(kb, mode);

    /* Tapping OK / close hides the overlay. */
    lv_obj_add_event_cb(kb, kb_dismiss_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(kb, kb_dismiss_cb, LV_EVENT_CANCEL, NULL);

    s_kb_backdrop = bd;
    s_kb          = kb;
    s_kb_ta       = ta;
}

static void keyboard_hide_impl(void)
{
    if (s_kb_backdrop) { lv_obj_del(s_kb_backdrop); s_kb_backdrop = NULL; }
    s_kb = NULL;
    s_kb_ta = NULL;
}

void deck_bridge_ui_overlay_keyboard_show(const char *kind_atom)
{
    if (deck_bridge_ui_lock(200)) { keyboard_show_impl(kind_atom); deck_bridge_ui_unlock(); }
}
void deck_bridge_ui_overlay_keyboard_hide(void)
{
    if (deck_bridge_ui_lock(200)) { keyboard_hide_impl(); deck_bridge_ui_unlock(); }
}
