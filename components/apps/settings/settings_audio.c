/*
 * CyberDeck — Settings > Audio
 * Volume control — auto-saves on slider change, no button needed.
 */

#include "app_settings.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "svc_settings.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "settings_audio";

typedef struct {
    lv_obj_t *vol_val;   /* data-row value label: "50%" */
    lv_obj_t *slider;
} audio_state_t;

static void slider_cb(lv_event_t *e)
{
    audio_state_t *s = (audio_state_t *)lv_event_get_user_data(e);
    if (!s) return;
    int32_t val = lv_slider_get_value(s->slider);

    char buf[8];
    snprintf(buf, sizeof(buf), "%ld%%", (long)val);
    lv_label_set_text(s->vol_val, buf);

    svc_settings_set_volume((uint8_t)val);
    ESP_LOGI(TAG, "Volume: %ld%%", (long)val);
}

/* D1: returns state* */
static void *audio_on_create(lv_obj_t *screen, const view_args_t *args)
{
    (void)args;
    const cyberdeck_theme_t *t = ui_theme_get();

    audio_state_t *s = (audio_state_t *)lv_mem_alloc(sizeof(audio_state_t));
    if (!s) return NULL;

    uint8_t cur_vol = 50;
    svc_settings_get_volume(&cur_vol);

    ui_statusbar_set_title("SETTINGS");

    lv_obj_t *content = ui_common_content_area(screen);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", cur_vol);
    s->vol_val = ui_common_data_row(content, "MASTER VOLUME:", buf);

    s->slider = lv_slider_create(content);
    lv_obj_set_width(s->slider, LV_PCT(100));
    lv_slider_set_range(s->slider, 0, 100);
    lv_slider_set_value(s->slider, cur_vol, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(s->slider, t->primary_dim, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s->slider, t->primary, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s->slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s->slider, t->primary, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s->slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(s->slider, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s->slider, 2, LV_PART_KNOB);

    lv_obj_add_event_cb(s->slider, slider_cb, LV_EVENT_VALUE_CHANGED, s);

    return s;
}

static void audio_on_destroy(lv_obj_t *screen, void *state)
{
    (void)screen;
    lv_mem_free(state);
}

const activity_cbs_t settings_audio_cbs = {
    .on_create  = audio_on_create,
    .on_resume  = NULL,
    .on_pause   = NULL,
    .on_destroy = audio_on_destroy,
};
