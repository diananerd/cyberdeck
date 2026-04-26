/* deck_bridge_ui — SDI bridge.ui driver, LVGL backend.
 *
 * The skeleton from F25.7 (`deck_sdi_bridge_ui_register_skeleton`)
 * accepted DVC bytes and discarded them. This module replaces it: on
 * `push_snapshot` it decodes the bytes into a tree (over a per-render
 * arena) and renders the tree into LVGL widgets on the active screen.
 *
 * The driver re-registers with the same `bridge.ui` SDI name +
 * `DECK_SDI_DRIVER_BRIDGE_UI` id but bumps the version string from the
 * skeleton's "0.1.0" to "1.0.0".
 */

#include "deck_bridge_ui.h"
#include "deck_bridge_ui_internal.h"

#include "deck_sdi_registry.h"
#include "drivers/deck_sdi_bridge_ui.h"
#include "deck_dvc.h"

#include "hal_backlight.h"

#include "esp_log.h"
#include "lvgl.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "bridge_ui";

void deck_bridge_ui_clear_screen(void)
{
    lv_obj_t *scr = lv_scr_act();
    if (!scr) return;
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

/* ---------- driver vtable ---------- */

static deck_sdi_err_t bui_init_impl(void *ctx)
{
    (void)ctx;
    return deck_bridge_ui_lvgl_init();
}

/* BRIDGE §9 diffing state. Per-(app_id, machine_id) slot carries two
 * arenas (alternating) so the previous tree survives decode-reset of
 * the new one, plus a pre-order widget map captured at the last
 * REBUILD/REPLACE/PUSH render. If a subsequent snapshot lands on the
 * same identity triple AND deck_dvc_tree_same_shape(old, new) == 0, the
 * bridge invokes the PATCH path — updating leaf attrs in place without
 * destroying the widget tree. Any unsupported leaf diff bails to
 * REBUILD as a conformant fallback. */
typedef enum {
    BRIDGE_DIFF_PUSH = 0,      /* new activity */
    BRIDGE_DIFF_REPLACE,       /* same (app, machine), different state */
    BRIDGE_DIFF_PATCH,         /* same state, same tree shape */
    BRIDGE_DIFF_REBUILD,       /* same state, different tree shape */
} bridge_diff_action_t;

#define BRIDGE_DIFF_SLOTS 4
typedef struct {
    bool     used;
    uint32_t app_id;
    uint32_t machine_id;
    uint32_t state_id;
    uint32_t frame_id;
    deck_arena_t arenas[2];       /* ping-pong: new decodes land in the
                                      OTHER one; cached tree in arena_cur */
    bool arenas_inited;
    uint8_t arena_cur;            /* 0 or 1 */
    deck_dvc_node_t *cached_root; /* lives in arenas[arena_cur]; NULL until first render */
    deck_bridge_ui_patch_entry_t *entries;
    size_t n_entries;
} bridge_slot_t;

static bridge_slot_t s_slots[BRIDGE_DIFF_SLOTS];

static int bridge_slot_find_or_alloc(const deck_dvc_envelope_t *env, bool *created)
{
    int empty = -1;
    for (int i = 0; i < BRIDGE_DIFF_SLOTS; i++) {
        if (!s_slots[i].used) { if (empty < 0) empty = i; continue; }
        if (s_slots[i].app_id == env->app_id &&
            s_slots[i].machine_id == env->machine_id) {
            *created = false;
            return i;
        }
    }
    int slot = (empty >= 0) ? empty : 0;   /* evict slot 0 if full */
    if (s_slots[slot].entries) {
        free(s_slots[slot].entries);
        s_slots[slot].entries = NULL;
        s_slots[slot].n_entries = 0;
    }
    memset(&s_slots[slot], 0, sizeof(s_slots[slot]));
    s_slots[slot].used       = true;
    s_slots[slot].app_id     = env->app_id;
    s_slots[slot].machine_id = env->machine_id;
    *created = true;
    return slot;
}

static deck_arena_t *bridge_slot_working_arena(bridge_slot_t *slot)
{
    if (!slot->arenas_inited) {
        deck_arena_init(&slot->arenas[0], 4 * 1024);
        deck_arena_init(&slot->arenas[1], 4 * 1024);
        slot->arenas_inited = true;
        slot->arena_cur = 0;
    }
    /* The NEW tree decodes into the arena that is NOT currently holding
     * the cached tree. First time: arenas[1]. */
    uint8_t target = slot->arena_cur ^ 1;
    deck_arena_reset(&slot->arenas[target]);
    return &slot->arenas[target];
}

static void bridge_slot_commit_new_tree(bridge_slot_t *slot,
                                         deck_dvc_node_t *new_root)
{
    /* The new tree now becomes the cached one; its arena is the one we
     * just decoded into (arena_cur ^ 1). Flip the pointer. */
    slot->arena_cur   ^= 1;
    slot->cached_root  = new_root;
}

static bridge_diff_action_t bridge_slot_decide(bridge_slot_t *slot,
                                                const deck_dvc_envelope_t *env,
                                                const deck_dvc_node_t *new_root,
                                                bool first_time,
                                                bool *out_stale)
{
    *out_stale = false;
    if (first_time) {
        slot->state_id = env->state_id;
        slot->frame_id = env->frame_id;
        return BRIDGE_DIFF_PUSH;
    }
    if (env->frame_id < slot->frame_id) {
        *out_stale = true;
        return BRIDGE_DIFF_REBUILD;
    }
    bridge_diff_action_t act;
    if (env->state_id != slot->state_id) {
        act = BRIDGE_DIFF_REPLACE;
    } else if (slot->cached_root &&
               deck_dvc_tree_same_shape(slot->cached_root, new_root) == 0) {
        act = BRIDGE_DIFF_PATCH;
    } else {
        act = BRIDGE_DIFF_REBUILD;
    }
    slot->state_id = env->state_id;
    slot->frame_id = env->frame_id;
    return act;
}

/* After a successful PATCH, the recorded entries[] still point at the
 * OLD tree's nodes — which is about to be freed when the arena rotates.
 * Walk the new tree pre-order and overwrite entries[i].node so the next
 * patch operates on valid pointers. Shape equality guarantees counts
 * match; we bail to n_entries on mismatch (next snapshot triggers
 * REBUILD on a stale-entries check). */
static void bridge_remap_entries_to_new(bridge_slot_t *slot,
                                         const deck_dvc_node_t *new_root);

static void bridge_remap_walk(const deck_dvc_node_t *n,
                               deck_bridge_ui_patch_entry_t *entries,
                               size_t cap, size_t *idx)
{
    if (!n || *idx >= cap) return;
    entries[*idx].node = n;
    (*idx)++;
    for (uint16_t i = 0; i < n->child_count; i++) {
        bridge_remap_walk(n->children[i], entries, cap, idx);
    }
}

static void bridge_remap_entries_to_new(bridge_slot_t *slot,
                                         const deck_dvc_node_t *new_root)
{
    if (!slot->entries || !slot->n_entries) return;
    size_t idx = 0;
    bridge_remap_walk(new_root, slot->entries, slot->n_entries, &idx);
}

static const char *bridge_diff_name(bridge_diff_action_t a)
{
    switch (a) {
        case BRIDGE_DIFF_PUSH:    return "push";
        case BRIDGE_DIFF_REPLACE: return "replace";
        case BRIDGE_DIFF_PATCH:   return "patch";
        case BRIDGE_DIFF_REBUILD: return "rebuild";
    }
    return "?";
}

static deck_sdi_err_t bridge_do_rebuild_render(bridge_slot_t *slot,
                                                const deck_dvc_node_t *new_root)
{
    /* Replace the recorded widget map — cap hint from previous count so
     * realloc churn is minimal on steady-state frame cadence. */
    size_t cap_hint = slot->n_entries ? slot->n_entries : 16;
    deck_bridge_ui_patch_begin(cap_hint);
    deck_sdi_err_t rv = deck_bridge_ui_render(new_root);
    deck_bridge_ui_patch_entry_t *entries = NULL;
    size_t n = deck_bridge_ui_patch_snapshot(&entries);
    if (slot->entries) free(slot->entries);
    slot->entries   = entries;
    slot->n_entries = n;
    return rv;
}

static deck_sdi_err_t bui_push_snapshot_impl(void *ctx,
                                              const void *bytes, size_t len)
{
    (void)ctx;
    if (!bytes || len == 0)             return DECK_SDI_ERR_INVALID_ARG;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;

    /* Peek envelope to find the slot; decoding into the correct arena
     * requires slot lookup first. Do a two-step: first decode the
     * envelope alone would require a new API; instead decode into a
     * scratch arena (slot 0's spare) and once we know the slot, keep
     * that working arena. Simpler: decode into arenas[1] of a transient
     * scratch, then reparent to the target slot. To avoid copies, we
     * lookup the slot by peeking the envelope from the first 20 bytes
     * of the wire header (app_id lives there). */

    if (len < 20) return DECK_SDI_ERR_INVALID_ARG;
    const uint8_t *hb = (const uint8_t *)bytes;
    /* header: magic u16 | version u8 | flags u8 | app_id u32 | machine_id u32 | state u32 | frame u32 (LE) */
    deck_dvc_envelope_t peek = {0};
    peek.app_id     = (uint32_t)hb[4]  | ((uint32_t)hb[5]  << 8) | ((uint32_t)hb[6]  << 16) | ((uint32_t)hb[7]  << 24);
    peek.machine_id = (uint32_t)hb[8]  | ((uint32_t)hb[9]  << 8) | ((uint32_t)hb[10] << 16) | ((uint32_t)hb[11] << 24);
    peek.state_id   = (uint32_t)hb[12] | ((uint32_t)hb[13] << 8) | ((uint32_t)hb[14] << 16) | ((uint32_t)hb[15] << 24);
    peek.frame_id   = (uint32_t)hb[16] | ((uint32_t)hb[17] << 8) | ((uint32_t)hb[18] << 16) | ((uint32_t)hb[19] << 24);

    bool first = false;
    int slot_idx = bridge_slot_find_or_alloc(&peek, &first);
    bridge_slot_t *slot = &s_slots[slot_idx];

    deck_arena_t *work = bridge_slot_working_arena(slot);
    deck_dvc_node_t *new_root = NULL;
    deck_dvc_envelope_t env = {0};
    deck_err_t r = deck_dvc_decode(bytes, len, work, &env, &new_root);
    if (r != DECK_RT_OK || !new_root) {
        ESP_LOGE(TAG, "decode failed: %s", deck_err_name(r));
        return DECK_SDI_ERR_INVALID_ARG;
    }

    bool stale = false;
    bridge_diff_action_t act = bridge_slot_decide(slot, &env, new_root, first, &stale);
    if (stale) {
        ESP_LOGW(TAG, "stale snapshot frame=%u (already past) — dropping",
                 (unsigned)env.frame_id);
        /* The failed-decode arena stays scratch; next push resets it. */
        return DECK_SDI_OK;
    }
    ESP_LOGD(TAG, "snapshot app=%08x machine=%08x state=%08x frame=%u — %s",
             (unsigned)env.app_id, (unsigned)env.machine_id,
             (unsigned)env.state_id, (unsigned)env.frame_id,
             bridge_diff_name(act));

    if (!deck_bridge_ui_lock(500)) {
        ESP_LOGE(TAG, "ui_lock timeout — render dropped");
        return DECK_SDI_ERR_BUSY;
    }

    deck_sdi_err_t rv = DECK_SDI_OK;
    if (act == BRIDGE_DIFF_PATCH) {
        int prc = deck_bridge_ui_patch_apply(slot->cached_root, new_root,
                                              slot->entries, slot->n_entries);
        if (prc == 0) {
            /* Success — entries still reference OLD tree; remap to NEW
             * before we rotate arenas and free the old. */
            bridge_remap_entries_to_new(slot, new_root);
        } else {
            /* Bail to REBUILD. */
            ESP_LOGD(TAG, "patch bailed (code=%d) — rebuilding", prc);
            rv = bridge_do_rebuild_render(slot, new_root);
        }
    } else {
        rv = bridge_do_rebuild_render(slot, new_root);
    }
    deck_bridge_ui_unlock();

    /* Commit the new tree as cached (rotates arena_cur). The OLD tree's
     * arena is the one we will reset on the NEXT push, so it remains
     * valid until then — safe to return. */
    bridge_slot_commit_new_tree(slot, new_root);
    return rv;
}

static deck_sdi_err_t bui_clear_impl(void *ctx)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    if (!deck_bridge_ui_lock(500)) return DECK_SDI_ERR_BUSY;
    deck_bridge_ui_clear_screen();
    deck_bridge_ui_unlock();
    return DECK_SDI_OK;
}

