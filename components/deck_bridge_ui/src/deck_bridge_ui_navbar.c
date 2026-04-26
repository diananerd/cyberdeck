/* deck_bridge_ui_navbar — pre-deck-lang TUI navigation bar.
 *
 * Three zones: BACK (◀ triangle, canvas) | HOME (○ outline circle) |
 * TASKS (□ outline square). Bottom dock in portrait, right sidebar in
 * landscape. Single primary accent border (top in portrait, left in
 * landscape). Press feedback = primary tint at 20% opacity.
 *
 * Mounted on lv_layer_sys() so the bar always paints above content.
 */

#include "deck_bridge_ui.h"
#include "deck_bridge_ui_internal.h"

#include "lvgl.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "bridge_ui.nb";

#define NB_THICK   72        /* roughly 2× statusbar height (pre-deck-lang) */
#define ICON_SZ    38

static lv_color_t s_pal_primary;
static lv_color_t s_pal_dim;
static lv_color_t s_pal_bg;
static bool       s_pal_inited = false;

static void pal_apply(const char *atom)
{
    if (atom && !strcmp(atom, "amber")) {
        s_pal_primary = lv_color_hex(0xFFB000);
        s_pal_dim     = lv_color_hex(0x4D3500);
    } else if (atom && !strcmp(atom, "neon")) {
        s_pal_primary = lv_color_hex(0xFF00FF);
        s_pal_dim     = lv_color_hex(0x500050);
    } else {
        s_pal_primary = lv_color_hex(0x00FF41);
        s_pal_dim     = lv_color_hex(0x004D13);
    }
    s_pal_bg     = lv_color_black();
    s_pal_inited = true;
}

static lv_obj_t *s_navbar      = NULL;
static lv_obj_t *s_btn_back    = NULL;
static lv_obj_t *s_btn_home    = NULL;
static lv_obj_t *s_btn_tasks   = NULL;
static lv_obj_t *s_icon_tri    = NULL;
static lv_obj_t *s_icon_circle = NULL;
static lv_obj_t *s_icon_square = NULL;
static lv_color_t s_tri_buf[ICON_SZ * ICON_SZ];

static deck_bridge_ui_nav_cb_t s_back_cb  = NULL;
static deck_bridge_ui_nav_cb_t s_home_cb  = NULL;
static deck_bridge_ui_nav_cb_t s_tasks_cb = NULL;

static bool is_portrait(void)
{
    lv_disp_t *d = lv_disp_get_default();
    return lv_disp_get_hor_res(d) < lv_disp_get_ver_res(d);
}

static void draw_triangle(lv_obj_t *canvas)
{
    lv_canvas_fill_bg(canvas, s_pal_bg, LV_OPA_COVER);
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color       = s_pal_primary;
    ld.width       = 2;
    ld.opa         = LV_OPA_COVER;
    ld.round_start = 1;
    ld.round_end   = 1;
    lv_point_t pts[4] = {
        { ICON_SZ - 9, 2          },
        { 3,           ICON_SZ/2  },
        { ICON_SZ - 9, ICON_SZ-3  },
        { ICON_SZ - 9, 2          },
    };
    lv_canvas_draw_line(canvas, pts, 4, &ld);
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "BACK pressed");
    if (s_back_cb) s_back_cb();
}

static void home_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "HOME pressed");
    if (s_home_cb) s_home_cb();
}

static void tasks_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "TASKS pressed");
    if (s_tasks_cb) s_tasks_cb();
}

