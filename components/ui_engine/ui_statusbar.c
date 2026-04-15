/*
 * S3 Cyber-Deck — Status bar
 * Layout:
 *   LEFT:  Title polygon (parallelogram, primary fill, inverse/negative text)
 *            Shape: left/top/bottom edges flush, right side = 45° ascending diagonal
 *   RIGHT: [BT] [SD] [Battery] [Clock] [WiFi]  (flex row, right-aligned)
 */

#include "ui_statusbar.h"
#include "ui_theme.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "statusbar";

/* Canvas dimensions for the title polygon */
#define TITLE_POLY_W    200
#define TITLE_POLY_H    UI_STATUSBAR_HEIGHT   /* 36 */

/* Bar */
static lv_obj_t *bar_obj       = NULL;

/* Title polygon canvas */
static lv_obj_t      *title_canvas = NULL;
static lv_color_t     s_canvas_buf[TITLE_POLY_W * TITLE_POLY_H];
static char           s_title[64]  = "";   /* cached for theme refresh */

/* WiFi bars */
static lv_obj_t *wifi_cont    = NULL;
static lv_obj_t *wifi_bars[4] = {NULL};

/* Battery */
static lv_obj_t *batt_body    = NULL;
static lv_obj_t *batt_fill    = NULL;
static lv_obj_t *batt_tip     = NULL;
static lv_obj_t *lbl_batt_pct = NULL;
static lv_obj_t *batt_bolt    = NULL;

/* Discrete status icons */
static lv_obj_t *lbl_sdcard    = NULL;
static lv_obj_t *lbl_bluetooth = NULL;
static lv_obj_t *lbl_time      = NULL;

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

/*
 * Redraw the title polygon onto the canvas.
 * Parallelogram — 45° ascending diagonal on the right edge:
 *
 *   A=(0,0) ─────────────────── B=(W-1, 0)
 *   |                          ╱  ← 45° diagonal
 *   F=(0, H-1) ─────── E=(W-H, H-1)
 *
 * Fill = primary.  Text = bg_dark (inverse/negative).
 */
static void title_canvas_redraw(const char *title)
{
    if (!title_canvas) return;
    const cyberdeck_theme_t *t = ui_theme_get();
    const lv_coord_t W = TITLE_POLY_W;
    const lv_coord_t H = TITLE_POLY_H;

    /* 1. Clear to statusbar background */
    lv_canvas_fill_bg(title_canvas, t->bg_dark, LV_OPA_COVER);

    /* 2. Filled parallelogram (4 vertices) */
    lv_point_t pts[4] = {
        {0,     0    },   /* A: top-left          */
        {W - 1, 0    },   /* B: top-right         */
        {W - H, H - 1},   /* C: bottom of diagonal*/
        {0,     H - 1},   /* D: bottom-left       */
    };
    lv_draw_rect_dsc_t poly_dsc;
    lv_draw_rect_dsc_init(&poly_dsc);
    poly_dsc.bg_color     = t->primary;
    poly_dsc.bg_opa       = LV_OPA_COVER;
    poly_dsc.border_width = 0;
    poly_dsc.radius       = 0;
    lv_canvas_draw_polygon(title_canvas, pts, 4, &poly_dsc);

    /* 3. Inverse title text (bg_dark on primary = negative) */
    if (title && title[0]) {
        lv_draw_label_dsc_t txt_dsc;
        lv_draw_label_dsc_init(&txt_dsc);
        txt_dsc.color = t->bg_dark;
        txt_dsc.font  = &CYBERDECK_FONT_SM;
        /* Vertically center */
        lv_coord_t ty = (H - (lv_coord_t)CYBERDECK_FONT_SM.line_height) / 2;
        if (ty < 0) ty = 0;
        /* Safe width: at text mid-height (y=H/2) the diagonal is at x=(W-1)-(H/2).
         * Subtract left pad (12) and a small margin (4). */
        lv_coord_t max_w = (W - 1 - H / 2) - 12 - 4;
        lv_canvas_draw_text(title_canvas, 12, ty, max_w, &txt_dsc, title);
    }
}

