/* deck_bridge_ui_decode — DVC tree → LVGL widgets.
 *
 * F26.3 ships LABEL, GROUP, COLUMN, ROW, TRIGGER (button) decoders.
 * F26.4 will add input widgets (PASSWORD, CHOICE, TOGGLE, RANGE).
 * F26.5 will add overlays (CONFIRM, LOADING, KEYBOARD).
 *
 * Each render wipes the active screen and rebuilds the tree top-down.
 * Per-render diffing is a future optimization — for the current scale
 * (≤ 50 nodes per screen) full rebuild is fast enough and avoids
 * lifecycle bugs.
 *
 * Intent firing: trigger nodes register an LV_EVENT_CLICKED handler
 * carrying their `intent_id`. F26 ships the wiring; the runtime side
 * (`deck_intent_fire`) is wired in F28.
 */

#include "deck_bridge_ui.h"
#include "deck_bridge_ui_internal.h"

#include "deck_dvc.h"

#include "lvgl.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "bridge_ui.dec";

/* CyberDeck terminal palette (matches CLAUDE.md theme.green default). */
#define CD_PRIMARY      lv_color_hex(0x00FF41)
#define CD_PRIMARY_DIM  lv_color_hex(0x004D13)
#define CD_BG_DARK      lv_color_black()
#define CD_BG_CARD      lv_color_hex(0x0A0A0A)

static const char *attr_str(const deck_dvc_node_t *n, const char *atom,
                             const char *fallback)
{
    const deck_dvc_attr_t *a = deck_dvc_find_attr(n, atom);
    if (!a || (a->type != DVC_ATTR_STR && a->type != DVC_ATTR_ATOM)) return fallback;
    return a->value.s ? a->value.s : fallback;
}

static bool attr_bool(const deck_dvc_node_t *n, const char *atom, bool fallback)
{
    const deck_dvc_attr_t *a = deck_dvc_find_attr(n, atom);
    if (!a || a->type != DVC_ATTR_BOOL) return fallback;
    return a->value.b;
}

static int64_t attr_i64(const deck_dvc_node_t *n, const char *atom, int64_t fallback)
{
    const deck_dvc_attr_t *a = deck_dvc_find_attr(n, atom);
    if (!a) return fallback;
    if (a->type == DVC_ATTR_I64) return a->value.i;
    if (a->type == DVC_ATTR_F64) return (int64_t)a->value.f;
    return fallback;
}

/* Forward decl. */
static lv_obj_t *render_node(lv_obj_t *parent, const deck_dvc_node_t *n);

/* ---------- per-type renderers ---------- */

static void style_card(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, CD_BG_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(obj, 14, LV_PART_MAIN);
}

static lv_obj_t *render_group(lv_obj_t *parent, const deck_dvc_node_t *n)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    style_card(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    const char *title = attr_str(n, "title", NULL);
    if (title && *title) {
        lv_obj_t *t = lv_label_create(card);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, CD_PRIMARY, LV_PART_MAIN);
    }

    for (uint16_t i = 0; i < n->child_count; i++) {
        render_node(card, n->children[i]);
    }
    return card;
}

static lv_obj_t *render_column(lv_obj_t *parent, const deck_dvc_node_t *n)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_width(col, lv_pct(100));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(col, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    for (uint16_t i = 0; i < n->child_count; i++) {
        render_node(col, n->children[i]);
    }
    return col;
}

static lv_obj_t *render_row(lv_obj_t *parent, const deck_dvc_node_t *n)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (uint16_t i = 0; i < n->child_count; i++) {
        render_node(row, n->children[i]);
    }
    return row;
}

static lv_obj_t *render_label(lv_obj_t *parent, const deck_dvc_node_t *n)
{
    lv_obj_t *lbl = lv_label_create(parent);
    const char *text = attr_str(n, "value", NULL);
    if (!text) text = attr_str(n, "label", "");
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, CD_PRIMARY, LV_PART_MAIN);
    return lbl;
}

/* Registered by higher layers (shell) via deck_bridge_ui_set_intent_hook.
 * When NULL we still log-only, which is how the decoder behaves on
 * platforms that don't wire a hook. */
static deck_bridge_ui_intent_hook_t s_intent_hook = NULL;

void deck_bridge_ui_set_intent_hook(deck_bridge_ui_intent_hook_t hook)
{
    s_intent_hook = hook;
}

