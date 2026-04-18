/* deck_bridge_ui_statusbar — top dock with time + WiFi + battery.
 *
 * Owns a docked LVGL container at lv_scr_act() top edge, height 36 px.
 * Refresh on a 2 s timer; pulls live data from system.time, network.wifi,
 * system.battery via SDI wrappers.
 *
 * Lives separate from the activity stack — added once at boot, persists
 * across activity push/pop and rotation.
 */

#include "deck_bridge_ui.h"
#include "deck_bridge_ui_internal.h"

#include "drivers/deck_sdi_battery.h"
#include "drivers/deck_sdi_wifi.h"
#include "drivers/deck_sdi_time.h"

#include "lvgl.h"
#include "esp_log.h"

#include <time.h>
#include <stdio.h>

static const char *TAG = "bridge_ui.sb";

#define CD_PRIMARY      lv_color_hex(0x00FF41)
#define CD_PRIMARY_DIM  lv_color_hex(0x004D13)
#define CD_BG_DARK      lv_color_black()

#define SB_HEIGHT       36
#define SB_REFRESH_MS   2000

static lv_obj_t *s_bar    = NULL;
static lv_obj_t *s_lbl_title = NULL;
static lv_obj_t *s_lbl_time  = NULL;
static lv_obj_t *s_lbl_wifi  = NULL;
static lv_obj_t *s_lbl_batt  = NULL;
static lv_timer_t *s_refresh_timer = NULL;

void deck_bridge_ui_statusbar_refresh(void)
{
    if (!s_bar) return;

    /* Time. */
    if (s_lbl_time) {
        char tbuf[16] = "--:--";
        if (deck_sdi_time_wall_is_set()) {
            int64_t epoch = deck_sdi_time_wall_epoch_s();
            time_t t = (time_t)epoch;
            struct tm tm;
            localtime_r(&t, &tm);
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm.tm_hour, tm.tm_min);
        }
        lv_label_set_text(s_lbl_time, tbuf);
    }

    /* WiFi. */
    if (s_lbl_wifi) {
        const char *icon = "WIFI:--";
        char wbuf[16];
        deck_sdi_wifi_state_t st = deck_sdi_wifi_status();
        if (st == DECK_SDI_WIFI_CONNECTED) {
            int8_t r = deck_sdi_wifi_rssi();
            snprintf(wbuf, sizeof(wbuf), "WIFI:%ddBm", (int)r);
            icon = wbuf;
        } else if (st == DECK_SDI_WIFI_CONNECTING) {
            icon = "WIFI:CONN";
        }
        lv_label_set_text(s_lbl_wifi, icon);
    }

    /* Battery. */
    if (s_lbl_batt) {
        uint8_t pct = 0;
        char bbuf[16] = "BAT:--";
        if (deck_sdi_battery_read_pct(&pct) == DECK_SDI_OK) {
            snprintf(bbuf, sizeof(bbuf), "BAT:%u%%", pct);
        }
        lv_label_set_text(s_lbl_batt, bbuf);
    }
}

static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    deck_bridge_ui_statusbar_refresh();
}

deck_sdi_err_t deck_bridge_ui_statusbar_init(void)
{
    if (s_bar) return DECK_SDI_OK;
    if (!deck_bridge_ui_lock(200)) return DECK_SDI_ERR_BUSY;

    lv_obj_t *scr = lv_scr_act();
    s_bar = lv_obj_create(scr);
    lv_obj_set_size(s_bar, lv_pct(100), SB_HEIGHT);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_bar, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bar, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(s_bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_bar, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_bar, 4,  LV_PART_MAIN);
    lv_obj_set_flex_flow(s_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_title = lv_label_create(s_bar);
    lv_label_set_text(s_lbl_title, "CYBERDECK");
    lv_obj_set_style_text_color(s_lbl_title, CD_PRIMARY, LV_PART_MAIN);

    /* Right-side cluster: time | wifi | battery. */
    lv_obj_t *right = lv_obj_create(s_bar);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(right, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(right, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(right, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_lbl_time = lv_label_create(right);
    lv_obj_set_style_text_color(s_lbl_time, CD_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(s_lbl_time, "--:--");

    s_lbl_wifi = lv_label_create(right);
    lv_obj_set_style_text_color(s_lbl_wifi, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_label_set_text(s_lbl_wifi, "WIFI:--");

    s_lbl_batt = lv_label_create(right);
    lv_obj_set_style_text_color(s_lbl_batt, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_label_set_text(s_lbl_batt, "BAT:--");

    deck_bridge_ui_statusbar_refresh();
    if (!s_refresh_timer) {
        s_refresh_timer = lv_timer_create(refresh_timer_cb, SB_REFRESH_MS, NULL);
    }

    deck_bridge_ui_unlock();
    ESP_LOGI(TAG, "statusbar mounted (%dpx, refresh=%dms)", SB_HEIGHT, SB_REFRESH_MS);
    return DECK_SDI_OK;
}
