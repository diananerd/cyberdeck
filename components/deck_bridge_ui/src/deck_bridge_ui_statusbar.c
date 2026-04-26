/* deck_bridge_ui_statusbar — top dock with parallelogram title + icons.
 *
 * Pre-deck-lang TUI design ported to the bridge.ui driver:
 *
 *   ╭───────────╮  ─────────────────────────────────────────  ╮
 *   │ CYBERDECK │              [BT] [SD] [WIFI] [BATT]   ────│ 2px primary border
 *   ╰─╲─────────╯  ─────────────────────────────────────────  ╯
 *      ╲ 45° diagonal
 *
 * Title is drawn into an LVGL canvas as a filled parallelogram with
 * inverse (negative) text. Right cluster uses fixed-width per-icon
 * containers so cross-axis centering is stable. Borders:
 *   top + bottom: 2px primary
 *   right:        2px primary in portrait only (in landscape the
 *                 navbar abuts the right edge so no border is needed).
 *
 * Refresh on a 2 s timer; data sourced from the SDI drivers
 * (battery / wifi / time) plus hal_sdcard for SD presence. BT is
 * stubbed off (no BT module on the reference board).
 */

#include "deck_bridge_ui.h"
#include "deck_bridge_ui_internal.h"

#include "drivers/deck_sdi_battery.h"
#include "drivers/deck_sdi_wifi.h"
#include "drivers/deck_sdi_time.h"
#include "hal_sdcard.h"

#include "lvgl.h"
#include "esp_log.h"

#include <time.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "bridge_ui.sb";

/* Theme palette — three atoms (green/amber/neon). Filled at runtime via
 * pal_set() because LV_COLOR_MAKE / lv_color_hex are not constexpr in
 * LVGL 8.x and cannot initialise file-scope statics. */
typedef struct {
    lv_color_t primary;
    lv_color_t primary_dim;
    lv_color_t text_dim;
    lv_color_t accent;
    lv_color_t bg_dark;
} sb_palette_t;

static sb_palette_t s_pal;

static void pal_set(sb_palette_t *p, uint32_t prim, uint32_t dim,
                     uint32_t txtdim, uint32_t accent)
{
    p->primary     = lv_color_hex(prim);
    p->primary_dim = lv_color_hex(dim);
    p->text_dim    = lv_color_hex(txtdim);
    p->accent      = lv_color_hex(accent);
    p->bg_dark     = lv_color_black();
}

static void pal_apply_atom(const char *atom)
{
    if      (atom && !strcmp(atom, "amber")) pal_set(&s_pal, 0xFFB000, 0x4D3500, 0x805800, 0xFF3300);
    else if (atom && !strcmp(atom, "neon"))  pal_set(&s_pal, 0xFF00FF, 0x500050, 0x800080, 0xFF0055);
    else                                      pal_set(&s_pal, 0x00FF41, 0x004D13, 0x008020, 0xFF0055);
}

#define SB_HEIGHT       36
#define SB_REFRESH_MS   2000
#define TITLE_POLY_W    220
#define TITLE_POLY_H    SB_HEIGHT
#define NB_THICK        72      /* match navbar thickness (pre-deck-lang) */

static lv_obj_t   *s_bar         = NULL;
static lv_obj_t   *s_title_cv    = NULL;
static lv_color_t  s_title_buf[TITLE_POLY_W * TITLE_POLY_H];
static char        s_title[32]   = "CYBERDECK";

static lv_obj_t *s_lbl_time   = NULL;
static lv_obj_t *s_lbl_bt     = NULL;
static lv_obj_t *s_lbl_sd     = NULL;
static lv_obj_t *s_wifi_cont  = NULL;
static lv_obj_t *s_wifi_bars[4] = { NULL };

static lv_obj_t *s_batt_body  = NULL;
static lv_obj_t *s_batt_tip   = NULL;
static lv_obj_t *s_batt_fill  = NULL;
static lv_obj_t *s_batt_pct_l = NULL;
static lv_obj_t *s_batt_bolt  = NULL;