static deck_sdi_err_t bui_toast_impl(void *ctx, const char *text, uint32_t duration_ms)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    deck_bridge_ui_overlay_toast(text ? text : "", duration_ms ? duration_ms : 2000);
    return DECK_SDI_OK;
}

static deck_sdi_err_t bui_confirm_impl(void *ctx,
                                        const char *title, const char *message,
                                        const char *ok_label, const char *cancel_label,
                                        deck_sdi_bridge_ui_cb_t on_ok,
                                        deck_sdi_bridge_ui_cb_t on_cancel,
                                        void *user_data)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    deck_bridge_ui_overlay_confirm_cb(title, message, ok_label, cancel_label,
                                       on_ok, on_cancel, user_data);
    return DECK_SDI_OK;
}

static deck_sdi_err_t bui_loading_show_impl(void *ctx, const char *label)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    deck_bridge_ui_overlay_loading_show(label);
    return DECK_SDI_OK;
}
static deck_sdi_err_t bui_loading_hide_impl(void *ctx)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    deck_bridge_ui_overlay_loading_hide();
    return DECK_SDI_OK;
}

static deck_sdi_err_t bui_progress_show_impl(void *ctx, const char *label)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    deck_bridge_ui_overlay_progress_show(label);
    return DECK_SDI_OK;
}
static deck_sdi_err_t bui_progress_set_impl(void *ctx, float pct)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    deck_bridge_ui_overlay_progress_set(pct);
    return DECK_SDI_OK;
}
static deck_sdi_err_t bui_progress_hide_impl(void *ctx)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    deck_bridge_ui_overlay_progress_hide();
    return DECK_SDI_OK;
}

