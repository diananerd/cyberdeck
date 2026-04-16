/*
 * CyberDeck — Settings > Display
 * Theme selector, rotation toggle, screen timeout.
 *
 * Groups are separated by spacing only (no dividers).
 * Layout: theme row → rotation data row + toggle → timeout selector.
 */

#include "app_settings.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_engine.h"
#include "ui_statusbar.h"
#include "ui_navbar.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "svc_settings.h"
#include "svc_event.h"
#include "os_settings.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "settings_display";

/* ---- Theme ---- */

typedef struct { cyberdeck_theme_id_t theme_id; } theme_ctx_t;

static void theme_btn_cb(lv_event_t *e)
{
    theme_ctx_t *ctx = (theme_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    ui_theme_apply(ctx->theme_id);
    os_settings_set_theme((uint8_t)ctx->theme_id);  /* E2: cache + NVS + event */
    ui_statusbar_refresh_theme();
    ui_navbar_refresh_theme();
    ui_activity_recreate_all();
    ui_effect_toast("Theme applied", 1200);
    ESP_LOGI(TAG, "Theme changed to %d", (int)ctx->theme_id);
}

/* ---- Rotation ---- */

static void rotation_btn_cb(lv_event_t *e)
{
    (void)e;
    uint8_t cur = os_settings_get()->rotation;  /* E3: read from cache */
    uint8_t new_rot = cur ? 0 : 1;
    ui_engine_set_rotation(new_rot);
    os_settings_set_rotation(new_rot);           /* E2: cache + NVS + EVT_SETTINGS_CHANGED */
    svc_event_post(EVT_DISPLAY_ROTATED, &new_rot, sizeof(new_rot));
    ESP_LOGI(TAG, "Rotation toggled to %s", new_rot ? "portrait" : "landscape");
}

/* ---- Screen timeout ---- */

#define TIMEOUT_COUNT 4

typedef struct {
    uint16_t    val;
    lv_obj_t   *cur_val_lbl;       /* data-row value label */
    lv_obj_t   *btns[TIMEOUT_COUNT];
} timeout_ctx_t;

static const struct { const char *label; uint16_t val; } s_timeouts[TIMEOUT_COUNT] = {
    { "Never", 0   },
    { "30s",   30  },
    { "1 min", 60  },
    { "5 min", 300 },
};

static const char *timeout_label(uint16_t val)
{
    if (val == 0)   return "Never";
    if (val == 30)  return "30s";
    if (val == 60)  return "1 min";
    if (val == 300) return "5 min";
    return "Custom";
}

static void timeout_btn_cb(lv_event_t *e)
{
    timeout_ctx_t *ctx = (timeout_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    os_settings_set_screen_timeout(ctx->val);  /* E2: cache + NVS + event */
    if (ctx->cur_val_lbl)
        lv_label_set_text(ctx->cur_val_lbl, timeout_label(ctx->val));
    const cyberdeck_theme_t *t = ui_theme_get();
    for (int i = 0; i < TIMEOUT_COUNT; i++) {
        lv_obj_t *btn = ctx->btns[i];
        if (!btn) continue;
        bool active = (s_timeouts[i].val == ctx->val);
        lv_obj_set_style_bg_color(btn, t->primary, 0);
        lv_obj_set_style_bg_opa(btn, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl)
            lv_obj_set_style_text_color(lbl, active ? t->bg_dark : t->primary, 0);
    }
    ESP_LOGI(TAG, "Timeout set to %ds", ctx->val);
}

/* ---- Section label helper ---- */

static lv_obj_t *section_label(lv_obj_t *content, const char *text)
{
    lv_obj_t *lbl = lv_label_create(content);
    lv_label_set_text(lbl, text);
    ui_theme_style_label_dim(lbl, &CYBERDECK_FONT_SM);
    /* Extra top spacing via top padding on the label itself */
    lv_obj_set_style_pad_top(lbl, 20, 0);
    return lbl;
}

/* ---- Activity on_create ---- */

static void *display_on_create(lv_obj_t *screen, const view_args_t *args, void *app_data)
{
    (void)args;
    (void)app_data;
    const cyberdeck_theme_t *t = ui_theme_get();
    cyberdeck_theme_id_t cur_theme = ui_theme_get_id();
    const cyberdeck_settings_t *cfg = os_settings_get();  /* E3: read from cache */
    uint8_t  cur_rotation = cfg->rotation;
    uint16_t cur_timeout  = cfg->screen_timeout;

    ui_statusbar_set_title("SETTINGS");
    lv_obj_t *content = ui_common_content_area(screen);

    /* ==============================
     * THEME
     * ============================== */
    section_label(content, "THEME:");

    static const struct { const char *label; cyberdeck_theme_id_t id; } themes[] = {
        { "Green",  THEME_GREEN },
        { "Amber",  THEME_AMBER },
        { "Neon",   THEME_NEON  },
    };

    lv_obj_t *theme_row = lv_obj_create(content);
    lv_obj_set_width(theme_row, LV_PCT(100));
    lv_obj_set_height(theme_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(theme_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(theme_row, 0, 0);
    lv_obj_set_style_pad_all(theme_row, 0, 0);
    lv_obj_set_flex_flow(theme_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(theme_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(theme_row, 12, 0);
    lv_obj_clear_flag(theme_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = ui_common_btn(theme_row, themes[i].label);
        if (themes[i].id == cur_theme) {
            /* Active theme: filled */
            lv_obj_set_style_bg_color(btn, t->primary, 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (lbl) lv_obj_set_style_text_color(lbl, t->bg_dark, 0);
        }
        theme_ctx_t *ctx = (theme_ctx_t *)lv_mem_alloc(sizeof(theme_ctx_t));
        if (ctx) {
            ctx->theme_id = themes[i].id;
            lv_obj_add_event_cb(btn, theme_btn_cb, LV_EVENT_CLICKED, ctx);
        }
    }

    /* ==============================
     * ORIENTATION
     * ============================== */
    section_label(content, "ORIENTATION:");

    lv_obj_t *rot_val = lv_label_create(content);
    lv_label_set_text(rot_val,
        cur_rotation ? "PORTRAIT  480x800" : "LANDSCAPE  800x480");
    ui_theme_style_label(rot_val, &CYBERDECK_FONT_MD);

    lv_obj_t *rot_btn = ui_common_btn(content,
        cur_rotation ? "Switch to Landscape" : "Switch to Portrait");
    lv_obj_add_event_cb(rot_btn, rotation_btn_cb, LV_EVENT_CLICKED, NULL);

    /* ==============================
     * SCREEN TIMEOUT
     * ============================== */
    section_label(content, "SCREEN TIMEOUT:");

    /* Data row: current value */
    lv_obj_t *to_val = lv_label_create(content);
    lv_label_set_text(to_val, timeout_label(cur_timeout));
    ui_theme_style_label(to_val, &CYBERDECK_FONT_MD);

    /* Timeout buttons row */
    lv_obj_t *to_row = lv_obj_create(content);
    lv_obj_set_width(to_row, LV_PCT(100));
    lv_obj_set_height(to_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(to_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(to_row, 0, 0);
    lv_obj_set_style_pad_all(to_row, 0, 0);
    lv_obj_set_flex_flow(to_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(to_row, 10, 0);
    lv_obj_set_style_pad_row(to_row, 8, 0);
    lv_obj_clear_flag(to_row, LV_OBJ_FLAG_SCROLLABLE);

    timeout_ctx_t *to_ctxs[TIMEOUT_COUNT];
    lv_obj_t      *to_btns[TIMEOUT_COUNT];

    for (int i = 0; i < TIMEOUT_COUNT; i++) {
        lv_obj_t *btn = ui_common_btn(to_row, s_timeouts[i].label);
        to_btns[i] = btn;
        if (s_timeouts[i].val == cur_timeout) {
            lv_obj_set_style_bg_color(btn, t->primary, 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (lbl) lv_obj_set_style_text_color(lbl, t->bg_dark, 0);
        }
        timeout_ctx_t *ctx = (timeout_ctx_t *)lv_mem_alloc(sizeof(timeout_ctx_t));
        to_ctxs[i] = ctx;
        if (ctx) {
            ctx->val       = s_timeouts[i].val;
            ctx->cur_val_lbl = to_val;
        }
    }

    for (int i = 0; i < TIMEOUT_COUNT; i++) {
        if (!to_ctxs[i]) continue;
        for (int j = 0; j < TIMEOUT_COUNT; j++) to_ctxs[i]->btns[j] = to_btns[j];
        lv_obj_add_event_cb(to_btns[i], timeout_btn_cb, LV_EVENT_CLICKED, to_ctxs[i]);
    }

    ESP_LOGI(TAG, "Display settings created");
    return NULL;
}

const view_cbs_t settings_display_cbs = {
    .on_create  = display_on_create,
    .on_resume  = NULL,
    .on_pause   = NULL,
    .on_destroy = NULL,
};
