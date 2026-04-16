/*
 * CyberDeck — Navigation bar
 *
 * Landscape: right sidebar, UI_NAVBAR_THICK wide, flex COLUMN, left border
 * Portrait:  bottom bar,   UI_NAVBAR_THICK tall, flex ROW,    top  border
 *
 * Icons (outline geometric, theme color):
 *   [0] ◀ triangle  → EVT_GESTURE_BACK    (canvas — no widget equivalent)
 *   [1] ○ circle    → EVT_GESTURE_HOME    (styled lv_obj, LV_RADIUS_CIRCLE)
 *   [2] □ square    → EVT_NAV_PROCESSES   (styled lv_obj, radius=0)
 *
 * Theme refresh: borders + canvas bg updated in place.
 * Orientation change: full destroy + rebuild via ui_navbar_adapt().
 */

#include "ui_navbar.h"
#include "ui_theme.h"
#include "ui_activity.h"
#include "svc_event.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "navbar";

#define ICON_SZ  38

/* ---- Static references ---- */
static lv_obj_t  *s_navbar        = NULL;
static lv_obj_t  *s_btn[3]        = {NULL, NULL, NULL};
static lv_obj_t  *s_icon_tri      = NULL;   /* canvas: triangle */
static lv_obj_t  *s_icon_circle   = NULL;   /* styled obj: circle */
static lv_obj_t  *s_icon_square   = NULL;   /* styled obj: square */

static lv_color_t s_tri_buf[ICON_SZ * ICON_SZ];   /* canvas pixel buffer */

/* ---- Orientation helper ---- */

static bool s_portrait(void)
{
    lv_disp_t *d = lv_disp_get_default();
    return lv_disp_get_hor_res(d) < lv_disp_get_ver_res(d);
}

/* ---- Triangle canvas drawing ---- */

static void draw_triangle(lv_obj_t *canvas, lv_color_t bg, lv_color_t fg)
{
    lv_canvas_fill_bg(canvas, bg, LV_OPA_COVER);

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color       = fg;
    ld.width       = 2;
    ld.opa         = LV_OPA_COVER;
    ld.round_start = 1;
    ld.round_end   = 1;

    /* Left-pointing ◀: base pulled inward to be slightly narrower than tall */
    lv_point_t pts[4] = {
        {ICON_SZ - 9, 2         },
        {3,           ICON_SZ/2 },
        {ICON_SZ - 9, ICON_SZ-3},
        {ICON_SZ - 9, 2         },
    };
    lv_canvas_draw_line(canvas, pts, 4, &ld);
}

/* ---- Button callbacks ---- */

/* Back and Home are called from the LVGL task (button callback) — call directly,
 * no need to bounce through the event loop (avoids async gap / glitch frame). */
static void back_btn_cb(lv_event_t *e)       { (void)e; ui_activity_pop(); }
static void home_btn_cb(lv_event_t *e)       { (void)e; ui_activity_suspend_to_home(); }
static void processes_btn_cb(lv_event_t *e)  { (void)e; svc_event_post(EVT_NAV_PROCESSES, NULL, 0); }

static lv_event_cb_t s_btn_cbs[3] = {
    back_btn_cb,
    home_btn_cb,
    processes_btn_cb,
};

/* ---- Core builder ---- */