static lv_obj_t *s_top_line   = NULL;
static lv_obj_t *s_bot_line   = NULL;
static lv_obj_t *s_right_line = NULL;

static lv_timer_t *s_refresh_timer = NULL;

static int  s_wifi_level = 0;
static bool s_charging   = false;
static int  s_pct        = 0;
static bool s_sd_present = false;

/* ---- Helpers ---- */

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

static void title_canvas_redraw(void)
{
    if (!s_title_cv) return;
    const lv_coord_t W = TITLE_POLY_W;
    const lv_coord_t H = TITLE_POLY_H;

    lv_canvas_fill_bg(s_title_cv, s_pal.bg_dark, LV_OPA_COVER);

    lv_point_t pts[4] = {
        { 0,     0     },
        { W - 1, 0     },
        { W - H, H - 1 },
        { 0,     H - 1 },
    };
    lv_draw_rect_dsc_t poly_dsc;
    lv_draw_rect_dsc_init(&poly_dsc);
    poly_dsc.bg_color     = s_pal.primary;
    poly_dsc.bg_opa       = LV_OPA_COVER;
    poly_dsc.border_width = 0;
    poly_dsc.radius       = 0;
    lv_canvas_draw_polygon(s_title_cv, pts, 4, &poly_dsc);

    if (s_title[0]) {
        lv_draw_label_dsc_t txt_dsc;
        lv_draw_label_dsc_init(&txt_dsc);
        txt_dsc.color = s_pal.bg_dark;
        txt_dsc.font  = &lv_font_montserrat_18;
        lv_coord_t ty = (H - (lv_coord_t)lv_font_montserrat_18.line_height) / 2;
        if (ty < 0) ty = 0;
        lv_coord_t max_w = (W - 1 - H / 2) - 12 - 4;
        lv_canvas_draw_text(s_title_cv, 12, ty, max_w,     &txt_dsc, s_title);
        lv_canvas_draw_text(s_title_cv, 13, ty, max_w - 1, &txt_dsc, s_title);
    }
}

/* ---- Public refresh ---- */

