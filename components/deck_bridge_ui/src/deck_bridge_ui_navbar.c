/* deck_bridge_ui_navbar — bottom dock with BACK + HOME.
 *
 * Two outline buttons docked at the screen bottom. Pressing BACK
 * invokes the registered back callback; HOME invokes the home callback.
 *
 * F27 will replace these callbacks with the activity-stack pop / pop_to_home
 * defaults; today the host wires them in.
 */

#include "deck_bridge_ui.h"
#include "deck_bridge_ui_internal.h"

#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "bridge_ui.nb";

#define CD_PRIMARY      lv_color_hex(0x00FF41)
#define CD_PRIMARY_DIM  lv_color_hex(0x004D13)
#define CD_BG_DARK      lv_color_black()

#define NB_HEIGHT       48

static lv_obj_t *s_bar = NULL;
static deck_bridge_ui_nav_cb_t s_back_cb = NULL;
static deck_bridge_ui_nav_cb_t s_home_cb = NULL;

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "BACK pressed");
    if (s_back_cb) s_back_cb();
}
static void home_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "HOME pressed");
    if (s_home_cb) s_home_cb();
}

static lv_obj_t *make_outline_btn(lv_obj_t *parent, const char *text)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(b, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(b, 6,  LV_PART_MAIN);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_center(l);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return b;
}

deck_sdi_err_t deck_bridge_ui_navbar_init(deck_bridge_ui_nav_cb_t back_cb,
                                          deck_bridge_ui_nav_cb_t home_cb)
{
    s_back_cb = back_cb;
    s_home_cb = home_cb;
    if (s_bar) return DECK_SDI_OK;
    if (!deck_bridge_ui_lock(200)) return DECK_SDI_ERR_BUSY;

    /* Same reasoning as statusbar — live on lv_layer_top to survive
     * activity screen swaps. */
    lv_obj_t *layer = lv_layer_top();
    s_bar = lv_obj_create(layer);
    lv_obj_set_size(s_bar, lv_disp_get_hor_res(NULL), NB_HEIGHT);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_bar, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bar, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_side(s_bar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_bar, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_bar, 6,  LV_PART_MAIN);
    lv_obj_set_flex_flow(s_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = make_outline_btn(s_bar, "< BACK");
    lv_obj_add_event_cb(back, back_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *home = make_outline_btn(s_bar, "HOME");
    lv_obj_add_event_cb(home, home_click_cb, LV_EVENT_CLICKED, NULL);

    deck_bridge_ui_unlock();
    ESP_LOGI(TAG, "navbar mounted (%dpx)", NB_HEIGHT);
    return DECK_SDI_OK;
}

void deck_bridge_ui_navbar_relayout(void)
{
    /* Re-size + re-align after rotation. Caller holds the UI lock. */
    if (!s_bar) return;
    lv_obj_set_size(s_bar, lv_disp_get_hor_res(NULL), NB_HEIGHT);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
}

void deck_bridge_ui_navbar_set_visible(bool visible)
{
    if (!s_bar) return;
    if (!deck_bridge_ui_lock(200)) return;
    if (visible) lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    deck_bridge_ui_unlock();
}
