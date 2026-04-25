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
#include <string.h>

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

    /* Statusbar lives on lv_layer_top so it survives activity screen
     * swaps + lv_obj_clean cycles in DVC render. Toasts and dialogs
     * also live on lv_layer_top — they layer above by virtue of being
     * created later (LVGL z-order = creation order within parent). */
    lv_obj_t *layer = lv_layer_top();
    s_bar = lv_obj_create(layer);
    lv_obj_set_size(s_bar, lv_disp_get_hor_res(NULL), SB_HEIGHT);
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

void deck_bridge_ui_statusbar_relayout(void)
{
    /* Re-size + re-align to the new display dimensions after a rotation.
     * Must run with the UI lock already held (caller's responsibility). */
    if (!s_bar) return;
    lv_obj_set_size(s_bar, lv_disp_get_hor_res(NULL), SB_HEIGHT);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 0);
}

void deck_bridge_ui_statusbar_set_visible(bool visible)
{
    if (!s_bar) return;
    if (!deck_bridge_ui_lock(200)) return;
    if (visible) lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    deck_bridge_ui_unlock();
}

/* J7 — recolor docks on theme atom change. Unknown atoms fall back
 * to green. Triggers no re-layout, just hue swap. */
static lv_color_t theme_primary(const char *atom)
{
    if (atom && !strcmp(atom, "amber")) return lv_color_hex(0xFFB000);
    if (atom && !strcmp(atom, "neon"))  return lv_color_hex(0xFF00FF);
    return lv_color_hex(0x00FF41);
}
static lv_color_t theme_dim(const char *atom)
{
    if (atom && !strcmp(atom, "amber")) return lv_color_hex(0x4D3500);
    if (atom && !strcmp(atom, "neon"))  return lv_color_hex(0x500050);
    return lv_color_hex(0x004D13);
}

void deck_bridge_ui_statusbar_apply_theme(const char *atom)
{
    if (!s_bar) return;
    if (!deck_bridge_ui_lock(200)) return;
    lv_color_t prim = theme_primary(atom);
    lv_color_t dim  = theme_dim(atom);
    lv_obj_set_style_border_color(s_bar, dim, LV_PART_MAIN);
    if (s_lbl_title) lv_obj_set_style_text_color(s_lbl_title, prim, LV_PART_MAIN);
    if (s_lbl_time)  lv_obj_set_style_text_color(s_lbl_time,  prim, LV_PART_MAIN);
    if (s_lbl_wifi)  lv_obj_set_style_text_color(s_lbl_wifi,  prim, LV_PART_MAIN);
    if (s_lbl_batt)  lv_obj_set_style_text_color(s_lbl_batt,  prim, LV_PART_MAIN);
    lv_obj_invalidate(s_bar);
    deck_bridge_ui_unlock();
    ESP_LOGI(TAG, "statusbar: theme → %s", atom ? atom : "?");
}

/* J4 — per-app badge pills. Up to 4 concurrent. */
#define BADGE_MAX 4
typedef struct {
    char        app_id[32];
    int         count;
    lv_obj_t   *pill;
} badge_slot_t;
static badge_slot_t s_badges[BADGE_MAX];

static badge_slot_t *find_badge(const char *app_id, bool make)
{
    if (!app_id) return NULL;
    badge_slot_t *empty = NULL;
    for (int i = 0; i < BADGE_MAX; i++) {
        if (s_badges[i].app_id[0] == '\0') {
            if (!empty) empty = &s_badges[i];
            continue;
        }
        if (strcmp(s_badges[i].app_id, app_id) == 0) return &s_badges[i];
    }
    if (!make || !empty) return NULL;
    strncpy(empty->app_id, app_id, sizeof(empty->app_id) - 1);
    empty->app_id[sizeof(empty->app_id) - 1] = '\0';
    return empty;
}

void deck_bridge_ui_statusbar_set_badge(const char *app_id, int count)
{
    if (!s_bar || !app_id) return;
    if (!deck_bridge_ui_lock(200)) return;
    badge_slot_t *b = find_badge(app_id, count > 0);
    if (!b) { deck_bridge_ui_unlock(); return; }
    if (count <= 0) {
        if (b->pill) { lv_obj_del(b->pill); b->pill = NULL; }
        b->app_id[0] = '\0';
        b->count = 0;
        deck_bridge_ui_unlock();
        return;
    }
    b->count = count;
    if (!b->pill) {
        /* Create a small bg-tinted pill with a numeric label. Lives in
         * the statusbar's right-side row container (last child). */
        uint32_t cnt = lv_obj_get_child_cnt(s_bar);
        lv_obj_t *right = cnt >= 2 ? lv_obj_get_child(s_bar, cnt - 1) : s_bar;
        b->pill = lv_obj_create(right);
        lv_obj_set_size(b->pill, LV_SIZE_CONTENT, 22);
        lv_obj_set_style_bg_color(b->pill, lv_color_hex(0x004D13), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b->pill, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(b->pill, lv_color_hex(0x00FF41), LV_PART_MAIN);
        lv_obj_set_style_border_width(b->pill, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(b->pill, 11, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(b->pill, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(b->pill, 0, LV_PART_MAIN);
        lv_obj_t *l = lv_label_create(b->pill);
        lv_obj_set_style_text_color(l, lv_color_hex(0x00FF41), LV_PART_MAIN);
        lv_obj_center(l);
    }
    if (b->pill && lv_obj_get_child_cnt(b->pill) > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", count);
        lv_label_set_text(lv_obj_get_child(b->pill, 0), buf);
    }
    deck_bridge_ui_unlock();
    ESP_LOGI(TAG, "badge: %s = %d", app_id, count);
}