/* Adapter: overlay choice cb (void*, int) → SDI choice cb (same shape). */
static deck_sdi_err_t bui_choice_show_impl(void *ctx, const char *title,
                                            const char *const *options, uint16_t n,
                                            deck_sdi_bridge_ui_choice_cb_t on_pick,
                                            void *user_data)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    /* The SDI cb type and overlay cb type have the same shape — cast. */
    deck_bridge_ui_overlay_choice_show(title, options, n,
                                        (deck_bridge_ui_overlay_choice_cb_t)on_pick,
                                        user_data);
    return DECK_SDI_OK;
}

/* Bridge between overlay multiselect (gets selected array) and SDI cb
 * which just wants a completion ping. The app queries selection state
 * via its own machine. */
typedef struct {
    deck_sdi_bridge_ui_cb_t sdi_cb;
    void                   *user_data;
} mselect_adapter_t;

static void bui_mselect_done_adapter(void *ud, const bool *selected, uint16_t n)
{
    (void)selected; (void)n;
    mselect_adapter_t *a = (mselect_adapter_t *)ud;
    if (!a) return;
    if (a->sdi_cb) a->sdi_cb(a->user_data);
    free(a);
}
static deck_sdi_err_t bui_multiselect_show_impl(void *ctx, const char *title,
                                                 const char *const *options, uint16_t n,
                                                 const bool *initially,
                                                 deck_sdi_bridge_ui_cb_t on_done,
                                                 void *user_data)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    mselect_adapter_t *a = malloc(sizeof(*a));
    if (!a) return DECK_SDI_ERR_NO_MEMORY;
    a->sdi_cb    = on_done;
    a->user_data = user_data;
    deck_bridge_ui_overlay_multiselect_show(title, options, n, initially,
                                             bui_mselect_done_adapter, a);
    return DECK_SDI_OK;
}

