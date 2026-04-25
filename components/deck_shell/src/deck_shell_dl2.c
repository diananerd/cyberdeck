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

#include <string.h>

static const char *TAG = "shell.dl2";

#define APP_ID_LAUNCHER  0
#define APP_ID_TASKMAN   1
#define APP_ID_COUNTER   4
#define APP_ID_NET_HELLO 7
#define APP_ID_SETTINGS  9

#define UI_STATUSBAR_H   36
#define UI_NAVBAR_H      48

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

static lv_obj_t *make_app_card(lv_obj_t *parent, const char *icon,
                                const char *name, lv_event_cb_t cb,
                                void *cb_data, bool active)
{
    lv_color_t color = active ? CD_PRIMARY : CD_PRIMARY_DIM;
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 140, 120);
    lv_obj_set_style_bg_color(c, CD_BG_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(c, color, LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *ic = lv_label_create(c);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, color, LV_PART_MAIN);

    lv_obj_t *nm = lv_label_create(c);
    lv_label_set_text(nm, name);
    lv_obj_set_style_text_color(nm, color, LV_PART_MAIN);
    lv_obj_set_style_pad_top(nm, 4, LV_PART_MAIN);

    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, cb_data);
    return c;
}

static void launcher_on_create(deck_bridge_ui_activity_t *act, void *intent_data)
{
    (void)intent_data;
    lv_obj_t *scr = (lv_obj_t *)act->lvgl_screen;
    lv_obj_set_style_bg_color(scr, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    lv_obj_t *area = lv_obj_create(scr);
    lv_obj_set_size(area, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_top(area, UI_STATUSBAR_H + 24, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(area, UI_NAVBAR_H + 24, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(area, 24, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(area, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(area, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_column(area, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(area, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Bundled DL2 demo apps (C-side). */
    make_app_card(area, "[#]", "SETTINGS",
                   launcher_navigate_cb, (void *)(uintptr_t)APP_ID_SETTINGS, true);
    make_app_card(area, "[+]", "COUNTER",
                   launcher_navigate_cb, (void *)(uintptr_t)APP_ID_COUNTER, true);
    make_app_card(area, "[T]", "TASKMAN",
                   launcher_navigate_cb, (void *)(uintptr_t)APP_ID_TASKMAN, true);
    make_app_card(area, "[N]", "NET HELLO",
                   launcher_navigate_cb, (void *)(uintptr_t)APP_ID_NET_HELLO, true);

    /* F28 Phase 2 — .deck source apps. Label is the app name (uppercased
     * for the visual grammar); tapping fires @on resume on the loaded
     * handle. Stubs fill remaining slots if fewer than 4 .deck apps. */
    uint32_t n_deck = deck_shell_deck_apps_count();
    if (n_deck > 4) n_deck = 4;
    for (uint32_t i = 0; i < n_deck; i++) {
        deck_shell_deck_app_info_t info;
        deck_shell_deck_apps_info(i, &info);
        char label[32];
        const char *src = info.name ? info.name : (info.id ? info.id : info.path);
        size_t k = 0;
        for (; src && src[k] && k < sizeof(label) - 1; k++) {
            char ch = src[k];
            label[k] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
        }
        label[k] = '\0';
        make_app_card(area, "[*]", label,
                       launcher_navigate_cb, (void *)(uintptr_t)info.app_id, true);
    }
    for (uint32_t i = n_deck; i < 4; i++) {
        make_app_card(area, "[?]", "SLOT",  launcher_card_stub_cb, NULL, false);
    }
}

static const deck_bridge_ui_lifecycle_t s_launcher_cbs = {
    .on_create = launcher_on_create,
};

/* ---------- shell boot ---------- */

static void on_unlocked(void)
{
    s_unlocked = true;
    ESP_LOGI(TAG, "unlocked — pushing launcher");
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
    canary_dispatch(settings, "trigger_lock_now");
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

    /* Register intent resolvers for all bundled apps. */
    deck_shell_settings_register();
    deck_shell_apps_register();

    /* F28 Phase 2 — scan the apps partition for *.deck files, load each
     * via the persistent app runtime, and register an intent resolver so
     * taps fire @on resume. Failures here are non-fatal — we boot the
     * OS without user apps if none are present or any fail to parse. */
    deck_shell_deck_apps_scan_and_register();

    /* H3 — runtime-side intent canary. */
    run_intent_canary();

    /* K5 — DL3 tick-scheduler canary. */
    deck_runtime_dl3_tick_canary();

    /* Show lockscreen — fires on_unlocked synchronously if no PIN. */
    deck_shell_lockscreen_show(on_unlocked);
    return DECK_RT_OK;
}
