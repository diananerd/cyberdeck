/*
 * S3 Cyber-Deck — Status bar
 * Layout:
 *   LEFT:  Title polygon (parallelogram, primary fill, inverse/negative text)
 *            Shape: left/top/bottom edges flush, right side = 45° ascending diagonal
 *   RIGHT: [BT] [SD] [Battery] [Clock] [WiFi]  (flex row, right-aligned)
 */

#include "ui_statusbar.h"
#include "ui_navbar.h"
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

/* Border accent lines */
static lv_obj_t *s_top_line    = NULL;
static lv_obj_t *s_bot_line    = NULL;
static lv_obj_t *s_right_line  = NULL;

/* Cached icon states for theme refresh */
static bool    s_sd_mounted    = false;
static bool    s_bt_connected  = false;
static int     s_wifi_bars     = 0;     /* 0=disconnected, 1-4=signal level */
static bool    s_batt_charging = false;
static uint8_t s_batt_pct      = 0;

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

    /* 3. Inverse title text (bg_dark on primary = negative), drawn twice for bold */
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
        /* Draw twice with 1px horizontal offset for bold effect */
        lv_canvas_draw_text(title_canvas, 12, ty, max_w, &txt_dsc, title);
        lv_canvas_draw_text(title_canvas, 13, ty, max_w - 1, &txt_dsc, title);
    }
}

/* ========== Init ========== */

