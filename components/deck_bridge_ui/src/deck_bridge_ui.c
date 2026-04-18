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
    deck_err_t r = deck_dvc_decode(bytes, len, &s_render_arena, &root);
    if (r != DECK_RT_OK || !root) {
        ESP_LOGE(TAG, "decode failed: %s", deck_err_name(r));
        return DECK_SDI_ERR_INVALID_ARG;
    }

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

static const deck_sdi_bridge_ui_vtable_t s_vtable = {
    .init          = bui_init_impl,
    .push_snapshot = bui_push_snapshot_impl,
    .clear         = bui_clear_impl,
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

    /* Encode → push_snapshot via the driver. */
    size_t need = 0;
    (void)deck_dvc_encode(root, NULL, 0, &need);
    uint8_t *buf = deck_arena_alloc(&arena, need);
    if (!buf) { deck_arena_reset(&arena); return DECK_SDI_ERR_NO_MEMORY; }
    size_t wrote = 0;
    if (deck_dvc_encode(root, buf, need, &wrote) != DECK_RT_OK) {
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