static void trigger_click_cb(lv_event_t *e)
{
    uint32_t intent_id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (intent_id == 0) return;
    ESP_LOGI(TAG, "intent fired: id=%u", (unsigned)intent_id);
    if (s_intent_hook) s_intent_hook(intent_id);
}

static lv_obj_t *render_trigger(lv_obj_t *parent, const deck_dvc_node_t *n)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(btn, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(btn, 8,  LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, CD_PRIMARY, LV_PART_MAIN);

    const char *variant = attr_str(n, "variant", NULL);
    bool primary = variant && strcmp(variant, "primary") == 0;
    if (primary) {
        lv_obj_set_style_bg_color(btn, CD_PRIMARY, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    }

    if (attr_bool(n, "disabled", false)) {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }

    lv_obj_t *lbl = lv_label_create(btn);
    const char *label = attr_str(n, "label", "BUTTON");
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl,
                                 primary ? CD_BG_DARK : CD_PRIMARY,
                                 LV_PART_MAIN);
    lv_obj_center(lbl);

    if (n->intent_id != 0) {
        lv_obj_add_event_cb(btn, trigger_click_cb, LV_EVENT_CLICKED,
                             (void *)(uintptr_t)n->intent_id);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    }
    return btn;
}

static lv_obj_t *render_spacer(lv_obj_t *parent, const deck_dvc_node_t *n)
{
    (void)n;
    lv_obj_t *sp = lv_obj_create(parent);
    lv_obj_set_size(sp, 1, 1);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(sp, 0, LV_PART_MAIN);
    return sp;
}

static lv_obj_t *render_divider(lv_obj_t *parent, const deck_dvc_node_t *n)
{
    (void)n;
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_set_size(d, lv_pct(100), 1);
    lv_obj_set_style_bg_color(d, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(d, 0, LV_PART_MAIN);
    return d;
}

/* Common: dim caption above an input widget if the node has :label. */
static void with_caption(lv_obj_t *parent, const char *label_text)
{
    if (!label_text || !*label_text) return;
    lv_obj_t *cap = lv_label_create(parent);
    lv_label_set_text(cap, label_text);
    lv_obj_set_style_text_color(cap, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_pad_top(cap, 4, LV_PART_MAIN);
}

static void style_input_box(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 8, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 8, LV_PART_MAIN);
}

/* ---------- TEXT / PASSWORD ---------- */

static void textarea_event_cb(lv_event_t *e)
{
    uint32_t intent_id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target(e);
    if (intent_id == 0) return;
    const char *txt = lv_textarea_get_text(ta);
    ESP_LOGI(TAG, "intent fired: id=%u text=\"%s\"",
             (unsigned)intent_id, txt ? txt : "");
}

static lv_obj_t *render_text_input(lv_obj_t *parent, const deck_dvc_node_t *n,
                                    bool password)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_width(col, lv_pct(100));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(col, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

    const char *label = attr_str(n, "label", NULL);
    with_caption(col, label);

    lv_obj_t *ta = lv_textarea_create(col);
    lv_obj_set_width(ta, lv_pct(100));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, password);
    const char *placeholder = attr_str(n, "placeholder", NULL);
    if (placeholder) lv_textarea_set_placeholder_text(ta, placeholder);
    const char *value = attr_str(n, "value", NULL);
    if (value) lv_textarea_set_text(ta, value);
    style_input_box(ta);

    if (attr_bool(n, "disabled", false)) lv_obj_add_state(ta, LV_STATE_DISABLED);

    if (n->intent_id != 0) {
        lv_obj_add_event_cb(ta, textarea_event_cb, LV_EVENT_DEFOCUSED,
                             (void *)(uintptr_t)n->intent_id);
        lv_obj_add_event_cb(ta, textarea_event_cb, LV_EVENT_READY,
                             (void *)(uintptr_t)n->intent_id);
    }
    return col;
}

/* ---------- TOGGLE / SWITCH ---------- */

static void toggle_event_cb(lv_event_t *e)
{
    uint32_t intent_id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *sw = lv_event_get_target(e);
    if (intent_id == 0) return;
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "intent fired: id=%u toggle=%s",
             (unsigned)intent_id, checked ? "true" : "false");
}