void ui_statusbar_init(void)
{
    const cyberdeck_theme_t *t = ui_theme_get();

    /* ---- Bar container on layer_top — full width in portrait,
     *       stops at navbar's left edge in landscape ---- */
    lv_disp_t  *s_disp  = lv_disp_get_default();
    lv_coord_t  hor_res = lv_disp_get_hor_res(s_disp);
    lv_coord_t  ver_res = lv_disp_get_ver_res(s_disp);
    bool        s_port  = (hor_res < ver_res);
    lv_coord_t  bar_w   = s_port ? hor_res : (hor_res - UI_NAVBAR_THICK);
    bar_obj = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bar_obj, bar_w, UI_STATUSBAR_HEIGHT);
    lv_obj_align(bar_obj, LV_ALIGN_TOP_LEFT, 0, 0);
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

    /* --- 2. Bluetooth — fixed-width wrapper so LV_ALIGN_CENTER has room --- */
    {
        lv_obj_t *bt_cont = lv_obj_create(right_cont);
        lv_obj_set_size(bt_cont, 18, UI_STATUSBAR_HEIGHT);
        lv_obj_set_style_bg_opa(bt_cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bt_cont, 0, 0);
        lv_obj_set_style_pad_all(bt_cont, 0, 0);
        lv_obj_clear_flag(bt_cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_layout(bt_cont, 0, 0);
        lbl_bluetooth = lv_label_create(bt_cont);
        lv_label_set_text(lbl_bluetooth, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_color(lbl_bluetooth, t->text_dim, 0);
        lv_obj_set_style_text_font(lbl_bluetooth, &lv_font_montserrat_18, 0);
        lv_obj_align(lbl_bluetooth, LV_ALIGN_CENTER, 0, 0);
    }

    /* --- 3. SD card — same fixed-width wrapper --- */
    {
        lv_obj_t *sd_cont = lv_obj_create(right_cont);
        lv_obj_set_size(sd_cont, 18, UI_STATUSBAR_HEIGHT);
        lv_obj_set_style_bg_opa(sd_cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sd_cont, 0, 0);
        lv_obj_set_style_pad_all(sd_cont, 0, 0);
        lv_obj_clear_flag(sd_cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_layout(sd_cont, 0, 0);
        lbl_sdcard = lv_label_create(sd_cont);
        lv_label_set_text(lbl_sdcard, LV_SYMBOL_SD_CARD);
        lv_obj_set_style_text_color(lbl_sdcard, t->text_dim, 0);
        lv_obj_set_style_text_font(lbl_sdcard, &lv_font_montserrat_18, 0);
        lv_obj_align(lbl_sdcard, LV_ALIGN_CENTER, 0, 0);
    }

    /* --- 4. WiFi bars — full bar height container, bars bottom-anchored at bar-8 ---
     * UI_STATUSBAR_HEIGHT=36, bar_base_y=28: tallest bar (h=20) → y=8..28, center y=18
     * Battery body (h=18) is LV_ALIGN_LEFT_MID → y=9..27, center y=18 — matches. */
    wifi_cont = lv_obj_create(right_cont);
    lv_obj_set_size(wifi_cont, 26, UI_STATUSBAR_HEIGHT);
    lv_obj_set_style_bg_opa(wifi_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_cont, 0, 0);
    lv_obj_set_style_pad_all(wifi_cont, 0, 0);
    lv_obj_clear_flag(wifi_cont, LV_OBJ_FLAG_SCROLLABLE);

    const lv_coord_t bw = 4, bgap = 2;
    const lv_coord_t bh[] = {6, 10, 16, 20};
    /* Bottom of bars at UI_STATUSBAR_HEIGHT-8 = 28; tallest bar centers at y=18 */
    const lv_coord_t bar_base_y = UI_STATUSBAR_HEIGHT - 8;
    for (int i = 0; i < 4; i++) {
        wifi_bars[i] = make_rect(wifi_cont, bw, bh[i], t->text_dim);
        lv_obj_set_pos(wifi_bars[i],
                       i * (bw + bgap),
                       bar_base_y - bh[i]);
    }

    /* --- 5. Battery (sub-container: body outline + fill + tip) — rightmost --- */
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

    /* ---- Border lines (drawn last = highest z-order, paint over all children) ---- */
    /* Top line */
    s_top_line = lv_obj_create(bar_obj);
    lv_obj_set_size(s_top_line, LV_PCT(100), 2);
    lv_obj_set_style_bg_color(s_top_line, t->primary, 0);
    lv_obj_set_style_bg_opa(s_top_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_top_line, 0, 0);
    lv_obj_set_style_radius(s_top_line, 0, 0);
    lv_obj_set_style_pad_all(s_top_line, 0, 0);
    lv_obj_clear_flag(s_top_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_top_line, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Bottom line */
    s_bot_line = lv_obj_create(bar_obj);
    lv_obj_set_size(s_bot_line, LV_PCT(100), 2);
    lv_obj_set_style_bg_color(s_bot_line, t->primary, 0);
    lv_obj_set_style_bg_opa(s_bot_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bot_line, 0, 0);
    lv_obj_set_style_radius(s_bot_line, 0, 0);
    lv_obj_set_style_pad_all(s_bot_line, 0, 0);
    lv_obj_clear_flag(s_bot_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_bot_line, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Right line — created always, visible in portrait only */
    s_right_line = lv_obj_create(bar_obj);
    lv_obj_set_size(s_right_line, 2, LV_PCT(100));
    lv_obj_set_style_bg_color(s_right_line, t->primary, 0);
    lv_obj_set_style_bg_opa(s_right_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_right_line, 0, 0);
    lv_obj_set_style_radius(s_right_line, 0, 0);
    lv_obj_set_style_pad_all(s_right_line, 0, 0);
    lv_obj_clear_flag(s_right_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_right_line, LV_ALIGN_TOP_RIGHT, 0, 0);
    if (!s_port) lv_obj_add_flag(s_right_line, LV_OBJ_FLAG_HIDDEN);

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

    s_wifi_bars = 0;
    if (connected) {
        if      (rssi > -50) s_wifi_bars = 4;
        else if (rssi > -60) s_wifi_bars = 3;
        else if (rssi > -70) s_wifi_bars = 2;
        else                 s_wifi_bars = 1;
    }

    for (int i = 0; i < 4; i++) {
        lv_color_t c = (i < s_wifi_bars) ? t->primary : t->text_dim;
        lv_obj_set_style_bg_color(wifi_bars[i], c, 0);
    }
}

void ui_statusbar_set_battery(uint8_t pct, bool charging)
{
    s_batt_pct      = pct;
    s_batt_charging = charging;
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
    s_sd_mounted = inserted;
    if (!lbl_sdcard) return;
    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_set_style_text_color(lbl_sdcard,
                                inserted ? t->text : t->text_dim, 0);
}

void ui_statusbar_set_bluetooth(bool connected)
{
    s_bt_connected = connected;
    if (!lbl_bluetooth) return;
    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_set_style_text_color(lbl_bluetooth,
                                connected ? t->primary : t->text_dim, 0);
}

void ui_statusbar_set_visible(bool visible)
{
    if (!bar_obj) return;
    if (visible) lv_obj_clear_flag(bar_obj, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(bar_obj,   LV_OBJ_FLAG_HIDDEN);
}

void ui_statusbar_refresh_theme(void)
{
    if (!bar_obj) return;
    const cyberdeck_theme_t *t = ui_theme_get();

    /* Recalculate width — display dimensions may have changed after rotation */
    lv_disp_t  *disp    = lv_disp_get_default();
    lv_coord_t  hor_res = lv_disp_get_hor_res(disp);
    lv_coord_t  ver_res = lv_disp_get_ver_res(disp);
    bool        port    = (hor_res < ver_res);
    lv_coord_t  bar_w   = port ? hor_res : (hor_res - UI_NAVBAR_THICK);
    lv_obj_set_width(bar_obj, bar_w);

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

    /* Border accent lines */
    if (s_top_line)   lv_obj_set_style_bg_color(s_top_line,   t->primary, 0);
    if (s_bot_line)   lv_obj_set_style_bg_color(s_bot_line,   t->primary, 0);
    if (s_right_line) {
        lv_obj_set_style_bg_color(s_right_line, t->primary, 0);
        if (port) lv_obj_clear_flag(s_right_line, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(s_right_line,   LV_OBJ_FLAG_HIDDEN);
    }

    /* WiFi bars — restore with cached signal level */
    for (int i = 0; i < 4; i++) {
        lv_color_t c = (i < s_wifi_bars) ? t->primary : t->text_dim;
        lv_obj_set_style_bg_color(wifi_bars[i], c, 0);
    }

    /* Battery — mirror the same logic as ui_statusbar_set_battery() */
    bool dc_power = (s_batt_pct == 0 && !s_batt_charging);
    if (dc_power) {
        /* DC power: bolt + primary color, no fill */
        lv_obj_set_style_border_color(batt_body, t->primary, 0);
        lv_obj_set_style_bg_color(batt_tip,  t->primary, 0);
        lv_obj_set_style_bg_color(batt_fill, t->primary, 0);
        lv_obj_clear_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(batt_bolt, t->primary, 0);
    } else if (s_batt_charging) {
        lv_obj_set_style_border_color(batt_body, t->primary,   0);
        lv_obj_set_style_bg_color(batt_tip,      t->primary,   0);
        lv_obj_clear_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(batt_bolt, t->bg_dark, 0);
        lv_color_t fill_c = (s_batt_pct > 20) ? t->primary : t->accent;
        lv_obj_set_style_bg_color(batt_fill, fill_c, 0);
    } else {
        lv_obj_set_style_border_color(batt_body, t->text_dim, 0);
        lv_obj_set_style_bg_color(batt_tip,      t->text_dim, 0);
        lv_obj_add_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);
        lv_color_t fill_c = (s_batt_pct > 20) ? t->primary : t->accent;
        lv_obj_set_style_bg_color(batt_fill, fill_c, 0);
    }

    /* Discrete icons — restore with cached states */
    lv_obj_set_style_text_color(lbl_bluetooth,
                                s_bt_connected ? t->primary : t->text_dim, 0);
    lv_obj_set_style_text_color(lbl_sdcard,
                                s_sd_mounted   ? t->text    : t->text_dim, 0);
}
