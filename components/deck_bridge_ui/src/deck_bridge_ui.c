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

    char msg[48] = "--";
    time_t t = (time_t)(initial_epoch_ms / 1000);
    struct tm tm;
    if (gmtime_r(&t, &tm)) {
        snprintf(msg, sizeof(msg), "%04d-%02d-%02d %02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min);
    }

    date_adapter_t *a = malloc(sizeof(*a));
    if (!a) return DECK_SDI_ERR_NO_MEMORY;
    a->sdi_cb    = on_pick;
    a->user_data = user_data;
    deck_bridge_ui_overlay_confirm_cb(title ? title : "PICK DATE", msg,
                                       "OK", "CANCEL",
                                       bui_date_ok_adapter,
                                       bui_date_cancel_adapter, a);
    return DECK_SDI_OK;
}

static deck_sdi_err_t bui_share_show_impl(void *ctx, const char *text, const char *url)
{
    (void)ctx;
    if (!deck_bridge_ui_lvgl_is_ready()) return DECK_SDI_ERR_FAIL;
    /* Reference bridge has no system share sheet — surface a toast with
     * the payload so developers can verify the request went through. */
    char buf[96];
    if (url && *url) {
        snprintf(buf, sizeof(buf), "SHARE: %s", url);
    } else if (text && *text) {
        snprintf(buf, sizeof(buf), "SHARE: %s", text);
    } else {
        snprintf(buf, sizeof(buf), "SHARE");
    }
    deck_bridge_ui_overlay_toast(buf, 2000);
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
    char title[48];
    snprintf(title, sizeof(title), "PERMISSION: %s",
             permission_name ? permission_name : "?");
    perm_adapter_t *a = malloc(sizeof(*a));
    if (!a) return DECK_SDI_ERR_NO_MEMORY;
    a->on_grant = on_grant; a->on_deny = on_deny; a->user_data = user_data;
    deck_bridge_ui_overlay_confirm_cb(title, rationale ? rationale : "",
                                       "ALLOW", "DENY",
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
    /* Reference bridge has no per-app badge surface yet. Log so callers
     * can verify the request routed. Future: statusbar pill indicator. */
    ESP_LOGI(TAG, "set_badge: app=%s count=%d", app_id ? app_id : "?", count);
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
    /* Board HAL exposes on/off only. Threshold at 1% — anything above
     * that turns the backlight on; below that (including 0) turns it off.
     * True PWM dimming is future work in hal_backlight. */
    esp_err_t rc = hal_backlight_set(level > 0.01f);
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
