/*
 * CyberDeck — Settings > Time
 * SNTP sync status, current time, timezone offset.
 *
 * Auto-syncs on create if WiFi is connected and time is not synced.
 * Layout: data rows + timezone stepper + [Sync][Save TZ] action row.
 */

#include "app_settings.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "svc_settings.h"
#include "svc_time.h"
#include "svc_wifi.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "settings_time";

typedef struct {
    int8_t      tz_offset;
    lv_obj_t   *tz_val;        /* data-row value label for timezone */
    lv_obj_t   *time_lbl;      /* large clock label — updated every second */
    lv_timer_t *clock_timer;
} time_state_t;

static void update_tz_val(time_state_t *s)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "UTC%+d:00", (int)s->tz_offset);
    lv_label_set_text(s->tz_val, buf);
}

static void tz_minus_cb(lv_event_t *e)
{
    time_state_t *s = (time_state_t *)lv_event_get_user_data(e);
    if (s->tz_offset > -12) { s->tz_offset--; update_tz_val(s); }
}

static void tz_plus_cb(lv_event_t *e)
{
    time_state_t *s = (time_state_t *)lv_event_get_user_data(e);
    if (s->tz_offset < 14) { s->tz_offset++; update_tz_val(s); }
}

static void clock_tick_cb(lv_timer_t *timer)
{
    time_state_t *s = (time_state_t *)timer->user_data;
    if (!s || !s->time_lbl) return;
    uint8_t h = 0, m = 0, sec = 0;
    svc_time_get_hms(&h, &m, &sec);
    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, sec);
    lv_label_set_text(s->time_lbl, buf);
}

static void save_btn_cb(lv_event_t *e)
{
    time_state_t *s = (time_state_t *)lv_event_get_user_data(e);
    svc_settings_set_tz_offset(s->tz_offset);
    ui_effect_toast("Timezone saved", 1200);
    ESP_LOGI(TAG, "TZ offset saved: %+d", (int)s->tz_offset);
}

static void sync_btn_cb(lv_event_t *e)
{
    (void)e;
    if (svc_time_sync() == ESP_OK) {
        ui_effect_toast("SNTP sync started", 1500);
    } else {
        ui_effect_toast("Sync failed - check WiFi", 2000);
    }
}

static void time_on_create(lv_obj_t *screen, void *intent_data)
{
    (void)intent_data;
    const cyberdeck_theme_t *t = ui_theme_get();

    time_state_t *s = (time_state_t *)lv_mem_alloc(sizeof(time_state_t));
    if (!s) return;
    ui_activity_set_state(s);

    int8_t saved_tz = 0;
    svc_settings_get_tz_offset(&saved_tz);
    s->tz_offset = saved_tz;

    ui_statusbar_set_title("SETTINGS");

    lv_obj_t *content = ui_common_content_area(screen);

    /* ---- Current time (large, primary) ---- */
    uint8_t h = 0, m = 0, sec = 0;
    svc_time_get_hms(&h, &m, &sec);
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", h, m, sec);

    lv_obj_t *time_lbl_key = lv_label_create(content);
    lv_label_set_text(time_lbl_key, "SYSTEM TIME:");
    ui_theme_style_label_dim(time_lbl_key, &CYBERDECK_FONT_SM);

    s->time_lbl = lv_label_create(content);
    lv_label_set_text(s->time_lbl, time_str);
    lv_obj_set_style_text_font(s->time_lbl, &CYBERDECK_FONT_XL, 0);
    lv_obj_set_style_text_color(s->time_lbl, t->primary, 0);

    /* Live clock — update every second like the statusbar */
    s->clock_timer = lv_timer_create(clock_tick_cb, 1000, s);

    /* Section gap: clock → sync status */
    ui_common_section_gap(content);

    /* ---- Sync status ---- */
    ui_common_data_row(content, "SNTP SYNC:",
                       svc_time_is_synced() ? "SYNCHRONIZED" : "NOT SYNCHRONIZED");

    /* Section gap: sync status → timezone stepper */
    ui_common_section_gap(content);

    /* ---- Timezone stepper ---- */
    lv_obj_t *tz_key = lv_label_create(content);
    lv_label_set_text(tz_key, "TIMEZONE:");
    ui_theme_style_label_dim(tz_key, &CYBERDECK_FONT_SM);

    lv_obj_t *tz_row = lv_obj_create(content);
    lv_obj_set_width(tz_row, LV_PCT(100));
    lv_obj_set_height(tz_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(tz_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tz_row, 0, 0);
    lv_obj_set_style_pad_all(tz_row, 0, 0);
    lv_obj_set_flex_flow(tz_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tz_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tz_row, 16, 0);
    lv_obj_clear_flag(tz_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *minus_btn = ui_common_btn(tz_row, "  -  ");
    lv_obj_add_event_cb(minus_btn, tz_minus_cb, LV_EVENT_CLICKED, s);

    s->tz_val = lv_label_create(tz_row);
    update_tz_val(s);
    lv_obj_set_style_text_font(s->tz_val, &CYBERDECK_FONT_MD, 0);
    lv_obj_set_style_text_color(s->tz_val, t->primary, 0);

    lv_obj_t *plus_btn = ui_common_btn(tz_row, "  +  ");
    lv_obj_add_event_cb(plus_btn, tz_plus_cb, LV_EVENT_CLICKED, s);

    /* ---- Auto-sync if WiFi connected and not yet synced ---- */
    if (!svc_time_is_synced() && svc_wifi_is_connected()) {
        svc_time_sync();
        ESP_LOGI(TAG, "Auto-SNTP sync triggered");
    }

    /* ---- Spacer + action row ---- */
    ui_common_spacer(content);

    lv_obj_t *btn_row = ui_common_action_row(content);

    lv_obj_t *sync_btn = ui_common_btn(btn_row, "Sync Now");
    lv_obj_add_event_cb(sync_btn, sync_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_btn = ui_common_btn(btn_row, "Save TZ");
    ui_common_btn_style_primary(save_btn);
    lv_obj_add_event_cb(save_btn, save_btn_cb, LV_EVENT_CLICKED, s);
}

static void time_on_destroy(lv_obj_t *screen, void *state)
{
    (void)screen;
    time_state_t *s = (time_state_t *)state;
    if (s && s->clock_timer) lv_timer_del(s->clock_timer);
    lv_mem_free(state);
}

const activity_cbs_t settings_time_cbs = {
    .on_create  = time_on_create,
    .on_resume  = NULL,
    .on_pause   = NULL,
    .on_destroy = time_on_destroy,
};