/* Date picker — minimum viable: confirm dialog showing the initial date
 * formatted as "YYYY-MM-DD HH:MM". OK fires on_pick. A future revision
 * replaces this with a proper wheel picker. */
typedef struct {
    deck_sdi_bridge_ui_cb_t sdi_cb;
    void                   *user_data;
} date_adapter_t;

static void bui_date_ok_adapter(void *ud)
{
    date_adapter_t *a = (date_adapter_t *)ud;
    if (!a) return;
    if (a->sdi_cb) a->sdi_cb(a->user_data);
    free(a);
}
static void bui_date_cancel_adapter(void *ud)
{
    date_adapter_t *a = (date_adapter_t *)ud;
    if (a) free(a);
}
static deck_sdi_err_t bui_date_show_impl(void *ctx, const char *title,
                                          int64_t initial_epoch_ms,
                                          deck_sdi_bridge_ui_cb_t on_pick,
                                          void *user_data)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;

    /* J5 — native roller picker. Decompose epoch_ms into Y/M/D for the
     * roller initial values; OK fires the SDI cb (the picked date is
     * readable through deck_bridge_ui_overlay_date_picked_*()). */
    time_t t = (time_t)(initial_epoch_ms / 1000);
    struct tm tm;
    int yy = 2026, mm = 1, dd = 1;
    if (gmtime_r(&t, &tm)) {
        yy = tm.tm_year + 1900;
        mm = tm.tm_mon + 1;
        dd = tm.tm_mday;
    }

    date_adapter_t *a = malloc(sizeof(*a));
    if (!a) return DECK_SDI_ERR_NO_MEMORY;
    a->sdi_cb    = on_pick;
    a->user_data = user_data;
    deck_bridge_ui_overlay_date_show(title ? title : "PICK DATE",
                                      yy, mm, dd,
                                      bui_date_ok_adapter,
                                      bui_date_cancel_adapter, a);
    return DECK_SDI_OK;
}