static void build_navbar(void)
{
    const cyberdeck_theme_t *t = ui_theme_get();
    bool portrait = s_portrait();

    lv_disp_t  *disp = lv_disp_get_default();
    lv_coord_t  sw   = lv_disp_get_hor_res(disp);
    lv_coord_t  sh   = lv_disp_get_ver_res(disp);

    /* ---- Container on lv_layer_sys() — always on top ---- */
    s_navbar = lv_obj_create(lv_layer_sys());

    if (portrait) {
        lv_obj_set_size(s_navbar, sw, UI_NAVBAR_THICK);
        lv_obj_align(s_navbar, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_flex_flow(s_navbar, LV_FLEX_FLOW_ROW);
    } else {
        lv_obj_set_size(s_navbar, UI_NAVBAR_THICK, sh);
        lv_obj_align(s_navbar, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_flex_flow(s_navbar, LV_FLEX_FLOW_COLUMN);
    }

    lv_obj_set_style_bg_color(s_navbar, t->bg_dark, 0);
    lv_obj_set_style_bg_opa(s_navbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_navbar, t->primary, 0);
    lv_obj_set_style_border_width(s_navbar, 2, 0);
    lv_obj_set_style_border_opa(s_navbar, LV_OPA_COVER, 0);
    /* Portrait (bottom bar): top edge only.
     * Landscape (right sidebar): left edge only. */
    lv_border_side_t sides = portrait
        ? LV_BORDER_SIDE_TOP
        : LV_BORDER_SIDE_LEFT;
    lv_obj_set_style_border_side(s_navbar, sides, 0);
    lv_obj_set_style_radius(s_navbar, 0, 0);
    lv_obj_set_style_pad_all(s_navbar, 0, 0);
    lv_obj_clear_flag(s_navbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_align(s_navbar,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* ---- Three button zones ---- */
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_obj_create(s_navbar);
        s_btn[i] = btn;

        /* Set the non-growing axis to fill the navbar thickness;
         * the growing axis is set small — flex_grow=1 expands it. */
        if (portrait) {
            lv_obj_set_size(btn, 10, UI_NAVBAR_THICK);   /* width grows */
        } else {
            lv_obj_set_size(btn, UI_NAVBAR_THICK, 10);   /* height grows */
        }
        lv_obj_set_style_flex_grow(btn, 1, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /* Subtle press highlight */
        lv_obj_set_style_bg_color(btn, t->primary, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);

        /* ---- Icon ---- */
        if (i == 0) {
            /* Triangle ◀ — canvas */
            lv_obj_t *cv = lv_canvas_create(btn);
            s_icon_tri = cv;
            lv_canvas_set_buffer(cv, s_tri_buf, ICON_SZ, ICON_SZ,
                                 LV_IMG_CF_TRUE_COLOR);
            lv_obj_set_size(cv, ICON_SZ, ICON_SZ);
            lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
            /* LVGL renders the canvas image slightly left of its logical
             * center because the triangle tip sits near x=3. Shift right
             * by 3 px so the glyph reads as optically centred in the button. */
            lv_obj_set_style_translate_x(cv, 3, 0);
            draw_triangle(cv, t->bg_dark, t->primary);

        } else if (i == 1) {
            /* Circle ○ — styled obj */
            lv_obj_t *ic = lv_obj_create(btn);
            s_icon_circle = ic;
            lv_obj_set_size(ic, ICON_SZ, ICON_SZ);
            lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(ic, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(ic, t->primary, 0);
            lv_obj_set_style_border_width(ic, 2, 0);
            lv_obj_set_style_border_opa(ic, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(ic, 0, 0);
            lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        } else {
            /* Square □ — styled obj, slightly smaller than circle for visual balance */
            lv_obj_t *ic = lv_obj_create(btn);
            s_icon_square = ic;
            lv_obj_set_size(ic, ICON_SZ - 8, ICON_SZ - 8);
            lv_obj_set_style_radius(ic, 0, 0);
            lv_obj_set_style_bg_opa(ic, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(ic, t->primary, 0);
            lv_obj_set_style_border_width(ic, 2, 0);
            lv_obj_set_style_border_opa(ic, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(ic, 0, 0);
            lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }

        lv_obj_add_event_cb(btn, s_btn_cbs[i], LV_EVENT_CLICKED, NULL);
    }

    ESP_LOGI(TAG, "Navbar built (%s, %dx%d)",
             portrait ? "portrait/bottom" : "landscape/right",
             (int)sw, (int)sh);
}

/* ---- Public API ---- */

esp_err_t ui_navbar_init(void)
{
    build_navbar();
    return ESP_OK;
}

void ui_navbar_adapt(void)
{
    /* Destroy existing navbar and rebuild for the current orientation.
     * Must be called with LVGL mutex held. */
    if (s_navbar) {
        lv_obj_del(s_navbar);
        s_navbar      = NULL;
        s_icon_tri    = NULL;
        s_icon_circle = NULL;
        s_icon_square = NULL;
        for (int i = 0; i < 3; i++) s_btn[i] = NULL;
    }
    build_navbar();
    ESP_LOGI(TAG, "Navbar adapted to new orientation");
}

void ui_navbar_set_visible(bool visible)
{
    if (!s_navbar) return;
    if (visible) lv_obj_clear_flag(s_navbar, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_navbar,   LV_OBJ_FLAG_HIDDEN);
}

void ui_navbar_refresh_theme(void)
{
    if (!s_navbar) return;
    const cyberdeck_theme_t *t = ui_theme_get();

    lv_obj_set_style_bg_color(s_navbar, t->bg_dark, 0);
    lv_obj_set_style_border_color(s_navbar, t->primary, 0);

    for (int i = 0; i < 3; i++) {
        if (s_btn[i]) {
            lv_obj_set_style_bg_color(s_btn[i], t->primary, LV_STATE_PRESSED);
        }
    }

    /* Triangle canvas — redraw with new colors */
    if (s_icon_tri) {
        draw_triangle(s_icon_tri, t->bg_dark, t->primary);
    }

    /* Circle + square — update border color */
    if (s_icon_circle) {
        lv_obj_set_style_border_color(s_icon_circle, t->primary, 0);
    }
    if (s_icon_square) {
        lv_obj_set_style_border_color(s_icon_square, t->primary, 0);
    }
}
