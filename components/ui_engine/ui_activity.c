/*
 * S3 Cyber-Deck — Activity stack manager
 * Manages a stack of up to 4 activities with lifecycle callbacks.
 */

#include "ui_activity.h"
#include "ui_statusbar.h"
#include "ui_navbar.h"
#include "ui_theme.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "activity";

static activity_t stack[ACTIVITY_STACK_MAX];
static uint8_t    depth = 0;

/* ---------- Internal helpers ---------- */

static void apply_screen_padding(lv_obj_t *scr)
{
    /* Leave room for statusbar (top) and navbar (right in landscape, bottom in portrait).
     * Uses 2px gap between content and the bar so borders don't overlap content. */
    lv_disp_t *d = lv_disp_get_default();
    bool portrait = lv_disp_get_hor_res(d) < lv_disp_get_ver_res(d);

    lv_obj_set_style_pad_top(scr, UI_STATUSBAR_HEIGHT + 2, 0);
    if (portrait) {
        lv_obj_set_style_pad_right(scr,  2, 0);
        lv_obj_set_style_pad_bottom(scr, UI_NAVBAR_THICK + 2, 0);
    } else {
        lv_obj_set_style_pad_right(scr,  UI_NAVBAR_THICK + 2, 0);
        lv_obj_set_style_pad_bottom(scr, 2, 0);
    }
}

static lv_obj_t *create_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_set_style_bg_color(scr, t->bg_dark, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    apply_screen_padding(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static void destroy_entry(activity_t *a)
{
    if (!a->screen) return;

    if (a->cbs.on_destroy) {
        a->cbs.on_destroy(a->screen, a->state);
    }
    lv_obj_del(a->screen);
    a->screen = NULL;
    a->state = NULL;
    ESP_LOGD(TAG, "Destroyed activity app=%d scr=%d", a->app_id, a->screen_id);
}

static void pause_top(void)
{
    if (depth == 0) return;
    activity_t *a = &stack[depth - 1];
    if (a->cbs.on_pause) {
        a->cbs.on_pause(a->screen, a->state);
    }
    lv_obj_add_flag(a->screen, LV_OBJ_FLAG_HIDDEN);
}

static void resume_top(void)
{
    if (depth == 0) return;
    activity_t *a = &stack[depth - 1];
    lv_obj_clear_flag(a->screen, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(a->screen);
    if (a->cbs.on_resume) {
        a->cbs.on_resume(a->screen, a->state);
    }
}

/* ---------- Public API ---------- */

void ui_activity_init(void)
{
    memset(stack, 0, sizeof(stack));
    depth = 0;
    ESP_LOGI(TAG, "Activity system initialized");
}

bool ui_activity_push(uint8_t app_id, uint8_t screen_id,
                      const activity_cbs_t *cbs, void *intent_data)
{
    if (!cbs || !cbs->on_create) {
        ESP_LOGE(TAG, "Cannot push activity without on_create callback");
        return false;
    }

    /* Pause current top */
    pause_top();

    /* If stack is full, destroy entry [1] and shift down */
    if (depth >= ACTIVITY_STACK_MAX) {
        destroy_entry(&stack[1]);
        /* Shift entries [2..depth-1] down by one */
        for (int i = 1; i < depth - 1; i++) {
            stack[i] = stack[i + 1];
        }
        depth--;
        ESP_LOGW(TAG, "Stack full, evicted entry [1]");
    }

    /* Create new screen */
    lv_obj_t *scr = create_screen();
    activity_t *a = &stack[depth];
    memset(a, 0, sizeof(activity_t));
    a->app_id    = app_id;
    a->screen_id = screen_id;
    a->screen    = scr;
    a->cbs       = *cbs;
    a->state     = NULL;

    depth++;

    /* Call on_create */
    a->cbs.on_create(scr, intent_data);

    /* Show it */
    lv_scr_load(scr);

    ESP_LOGI(TAG, "Pushed activity app=%d scr=%d (depth=%d)", app_id, screen_id, depth);
    return true;
}

bool ui_activity_pop(void)
{
    if (depth <= 1) {
        ESP_LOGD(TAG, "Cannot pop last activity (launcher)");
        return false;
    }

    /* Copy old top so we can destroy it after loading the new screen.
     * This prevents a flash-of-empty-screen glitch. */
    activity_t old_top = stack[depth - 1];

    /* Reduce depth and resume the now-top activity first */
    depth--;
    resume_top();   /* lv_scr_load(stack[depth-1].screen) */

    /* Now it's safe to destroy the old screen (no longer active) */
    if (old_top.cbs.on_destroy) {
        old_top.cbs.on_destroy(old_top.screen, old_top.state);
    }
    lv_obj_del(old_top.screen);

    /* Clear the vacated slot */
    memset(&stack[depth], 0, sizeof(activity_t));

    ESP_LOGI(TAG, "Popped, new depth=%d", depth);
    return true;
}

void ui_activity_pop_to_home(void)
{
    while (depth > 1) {
        destroy_entry(&stack[depth - 1]);
        depth--;
    }
    resume_top();
    ESP_LOGI(TAG, "Popped to home");
}

const activity_t *ui_activity_current(void)
{
    if (depth == 0) return NULL;
    return &stack[depth - 1];
}

uint8_t ui_activity_depth(void)
{
    return depth;
}

void ui_activity_set_state(void *state)
{
    if (depth == 0) return;
    stack[depth - 1].state = state;
}

void ui_activity_recreate_all(void)
{
    /* Recreate each activity in-place so layouts adapt to new display dimensions.
     * Called with LVGL mutex held (from EVT_DISPLAY_ROTATED handler). */
    for (int i = 0; i < depth; i++) {
        activity_t *a = &stack[i];
        if (!a->screen || !a->cbs.on_create) continue;

        /* Let the app clean up its state */
        if (a->cbs.on_destroy) {
            a->cbs.on_destroy(a->screen, a->state);
        }
        a->state = NULL;

        /* Remove all children (layout objects), keep screen itself */
        lv_obj_clean(a->screen);

        /* Re-apply base screen styling */
        const cyberdeck_theme_t *t = ui_theme_get();
        lv_obj_set_style_bg_color(a->screen, t->bg_dark, 0);
        lv_obj_set_style_bg_opa(a->screen, LV_OPA_COVER, 0);
        apply_screen_padding(a->screen);

        /* Recreate layout with current display dimensions */
        a->cbs.on_create(a->screen, NULL);

        ESP_LOGD(TAG, "Recreated activity app=%d scr=%d", a->app_id, a->screen_id);
    }

    /* Reload the top screen */
    if (depth > 0) {
        lv_scr_load(stack[depth - 1].screen);
    }

    ESP_LOGI(TAG, "All %d activities recreated for rotation", depth);
}