static deck_sdi_err_t bui_share_show_impl(void *ctx, const char *text, const char *url)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    /* J6 — native share sheet with COPY + DISMISS buttons. */
    deck_bridge_ui_overlay_share_show(text, url, NULL, NULL, NULL);
    return DECK_SDI_OK;
}

typedef struct {
    deck_sdi_bridge_ui_cb_t on_grant;
    deck_sdi_bridge_ui_cb_t on_deny;
    void                   *user_data;
} perm_adapter_t;

static void bui_perm_grant_adapter(void *ud)
{
    perm_adapter_t *a = (perm_adapter_t *)ud;
    if (!a) return;
    if (a->on_grant) a->on_grant(a->user_data);
    free(a);
}
static void bui_perm_deny_adapter(void *ud)
{
    perm_adapter_t *a = (perm_adapter_t *)ud;
    if (!a) return;
    if (a->on_deny) a->on_deny(a->user_data);
    free(a);
}
static deck_sdi_err_t bui_permission_show_impl(void *ctx,
                                                const char *permission_name,
                                                const char *rationale,
                                                deck_sdi_bridge_ui_cb_t on_grant,
                                                deck_sdi_bridge_ui_cb_t on_deny,
                                                void *user_data)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    perm_adapter_t *a = malloc(sizeof(*a));
    if (!a) return DECK_SDI_ERR_NO_MEMORY;
    a->on_grant = on_grant; a->on_deny = on_deny; a->user_data = user_data;
    /* J6 — dedicated permission sheet (not a generic confirm). */
    deck_bridge_ui_overlay_permission_show(permission_name, rationale,
                                            bui_perm_grant_adapter,
                                            bui_perm_deny_adapter, a);
    return DECK_SDI_OK;
}

/* ---- Shell-injected handlers ---- */
static deck_bridge_ui_lock_handler_t  s_lock_handler  = NULL;
static deck_bridge_ui_theme_handler_t s_theme_handler = NULL;
static char s_theme_atom[32] = {0};

