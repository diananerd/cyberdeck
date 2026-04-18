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

#include "deck_bridge_ui.h"

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

    /* Bundled DL2 demo apps. */
    make_app_card(area, "[#]", "SETTINGS",
                   launcher_navigate_cb, (void *)(uintptr_t)APP_ID_SETTINGS, true);
    make_app_card(area, "[+]", "COUNTER",
                   launcher_navigate_cb, (void *)(uintptr_t)APP_ID_COUNTER, true);
    make_app_card(area, "[T]", "TASKMAN",
                   launcher_navigate_cb, (void *)(uintptr_t)APP_ID_TASKMAN, true);
    make_app_card(area, "[N]", "NET HELLO",
                   launcher_navigate_cb, (void *)(uintptr_t)APP_ID_NET_HELLO, true);
    /* Stubs for visual variety / future apps. */
    make_app_card(area, "[?]", "BOOKS",    launcher_card_stub_cb, NULL, false);
    make_app_card(area, "[?]", "NOTES",    launcher_card_stub_cb, NULL, false);
    make_app_card(area, "[?]", "FILES",    launcher_card_stub_cb, NULL, false);
    make_app_card(area, "[?]", "WIFI",     launcher_card_stub_cb, NULL, false);
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

deck_err_t deck_shell_dl2_boot(void)
{
    if (s_booted) return DECK_RT_OK;
    s_booted = true;

    /* Restore display rotation before we start drawing real screens. */
    deck_shell_rotation_restore();

    /* Register intent resolvers for all bundled apps. */
    deck_shell_settings_register();
    deck_shell_apps_register();

    /* Show lockscreen — fires on_unlocked synchronously if no PIN. */
    deck_shell_lockscreen_show(on_unlocked);
    return DECK_RT_OK;
}
