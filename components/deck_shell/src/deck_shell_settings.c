/* deck_shell_settings — Settings activity + Display sub-screen.
 *
 * F27.3 ships:
 *   - Settings main: list with [DISPLAY, SECURITY, ABOUT] rows
 *   - Settings/Display: rotation chooser (0/90/180/270) + persists via
 *     deck_shell_rotation_set
 *
 * Both screens are LVGL activities pushed via the activity stack;
 * they don't go through DVC (they're system UI, not Deck apps).
 */

#include "deck_shell_settings.h"
#include "deck_shell_intent.h"
#include "deck_shell_rotation.h"

#include "deck_bridge_ui.h"

#include "lvgl.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "shell.settings";

#define APP_ID_SETTINGS  9
#define SCR_MAIN         0
#define SCR_DISPLAY      1

#define CD_PRIMARY      lv_color_hex(0x00FF41)
#define CD_PRIMARY_DIM  lv_color_hex(0x004D13)
#define CD_BG_DARK      lv_color_black()

/* ---------- chrome (under statusbar, above navbar) ---------- */

#define UI_STATUSBAR_H  36
#define UI_NAVBAR_H     48

static lv_obj_t *make_content_area(deck_bridge_ui_activity_t *act)
{
    lv_obj_t *scr = (lv_obj_t *)act->lvgl_screen;
    lv_obj_set_style_bg_color(scr, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    lv_obj_t *area = lv_obj_create(scr);
    lv_obj_set_size(area, lv_pct(100),
                     lv_pct(100));
    lv_obj_set_style_pad_top(area, UI_STATUSBAR_H + 16, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(area, UI_NAVBAR_H + 16, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(area, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(area, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(area, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(area, LV_FLEX_ALIGN_START,
                            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return area;
}

/* ---------- Display sub-screen ---------- */

typedef struct {
    int8_t selected;
} display_state_t;

static void rot_btn_cb(lv_event_t *e)
{
    int rot = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "rotation chosen: %d", rot);
    deck_shell_rotation_set((deck_bridge_ui_rotation_t)rot);
}

static void rot_unavailable_cb(lv_event_t *e)
{
    (void)e;
    deck_bridge_ui_overlay_toast(
        "Rotation 90/180/270 disabled — LVGL sw_rotate + partial buffer conflict. "
        "See CHANGELOG Known Issues.", 2500);
}

static void display_on_create(deck_bridge_ui_activity_t *act, void *intent_data)
{
    (void)intent_data;
    lv_obj_t *area = make_content_area(act);

    lv_obj_t *title = lv_label_create(area);
    lv_label_set_text(title, "DISPLAY");
    lv_obj_set_style_text_color(title, CD_PRIMARY, LV_PART_MAIN);

    lv_obj_t *cap = lv_label_create(area);
    lv_label_set_text(cap, "ROTATION:");
    lv_obj_set_style_text_color(cap, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_pad_top(cap, 8, LV_PART_MAIN);

    lv_obj_t *row = lv_obj_create(area);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    static const char *labels[4] = { "0°", "90°", "180°", "270°" };
    deck_bridge_ui_rotation_t cur = deck_bridge_ui_get_rotation();
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(row);
        lv_obj_set_size(b, 80, 48);
        lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
        bool is_cur = (i == (int)cur);
        /* Non-0 rotations are currently disabled: LVGL sw_rotate needs
         * a full-screen draw buffer to work, and our partial buffer
         * (48 lines) doesn't support it — enabling sw_rotate crashes at
         * boot. Plus the RGB LCD flush path doesn't translate rotated
         * coordinates back to native panel orientation, so pixels would
         * appear mis-placed even with a full buffer. Until a proper
         * rotation-aware flush_cb lands, only 0° is selectable; other
         * buttons render dim + tap shows a toast. */
        bool disabled = !is_cur && i != 0;
        lv_color_t border = is_cur ? CD_PRIMARY :
                            (disabled ? CD_PRIMARY_DIM : CD_PRIMARY);
        lv_obj_set_style_border_color(b, border, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
        if (is_cur) {
            lv_obj_set_style_bg_color(b, CD_PRIMARY, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
        }
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, labels[i]);
        lv_obj_set_style_text_color(l,
                                     is_cur    ? CD_BG_DARK :
                                     disabled  ? CD_PRIMARY_DIM : CD_PRIMARY,
                                     LV_PART_MAIN);
        lv_obj_center(l);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_event_cb(b, disabled ? rot_unavailable_cb : rot_btn_cb,
                             LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static const deck_bridge_ui_lifecycle_t s_display_cbs = {
    .on_create = display_on_create,
};

/* ---------- Main settings list ---------- */

static void item_display_cb(lv_event_t *e)
{
    (void)e;
    deck_bridge_ui_activity_push(APP_ID_SETTINGS, SCR_DISPLAY,
                                  &s_display_cbs, NULL);
}
static void item_about_cb(lv_event_t *e)
{
    (void)e;
    deck_bridge_ui_overlay_toast("Coming soon...", 1500);
}

static lv_obj_t *make_list_item(lv_obj_t *parent, const char *text,
                                 lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 12, LV_PART_MAIN);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, CD_PRIMARY, LV_PART_MAIN);

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    return row;
}

static void main_on_create(deck_bridge_ui_activity_t *act, void *intent_data)
{
    (void)intent_data;
    lv_obj_t *area = make_content_area(act);

    lv_obj_t *title = lv_label_create(area);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_color(title, CD_PRIMARY, LV_PART_MAIN);

    make_list_item(area, "DISPLAY", item_display_cb);
    make_list_item(area, "ABOUT",   item_about_cb);
}

static const deck_bridge_ui_lifecycle_t s_main_cbs = {
    .on_create = main_on_create,
};

/* ---------- intent resolver ---------- */

static deck_err_t settings_intent_resolver(const deck_shell_intent_t *intent)
{
    switch (intent->screen_id) {
        case SCR_MAIN:
            deck_bridge_ui_activity_push(APP_ID_SETTINGS, SCR_MAIN,
                                          &s_main_cbs, NULL);
            return DECK_RT_OK;
        case SCR_DISPLAY:
            deck_bridge_ui_activity_push(APP_ID_SETTINGS, SCR_DISPLAY,
                                          &s_display_cbs, NULL);
            return DECK_RT_OK;
    }
    return DECK_LOAD_UNRESOLVED;
}

deck_err_t deck_shell_settings_register(void)
{
    return deck_shell_intent_register(APP_ID_SETTINGS,
                                       settings_intent_resolver);
}