void deck_bridge_ui_set_lock_handler(deck_bridge_ui_lock_handler_t cb)
{
    s_lock_handler = cb;
}
void deck_bridge_ui_set_theme_handler(deck_bridge_ui_theme_handler_t cb)
{
    s_theme_handler = cb;
}
const char *deck_bridge_ui_get_theme(void)
{
    return s_theme_atom[0] ? s_theme_atom : NULL;
}

static deck_sdi_err_t bui_set_locked_impl(void *ctx, bool locked)
{
    (void)ctx;
    if (s_lock_handler) s_lock_handler(locked);
    return DECK_SDI_OK;
}

static deck_sdi_err_t bui_set_theme_impl(void *ctx, const char *theme_atom)
{
    (void)ctx;
    if (theme_atom) {
        strncpy(s_theme_atom, theme_atom, sizeof(s_theme_atom) - 1);
        s_theme_atom[sizeof(s_theme_atom) - 1] = '\0';
    } else {
        s_theme_atom[0] = '\0';
    }
    /* J7 — repaint the docks immediately. The activity content
     * underneath only refreshes when the runtime pushes its next
     * snapshot (decoded with the new palette baked in by the runtime
     * convention). */
    deck_bridge_ui_statusbar_apply_theme(theme_atom);
    deck_bridge_ui_navbar_apply_theme(theme_atom);
    if (s_theme_handler) s_theme_handler(theme_atom);
    return DECK_SDI_OK;
}

static deck_sdi_err_t bui_keyboard_show_impl(void *ctx, const char *kind_atom)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    deck_bridge_ui_overlay_keyboard_show(kind_atom);
    return DECK_SDI_OK;
}
static deck_sdi_err_t bui_keyboard_hide_impl(void *ctx)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    deck_bridge_ui_overlay_keyboard_hide();
    return DECK_SDI_OK;
}

static deck_sdi_err_t bui_set_statusbar_impl(void *ctx, bool visible)
{
    (void)ctx;
    deck_bridge_ui_statusbar_set_visible(visible);
    return DECK_SDI_OK;
}
static deck_sdi_err_t bui_set_navbar_impl(void *ctx, bool visible)
{
    (void)ctx;
    deck_bridge_ui_navbar_set_visible(visible);
    return DECK_SDI_OK;
}

static deck_sdi_err_t bui_set_badge_impl(void *ctx, const char *app_id, int count)
{
    (void)ctx;
    /* J4 — statusbar pill. count <= 0 hides; otherwise a small numeric
     * pill appears beside the time/wifi/batt cluster. */
    deck_bridge_ui_statusbar_set_badge(app_id, count);
    return DECK_SDI_OK;
}

static deck_sdi_err_t bui_set_rotation_impl(void *ctx, int rot)
{
    (void)ctx;
    deck_bridge_ui_rotation_t r;
    switch (rot) {
        case 0:   r = DECK_BRIDGE_UI_ROT_0;   break;
        case 90:  r = DECK_BRIDGE_UI_ROT_90;  break;
        case 180: r = DECK_BRIDGE_UI_ROT_180; break;
        case 270: r = DECK_BRIDGE_UI_ROT_270; break;
        default:  return DECK_SDI_ERR_INVALID_ARG;
    }
    return deck_bridge_ui_set_rotation(r);
}

static deck_sdi_err_t bui_set_brightness_impl(void *ctx, float level)
{
    (void)ctx;
    /* J3 — route through hal_backlight_set_level. The reference board's
     * BL line goes through CH422G EXIO2 (no hardware PWM), so the call
     * still quantises to on/off, but a future PWM-capable board picks
     * up smooth dimming without touching the bridge. */
    esp_err_t rc = hal_backlight_set_level(level);
    return (rc == ESP_OK) ? DECK_SDI_OK : DECK_SDI_ERR_FAIL;
}