void deck_bridge_ui_statusbar_refresh(void)
{
    if (!s_bar) return;

    /* Time. */
    if (s_lbl_time) {
        char tbuf[12] = "--:--";
        if (deck_sdi_time_wall_is_set()) {
            int64_t epoch = deck_sdi_time_wall_epoch_s();
            time_t t = (time_t)epoch;
            struct tm tm;
            localtime_r(&t, &tm);
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d",
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
        }
        lv_label_set_text(s_lbl_time, tbuf);
    }

    /* WiFi bars. */
    if (s_wifi_cont) {
        s_wifi_level = 0;
        deck_sdi_wifi_state_t st = deck_sdi_wifi_status();
        if (st == DECK_SDI_WIFI_CONNECTED) {
            int8_t r = deck_sdi_wifi_rssi();
            if      (r > -50) s_wifi_level = 4;
            else if (r > -60) s_wifi_level = 3;
            else if (r > -70) s_wifi_level = 2;
            else              s_wifi_level = 1;
        }
        for (int i = 0; i < 4; i++) {
            if (!s_wifi_bars[i]) continue;
            lv_color_t c = (i < s_wifi_level) ? s_pal.primary : s_pal.text_dim;
            lv_obj_set_style_bg_color(s_wifi_bars[i], c, 0);
        }
    }

    /* SD. */
    s_sd_present = hal_sdcard_is_mounted();
    if (s_lbl_sd) {
        lv_obj_set_style_text_color(s_lbl_sd,
            s_sd_present ? s_pal.primary : s_pal.text_dim, 0);
    }

    /* Battery. */
    s_charging = deck_sdi_battery_is_charging();
    uint8_t pct = 0;
    if (deck_sdi_battery_read_pct(&pct) == DECK_SDI_OK) s_pct = pct;
    if (s_batt_body) {
        bool dc_only = (s_pct == 0 && !s_charging);
        if (dc_only) {
            lv_obj_set_width(s_batt_fill, 0);
            lv_label_set_text(s_batt_pct_l, "");
            lv_obj_clear_flag(s_batt_bolt, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_color(s_batt_body, s_pal.primary, 0);
            lv_obj_set_style_bg_color(s_batt_tip, s_pal.primary, 0);
            lv_obj_set_style_text_color(s_batt_bolt, s_pal.primary, 0);
        } else {
            lv_coord_t fill_w = (s_pct > 100 ? 100 : s_pct) * 38 / 100;
            if (fill_w < 1 && s_pct > 0) fill_w = 1;
            lv_obj_set_width(s_batt_fill, fill_w);
            lv_color_t fill_c = (s_pct > 20) ? s_pal.primary : s_pal.accent;
            lv_obj_set_style_bg_color(s_batt_fill, fill_c, 0);
            if (s_charging) {
                lv_obj_clear_flag(s_batt_bolt, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_text_color(s_batt_bolt, s_pal.bg_dark, 0);
                lv_obj_set_style_border_color(s_batt_body, s_pal.primary, 0);
                lv_obj_set_style_bg_color(s_batt_tip, s_pal.primary, 0);
            } else {
                lv_obj_add_flag(s_batt_bolt, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_border_color(s_batt_body, s_pal.text_dim, 0);
                lv_obj_set_style_bg_color(s_batt_tip, s_pal.text_dim, 0);
            }
            char buf[6];
            snprintf(buf, sizeof(buf), "%d%%", s_pct);
            lv_label_set_text(s_batt_pct_l, buf);
            lv_obj_center(s_batt_pct_l);
        }
    }
}

static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    deck_bridge_ui_statusbar_refresh();
}

/* ---- Init ---- */

deck_sdi_err_t deck_bridge_ui_statusbar_init(void)
{
    if (s_bar) return DECK_SDI_OK;
    pal_apply_atom(NULL);
    if (!deck_bridge_ui_lock(200)) return DECK_SDI_ERR_BUSY;

    lv_disp_t  *disp    = lv_disp_get_default();
    lv_coord_t  hor_res = lv_disp_get_hor_res(disp);
    lv_coord_t  ver_res = lv_disp_get_ver_res(disp);
    bool        portrait = (hor_res < ver_res);
    lv_coord_t  bar_w   = portrait ? hor_res : (hor_res - NB_THICK);

    lv_obj_t *layer = lv_layer_top();
    s_bar = lv_obj_create(layer);
    lv_obj_set_size(s_bar, bar_w, SB_HEIGHT);
    lv_obj_align(s_bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_bar, s_pal.bg_dark, 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_radius(s_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_layout(s_bar, 0, 0);

    /* Title polygon (canvas). */
    s_title_cv = lv_canvas_create(s_bar);
    lv_canvas_set_buffer(s_title_cv, s_title_buf, TITLE_POLY_W, TITLE_POLY_H,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(s_title_cv, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(s_title_cv, LV_OBJ_FLAG_CLICKABLE);
    title_canvas_redraw();

    /* Right cluster. */
    lv_obj_t *right = lv_obj_create(s_bar);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_set_style_pad_column(right, 8, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(right, SB_HEIGHT);
    lv_obj_set_width(right, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, -8, 0);

    /* Clock. */
    s_lbl_time = lv_label_create(right);
    lv_label_set_text(s_lbl_time, "--:--:--");
    lv_obj_set_style_text_color(s_lbl_time, s_pal.primary, 0);
    lv_obj_set_style_text_font(s_lbl_time, &lv_font_montserrat_18, 0);

    /* BT (stubbed dim — no BT module on reference board). */
    {
        lv_obj_t *cont = lv_obj_create(right);
        lv_obj_set_size(cont, 18, SB_HEIGHT);
        lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(cont, 0, 0);
        lv_obj_set_style_pad_all(cont, 0, 0);
        lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        s_lbl_bt = lv_label_create(cont);
        lv_label_set_text(s_lbl_bt, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_color(s_lbl_bt, s_pal.text_dim, 0);
        lv_obj_set_style_text_font(s_lbl_bt, &lv_font_montserrat_18, 0);
        lv_obj_align(s_lbl_bt, LV_ALIGN_CENTER, 0, 0);
    }

    /* SD. */
    {
        lv_obj_t *cont = lv_obj_create(right);
        lv_obj_set_size(cont, 18, SB_HEIGHT);
        lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(cont, 0, 0);
        lv_obj_set_style_pad_all(cont, 0, 0);
        lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        s_lbl_sd = lv_label_create(cont);
        lv_label_set_text(s_lbl_sd, LV_SYMBOL_SD_CARD);
        lv_obj_set_style_text_color(s_lbl_sd, s_pal.text_dim, 0);
        lv_obj_set_style_text_font(s_lbl_sd, &lv_font_montserrat_18, 0);
        lv_obj_align(s_lbl_sd, LV_ALIGN_CENTER, 0, 0);
    }

    /* WiFi 4 bars. */
    s_wifi_cont = lv_obj_create(right);
    lv_obj_set_size(s_wifi_cont, 26, SB_HEIGHT);
    lv_obj_set_style_bg_opa(s_wifi_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_cont, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_cont, 0, 0);
    lv_obj_clear_flag(s_wifi_cont, LV_OBJ_FLAG_SCROLLABLE);
    {
        const lv_coord_t bw = 4, bgap = 2;
        const lv_coord_t bh[] = { 6, 10, 16, 20 };
        const lv_coord_t base_y = SB_HEIGHT - 8;
        for (int i = 0; i < 4; i++) {
            s_wifi_bars[i] = make_rect(s_wifi_cont, bw, bh[i], s_pal.text_dim);
            lv_obj_set_pos(s_wifi_bars[i], i * (bw + bgap), base_y - bh[i]);
        }
    }

    /* Battery. */
    {
        lv_obj_t *cont = lv_obj_create(right);
        lv_obj_set_size(cont, 48, SB_HEIGHT);
        lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(cont, 0, 0);
        lv_obj_set_style_pad_all(cont, 0, 0);
        lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_layout(cont, 0, 0);

        s_batt_body = lv_obj_create(cont);
        lv_obj_set_size(s_batt_body, 44, 18);
        lv_obj_set_style_bg_opa(s_batt_body, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_batt_body, s_pal.text_dim, 0);
        lv_obj_set_style_border_width(s_batt_body, 2, 0);
        lv_obj_set_style_radius(s_batt_body, 3, 0);
        lv_obj_set_style_pad_all(s_batt_body, 0, 0);
        lv_obj_clear_flag(s_batt_body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(s_batt_body, LV_ALIGN_LEFT_MID, 0, 0);

        s_batt_tip = make_rect(cont, 3, 8, s_pal.text_dim);
        lv_obj_align(s_batt_tip, LV_ALIGN_LEFT_MID, 45, 0);

        s_batt_fill = make_rect(s_batt_body, 0, 12, s_pal.primary);
        lv_obj_align(s_batt_fill, LV_ALIGN_LEFT_MID, 2, 0);

        s_batt_pct_l = lv_label_create(s_batt_body);
        lv_label_set_text(s_batt_pct_l, "");
        lv_obj_set_style_text_color(s_batt_pct_l, s_pal.primary, 0);
        lv_obj_set_style_text_font(s_batt_pct_l, &lv_font_montserrat_14, 0);
        lv_obj_center(s_batt_pct_l);

        s_batt_bolt = lv_label_create(s_batt_body);
        lv_label_set_text(s_batt_bolt, LV_SYMBOL_CHARGE);
        lv_obj_set_style_text_color(s_batt_bolt, s_pal.primary, 0);
        lv_obj_set_style_text_font(s_batt_bolt, &lv_font_montserrat_14, 0);
        lv_obj_center(s_batt_bolt);
        lv_obj_add_flag(s_batt_bolt, LV_OBJ_FLAG_HIDDEN);
    }

    /* Border accent lines (top, bottom; right only in portrait). */
    s_top_line = make_rect(s_bar, bar_w, 2, s_pal.primary);
    lv_obj_align(s_top_line, LV_ALIGN_TOP_LEFT, 0, 0);
    s_bot_line = make_rect(s_bar, bar_w, 2, s_pal.primary);
    lv_obj_align(s_bot_line, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    s_right_line = make_rect(s_bar, 2, SB_HEIGHT, s_pal.primary);
    lv_obj_align(s_right_line, LV_ALIGN_TOP_RIGHT, 0, 0);
    if (!portrait) lv_obj_add_flag(s_right_line, LV_OBJ_FLAG_HIDDEN);

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
    if (!s_bar) return;
    lv_disp_t  *disp    = lv_disp_get_default();
    lv_coord_t  hor_res = lv_disp_get_hor_res(disp);
    lv_coord_t  ver_res = lv_disp_get_ver_res(disp);
    bool        portrait = (hor_res < ver_res);
    lv_coord_t  bar_w   = portrait ? hor_res : (hor_res - NB_THICK);
    lv_obj_set_size(s_bar, bar_w, SB_HEIGHT);
    lv_obj_align(s_bar, LV_ALIGN_TOP_LEFT, 0, 0);
    if (s_top_line) lv_obj_set_size(s_top_line, bar_w, 2);
    if (s_bot_line) lv_obj_set_size(s_bot_line, bar_w, 2);
    if (s_right_line) {
        if (portrait) lv_obj_clear_flag(s_right_line, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(s_right_line,   LV_OBJ_FLAG_HIDDEN);
    }
}

void deck_bridge_ui_statusbar_set_visible(bool visible)
{
    if (!s_bar) return;
    if (!deck_bridge_ui_lock(200)) return;
    if (visible) lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    deck_bridge_ui_unlock();
}

void deck_bridge_ui_statusbar_apply_theme(const char *atom)
{
    if (!s_bar) return;
    if (!deck_bridge_ui_lock(200)) return;
    pal_apply_atom(atom);

    title_canvas_redraw();

    lv_obj_set_style_text_color(s_lbl_time, s_pal.primary, 0);

    if (s_top_line)   lv_obj_set_style_bg_color(s_top_line,   s_pal.primary, 0);
    if (s_bot_line)   lv_obj_set_style_bg_color(s_bot_line,   s_pal.primary, 0);
    if (s_right_line) lv_obj_set_style_bg_color(s_right_line, s_pal.primary, 0);

    deck_bridge_ui_unlock();
    deck_bridge_ui_statusbar_refresh();
    ESP_LOGI(TAG, "statusbar theme → %s", atom ? atom : "?");
}

/* ---- Badge pills (J4) ---- */
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
        /* Mount the pill on the right cluster (last child of s_bar) so
         * the flex row renders it in line with the icons. */
        uint32_t cnt = lv_obj_get_child_cnt(s_bar);
        lv_obj_t *right = cnt >= 2 ? lv_obj_get_child(s_bar, 1) : s_bar;
        b->pill = lv_obj_create(right);
        lv_obj_set_size(b->pill, LV_SIZE_CONTENT, 22);
        lv_obj_set_style_bg_color(b->pill, s_pal.primary_dim, 0);
        lv_obj_set_style_bg_opa(b->pill, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b->pill, s_pal.primary, 0);
        lv_obj_set_style_border_width(b->pill, 1, 0);
        lv_obj_set_style_radius(b->pill, 11, 0);
        lv_obj_set_style_pad_hor(b->pill, 6, 0);
        lv_obj_set_style_pad_ver(b->pill, 0, 0);
        lv_obj_t *l = lv_label_create(b->pill);
        lv_obj_set_style_text_color(l, s_pal.primary, 0);
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
