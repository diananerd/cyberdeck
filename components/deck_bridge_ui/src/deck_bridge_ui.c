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

#include "esp_log.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "bridge_ui";

/* Reusable decode arena. Reset on each render so memory stays bounded. */
static deck_arena_t s_render_arena = {0};
static bool         s_arena_inited = false;

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

/* BRIDGE §9 diffing state. Last envelope seen per (app_id, machine_id).
 * The table is tiny — apps rarely run >1 concurrently in the reference
 * bridge, so a flat 4-slot array is enough. */
typedef enum {
    BRIDGE_DIFF_PUSH = 0,      /* new activity */
    BRIDGE_DIFF_REPLACE,       /* same (app, machine), different state */
    BRIDGE_DIFF_PATCH,         /* same state, same tree shape */
    BRIDGE_DIFF_REBUILD,       /* same state, different tree shape */
} bridge_diff_action_t;

#define BRIDGE_DIFF_SLOTS 4
static struct {
    bool     used;
    uint32_t app_id;
    uint32_t machine_id;
    uint32_t state_id;
    uint32_t frame_id;
} s_diff_slots[BRIDGE_DIFF_SLOTS];

static bridge_diff_action_t bridge_diff_decide(const deck_dvc_envelope_t *env,
                                                const deck_dvc_node_t *root,
                                                bool *out_stale)
{
    (void)root;  /* Tree-shape comparison would consult root for PATCH
                   detection; current bridge always does full rebuild on
                   state changes, so shape equality downgrades to REBUILD
                   with the same outcome. Reserved for future. */
    *out_stale = false;
    int hit = -1;
    int empty = -1;
    for (int i = 0; i < BRIDGE_DIFF_SLOTS; i++) {
        if (!s_diff_slots[i].used) { if (empty < 0) empty = i; continue; }
        if (s_diff_slots[i].app_id == env->app_id &&
            s_diff_slots[i].machine_id == env->machine_id) {
            hit = i; break;
        }
    }
    if (hit < 0) {
        /* First snapshot for this (app, machine) — push as a new activity. */
        int slot = (empty >= 0) ? empty : 0;   /* evict slot 0 if full */
        s_diff_slots[slot].used       = true;
        s_diff_slots[slot].app_id     = env->app_id;
        s_diff_slots[slot].machine_id = env->machine_id;
        s_diff_slots[slot].state_id   = env->state_id;
        s_diff_slots[slot].frame_id   = env->frame_id;
        return BRIDGE_DIFF_PUSH;
    }
    /* Stale-snapshot guard: frame_id must be monotonically increasing
     * per (app, machine). An older frame_id means the runtime looped
     * — drop it to avoid overwriting newer state. */
    if (env->frame_id < s_diff_slots[hit].frame_id) {
        *out_stale = true;
        return BRIDGE_DIFF_REBUILD;
    }
    bridge_diff_action_t act;
    if (env->state_id != s_diff_slots[hit].state_id) {
        act = BRIDGE_DIFF_REPLACE;
    } else {
        /* Same identity triple — patch vs rebuild. BRIDGE §9 allows
         * "rebuild everywhere" as conformant; we take it. Future
         * refinement: compare tree shapes via deck_dvc_tree_equal and
         * flip to PATCH when shapes match. */
        act = BRIDGE_DIFF_REBUILD;
    }
    s_diff_slots[hit].state_id = env->state_id;
    s_diff_slots[hit].frame_id = env->frame_id;
    return act;
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

static deck_sdi_err_t bui_push_snapshot_impl(void *ctx,
                                              const void *bytes, size_t len)
{
    (void)ctx;
    if (!bytes || len == 0)             return DECK_SDI_ERR_INVALID_ARG;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;

    if (!s_arena_inited) {
        deck_arena_init(&s_render_arena, 4 * 1024);
        s_arena_inited = true;
    } else {
        deck_arena_reset(&s_render_arena);
    }

    deck_dvc_node_t *root = NULL;
    deck_dvc_envelope_t env = {0};
    deck_err_t r = deck_dvc_decode(bytes, len, &s_render_arena, &env, &root);
    if (r != DECK_RT_OK || !root) {
        ESP_LOGE(TAG, "decode failed: %s", deck_err_name(r));
        return DECK_SDI_ERR_INVALID_ARG;
    }

    bool stale = false;
    bridge_diff_action_t act = bridge_diff_decide(&env, root, &stale);
    if (stale) {
        ESP_LOGW(TAG, "stale snapshot frame=%u (already past) — dropping",
                 (unsigned)env.frame_id);
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
    deck_sdi_err_t rv = deck_bridge_ui_render(root);
    deck_bridge_ui_unlock();
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

static deck_sdi_err_t bui_confirm_impl(void *ctx,
                                        const char *title, const char *message,
                                        const char *ok_label, const char *cancel_label,
                                        deck_sdi_bridge_ui_cb_t on_ok,
                                        deck_sdi_bridge_ui_cb_t on_cancel,
                                        void *user_data)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    if (!deck_bridge_ui_lock(200))       return DECK_SDI_ERR_BUSY;
    deck_bridge_ui_overlay_confirm_cb(title, message, ok_label, cancel_label,
                                       on_ok, on_cancel, user_data);
    deck_bridge_ui_unlock();
    return DECK_SDI_OK;
}

static const deck_sdi_bridge_ui_vtable_t s_vtable = {
    .init          = bui_init_impl,
    .push_snapshot = bui_push_snapshot_impl,
    .clear         = bui_clear_impl,
    .confirm       = bui_confirm_impl,
};

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