static const deck_sdi_bridge_ui_vtable_t s_vtable = {
    /* Core */
    .init             = bui_init_impl,
    .push_snapshot    = bui_push_snapshot_impl,
    .clear            = bui_clear_impl,
    .toast            = bui_toast_impl,
    .confirm          = bui_confirm_impl,
    .loading_show     = bui_loading_show_impl,
    .loading_hide     = bui_loading_hide_impl,
    .progress_show    = bui_progress_show_impl,
    .progress_set     = bui_progress_set_impl,
    .progress_hide    = bui_progress_hide_impl,
    .choice_show      = bui_choice_show_impl,
    .multiselect_show = bui_multiselect_show_impl,
    .date_show        = bui_date_show_impl,
    .share_show       = bui_share_show_impl,
    .permission_show  = bui_permission_show_impl,
    .set_locked       = bui_set_locked_impl,
    .set_theme        = bui_set_theme_impl,
    /* Visual */
    .keyboard_show    = bui_keyboard_show_impl,
    .keyboard_hide    = bui_keyboard_hide_impl,
    .set_statusbar    = bui_set_statusbar_impl,
    .set_navbar       = bui_set_navbar_impl,
    .set_badge        = bui_set_badge_impl,
    /* Physical-display */
    .set_rotation     = bui_set_rotation_impl,
    .set_brightness   = bui_set_brightness_impl,
};

bool deck_bridge_ui_simulate_tap(uint32_t intent_id)
{
    if (intent_id == 0) return false;
    /* Walk every slot's entries[] looking for a node with matching
     * intent_id. First match wins — apps rarely run >1 concurrently
     * with overlapping ids. Held in ui_lock so the obj can't be
     * deleted mid-event. */
    if (!deck_bridge_ui_lock(200)) return false;
    bool found = false;
    for (int i = 0; i < BRIDGE_DIFF_SLOTS; i++) {
        bridge_slot_t *s = &s_slots[i];
        if (!s->used || !s->entries) continue;
        for (size_t k = 0; k < s->n_entries; k++) {
            const deck_dvc_node_t *n = s->entries[k].node;
            if (!n || n->intent_id != intent_id) continue;
            lv_obj_t *obj = s->entries[k].obj;
            if (!obj) continue;
            ESP_LOGI(TAG, "simulate_tap: intent_id=%u (slot=%d k=%zu)",
                     (unsigned)intent_id, i, k);
            lv_event_send(obj, LV_EVENT_CLICKED, NULL);
            found = true;
            break;
        }
        if (found) break;
    }
    deck_bridge_ui_unlock();
    return found;
}

bool deck_bridge_ui_assert_intent_visible(uint32_t intent_id)
{
    if (intent_id == 0) return false;
    if (!deck_bridge_ui_lock(200)) return false;
    bool ok = false;
    for (int i = 0; i < BRIDGE_DIFF_SLOTS; i++) {
        bridge_slot_t *s = &s_slots[i];
        if (!s->used || !s->entries) continue;
        for (size_t k = 0; k < s->n_entries; k++) {
            const deck_dvc_node_t *n = s->entries[k].node;
            if (!n || n->intent_id != intent_id) continue;
            lv_obj_t *obj = s->entries[k].obj;
            if (!obj) continue;
            if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) continue;
            ok = true;
            break;
        }
        if (ok) break;
    }
    deck_bridge_ui_unlock();
    return ok;
}

static bool dvc_label_matches(const deck_dvc_node_t *n, const char *needle)
{
    if (!n || n->type != DVC_LABEL) return false;
    const deck_dvc_attr_t *a = deck_dvc_find_attr(n, "value");
    if (!a || a->type != DVC_ATTR_STR || !a->value.s) return false;
    return strstr(a->value.s, needle) != NULL;
}

bool deck_bridge_ui_assert_label_visible(const char *needle)
{
    if (!needle || !*needle) return false;
    if (!deck_bridge_ui_lock(200)) return false;
    bool ok = false;
    for (int i = 0; i < BRIDGE_DIFF_SLOTS && !ok; i++) {
        bridge_slot_t *s = &s_slots[i];
        if (!s->used || !s->entries) continue;
        for (size_t k = 0; k < s->n_entries; k++) {
            const deck_dvc_node_t *n = s->entries[k].node;
            if (dvc_label_matches(n, needle)) {
                lv_obj_t *obj = s->entries[k].obj;
                if (obj && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) { ok = true; break; }
            }
        }
    }
    deck_bridge_ui_unlock();
    return ok;
}