static lv_obj_t *render_toggle(lv_obj_t *parent, const deck_dvc_node_t *n)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const char *label = attr_str(n, "label", NULL);
    if (label) {
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, label);
        lv_obj_set_style_text_color(l, CD_PRIMARY, LV_PART_MAIN);
    }

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_style_bg_color(sw, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, CD_PRIMARY,     LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (attr_bool(n, "value", false))    lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (attr_bool(n, "disabled", false)) lv_obj_add_state(sw, LV_STATE_DISABLED);

    if (n->intent_id != 0) {
        lv_obj_add_event_cb(sw, toggle_event_cb, LV_EVENT_VALUE_CHANGED,
                             (void *)(uintptr_t)n->intent_id);
    }
    return row;
}

/* ---------- SLIDER / RANGE ---------- */

static void slider_event_cb(lv_event_t *e)
{
    uint32_t intent_id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *sl = lv_event_get_target(e);
    if (intent_id == 0) return;
    int32_t v = lv_slider_get_value(sl);
    ESP_LOGI(TAG, "intent fired: id=%u slider=%d", (unsigned)intent_id, (int)v);
}

static lv_obj_t *render_slider(lv_obj_t *parent, const deck_dvc_node_t *n)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_width(col, lv_pct(100));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(col, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

    const char *label = attr_str(n, "label", NULL);
    with_caption(col, label);

    lv_obj_t *sl = lv_slider_create(col);
    lv_obj_set_width(sl, lv_pct(100));
    int32_t min = (int32_t)attr_i64(n, "min", 0);
    int32_t max = (int32_t)attr_i64(n, "max", 100);
    int32_t val = (int32_t)attr_i64(n, "value", min);
    lv_slider_set_range(sl, min, max);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, CD_PRIMARY,     LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, CD_PRIMARY,     LV_PART_KNOB);
    if (attr_bool(n, "disabled", false)) lv_obj_add_state(sl, LV_STATE_DISABLED);

    if (n->intent_id != 0) {
        lv_obj_add_event_cb(sl, slider_event_cb, LV_EVENT_RELEASED,
                             (void *)(uintptr_t)n->intent_id);
    }
    return col;
}

/* ---------- CHOICE ---------- */

static void choice_event_cb(lv_event_t *e)
{
    uint32_t intent_id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *dd = lv_event_get_target(e);
    if (intent_id == 0) return;
    char buf[32] = {0};
    lv_dropdown_get_selected_str(dd, buf, sizeof(buf));
    ESP_LOGI(TAG, "intent fired: id=%u choice=\"%s\"",
             (unsigned)intent_id, buf);
}

static lv_obj_t *render_choice(lv_obj_t *parent, const deck_dvc_node_t *n)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_width(col, lv_pct(100));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(col, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

    const char *label = attr_str(n, "label", NULL);
    with_caption(col, label);

    lv_obj_t *dd = lv_dropdown_create(col);
    lv_obj_set_width(dd, lv_pct(100));
    style_input_box(dd);

    /* Build "a\nb\nc" string from :options. */
    const deck_dvc_attr_t *opts = deck_dvc_find_attr(n, "options");
    if (opts && opts->type == DVC_ATTR_LIST_STR && opts->value.list_str.count) {
        size_t total = 0;
        for (uint16_t i = 0; i < opts->value.list_str.count; i++) {
            total += strlen(opts->value.list_str.items[i]) + 1;
        }
        char *flat = lv_mem_alloc(total + 1);
        if (flat) {
            flat[0] = '\0';
            for (uint16_t i = 0; i < opts->value.list_str.count; i++) {
                if (i) strcat(flat, "\n");
                strcat(flat, opts->value.list_str.items[i]);
            }
            lv_dropdown_set_options(dd, flat);
            lv_mem_free(flat);
        }
    }

    const char *current = attr_str(n, "value", NULL);
    if (current) {
        int32_t idx = lv_dropdown_get_option_index(dd, current);
        if (idx >= 0) lv_dropdown_set_selected(dd, (uint16_t)idx);
    }
    if (attr_bool(n, "disabled", false)) lv_obj_add_state(dd, LV_STATE_DISABLED);

    if (n->intent_id != 0) {
        lv_obj_add_event_cb(dd, choice_event_cb, LV_EVENT_VALUE_CHANGED,
                             (void *)(uintptr_t)n->intent_id);
    }
    return col;
}

/* ---------- PROGRESS ---------- */

static lv_obj_t *render_progress(lv_obj_t *parent, const deck_dvc_node_t *n)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, 12);
    int32_t min = (int32_t)attr_i64(n, "min", 0);
    int32_t max = (int32_t)attr_i64(n, "max", 100);
    int32_t val = (int32_t)attr_i64(n, "value", 0);
    lv_bar_set_range(bar, min, max);
    lv_bar_set_value(bar, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, CD_PRIMARY,     LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    return bar;
}