static lv_obj_t *make_zone(lv_obj_t *parent, bool portrait)
{
    lv_obj_t *btn = lv_obj_create(parent);
    if (portrait) {
        lv_obj_set_size(btn, 10, NB_THICK);
    } else {
        lv_obj_set_size(btn, NB_THICK, 10);
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
    lv_obj_set_style_bg_color(btn, s_pal_primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    return btn;
}

static void build_navbar(void)
{
    bool portrait = is_portrait();
    lv_disp_t  *disp = lv_disp_get_default();
    lv_coord_t  sw   = lv_disp_get_hor_res(disp);
    lv_coord_t  sh   = lv_disp_get_ver_res(disp);

    s_navbar = lv_obj_create(lv_layer_sys());

    if (portrait) {
        lv_obj_set_size(s_navbar, sw, NB_THICK);
        lv_obj_align(s_navbar, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_flex_flow(s_navbar, LV_FLEX_FLOW_ROW);
    } else {
        lv_obj_set_size(s_navbar, NB_THICK, sh);
        lv_obj_align(s_navbar, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_flex_flow(s_navbar, LV_FLEX_FLOW_COLUMN);
    }

    lv_obj_set_style_bg_color(s_navbar, s_pal_bg, 0);
    lv_obj_set_style_bg_opa(s_navbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_navbar, s_pal_primary, 0);
    lv_obj_set_style_border_width(s_navbar, 2, 0);
    lv_obj_set_style_border_opa(s_navbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_navbar,
        portrait ? LV_BORDER_SIDE_TOP : LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_radius(s_navbar, 0, 0);
    lv_obj_set_style_pad_all(s_navbar, 0, 0);
    lv_obj_clear_flag(s_navbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_align(s_navbar,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* TASKS — outline square (slightly smaller for visual balance).
     * Order: TASKS first (left/top), HOME middle, BACK last (right/bottom). */
    s_btn_tasks = make_zone(s_navbar, portrait);
    s_icon_square = lv_obj_create(s_btn_tasks);
    lv_obj_set_size(s_icon_square, ICON_SZ - 8, ICON_SZ - 8);
    lv_obj_set_style_radius(s_icon_square, 0, 0);
    lv_obj_set_style_bg_opa(s_icon_square, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_icon_square, s_pal_primary, 0);
    lv_obj_set_style_border_width(s_icon_square, 2, 0);
    lv_obj_set_style_border_opa(s_icon_square, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_icon_square, 0, 0);
    lv_obj_clear_flag(s_icon_square, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_btn_tasks, tasks_click_cb, LV_EVENT_CLICKED, NULL);

    /* HOME — outline circle. */
    s_btn_home = make_zone(s_navbar, portrait);
    s_icon_circle = lv_obj_create(s_btn_home);
    lv_obj_set_size(s_icon_circle, ICON_SZ, ICON_SZ);
    lv_obj_set_style_radius(s_icon_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_icon_circle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_icon_circle, s_pal_primary, 0);
    lv_obj_set_style_border_width(s_icon_circle, 2, 0);
    lv_obj_set_style_border_opa(s_icon_circle, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_icon_circle, 0, 0);
    lv_obj_clear_flag(s_icon_circle, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_btn_home, home_click_cb, LV_EVENT_CLICKED, NULL);

    /* BACK — triangle canvas. */
    s_btn_back = make_zone(s_navbar, portrait);
    s_icon_tri = lv_canvas_create(s_btn_back);
    lv_canvas_set_buffer(s_icon_tri, s_tri_buf, ICON_SZ, ICON_SZ,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_size(s_icon_tri, ICON_SZ, ICON_SZ);
    lv_obj_clear_flag(s_icon_tri, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_translate_x(s_icon_tri, 3, 0);
    draw_triangle(s_icon_tri);
    lv_obj_add_event_cb(s_btn_back, back_click_cb, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "navbar mounted (%s, %dx%d)",
             portrait ? "portrait/bottom" : "landscape/right",
             (int)sw, (int)sh);
}

deck_sdi_err_t deck_bridge_ui_navbar_init(deck_bridge_ui_nav_cb_t back_cb,
                                          deck_bridge_ui_nav_cb_t home_cb)
{
    s_back_cb = back_cb;
    s_home_cb = home_cb;
    if (!s_pal_inited) pal_apply(NULL);
    if (s_navbar) return DECK_SDI_OK;
    if (!deck_bridge_ui_lock(200)) return DECK_SDI_ERR_BUSY;
    build_navbar();
    deck_bridge_ui_unlock();
    return DECK_SDI_OK;
}

void deck_bridge_ui_navbar_relayout(void)
{
    if (!s_navbar) return;
    lv_obj_del(s_navbar);
    s_navbar = s_btn_back = s_btn_home = s_btn_tasks = NULL;
    s_icon_tri = s_icon_circle = s_icon_square = NULL;
    build_navbar();
}

void deck_bridge_ui_navbar_set_tasks_cb(deck_bridge_ui_nav_cb_t tasks_cb)
{
    s_tasks_cb = tasks_cb;
}

void deck_bridge_ui_navbar_set_visible(bool visible)
{
    if (!s_navbar) return;
    if (!deck_bridge_ui_lock(200)) return;
    if (visible) lv_obj_clear_flag(s_navbar, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_navbar, LV_OBJ_FLAG_HIDDEN);
    deck_bridge_ui_unlock();
}

void deck_bridge_ui_navbar_apply_theme(const char *atom)
{
    if (!s_navbar) return;
    if (!deck_bridge_ui_lock(200)) return;
    pal_apply(atom);
    lv_obj_set_style_border_color(s_navbar, s_pal_primary, 0);
    if (s_icon_tri) draw_triangle(s_icon_tri);
    if (s_icon_circle) lv_obj_set_style_border_color(s_icon_circle, s_pal_primary, 0);
    if (s_icon_square) lv_obj_set_style_border_color(s_icon_square, s_pal_primary, 0);
    if (s_btn_back)  lv_obj_set_style_bg_color(s_btn_back,  s_pal_primary, LV_STATE_PRESSED);
    if (s_btn_home)  lv_obj_set_style_bg_color(s_btn_home,  s_pal_primary, LV_STATE_PRESSED);
    if (s_btn_tasks) lv_obj_set_style_bg_color(s_btn_tasks, s_pal_primary, LV_STATE_PRESSED);
    (void)s_pal_dim;
    deck_bridge_ui_unlock();
    ESP_LOGI(TAG, "navbar theme → %s", atom ? atom : "?");
}
