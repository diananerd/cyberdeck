/*
 * S3 Cyber-Deck — Settings > Display
 * Theme selector, rotation toggle, screen timeout.
 * Theme and rotation changes take effect immediately and recreate all screens.
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
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "settings_display";

/* ---- Theme buttons ---- */

typedef struct {
    cyberdeck_theme_id_t theme_id;
} theme_ctx_t;

static void theme_btn_cb(lv_event_t *e)
{
    theme_ctx_t *ctx = (theme_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    cyberdeck_theme_id_t new_theme = ctx->theme_id;
    ui_theme_apply(new_theme);
    svc_settings_set_theme((uint8_t)new_theme);

    /* Refresh persistent UI elements (live on layer_top/sys, not recreated by recreate_all) */
    ui_statusbar_refresh_theme();
    ui_navbar_refresh_theme();

    /* Recreate all activity layouts with the new theme.
     * We're on the LVGL task here (button callback), so call directly. */
    ui_activity_recreate_all();

    /* The display settings screen was just recreated — post a delayed toast
     * via the effect layer which survives screen recreation. */
    ui_effect_toast("Theme applied", 1200);

    ESP_LOGI(TAG, "Theme changed to %d", (int)new_theme);
}

/* ---- Rotation toggle ---- */

static void rotation_btn_cb(lv_event_t *e)
{
    (void)e;
    uint8_t cur = 0;
    svc_settings_get_rotation(&cur);
    uint8_t new_rot = cur ? 0 : 1;

    ui_engine_set_rotation(new_rot);
    svc_settings_set_rotation(new_rot);

    /* EVT_DISPLAY_ROTATED triggers ui_activity_recreate_all() in main.c */
    svc_event_post(EVT_DISPLAY_ROTATED, &new_rot, sizeof(new_rot));

    ESP_LOGI(TAG, "Rotation toggled to %s", new_rot ? "portrait" : "landscape");
}

/* ---- Screen timeout ---- */

#define TIMEOUT_COUNT 4

typedef struct {
    uint16_t    timeout_s;
    uint16_t    val;
    lv_obj_t   *cur_lbl;
    lv_obj_t   *btns[TIMEOUT_COUNT];
} timeout_ctx_t;

static const struct { const char *label; uint16_t val; } s_timeouts[TIMEOUT_COUNT] = {
    { "Never", 0   },
    { "30s",   30  },
    { "1 min", 60  },
    { "5 min", 300 },
};

static void timeout_btn_cb(lv_event_t *e)
{
    timeout_ctx_t *ctx = (timeout_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    svc_settings_set_screen_timeout(ctx->val);

    /* Update "Current:" label */
    if (ctx->cur_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Current: %s",
                 (ctx->val == 0)   ? "Never"  :
                 (ctx->val == 30)  ? "30s"    :
                 (ctx->val == 60)  ? "1 min"  :
                 (ctx->val == 300) ? "5 min"  : "Custom");
        lv_label_set_text(ctx->cur_lbl, buf);
    }

    /* Re-style all timeout buttons: active = filled, others = outline */
    const cyberdeck_theme_t *t = ui_theme_get();
    for (int i = 0; i < TIMEOUT_COUNT; i++) {
        lv_obj_t *btn = ctx->btns[i];
        if (!btn) continue;
        bool active = (s_timeouts[i].val == ctx->val);
        lv_obj_set_style_bg_color(btn, t->primary, 0);
        lv_obj_set_style_bg_opa(btn, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, active ? t->bg_dark : t->primary, 0);
    }

    ui_effect_toast(ctx->val == 0 ? "Timeout: Never"
                                  : (ctx->val < 60 ? "Timeout: 30s"
                                                   : (ctx->val < 300 ? "Timeout: 1 min"
                                                                     : "Timeout: 5 min")), 1200);
    ESP_LOGI(TAG, "Screen timeout set to %ds", ctx->val);
}

/* ---- Activity on_create ---- */

static void display_on_create(lv_obj_t *screen, void *intent_data)
{
    (void)intent_data;
    const cyberdeck_theme_t *t = ui_theme_get();
    cyberdeck_theme_id_t cur_theme = ui_theme_get_id();
    uint8_t cur_rotation = 0;
    svc_settings_get_rotation(&cur_rotation);
    uint16_t cur_timeout = 60;
    svc_settings_get_screen_timeout(&cur_timeout);

    ui_statusbar_set_title("SETTINGS");
    lv_obj_t *content = ui_common_content_area(screen);

    /* --- Theme section --- */
    lv_obj_t *theme_title = lv_label_create(content);
    lv_label_set_text(theme_title, "Theme");
    ui_theme_style_label(theme_title, &CYBERDECK_FONT_MD);

    /* Three theme buttons in a row */
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

    static const struct { const char *label; cyberdeck_theme_id_t id; } themes[] = {
        { "Green",  THEME_GREEN },
        { "Amber",  THEME_AMBER },
        { "Neon",   THEME_NEON  },
    };

    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = ui_common_btn(theme_row, themes[i].label);

        /* Highlight active theme with full fill */
        if (themes[i].id == cur_theme) {
            lv_obj_set_style_bg_color(btn, t->primary, 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            /* Text inverse for active button */
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (lbl) lv_obj_set_style_text_color(lbl, t->bg_dark, 0);
        }

        theme_ctx_t *ctx = (theme_ctx_t *)lv_mem_alloc(sizeof(theme_ctx_t));
        if (ctx) {
            ctx->theme_id = themes[i].id;
            lv_obj_add_event_cb(btn, theme_btn_cb, LV_EVENT_CLICKED, ctx);
        }
    }

    ui_common_divider(content);

    /* --- Rotation section --- */
    lv_obj_t *rot_title = lv_label_create(content);
    lv_label_set_text(rot_title, "Rotation");
    ui_theme_style_label(rot_title, &CYBERDECK_FONT_MD);

    char rot_info[32];
    snprintf(rot_info, sizeof(rot_info), "Current: %s",
             cur_rotation ? "Portrait (480x800)" : "Landscape (800x480)");
    lv_obj_t *rot_lbl = lv_label_create(content);
    lv_label_set_text(rot_lbl, rot_info);
    ui_theme_style_label_dim(rot_lbl, &CYBERDECK_FONT_SM);

    lv_obj_t *rot_btn = ui_common_btn_full(content,
        cur_rotation ? "Switch to Landscape" : "Switch to Portrait");
    lv_obj_add_event_cb(rot_btn, rotation_btn_cb, LV_EVENT_CLICKED, NULL);

    ui_common_divider(content);

    /* --- Screen timeout section --- */
    lv_obj_t *timeout_title = lv_label_create(content);
    lv_label_set_text(timeout_title, "Screen Timeout");
    ui_theme_style_label(timeout_title, &CYBERDECK_FONT_MD);

    char cur_timeout_str[32];
    snprintf(cur_timeout_str, sizeof(cur_timeout_str),
             "Current: %s", (cur_timeout == 0) ? "Never" :
             (cur_timeout == 30) ? "30s" :
             (cur_timeout == 60) ? "1 min" :
             (cur_timeout == 300) ? "5 min" : "Custom");
    lv_obj_t *to_lbl = lv_label_create(content);
    lv_label_set_text(to_lbl, cur_timeout_str);
    ui_theme_style_label_dim(to_lbl, &CYBERDECK_FONT_SM);

    lv_obj_t *to_row = lv_obj_create(content);
    lv_obj_set_width(to_row, LV_PCT(100));
    lv_obj_set_height(to_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(to_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(to_row, 0, 0);
    lv_obj_set_style_pad_all(to_row, 0, 0);
    lv_obj_set_flex_flow(to_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(to_row, 10, 0);
    lv_obj_set_style_pad_row(to_row, 6, 0);
    lv_obj_clear_flag(to_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Allocate one shared ctx array — all buttons share the same btns[] + cur_lbl */
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
            ctx->val     = s_timeouts[i].val;
            ctx->cur_lbl = to_lbl;
        }
    }

    /* Wire up btns[] cross-references and register callbacks */
    for (int i = 0; i < TIMEOUT_COUNT; i++) {
        if (!to_ctxs[i]) continue;
        for (int j = 0; j < TIMEOUT_COUNT; j++) to_ctxs[i]->btns[j] = to_btns[j];
        lv_obj_add_event_cb(to_btns[i], timeout_btn_cb, LV_EVENT_CLICKED, to_ctxs[i]);
    }

    ESP_LOGI(TAG, "Display settings created");
}

const activity_cbs_t settings_display_cbs = {
    .on_create  = display_on_create,
    .on_resume  = NULL,
    .on_pause   = NULL,
    .on_destroy = NULL,
};
