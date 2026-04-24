/* deck_bridge_ui_patch — widget-map recording + leaf-attr patch path.
 *
 * The render engine (deck_bridge_ui_decode.c) calls _record for every
 * primary widget it emits. After render completes, the bridge captures
 * the pre-order array with _snapshot and stores it in the slot cache.
 *
 * When a subsequent snapshot arrives with the same envelope identity
 * triple (app_id, machine_id, state_id) AND the same tree shape
 * (deck_dvc_tree_same_shape == 0), the bridge invokes _apply with the
 * old tree, new tree, and cached widget map. _apply walks the two
 * trees pre-order and patches leaf attribute values on the matching
 * LVGL widget.
 *
 * Supported leaf-attr diffs:
 *   LABEL.value        → lv_label_set_text
 *   TRIGGER/NAVIGATE.label → inner label (first child)
 *   RICH_TEXT.value / MARKDOWN.value / DATA_ROW.value
 *                      → lv_label_set_text on the primary widget
 *   TOGGLE/SWITCH.value → lv_obj_add/clear_state CHECKED
 *   SLIDER.value       → lv_slider_set_value
 *   PROGRESS.value     → lv_bar_set_value (0..100 → 0..1000)
 *
 * Any other leaf-attr diff (position, spacing, id, …) returns nonzero
 * from _apply and the bridge does a full REBUILD as conformant fallback.
 */

#include "deck_bridge_ui.h"
#include "deck_bridge_ui_internal.h"

#include "deck_dvc.h"
#include "lvgl.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "bridge_ui.patch";

/* ---------- recording ---------- */

static deck_bridge_ui_patch_entry_t *s_rec      = NULL;
static size_t                        s_rec_n    = 0;
static size_t                        s_rec_cap  = 0;
static bool                          s_recording = false;

void deck_bridge_ui_patch_begin(size_t cap_hint)
{
    /* Stop any previous recording that wasn't snapshot'd. */
    if (s_rec) { free(s_rec); s_rec = NULL; }
    s_rec_n    = 0;
    s_rec_cap  = cap_hint < 16 ? 16 : cap_hint;
    s_rec      = malloc(sizeof(*s_rec) * s_rec_cap);
    if (!s_rec) { s_rec_cap = 0; s_recording = false; return; }
    s_recording = true;
}

void deck_bridge_ui_patch_record(const deck_dvc_node_t *n, lv_obj_t *obj)
{
    if (!s_recording || !s_rec) return;
    if (s_rec_n >= s_rec_cap) {
        size_t new_cap = s_rec_cap * 2;
        deck_bridge_ui_patch_entry_t *grown = realloc(s_rec, sizeof(*s_rec) * new_cap);
        if (!grown) return;   /* drop silently; PATCH path will fall back */
        s_rec = grown;
        s_rec_cap = new_cap;
    }
    s_rec[s_rec_n].node = n;
    s_rec[s_rec_n].obj  = obj;
    s_rec_n++;
}

size_t deck_bridge_ui_patch_snapshot(deck_bridge_ui_patch_entry_t **out)
{
    if (!out) return 0;
    *out = s_rec;
    size_t n = s_rec_n;
    s_rec       = NULL;
    s_rec_n     = 0;
    s_rec_cap   = 0;
    s_recording = false;
    return n;
}

/* ---------- apply ---------- */

static bool attr_value_equal(const deck_dvc_attr_t *a, const deck_dvc_attr_t *b)
{
    if (!a || !b) return false;
    if (a->type != b->type) return false;
    switch (a->type) {
        case DVC_ATTR_NONE: return true;
        case DVC_ATTR_BOOL: return a->value.b == b->value.b;
        case DVC_ATTR_I64:  return a->value.i == b->value.i;
        case DVC_ATTR_F64:  return a->value.f == b->value.f;
        case DVC_ATTR_STR:
        case DVC_ATTR_ATOM: {
            const char *x = a->value.s ? a->value.s : "";
            const char *y = b->value.s ? b->value.s : "";
            return strcmp(x, y) == 0;
        }
        case DVC_ATTR_LIST_STR: {
            if (a->value.list_str.count != b->value.list_str.count) return false;
            for (uint16_t i = 0; i < a->value.list_str.count; i++) {
                const char *x = a->value.list_str.items[i]
                                    ? a->value.list_str.items[i] : "";
                const char *y = b->value.list_str.items[i]
                                    ? b->value.list_str.items[i] : "";
                if (strcmp(x, y) != 0) return false;
            }
            return true;
        }
    }
    return false;
}

/* Apply a single node's attr diff. Returns 0 if every diff was
 * handled, nonzero if any diff is unsupported (caller → REBUILD). */
