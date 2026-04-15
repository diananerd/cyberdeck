/*
 * S3 Cyber-Deck — Settings > Time
 * Timezone offset and SNTP sync status.
 */

#include "app_settings.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "svc_settings.h"
#include "svc_time.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "settings_time";

typedef struct {
    int8_t    tz_offset;
    lv_obj_t *tz_lbl;
} time_state_t;

static void update_tz_label(time_state_t *s)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "UTC%+d:00", (int)s->tz_offset);
    lv_label_set_text(s->tz_lbl, buf);
}

static void tz_minus_cb(lv_event_t *e)
{
    time_state_t *s = (time_state_t *)lv_event_get_user_data(e);
    if (s->tz_offset > -12) {
        s->tz_offset--;
        update_tz_label(s);
    }
}

static void tz_plus_cb(lv_event_t *e)
{
    time_state_t *s = (time_state_t *)lv_event_get_user_data(e);
    if (s->tz_offset < 14) {
        s->tz_offset++;
        update_tz_label(s);
    }
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
        ui_effect_toast("Sync failed — check WiFi", 2000);
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

    /* Sync status */
    lv_obj_t *sync_lbl = lv_label_create(content);
    lv_label_set_text(sync_lbl, svc_time_is_synced()
                       ? "SNTP: Synchronized"
                       : "SNTP: Not synchronized");
    ui_theme_style_label(sync_lbl, &CYBERDECK_FONT_MD);

    /* Current time */
    uint8_t h = 0, m = 0, sec = 0;
    svc_time_get_hms(&h, &m, &sec);
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", h, m, sec);
    lv_obj_t *cur_lbl = lv_label_create(content);
    lv_label_set_text(cur_lbl, time_str);
    lv_obj_set_style_text_font(cur_lbl, &CYBERDECK_FONT_XL, 0);
    lv_obj_set_style_text_color(cur_lbl, t->primary, 0);

    ui_common_divider(content);

    /* Timezone row: [ - ] UTC+0:00 [ + ] */
    lv_obj_t *tz_row = lv_obj_create(content);
    lv_obj_set_width(tz_row, LV_PCT(100));
    lv_obj_set_height(tz_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(tz_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tz_row, 0, 0);
    lv_obj_set_flex_flow(tz_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tz_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(tz_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(tz_row, 0, 0);

    lv_obj_t *minus_btn = ui_common_btn(tz_row, "  -  ");
    lv_obj_add_event_cb(minus_btn, tz_minus_cb, LV_EVENT_CLICKED, s);

    s->tz_lbl = lv_label_create(tz_row);
    update_tz_label(s);
    lv_obj_set_style_text_font(s->tz_lbl, &CYBERDECK_FONT_MD, 0);
    lv_obj_set_style_text_color(s->tz_lbl, t->primary, 0);

    lv_obj_t *plus_btn = ui_common_btn(tz_row, "  +  ");
    lv_obj_add_event_cb(plus_btn, tz_plus_cb, LV_EVENT_CLICKED, s);

    /* Hint */
    lv_obj_t *hint = lv_label_create(content);
    lv_label_set_text(hint, "Timezone offset from UTC");
    ui_theme_style_label_dim(hint, &CYBERDECK_FONT_SM);

    ui_common_divider(content);

    lv_obj_t *save_btn = ui_common_btn_full(content, "Save Timezone");
    lv_obj_add_event_cb(save_btn, save_btn_cb, LV_EVENT_CLICKED, s);

    lv_obj_t *sync_btn = ui_common_btn_full(content, "Sync Time Now");
    lv_obj_add_event_cb(sync_btn, sync_btn_cb, LV_EVENT_CLICKED, NULL);
}

static void time_on_destroy(lv_obj_t *screen, void *state)
{
    (void)screen;
    lv_mem_free(state);
}

const activity_cbs_t settings_time_cbs = {
    .on_create  = time_on_create,
    .on_resume  = NULL,
    .on_pause   = NULL,
    .on_destroy = time_on_destroy,
};
