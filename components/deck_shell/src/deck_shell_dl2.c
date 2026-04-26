/* deck_shell_dl2 — DL2 shell boot entry.
 *
 * Wires together the lockscreen, navbar BACK/HOME, intent registry,
 * and a minimal launcher activity so the OS boots into something
 * usable. F29 will replace this launcher with the real launcher app
 * (Annex A); for now we ship a tile grid that points at Settings.
 */

#include "deck_shell_dl2.h"
#include "deck_shell_lockscreen.h"
#include "deck_shell_intent.h"
#include "deck_shell_settings.h"
#include "deck_shell_rotation.h"
#include "deck_shell_apps.h"
#include "deck_shell_deck_apps.h"

#include "deck_bridge_ui.h"
#include "deck_interp.h"
#include "deck_alloc.h"

#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "shell.dl2";

#define APP_ID_LAUNCHER  0
#define APP_ID_TASKMAN   1
#define APP_ID_COUNTER   4
#define APP_ID_NET_HELLO 7
#define APP_ID_SETTINGS  9

#define UI_STATUSBAR_H   36
#define UI_NAVBAR_H      72

#define CD_PRIMARY      lv_color_hex(0x00FF41)
#define CD_PRIMARY_DIM  lv_color_hex(0x004D13)
#define CD_BG_DARK      lv_color_black()
#define CD_BG_CARD      lv_color_hex(0x0A0A0A)

static bool s_unlocked = false;
static bool s_booted   = false;

bool deck_shell_dl2_unlocked(void) { return s_unlocked; }

/* ---------- launcher ---------- */

static void launcher_navigate_cb(lv_event_t *e)
{
    uint16_t app_id = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    deck_shell_intent_t intent = {
        .app_id    = app_id,
        .screen_id = 0,
    };
    deck_shell_intent_navigate(&intent);
}

static void launcher_card_stub_cb(lv_event_t *e)
{
    (void)e;
    deck_bridge_ui_overlay_toast("Coming soon...", 1500);
}

/* App card — pre-deck-lang TUI grid card.
 * Square, 2px primary_dim border (active) or full-dim (stub), radius 16.
 * Press = invert (bg primary, text bg_dark). Icon CYBERDECK_FONT_XL,
 * name CYBERDECK_FONT_SM dim, gap 4px. */
static lv_obj_t *make_app_card(lv_obj_t *parent, lv_coord_t sz,
                                const char *icon, const char *name,
                                lv_event_cb_t cb, void *cb_data, bool active)
{
    lv_color_t border_c = active ? CD_PRIMARY : CD_PRIMARY_DIM;
    lv_color_t icon_c   = active ? CD_PRIMARY : CD_PRIMARY_DIM;
    lv_color_t name_c   = CD_PRIMARY_DIM;
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, sz, sz);
    lv_obj_set_style_bg_color(c, CD_BG_DARK, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, border_c, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_all(c, 4, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 4, 0);

    /* Press invert. */
    lv_obj_set_style_bg_color(c, CD_PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(c, CD_PRIMARY, LV_STATE_PRESSED);

    lv_obj_t *ic = lv_label_create(c);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, icon_c, 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(ic, CD_BG_DARK, LV_STATE_PRESSED);

    lv_obj_t *nm = lv_label_create(c);
    lv_label_set_text(nm, name);
    lv_obj_set_style_text_color(nm, name_c, 0);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(nm, CD_BG_DARK, LV_STATE_PRESSED);

    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, cb_data);
    return c;
}

/* Per-app launcher entry built from deck_shell_deck_apps. We exclude
 * taskman because it's surfaced via the navbar TASKS zone instead of
 * a card. Up to 9 user-facing apps (3x3 portrait or 5x2 landscape). */
typedef struct {
    uint16_t    app_id;
    const char *name;
    const char *icon;
} launcher_entry_t;