static int apply_node_diff(const deck_dvc_node_t *old_n,
                            const deck_dvc_node_t *new_n,
                            lv_obj_t *obj)
{
    if (!old_n || !new_n || !obj) return 1;
    if (old_n->attr_count != new_n->attr_count) return 2;

    int unsupported = 0;
    for (uint16_t i = 0; i < old_n->attr_count; i++) {
        const deck_dvc_attr_t *oa = &old_n->attrs[i];
        const deck_dvc_attr_t *na = &new_n->attrs[i];
        if (attr_value_equal(oa, na)) continue;

        /* Any attr atom we don't explicitly support causes a rebuild. */
        const char *atom = na->atom;

        switch ((deck_dvc_type_t)new_n->type) {
            case DVC_LABEL:
            case DVC_RICH_TEXT:
            case DVC_MARKDOWN:
                if (strcmp(atom, "value") == 0 && na->type == DVC_ATTR_STR) {
                    lv_label_set_text(obj, na->value.s ? na->value.s : "");
                    break;
                }
                if (strcmp(atom, "label") == 0 && na->type == DVC_ATTR_STR) {
                    lv_label_set_text(obj, na->value.s ? na->value.s : "");
                    break;
                }
                unsupported++;
                break;

            case DVC_TRIGGER:
            case DVC_NAVIGATE: {
                /* Inner label is the first child; update its text. */
                if (strcmp(atom, "label") == 0 && na->type == DVC_ATTR_STR) {
                    uint32_t cnt = lv_obj_get_child_cnt(obj);
                    if (cnt > 0) {
                        lv_obj_t *inner = lv_obj_get_child(obj, 0);
                        lv_label_set_text(inner, na->value.s ? na->value.s : "");
                    }
                    break;
                }
                if (strcmp(atom, "disabled") == 0 && na->type == DVC_ATTR_BOOL) {
                    if (na->value.b) lv_obj_add_state(obj, LV_STATE_DISABLED);
                    else             lv_obj_clear_state(obj, LV_STATE_DISABLED);
                    break;
                }
                unsupported++;
                break;
            }

            case DVC_TOGGLE:
            case DVC_SWITCH:
                if (strcmp(atom, "value") == 0 && na->type == DVC_ATTR_BOOL) {
                    if (na->value.b) lv_obj_add_state(obj, LV_STATE_CHECKED);
                    else             lv_obj_clear_state(obj, LV_STATE_CHECKED);
                    break;
                }
                unsupported++;
                break;

            case DVC_SLIDER:
                if (strcmp(atom, "value") == 0 &&
                    (na->type == DVC_ATTR_I64 || na->type == DVC_ATTR_F64)) {
                    int32_t v = (na->type == DVC_ATTR_I64)
                                    ? (int32_t)na->value.i
                                    : (int32_t)na->value.f;
                    lv_slider_set_value(obj, v, LV_ANIM_OFF);
                    break;
                }
                unsupported++;
                break;

            case DVC_PROGRESS:
                if (strcmp(atom, "value") == 0 &&
                    (na->type == DVC_ATTR_I64 || na->type == DVC_ATTR_F64)) {
                    int32_t v = (na->type == DVC_ATTR_I64)
                                    ? (int32_t)na->value.i
                                    : (int32_t)(na->value.f * 100.0);
                    lv_bar_set_value(obj, v, LV_ANIM_OFF);
                    break;
                }
                unsupported++;
                break;

            case DVC_DATA_ROW: {
                /* DATA_ROW renders as a flex-column with a dim caption
                 * (first child) + a primary value label (second child).
                 * The primary widget captured by the recorder is the row
                 * itself; descend one level to the value label. */
                if (strcmp(atom, "value") == 0 && na->type == DVC_ATTR_STR) {
                    uint32_t cnt = lv_obj_get_child_cnt(obj);
                    if (cnt >= 2) {
                        lv_obj_t *val = lv_obj_get_child(obj, cnt - 1);
                        lv_label_set_text(val, na->value.s ? na->value.s : "");
                    }
                    break;
                }
                if (strcmp(atom, "label") == 0 && na->type == DVC_ATTR_STR) {
                    uint32_t cnt = lv_obj_get_child_cnt(obj);
                    if (cnt >= 1) {
                        lv_obj_t *cap = lv_obj_get_child(obj, 0);
                        lv_label_set_text(cap, na->value.s ? na->value.s : "");
                    }
                    break;
                }
                unsupported++;
                break;
            }

            default:
                unsupported++;
                break;
        }
    }
    return unsupported;
}

/* Pre-order walker. *idx is the running index into entries[]. */
static int walk_apply(const deck_dvc_node_t *old_n,
                      const deck_dvc_node_t *new_n,
                      const deck_bridge_ui_patch_entry_t *entries,
                      size_t n_entries,
                      size_t *idx)
{
    if (!old_n || !new_n) return 100;
    if (*idx >= n_entries) return 101;
    if (entries[*idx].node != new_n && entries[*idx].node != old_n) {
        /* Recorder/walker order drift — rebuild. */
        return 102;
    }
    int rc = apply_node_diff(old_n, new_n, entries[*idx].obj);
    (*idx)++;
    if (rc) return rc;
    for (uint16_t i = 0; i < new_n->child_count; i++) {
        int cr = walk_apply(old_n->children[i], new_n->children[i],
                             entries, n_entries, idx);
        if (cr) return cr;
    }
    return 0;
}

int deck_bridge_ui_patch_apply(const deck_dvc_node_t *old_root,
                                const deck_dvc_node_t *new_root,
                                const deck_bridge_ui_patch_entry_t *entries,
                                size_t n_entries)
{
    if (!old_root || !new_root) return 1;
    if (!entries || n_entries == 0) return 2;

    size_t idx = 0;
    int rc = walk_apply(old_root, new_root, entries, n_entries, &idx);
    if (rc) {
        ESP_LOGD(TAG, "patch fell back to rebuild (code=%d idx=%u/%u)",
                 rc, (unsigned)idx, (unsigned)n_entries);
    }
    return rc;
}
