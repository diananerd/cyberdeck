/*
 * S3 Cyber-Deck — Settings > Audio
 * Volume control (stored in NVS; hardware effect depends on BT module).
 */

#include "app_settings.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "svc_settings.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "settings_audio";

typedef struct {
    lv_obj_t *vol_lbl;
    lv_obj_t *slider;
} audio_state_t;

static void slider_cb(lv_event_t *e)
{
    audio_state_t *s = (audio_state_t *)lv_event_get_user_data(e);
    if (!s) return;
    int32_t val = lv_slider_get_value(s->slider);
    char buf[16];
    snprintf(buf, sizeof(buf), "Volume: %ld%%", (long)val);
    lv_label_set_text(s->vol_lbl, buf);
}

static void save_btn_cb(lv_event_t *e)
{
    audio_state_t *s = (audio_state_t *)lv_event_get_user_data(e);
    if (!s) return;
    uint8_t vol = (uint8_t)lv_slider_get_value(s->slider);
    svc_settings_set_volume(vol);
    ui_effect_toast("Volume saved", 1200);
    ESP_LOGI(TAG, "Volume set to %d%%", vol);
}

static void audio_on_create(lv_obj_t *screen, void *intent_data)
{
    (void)intent_data;
    const cyberdeck_theme_t *t = ui_theme_get();

    audio_state_t *s = (audio_state_t *)lv_mem_alloc(sizeof(audio_state_t));
    if (!s) return;
    ui_activity_set_state(s);

    ui_statusbar_set_title("SETTINGS");

    lv_obj_t *content = ui_common_content_area(screen);

    /* Current volume */
    uint8_t cur_vol = 70;
    svc_settings_get_volume(&cur_vol);

    lv_obj_t *info = lv_label_create(content);
    lv_label_set_text(info, "Master volume");
    ui_theme_style_label(info, &CYBERDECK_FONT_MD);

    /* Volume label (updated by slider) */
    char buf[24];
    snprintf(buf, sizeof(buf), "Volume: %d%%", cur_vol);
    s->vol_lbl = lv_label_create(content);
    lv_label_set_text(s->vol_lbl, buf);
    ui_theme_style_label(s->vol_lbl, &CYBERDECK_FONT_LG);

    /* Slider */
    s->slider = lv_slider_create(content);
    lv_obj_set_width(s->slider, LV_PCT(90));
    lv_slider_set_range(s->slider, 0, 100);
    lv_slider_set_value(s->slider, cur_vol, LV_ANIM_OFF);

    /* Style the slider */
    lv_obj_set_style_bg_color(s->slider, t->primary_dim, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s->slider, t->primary, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s->slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s->slider, t->primary, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s->slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(s->slider, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s->slider, 2, LV_PART_KNOB);

    lv_obj_add_event_cb(s->slider, slider_cb, LV_EVENT_VALUE_CHANGED, s);

    ui_common_divider(content);

    /* Save button */
    lv_obj_t *save_btn = ui_common_btn_full(content, "Save");
    lv_obj_add_event_cb(save_btn, save_btn_cb, LV_EVENT_CLICKED, s);
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
