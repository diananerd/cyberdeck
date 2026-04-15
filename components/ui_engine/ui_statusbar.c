/*
 * S3 Cyber-Deck — Status bar
 * 36px top bar on lv_layer_top() with graphical indicators.
 */

#include "ui_statusbar.h"
#include "ui_theme.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "statusbar";

static lv_obj_t *bar_obj     = NULL;
static lv_obj_t *lbl_time    = NULL;
static lv_obj_t *lbl_title   = NULL;

/* WiFi indicator (icon + 4 signal bars) */
static lv_obj_t *wifi_cont   = NULL;
static lv_obj_t *wifi_icon   = NULL;
static lv_obj_t *wifi_bars[4] = {NULL};

/* Battery icon (outline body + fill + % label inside + bolt) */
static lv_obj_t *batt_body   = NULL;
static lv_obj_t *batt_fill   = NULL;
static lv_obj_t *batt_tip    = NULL;
static lv_obj_t *lbl_batt_pct = NULL;
static lv_obj_t *batt_bolt   = NULL;   /* lightning bolt symbol (DC/charging) */

/* Audio indicator (3 equalizer bars) */
static lv_obj_t *audio_cont  = NULL;
static lv_obj_t *audio_bar1  = NULL;
static lv_obj_t *audio_bar2  = NULL;
static lv_obj_t *audio_bar3  = NULL;

/* ========== Helpers ========== */

static lv_obj_t *make_rect(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_color_t color)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, color, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 1, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