/* DVC_DATA_ROW — stacked dim-label-over-primary-value pair, per
 * CLAUDE.md ui_common_data_row spec. Differs from DVC_LABEL (which
 * used to also render data rows, leaking the label attr). Font is the
 * LVGL default for both lines; we lean on color + opacity to convey the
 * label/value hierarchy since the decoder doesn't customize fonts yet. */
static lv_obj_t *render_data_row(lv_obj_t *parent, const deck_dvc_node_t *n)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_width(col, lv_pct(100));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(col, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START,
                                LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    const char *label = attr_str(n, "label", NULL);
    const char *value = attr_str(n, "value", "");
    if (label && *label) {
        lv_obj_t *l = lv_label_create(col);
        lv_label_set_text(l, label);
        lv_obj_set_style_text_color(l, CD_PRIMARY, LV_PART_MAIN);
        lv_obj_set_style_text_opa(l, LV_OPA_60, LV_PART_MAIN);
    }
    lv_obj_t *v = lv_label_create(col);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_color(v, CD_PRIMARY, LV_PART_MAIN);
    return col;
}

/* ---------- dispatch ---------- */

static lv_obj_t *render_node(lv_obj_t *parent, const deck_dvc_node_t *n)
{
    if (!n) return NULL;
    switch ((deck_dvc_type_t)n->type) {
        case DVC_GROUP:    return render_group(parent, n);
        case DVC_COLUMN:   return render_column(parent, n);
        case DVC_FORM:
        case DVC_LIST:
        case DVC_LIST_ITEM: return render_column(parent, n);
        case DVC_ROW:      return render_row(parent, n);
        case DVC_LABEL:    return render_label(parent, n);
        case DVC_DATA_ROW: return render_data_row(parent, n);
        case DVC_TRIGGER:
        case DVC_NAVIGATE: return render_trigger(parent, n);
        case DVC_TEXT:     return render_text_input(parent, n, false);
        case DVC_PASSWORD: return render_text_input(parent, n, true);
        case DVC_TOGGLE:
        case DVC_SWITCH:   return render_toggle(parent, n);
        case DVC_SLIDER:   return render_slider(parent, n);
        case DVC_CHOICE:   return render_choice(parent, n);
        case DVC_PROGRESS: return render_progress(parent, n);
        case DVC_SPACER:   return render_spacer(parent, n);
        case DVC_DIVIDER:  return render_divider(parent, n);
        case DVC_EMPTY:    return NULL;
        default:
            /* Unknown / not-yet-implemented type — placeholder label. */
            {
                lv_obj_t *lbl = lv_label_create(parent);
                char buf[40];
                snprintf(buf, sizeof(buf), "[dvc_type=%u]", n->type);
                lv_label_set_text(lbl, buf);
                lv_obj_set_style_text_color(lbl, CD_PRIMARY_DIM, LV_PART_MAIN);
                return lbl;
            }
    }
}

deck_sdi_err_t deck_bridge_ui_render(const deck_dvc_node_t *root)
{
    if (!root) return DECK_SDI_ERR_INVALID_ARG;

    /* Overlays render onto lv_layer_top, leaving the active screen
     * untouched. Useful for toasts/loading/confirm that don't disturb
     * the underlying activity. */
    if (deck_bridge_ui_render_overlay(root)) return DECK_SDI_OK;

    lv_obj_t *scr = lv_scr_act();
    if (!scr) return DECK_SDI_ERR_FAIL;

    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    /* Statusbar + navbar live on lv_layer_top (survive screen swaps) and
     * cover the top 36 px / bottom 48 px of the screen. Reserve that room
     * in pad_top / pad_bottom so content never slips behind either bar.
     * Side padding stays at 16 for breathing room. Matches the make_
     * content_area convention in deck_shell_apps.c. */
    lv_obj_set_style_pad_top(scr,    36 + 16, LV_PART_MAIN);  /* SB_HEIGHT + breath */
    lv_obj_set_style_pad_bottom(scr, 48 + 16, LV_PART_MAIN);  /* NB_HEIGHT + breath */
    lv_obj_set_style_pad_left(scr,   16, LV_PART_MAIN);
    lv_obj_set_style_pad_right(scr,  16, LV_PART_MAIN);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 14, LV_PART_MAIN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    render_node(scr, root);
    return DECK_SDI_OK;
}