size_t deck_bridge_ui_dvc_node_count(uint32_t app_id)
{
    if (!deck_bridge_ui_lock(200)) return 0;
    size_t n = 0;
    for (int i = 0; i < BRIDGE_DIFF_SLOTS; i++) {
        bridge_slot_t *s = &s_slots[i];
        if (!s->used) continue;
        if (app_id != 0 && s->app_id != app_id) continue;
        n += s->n_entries;
    }
    deck_bridge_ui_unlock();
    return n;
}

deck_sdi_err_t deck_bridge_ui_register_lvgl(void)
{
    /* Bring up LVGL eagerly so push_snapshot doesn't carry a slow
     * first-call. Failure is non-fatal for registration — the driver
     * still registers and surfaces FAIL on push until LVGL comes up. */
    deck_sdi_err_t init_rc = deck_bridge_ui_lvgl_init();
    if (init_rc != DECK_SDI_OK) {
        ESP_LOGW(TAG, "lvgl init returned %s — driver will report FAIL "
                      "on push until init succeeds",
                 deck_sdi_strerror(init_rc));
    }

    const deck_sdi_driver_t driver = {
        .name    = "bridge.ui",
        .id      = DECK_SDI_DRIVER_BRIDGE_UI,
        .version = "1.0.0",
        .vtable  = &s_vtable,
        .ctx     = NULL,
    };
    return deck_sdi_register(&driver);
}

/* ---------- selftest ---------- */

deck_sdi_err_t deck_bridge_ui_selftest(void)
{
    if (!deck_bridge_ui_lvgl_is_ready()) {
        ESP_LOGE(TAG, "selftest skipped — LVGL not initialized");
        return DECK_SDI_ERR_FAIL;
    }

    /* Build a tiny tree directly. */
    deck_arena_t arena = {0};
    deck_arena_init(&arena, 0);
    deck_dvc_node_t *root = deck_dvc_node_new(&arena, DVC_GROUP);
    if (!root) { deck_arena_reset(&arena); return DECK_SDI_ERR_NO_MEMORY; }
    deck_dvc_set_str(&arena, root, "title", "DL2 BRIDGE UI");
    deck_dvc_node_t *label = deck_dvc_node_new(&arena, DVC_LABEL);
    deck_dvc_add_child(&arena, root, label);
    deck_dvc_set_str(&arena, label, "value", "Hello from Deck DL2");

    /* Encode → push_snapshot via the driver. Selftest envelope uses
     * the reserved bridge-selftest identity triple. */
    const deck_dvc_envelope_t env = {
        .app_id = 0x2E1F5E57u, .machine_id = 0u,
        .state_id = 0u, .frame_id = 0u,
    };
    size_t need = 0;
    (void)deck_dvc_encode(&env, root, NULL, 0, &need);
    uint8_t *buf = deck_arena_alloc(&arena, need);
    if (!buf) { deck_arena_reset(&arena); return DECK_SDI_ERR_NO_MEMORY; }
    size_t wrote = 0;
    if (deck_dvc_encode(&env, root, buf, need, &wrote) != DECK_RT_OK) {
        deck_arena_reset(&arena);
        return DECK_SDI_ERR_FAIL;
    }

    deck_sdi_err_t rv = deck_sdi_bridge_ui_push_snapshot(buf, wrote);
    deck_arena_reset(&arena);
    if (rv != DECK_SDI_OK) {
        ESP_LOGE(TAG, "push_snapshot: %s", deck_sdi_strerror(rv));
        return rv;
    }
    ESP_LOGI(TAG, "selftest: PASS (rendered %u-byte tree to LVGL screen)",
             (unsigned)wrote);
    return DECK_SDI_OK;
}