void ui_statusbar_init(void)
{
    const cyberdeck_theme_t *t = ui_theme_get();

    /* Container on layer_top so it sits above all screens */
    bar_obj = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bar_obj, LV_PCT(100), UI_STATUSBAR_HEIGHT);
    lv_obj_align(bar_obj, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar_obj, t->bg_dark, 0);
    lv_obj_set_style_bg_opa(bar_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar_obj, 2, 0);
    lv_obj_set_style_border_side(bar_obj, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar_obj, t->primary, 0);
    lv_obj_set_style_radius(bar_obj, 0, 0);
    lv_obj_set_style_pad_hor(bar_obj, 10, 0);
    lv_obj_set_style_pad_ver(bar_obj, 0, 0);
    lv_obj_clear_flag(bar_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_layout(bar_obj, 0, 0); /* no layout */

    /* ---- WiFi icon + signal bars — far left ---- */
    wifi_cont = lv_obj_create(bar_obj);
    lv_obj_set_size(wifi_cont, 46, 24);
    lv_obj_set_style_bg_opa(wifi_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_cont, 0, 0);
    lv_obj_set_style_pad_all(wifi_cont, 0, 0);
    lv_obj_clear_flag(wifi_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(wifi_cont, LV_ALIGN_LEFT_MID, 4, 0);

    /* WiFi icon (Font Awesome) */
    wifi_icon = lv_label_create(wifi_cont);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi_icon, t->text_dim, 0);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_14, 0);
    lv_obj_align(wifi_icon, LV_ALIGN_LEFT_MID, 0, 0);

    /* 4 filled bars: widths 4px, gap 2px, heights 6/10/16/22 */
    const lv_coord_t bw = 4, bg = 2;
    const lv_coord_t bh[] = {6, 10, 16, 22};
    const lv_coord_t bars_x0 = 18; /* offset after icon */
    for (int i = 0; i < 4; i++) {
        wifi_bars[i] = make_rect(wifi_cont, bw, bh[i], t->text_dim);
        lv_obj_align(wifi_bars[i], LV_ALIGN_BOTTOM_LEFT, bars_x0 + i * (bw + bg), 0);
    }

    /* ---- Battery icon — left, after WiFi ---- */
    /* Body: outlined rectangle 44x18 (big enough for % text inside) */
    batt_body = lv_obj_create(bar_obj);
    lv_obj_set_size(batt_body, 44, 18);
    lv_obj_set_style_bg_opa(batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(batt_body, t->text_dim, 0);
    lv_obj_set_style_border_width(batt_body, 2, 0);
    lv_obj_set_style_border_opa(batt_body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(batt_body, 3, 0);
    lv_obj_set_style_pad_all(batt_body, 0, 0);
    lv_obj_clear_flag(batt_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align_to(batt_body, wifi_cont, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    /* Tip: small rectangle on right side of body */
    batt_tip = make_rect(bar_obj, 3, 8, t->text_dim);
    lv_obj_align_to(batt_tip, batt_body, LV_ALIGN_OUT_RIGHT_MID, 1, 0);

    /* Fill bar inside body (max 38px, starts empty) */
    batt_fill = make_rect(batt_body, 0, 12, t->primary);
    lv_obj_align(batt_fill, LV_ALIGN_LEFT_MID, 2, 0);

    /* Percentage label inside battery body */
    lbl_batt_pct = lv_label_create(batt_body);
    lv_label_set_text(lbl_batt_pct, "");
    lv_obj_set_style_text_color(lbl_batt_pct, t->text, 0);
    lv_obj_set_style_text_font(lbl_batt_pct, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_batt_pct);

    /* Lightning bolt symbol inside battery (for DC power / charging) */
    batt_bolt = lv_label_create(batt_body);
    lv_label_set_text(batt_bolt, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(batt_bolt, t->primary, 0);
    lv_obj_set_style_text_font(batt_bolt, &lv_font_montserrat_14, 0);
    lv_obj_center(batt_bolt);
    lv_obj_add_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);

    /* ---- Title — center ---- */
    lbl_title = lv_label_create(bar_obj);
    lv_label_set_text(lbl_title, "");
    lv_obj_set_style_text_color(lbl_title, t->text, 0);
    lv_obj_set_style_text_font(lbl_title, &CYBERDECK_FONT_SM, 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

    /* ---- Audio indicator — 3 equalizer bars ---- */
    audio_cont = lv_obj_create(bar_obj);
    lv_obj_set_size(audio_cont, 18, 20);
    lv_obj_set_style_bg_opa(audio_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(audio_cont, 0, 0);
    lv_obj_set_style_pad_all(audio_cont, 0, 0);
    lv_obj_clear_flag(audio_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(audio_cont, LV_ALIGN_RIGHT_MID, -110, 0);
    lv_obj_add_flag(audio_cont, LV_OBJ_FLAG_HIDDEN);

    audio_bar1 = make_rect(audio_cont, 4, 12, t->primary);
    lv_obj_align(audio_bar1, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    audio_bar2 = make_rect(audio_cont, 4, 18, t->primary);
    lv_obj_align(audio_bar2, LV_ALIGN_BOTTOM_LEFT, 6, 0);
    audio_bar3 = make_rect(audio_cont, 4, 8, t->primary);
    lv_obj_align(audio_bar3, LV_ALIGN_BOTTOM_LEFT, 12, 0);

    /* ---- Time — far right ---- */
    lbl_time = lv_label_create(bar_obj);
    lv_label_set_text(lbl_time, "00:00:00");
    lv_obj_set_style_text_color(lbl_time, t->text, 0);
    lv_obj_set_style_text_font(lbl_time, &CYBERDECK_FONT_SM, 0);
    lv_obj_align(lbl_time, LV_ALIGN_RIGHT_MID, -4, 0);

    ESP_LOGI(TAG, "Status bar initialized");
}

void ui_statusbar_set_time(uint8_t hour, uint8_t minute, uint8_t second)
{
    if (!lbl_time) return;
    char buf[12];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
             (unsigned)(hour % 24), (unsigned)(minute % 60), (unsigned)(second % 60));
    lv_label_set_text(lbl_time, buf);
}

void ui_statusbar_set_wifi(bool connected, int8_t rssi)
{
    if (!wifi_cont) return;
    const cyberdeck_theme_t *t = ui_theme_get();

    int bars = 0;
    if (connected) {
        if (rssi > -50) bars = 4;
        else if (rssi > -60) bars = 3;
        else if (rssi > -70) bars = 2;
        else bars = 1;
    }

    for (int i = 0; i < 4; i++) {
        lv_color_t c = (i < bars) ? t->primary : t->text_dim;
        lv_obj_set_style_bg_color(wifi_bars[i], c, 0);
    }
    lv_obj_set_style_text_color(wifi_icon, connected ? t->primary : t->text_dim, 0);
}

void ui_statusbar_set_battery(uint8_t pct, bool charging)
{
    if (!batt_body) return;
    const cyberdeck_theme_t *t = ui_theme_get();

    if (pct == 0 && !charging) {
        /* No battery detected — show DC power (bolt, no fill, no %) */
        lv_obj_set_width(batt_fill, 0);
        lv_label_set_text(lbl_batt_pct, "");
        lv_obj_clear_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_color(batt_body, t->primary, 0);
        lv_obj_set_style_bg_color(batt_tip, t->primary, 0);
        lv_obj_set_style_text_color(batt_bolt, t->primary, 0);
        return;
    }

    /* Hide bolt when on battery */
    lv_obj_add_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);

    /* Fill bar: max 38px width */
    lv_coord_t fill_w = (pct > 100 ? 100 : pct) * 38 / 100;
    if (fill_w < 1 && pct > 0) fill_w = 1;
    lv_obj_set_width(batt_fill, fill_w);

    /* Color based on level */
    lv_color_t fill_color = (pct > 20) ? t->primary : t->accent;
    lv_obj_set_style_bg_color(batt_fill, fill_color, 0);

    /* Charging: show bolt over the fill + bright border */
    if (charging) {
        lv_obj_clear_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(batt_bolt, t->bg_dark, 0);
        lv_obj_set_style_border_color(batt_body, t->primary, 0);
        lv_obj_set_style_bg_color(batt_tip, t->primary, 0);
    } else {
        lv_obj_set_style_border_color(batt_body, t->text_dim, 0);
        lv_obj_set_style_bg_color(batt_tip, t->text_dim, 0);
    }

    /* Percentage text inside */
    char buf[6];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(lbl_batt_pct, buf);
    lv_obj_center(lbl_batt_pct);
}

void ui_statusbar_set_audio(bool playing)
{
    if (!audio_cont) return;
    if (playing) {
        lv_obj_clear_flag(audio_cont, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(audio_cont, LV_OBJ_FLAG_HIDDEN);
    }
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
    lv_obj_set_style_text_color(lbl_batt_pct, t->text, 0);
    lv_obj_set_style_border_color(batt_body, t->text_dim, 0);
    lv_obj_set_style_bg_color(batt_tip, t->text_dim, 0);
}
