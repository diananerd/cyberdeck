/*
 * S3 Cyber-Deck — Status bar
 * 20px top bar on lv_layer_top() with time, WiFi, battery, audio, and app title.
 */

#include "ui_statusbar.h"
#include "ui_theme.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "statusbar";

static lv_obj_t *bar_obj    = NULL;
static lv_obj_t *lbl_time   = NULL;
static lv_obj_t *lbl_wifi   = NULL;
static lv_obj_t *lbl_batt   = NULL;
static lv_obj_t *lbl_audio  = NULL;
static lv_obj_t *lbl_title  = NULL;

void ui_statusbar_init(void)
{
    const cyberdeck_theme_t *t = ui_theme_get();

    /* Container on layer_top so it sits above all screens */
    bar_obj = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bar_obj, LV_PCT(100), UI_STATUSBAR_HEIGHT);
    lv_obj_align(bar_obj, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar_obj, t->bg_dark, 0);
    lv_obj_set_style_bg_opa(bar_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar_obj, 1, 0);
    lv_obj_set_style_border_side(bar_obj, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar_obj, t->primary, 0);
    lv_obj_set_style_radius(bar_obj, 0, 0);
    lv_obj_set_style_pad_hor(bar_obj, 4, 0);
    lv_obj_set_style_pad_ver(bar_obj, 0, 0);
    lv_obj_clear_flag(bar_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_layout(bar_obj, 0, 0); /* no layout */

    /* Time — left side */
    lbl_time = lv_label_create(bar_obj);
    lv_label_set_text(lbl_time, "00:00");
    lv_obj_set_style_text_color(lbl_time, t->text, 0);
    lv_obj_set_style_text_font(lbl_time, &CYBERDECK_FONT_SM, 0);
    lv_obj_align(lbl_time, LV_ALIGN_LEFT_MID, 2, 0);

    /* WiFi indicator — next to time */
    lbl_wifi = lv_label_create(bar_obj);
    lv_label_set_text(lbl_wifi, "");  /* hidden until state set */
    lv_obj_set_style_text_color(lbl_wifi, t->text_dim, 0);
    lv_obj_set_style_text_font(lbl_wifi, &CYBERDECK_FONT_SM, 0);
    lv_obj_align_to(lbl_wifi, lbl_time, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    /* Title — center */
    lbl_title = lv_label_create(bar_obj);
    lv_label_set_text(lbl_title, "");
    lv_obj_set_style_text_color(lbl_title, t->text, 0);
    lv_obj_set_style_text_font(lbl_title, &CYBERDECK_FONT_SM, 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

    /* Audio indicator — right side */
    lbl_audio = lv_label_create(bar_obj);
    lv_label_set_text(lbl_audio, "");
    lv_obj_set_style_text_color(lbl_audio, t->text, 0);
    lv_obj_set_style_text_font(lbl_audio, &CYBERDECK_FONT_SM, 0);
    lv_obj_align(lbl_audio, LV_ALIGN_RIGHT_MID, -50, 0);

    /* Battery — far right */
    lbl_batt = lv_label_create(bar_obj);
    lv_label_set_text(lbl_batt, "---");
    lv_obj_set_style_text_color(lbl_batt, t->text_dim, 0);
    lv_obj_set_style_text_font(lbl_batt, &CYBERDECK_FONT_SM, 0);
    lv_obj_align(lbl_batt, LV_ALIGN_RIGHT_MID, -2, 0);

    ESP_LOGI(TAG, "Status bar initialized");
}

void ui_statusbar_set_time(uint8_t hour, uint8_t minute)
{
    if (!lbl_time) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(hour % 24), (unsigned)(minute % 60));
    lv_label_set_text(lbl_time, buf);
}

void ui_statusbar_set_wifi(bool connected, int8_t rssi)
{
    if (!lbl_wifi) return;
    const cyberdeck_theme_t *t = ui_theme_get();
    if (!connected) {
        lv_label_set_text(lbl_wifi, "W:--");
        lv_obj_set_style_text_color(lbl_wifi, t->text_dim, 0);
    } else {
        /* Simple bar indicator */
        int bars = 0;
        if (rssi > -50) bars = 4;
        else if (rssi > -60) bars = 3;
        else if (rssi > -70) bars = 2;
        else bars = 1;
        char buf[8];
        snprintf(buf, sizeof(buf), "W:%d", bars);
        lv_label_set_text(lbl_wifi, buf);
        lv_obj_set_style_text_color(lbl_wifi, t->text, 0);
    }
}

void ui_statusbar_set_battery(uint8_t pct, bool charging)
{
    if (!lbl_batt) return;
    char buf[8];
    if (charging) {
        snprintf(buf, sizeof(buf), "%d%%+", pct);
    } else {
        snprintf(buf, sizeof(buf), "%d%%", pct);
    }
    lv_label_set_text(lbl_batt, buf);
}

void ui_statusbar_set_audio(bool playing)
{
    if (!lbl_audio) return;
    lv_label_set_text(lbl_audio, playing ? ">>>" : "");
}

void ui_statusbar_set_title(const char *title)
{
    if (!lbl_title) return;
    lv_label_set_text(lbl_title, title ? title : "");
}

void ui_statusbar_refresh_theme(void)
{
    if (!bar_obj) return;
    const cyberdeck_theme_t *t = ui_theme_get();

    lv_obj_set_style_bg_color(bar_obj, t->bg_dark, 0);
    lv_obj_set_style_border_color(bar_obj, t->primary, 0);

    lv_obj_set_style_text_color(lbl_time, t->text, 0);
    lv_obj_set_style_text_color(lbl_title, t->text, 0);
    lv_obj_set_style_text_color(lbl_audio, t->text, 0);
    lv_obj_set_style_text_color(lbl_batt, t->text_dim, 0);
    /* WiFi color depends on connected state, leave as-is */
}