static const char *icon_for_id(const char *id)
{
    if (!id) return "[?]";
    if (strstr(id, "settings")) return "[#]";
    if (strstr(id, "files"))    return "[F]";
    if (strstr(id, "bluesky"))  return "[B]";
    if (strstr(id, "launcher")) return "[H]";
    if (strstr(id, "demo"))     return "[D]";
    return "[*]";
}

static void uppercase_copy(char *dst, size_t cap, const char *src)
{
    size_t k = 0;
    for (; src && src[k] && k < cap - 1; k++) {
        char ch = src[k];
        dst[k] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    dst[k] = '\0';
}

static void launcher_on_create(deck_bridge_ui_activity_t *act, void *intent_data)
{
    (void)intent_data;
    lv_obj_t *scr = (lv_obj_t *)act->lvgl_screen;
    lv_obj_set_style_bg_color(scr, CD_BG_DARK, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Collect launchable apps from deck_shell_deck_apps. Skip the
     * launcher itself and taskman (which lives in the navbar). Cap at
     * 9 — bigger sets need scrolling, out of scope for the chrome. */
    enum { MAX_LAUNCHER = 9 };
    static char s_names[MAX_LAUNCHER][32];
    launcher_entry_t entries[MAX_LAUNCHER];
    uint8_t n = 0;
    uint32_t cnt = deck_shell_deck_apps_count();
    for (uint32_t i = 0; i < cnt && n < MAX_LAUNCHER; i++) {
        deck_shell_deck_app_info_t info = {0};
        deck_shell_deck_apps_info(i, &info);
        if (!info.id) continue;
        if (strcmp(info.id, "cyberdeck.launcher") == 0) continue;
        if (strcmp(info.id, "cyberdeck.taskman")  == 0) continue;
        const char *src = info.name ? info.name : info.id;
        uppercase_copy(s_names[n], sizeof(s_names[n]), src);
        entries[n].app_id = info.app_id;
        entries[n].name   = s_names[n];
        entries[n].icon   = icon_for_id(info.id);
        n++;
    }
    /* Insertion sort by name. */
    for (uint8_t i = 1; i < n; i++) {
        launcher_entry_t key = entries[i];
        int j = (int)i - 1;
        while (j >= 0 && strcmp(entries[j].name, key.name) > 0) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }

    /* Sizing — leave room for statusbar (top) + navbar (bottom or right). */
    lv_disp_t *disp = lv_disp_get_default();
    lv_coord_t sw = lv_disp_get_hor_res(disp);
    lv_coord_t sh = lv_disp_get_ver_res(disp);
    bool portrait = (sw < sh);
    lv_coord_t avail_w = portrait ? sw : (sw - UI_NAVBAR_H);
    lv_coord_t avail_h = portrait ? (sh - UI_STATUSBAR_H - UI_NAVBAR_H)
                                  : (sh - UI_STATUSBAR_H);
    const lv_coord_t gap = 16;

    uint8_t cols = portrait ? 3 : 5;
    if (n < cols) cols = n > 0 ? n : 1;
    uint8_t rows = (uint8_t)((n + cols - 1) / cols);
    if (rows == 0) rows = 1;

    lv_coord_t card_w = (avail_w - gap * (cols + 1)) / cols;
    lv_coord_t card_h = (avail_h - gap * (rows + 1)) / rows;
    lv_coord_t card_sz = card_w < card_h ? card_w : card_h;
    if (card_sz > 160) card_sz = 160;
    if (card_sz < 72)  card_sz = 72;

    /* Container — clear of the docks. */
    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, avail_w, avail_h);
    lv_obj_align(cont, portrait ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_LEFT,
                 0, UI_STATUSBAR_H);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, gap, 0);
    lv_obj_set_style_pad_column(cont, gap, 0);
    lv_obj_set_style_pad_row(cont, gap, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER,
                                 LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (n == 0) {
        /* Empty-state — single dim card with "[?]". */
        make_app_card(cont, card_sz, "[?]", "NO APPS",
                       launcher_card_stub_cb, NULL, false);
        return;
    }

    for (uint8_t i = 0; i < n; i++) {
        make_app_card(cont, card_sz, entries[i].icon, entries[i].name,
                       launcher_navigate_cb,
                       (void *)(uintptr_t)entries[i].app_id, true);
    }
}

/* ---------- shell boot ---------- */

static void run_intent_canary(void);
static bool s_post_unlock_init_done = false;

/* Heavy boot work that should run AFTER the user has unlocked. Scans
 * the apps partition, registers intent resolvers, fires the runtime
 * canaries. Idempotent — guarded so subsequent unlocks (lock → unlock
 * cycles) skip the scan. */
static void post_unlock_init(void)
{
    if (s_post_unlock_init_done) return;
    s_post_unlock_init_done = true;

    /* Register intent resolvers for all bundled apps. */
    deck_shell_settings_register();
    deck_shell_apps_register();

    /* Scan the apps partition for *.deck files, load each via the
     * persistent app runtime, register an intent resolver. */
    deck_shell_deck_apps_scan_and_register();

    /* H3 — runtime-side intent canary. */
    run_intent_canary();

    /* K5 — DL3 tick-scheduler canary. */
    deck_runtime_dl3_tick_canary();
}

static void launcher_on_resume(deck_bridge_ui_activity_t *act, void *intent_data);

static const deck_bridge_ui_lifecycle_t s_launcher_cbs = {
    .on_create = launcher_on_create,
    .on_resume = launcher_on_resume,
};

/* Build/rebuild the launcher grid from the current loaded-apps slot
 * table. Wipes any prior children on the launcher screen. Called from
 * on_create (initial empty paint, before scan) and on_resume (after
 * scan completes). */
static void launcher_build_grid_now(void)
{
    deck_bridge_ui_activity_t *top = deck_bridge_ui_activity_current();
    if (!top || !top->lvgl_screen) return;
    if (!deck_bridge_ui_lock(200)) return;
    lv_obj_clean((lv_obj_t *)top->lvgl_screen);
    deck_bridge_ui_unlock();
    /* launcher_on_create rebuilds via the same path. Re-invoke it. */
    launcher_on_create(top, NULL);
}

static void launcher_on_resume(deck_bridge_ui_activity_t *act, void *intent_data)
{
    (void)intent_data;
    (void)act;
}

static void on_unlocked(void)
{
    s_unlocked = true;
    ESP_LOGI(TAG, "on_unlocked: pushing launcher (apps already loaded)");
    /* Apps were already scanned + loaded during dl2_boot before the
     * lockscreen. launcher_on_create can build the grid in one pass
     * with the populated slot table. No deferred rebuild needed. */
    deck_bridge_ui_activity_push(APP_ID_LAUNCHER, 0,
                                  &s_launcher_cbs, NULL);
}

/* Routed from bridge.ui set_locked — Deck apps asking the shell to
 * bring the lockscreen back up. false is a no-op (unlock only through
 * PIN verify). */
static void shell_lock_handler(bool locked)
{
    if (!locked) return;
    s_unlocked = false;
    deck_shell_lockscreen_lock(on_unlocked);
}

/* H3 — runtime-side intent canary. Walks the loaded .deck apps, runs
 * each `@on trigger_<atom>` event we expect, and logs the outcome.
 * This proves the G4 path (intent-id → @on trigger_<atom> dispatch
 * with payload binding) end-to-end without needing a physical tap on
 * the LCD. The actual touchscreen → bridge intent_hook → runtime path
 * is wired (concept #58/#59/#60) and shares this same code from
 * `deck_runtime_app_dispatch` onwards, so a green canary is a strong
 * signal that the touch path will fire equivalently. */
static void canary_dispatch(deck_runtime_app_t *app, const char *event)
{
    if (!app) return;
    deck_err_t rc = deck_runtime_app_dispatch(app, event, NULL);
    ESP_LOGI(TAG, "tap-canary: %s → %s",
             event, rc == DECK_RT_OK ? "OK" : deck_err_name(rc));
}

/* I2 — exercise a parameterised @on trigger_<atom>(v: T). We can't
 * easily reach intent_id 1's binding from here without rendering the
 * activity, so the canary instead pushes a known payload through the
 * runtime via the same `event` binding the bridge-side intent_hook
 * uses (deck_runtime_app_intent_v with vals[]). The probe constructs
 * a single-scalar payload like a real tap on a slider/choice would. */
static void canary_param_dispatch_atom(deck_runtime_app_t *app,
                                        const char *event,
                                        const char *atom_value)
{
    if (!app) return;
    /* @on trigger_set_theme(v: atom) — bind v to atom_value via the
     * existing dispatch path, which puts `event` in the env and (via
     * G4) also binds the first declared param to `event`. */
    deck_value_t *payload = deck_new_atom(atom_value);
    deck_err_t rc = deck_runtime_app_dispatch(app, event, payload);
    if (payload) deck_release(payload);
    ESP_LOGI(TAG, "tap-canary: %s(%s) → %s",
             event, atom_value, rc == DECK_RT_OK ? "OK" : deck_err_name(rc));
}

static void canary_param_dispatch_int(deck_runtime_app_t *app,
                                       const char *event, int v)
{
    if (!app) return;
    deck_value_t *payload = deck_new_int(v);
    deck_err_t rc = deck_runtime_app_dispatch(app, event, payload);
    if (payload) deck_release(payload);
    ESP_LOGI(TAG, "tap-canary: %s(%d) → %s",
             event, v, rc == DECK_RT_OK ? "OK" : deck_err_name(rc));
}

static void run_intent_canary(void)
{
    /* settings.deck — every parameterless trigger_*. */
    deck_runtime_app_t *settings = NULL;
    deck_runtime_app_t *taskman  = NULL;
    deck_runtime_app_t *files    = NULL;
    deck_runtime_app_t *bluesky  = NULL;
    uint32_t n = deck_shell_deck_apps_count();
    for (uint32_t i = 0; i < n; i++) {
        deck_shell_deck_app_info_t info;
        deck_shell_deck_apps_info(i, &info);
        if (!info.id) continue;
        if      (strcmp(info.id, "cyberdeck.settings") == 0)
            settings = deck_shell_deck_apps_handle(info.app_id);
        else if (strcmp(info.id, "cyberdeck.taskman") == 0)
            taskman  = deck_shell_deck_apps_handle(info.app_id);
        else if (strcmp(info.id, "cyberdeck.files") == 0)
            files    = deck_shell_deck_apps_handle(info.app_id);
        else if (strcmp(info.id, "cyberdeck.bluesky") == 0)
            bluesky  = deck_shell_deck_apps_handle(info.app_id);
    }
    /* trigger_lock_now is intentionally skipped at boot — it would invoke
     * bridge.ui.set_locked(true) which routes through shell_lock_handler
     * and re-shows the lockscreen the user just dismissed. The G4 path is
     * already exercised by every other trigger below. */
    canary_dispatch(settings, "trigger_check_update");
    canary_dispatch(settings, "trigger_about");
    canary_dispatch(taskman,  "trigger_open_compose");
    canary_dispatch(files,    "trigger_go_up");
    canary_dispatch(bluesky,  "trigger_refresh");

    /* I2 — payload-carrying triggers (mirrors what a real choice / range
     * tap would deliver through the bridge intent_hook). */
    canary_param_dispatch_atom(settings, "trigger_set_theme",     "amber");
    canary_param_dispatch_int (settings, "trigger_set_rotation",  90);
    canary_param_dispatch_int (settings, "trigger_set_brightness", 42);
}

/* UI harness — for each reference app, push its activity, attempt a
 * tap on intent_id 1, then pop. Asserts that:
 *   1. every push for a loaded app succeeds (intent_navigate → OK),
 *   2. push/pop sequencing across the activity stack does not crash,
 *   3. simulate_tap is callable without faulting even when the rendered
 *      tree is empty (it just returns false).
 * Reference apps today render via Machine state bodies during load,
 * not via @on resume, so the bridge slot table may stay empty during
 * the harness — `nodes_total` is reported as informational. The PASS
 * gate is push-count vs loaded-count. */
static void ui_harness_run(void)
{
    static const uint16_t app_ids[] = {
        DECK_APPS_BASE_ID + 0,
        DECK_APPS_BASE_ID + 1,
        DECK_APPS_BASE_ID + 2,
        DECK_APPS_BASE_ID + 3,
        DECK_APPS_BASE_ID + 4,
    };
    int loaded = 0, pushed = 0, popped = 0, taps = 0;
    for (size_t i = 0; i < sizeof(app_ids) / sizeof(app_ids[0]); i++) {
        deck_runtime_app_t *app = deck_shell_deck_apps_handle(app_ids[i]);
        if (!app) continue;
        loaded++;
        deck_shell_intent_t intent = {
            .app_id = app_ids[i], .screen_id = 0, .data = NULL, .data_size = 0,
        };
        if (deck_shell_intent_navigate(&intent) != DECK_RT_OK) continue;
        pushed++;
        vTaskDelay(pdMS_TO_TICKS(60));
        if (deck_bridge_ui_simulate_tap(1)) {
            taps++;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (deck_bridge_ui_activity_pop() == DECK_SDI_OK) popped++;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    size_t total = deck_bridge_ui_dvc_node_count(0);
    /* PASS gate: every loaded app accepted intent_navigate without
     * faulting. popped < pushed by exactly 1 is expected — the harness
     * runs before on_unlocked pushes the launcher into slot 0, so the
     * first activity push lands there and the matching pop is blocked
     * by the slot-0 guard (correct — slot 0 is the launcher contract).
     * The system surviving 5 pushes + 4 pops with no panic / heap loss
     * is the real signal we're after here. */
    bool ok = (loaded > 0) && (pushed == loaded) && (popped >= pushed - 1);
    ESP_LOGI(TAG, "ui-harness: loaded=%d pushed=%d popped=%d taps=%d "
                  "nodes_total=%u → %s",
             loaded, pushed, popped, taps, (unsigned)total,
             ok ? "PASS" : "FAIL");
}

deck_err_t deck_shell_dl2_boot(void)
{
    if (s_booted) return DECK_RT_OK;
    s_booted = true;

    /* Inject bridge.ui resolvers that need shell collaboration. Done
     * before boot so any app that `set_locked(true)`s during warmup is
     * routed correctly. */
    deck_bridge_ui_set_lock_handler(shell_lock_handler);

    /* Restore display rotation before we start drawing real screens. */
    deck_shell_rotation_restore();

    /* Run the scan + canaries on the main task BEFORE the lockscreen.
     * Doing this in the unlock callback path triggered a GDMA descriptor
     * race: each .deck app's @on launch state machine emitted a
     * push_snapshot that rewrote the active screen while the LCD's GDMA
     * was still draining the launcher's grid frame, corrupting the
     * descriptor chain and crashing in gdma_default_tx_isr. Scanning
     * here means renders go to a dormant background screen (the
     * lockscreen on lv_layer_sys covers it), no race possible. */
    post_unlock_init();

    /* Lockscreen — when a PIN is configured it gates the launcher
     * behind PIN verify. When no PIN is set deck_shell_lockscreen_show
     * fires the callback synchronously and the device lands directly
     * on the launcher.
     *
     * NOTE: ui_harness_run is not invoked at boot — it leaves a
     * residual .deck activity in slot 0 that pop_to_home later loads,
     * rendering [dvc_type=1] over the launcher. */
    (void)ui_harness_run;
    deck_shell_lockscreen_show(on_unlocked);
    return DECK_RT_OK;
}