/* ========== Init ========== */

void ui_statusbar_init(void)
{
    const cyberdeck_theme_t *t = ui_theme_get();

    /* ---- Bar container on layer_top ---- */
    bar_obj = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bar_obj, LV_PCT(100), UI_STATUSBAR_HEIGHT);
    lv_obj_align(bar_obj, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar_obj, t->bg_dark, 0);
    lv_obj_set_style_bg_opa(bar_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar_obj, 0, 0);   /* no border — avoids inner-area inset */
    lv_obj_set_style_radius(bar_obj, 0, 0);
    lv_obj_set_style_pad_all(bar_obj, 0, 0);
    lv_obj_clear_flag(bar_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_layout(bar_obj, 0, 0);  /* absolute positioning */

    /* ---- Title polygon (left, canvas-based) ---- */
    title_canvas = lv_canvas_create(bar_obj);
    lv_canvas_set_buffer(title_canvas, s_canvas_buf,
                         TITLE_POLY_W, TITLE_POLY_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(title_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(title_canvas, LV_OBJ_FLAG_CLICKABLE);
    title_canvas_redraw("");   /* populated by ui_statusbar_set_title() */

    /* ---- Right-side flex container ---- */
    /* Children added in visual left→right order: BT | SD | Battery | Clock | WiFi */
    lv_obj_t *right_cont = lv_obj_create(bar_obj);
    lv_obj_set_style_bg_opa(right_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_cont, 0, 0);
    lv_obj_set_style_pad_all(right_cont, 0, 0);
    lv_obj_set_style_pad_column(right_cont, 8, 0);   /* gap between icons */
    lv_obj_clear_flag(right_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(right_cont, UI_STATUSBAR_HEIGHT);
    lv_obj_set_width(right_cont, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_cont,
                          LV_FLEX_ALIGN_START,   /* main: pack left */
                          LV_FLEX_ALIGN_CENTER,  /* cross: center vertically */
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(right_cont, LV_ALIGN_RIGHT_MID, -6, 0);

    /* --- 1. Clock (leftmost in the group) --- */
    lbl_time = lv_label_create(right_cont);
    lv_label_set_text(lbl_time, "00:00:00");
    lv_obj_set_style_text_color(lbl_time, t->text, 0);
    lv_obj_set_style_text_font(lbl_time, &CYBERDECK_FONT_SM, 0);

    /* --- 2. Bluetooth --- */
    lbl_bluetooth = lv_label_create(right_cont);
    lv_label_set_text(lbl_bluetooth, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(lbl_bluetooth, t->text_dim, 0);  /* dim = off */
    lv_obj_set_style_text_font(lbl_bluetooth, &lv_font_montserrat_14, 0);

    /* --- 3. SD card --- */
    lbl_sdcard = lv_label_create(right_cont);
    lv_label_set_text(lbl_sdcard, LV_SYMBOL_SD_CARD);
    lv_obj_set_style_text_color(lbl_sdcard, t->text_dim, 0);     /* dim = no card */
    lv_obj_set_style_text_font(lbl_sdcard, &lv_font_montserrat_14, 0);

    /* --- 4. Battery (sub-container: body outline + fill + tip) --- */
    lv_obj_t *batt_cont = lv_obj_create(right_cont);
    lv_obj_set_size(batt_cont, 48, UI_STATUSBAR_HEIGHT);
    lv_obj_set_style_bg_opa(batt_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(batt_cont, 0, 0);
    lv_obj_set_style_pad_all(batt_cont, 0, 0);
    lv_obj_clear_flag(batt_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_layout(batt_cont, 0, 0);

    batt_body = lv_obj_create(batt_cont);
    lv_obj_set_size(batt_body, 44, 18);
    lv_obj_set_style_bg_opa(batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(batt_body, t->text_dim, 0);
    lv_obj_set_style_border_width(batt_body, 2, 0);
    lv_obj_set_style_border_opa(batt_body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(batt_body, 3, 0);
    lv_obj_set_style_pad_all(batt_body, 0, 0);
    lv_obj_clear_flag(batt_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(batt_body, LV_ALIGN_LEFT_MID, 0, 0);

    batt_tip = make_rect(batt_cont, 3, 8, t->text_dim);
    lv_obj_align(batt_tip, LV_ALIGN_LEFT_MID, 45, 0);

    batt_fill = make_rect(batt_body, 0, 12, t->primary);
    lv_obj_align(batt_fill, LV_ALIGN_LEFT_MID, 2, 0);

    lbl_batt_pct = lv_label_create(batt_body);
    lv_label_set_text(lbl_batt_pct, "");
    lv_obj_set_style_text_color(lbl_batt_pct, t->text, 0);
    lv_obj_set_style_text_font(lbl_batt_pct, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_batt_pct);

    batt_bolt = lv_label_create(batt_body);
    lv_label_set_text(batt_bolt, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(batt_bolt, t->primary, 0);
    lv_obj_set_style_text_font(batt_bolt, &lv_font_montserrat_14, 0);
    lv_obj_center(batt_bolt);
    lv_obj_add_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);

    /* --- 5. WiFi bars (4 rects of increasing height) --- */
    wifi_cont = lv_obj_create(right_cont);
    lv_obj_set_size(wifi_cont, 26, 24);
    lv_obj_set_style_bg_opa(wifi_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_cont, 0, 0);
    lv_obj_set_style_pad_all(wifi_cont, 0, 0);
    lv_obj_clear_flag(wifi_cont, LV_OBJ_FLAG_SCROLLABLE);

    const lv_coord_t bw = 4, bgap = 2;
    const lv_coord_t bh[] = {6, 10, 16, 22};
    for (int i = 0; i < 4; i++) {
        wifi_bars[i] = make_rect(wifi_cont, bw, bh[i], t->text_dim);
        lv_obj_align(wifi_bars[i], LV_ALIGN_BOTTOM_LEFT, i * (bw + bgap), 0);
    }

    /* ---- Border lines (drawn last = highest z-order, paint over all children) ---- */
    /* Top line */
    lv_obj_t *top_line = lv_obj_create(bar_obj);
    lv_obj_set_size(top_line, LV_PCT(100), 2);
    lv_obj_set_style_bg_color(top_line, t->primary, 0);
    lv_obj_set_style_bg_opa(top_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(top_line, 0, 0);
    lv_obj_set_style_radius(top_line, 0, 0);
    lv_obj_set_style_pad_all(top_line, 0, 0);
    lv_obj_clear_flag(top_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(top_line, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Bottom line */
    lv_obj_t *bot_line = lv_obj_create(bar_obj);
    lv_obj_set_size(bot_line, LV_PCT(100), 2);
    lv_obj_set_style_bg_color(bot_line, t->primary, 0);
    lv_obj_set_style_bg_opa(bot_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bot_line, 0, 0);
    lv_obj_set_style_radius(bot_line, 0, 0);
    lv_obj_set_style_pad_all(bot_line, 0, 0);
    lv_obj_clear_flag(bot_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bot_line, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Right line */
    lv_obj_t *right_line = lv_obj_create(bar_obj);
    lv_obj_set_size(right_line, 2, LV_PCT(100));
    lv_obj_set_style_bg_color(right_line, t->primary, 0);
    lv_obj_set_style_bg_opa(right_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(right_line, 0, 0);
    lv_obj_set_style_radius(right_line, 0, 0);
    lv_obj_set_style_pad_all(right_line, 0, 0);
    lv_obj_clear_flag(right_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(right_line, LV_ALIGN_TOP_RIGHT, 0, 0);

    ESP_LOGI(TAG, "Status bar initialized");
}

/* ========== Public setters ========== */

void ui_statusbar_set_title(const char *title)
{
    if (!title_canvas) return;
    strncpy(s_title, title ? title : "", sizeof(s_title) - 1);
    s_title[sizeof(s_title) - 1] = '\0';
    title_canvas_redraw(s_title);
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
        if      (rssi > -50) bars = 4;
        else if (rssi > -60) bars = 3;
        else if (rssi > -70) bars = 2;
        else                 bars = 1;
    }

    for (int i = 0; i < 4; i++) {
        lv_color_t c = (i < bars) ? t->primary : t->text_dim;
        lv_obj_set_style_bg_color(wifi_bars[i], c, 0);
    }
}

void ui_statusbar_set_battery(uint8_t pct, bool charging)
{
    if (!batt_body) return;
    const cyberdeck_theme_t *t = ui_theme_get();

    if (pct == 0 && !charging) {
        /* DC power — bolt only, no fill, no % */
        lv_obj_set_width(batt_fill, 0);
        lv_label_set_text(lbl_batt_pct, "");
        lv_obj_clear_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_color(batt_body, t->primary, 0);
        lv_obj_set_style_bg_color(batt_tip, t->primary, 0);
        lv_obj_set_style_text_color(batt_bolt, t->primary, 0);
        return;
    }

    lv_obj_add_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);

    lv_coord_t fill_w = (pct > 100 ? 100 : pct) * 38 / 100;
    if (fill_w < 1 && pct > 0) fill_w = 1;
    lv_obj_set_width(batt_fill, fill_w);

    lv_color_t fill_color = (pct > 20) ? t->primary : t->accent;
    lv_obj_set_style_bg_color(batt_fill, fill_color, 0);

    if (charging) {
        lv_obj_clear_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(batt_bolt, t->bg_dark, 0);
        lv_obj_set_style_border_color(batt_body, t->primary, 0);
        lv_obj_set_style_bg_color(batt_tip, t->primary, 0);
    } else {
        lv_obj_set_style_border_color(batt_body, t->text_dim, 0);
        lv_obj_set_style_bg_color(batt_tip, t->text_dim, 0);
    }

    char buf[6];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(lbl_batt_pct, buf);
    lv_obj_center(lbl_batt_pct);
}

void ui_statusbar_set_audio(bool playing)
{
    (void)playing;  /* Audio indicator removed from this layout */
}

void ui_statusbar_set_sdcard(bool inserted)
{
    if (!lbl_sdcard) return;
    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_set_style_text_color(lbl_sdcard,
                                inserted ? t->text : t->text_dim, 0);
}

void ui_statusbar_set_bluetooth(bool connected)
{
    if (!lbl_bluetooth) return;
    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_set_style_text_color(lbl_bluetooth,
                                connected ? t->primary : t->text_dim, 0);
}

void ui_statusbar_refresh_theme(void)
{
    if (!bar_obj) return;
    const cyberdeck_theme_t *t = ui_theme_get();

    lv_obj_set_style_bg_color(bar_obj, t->bg_dark, 0);
    lv_obj_set_style_border_color(bar_obj, t->primary, 0);

    /* Redraw title polygon with new palette */
    title_canvas_redraw(s_title);

    /* Time + battery text */
    lv_obj_set_style_text_color(lbl_time, t->text, 0);
    lv_obj_set_style_text_color(lbl_batt_pct, t->text, 0);
    lv_obj_set_style_border_color(batt_body, t->text_dim, 0);
    lv_obj_set_style_bg_color(batt_tip, t->text_dim, 0);
    lv_obj_set_style_bg_color(batt_fill, t->primary, 0);

    /* Icon defaults (caller should re-set actual state) */
    lv_obj_set_style_text_color(lbl_bluetooth, t->text_dim, 0);
    lv_obj_set_style_text_color(lbl_sdcard, t->text_dim, 0);
}
