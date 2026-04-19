/* deck_bridge_ui_activity — push/pop activity stack.
 *
 * Slot 0 is reserved for the launcher and never popped. Stack depth
 * cap = DECK_BRIDGE_UI_ACTIVITY_MAX (4). On overflow, slot[1] is
 * evicted (calling its on_destroy), the remaining tail shifts down,
 * and the new activity is pushed at the new top.
 *
 * Lifecycle ordering:
 *   push: prev.on_pause → new.on_create → lv_scr_load(new) → new.on_resume
 *   pop:  top.on_pause → lv_scr_load(prev) → prev.on_resume → top.on_destroy
 *
 * `recreate_all` (called by F26.9 on rotation) destroys + recreates
 * every screen in stack order, preserving the top-of-stack focus.
 */

#include "deck_bridge_ui.h"
#include "deck_bridge_ui_internal.h"

#include "lvgl.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "bridge_ui.act";

static deck_bridge_ui_activity_t s_stack[DECK_BRIDGE_UI_ACTIVITY_MAX];
static size_t                    s_depth = 0;

static lv_obj_t *new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    return scr;
}

static void invoke_lifecycle(deck_bridge_ui_lifecycle_cb_t cb,
                              deck_bridge_ui_activity_t *act,
                              void *intent_data)
{
    if (cb) cb(act, intent_data);
}

deck_sdi_err_t deck_bridge_ui_activity_push(uint16_t app_id, uint16_t screen_id,
                                             const deck_bridge_ui_lifecycle_t *cbs,
                                             void *intent_data)
{
    if (!cbs) return DECK_SDI_ERR_INVALID_ARG;
    if (!deck_bridge_ui_lock(500)) return DECK_SDI_ERR_BUSY;

    /* Pause the current top. */
    if (s_depth > 0) {
        invoke_lifecycle(s_stack[s_depth - 1].cbs.on_pause,
                          &s_stack[s_depth - 1], NULL);
    }

    /* Evict slot[1] if at capacity (slot 0 is launcher, never popped). */
    if (s_depth >= DECK_BRIDGE_UI_ACTIVITY_MAX) {
        deck_bridge_ui_activity_t evicted = s_stack[1];
        ESP_LOGW(TAG, "stack full — evicting slot[1] app_id=%u",
                 (unsigned)evicted.app_id);
        invoke_lifecycle(evicted.cbs.on_destroy, &evicted, NULL);
        if (evicted.lvgl_screen) lv_obj_del((lv_obj_t *)evicted.lvgl_screen);
        for (size_t i = 1; i + 1 < DECK_BRIDGE_UI_ACTIVITY_MAX; i++) {
            s_stack[i] = s_stack[i + 1];
        }
        s_depth--;
    }

    deck_bridge_ui_activity_t *slot = &s_stack[s_depth++];
    memset(slot, 0, sizeof(*slot));
    slot->app_id      = app_id;
    slot->screen_id   = screen_id;
    slot->cbs         = *cbs;
    slot->lvgl_screen = new_screen();

    invoke_lifecycle(slot->cbs.on_create, slot, intent_data);
    if (slot->lvgl_screen) lv_scr_load((lv_obj_t *)slot->lvgl_screen);
    invoke_lifecycle(slot->cbs.on_resume, slot, NULL);

    deck_bridge_ui_unlock();
    ESP_LOGI(TAG, "push app=%u screen=%u (depth=%u)",
             (unsigned)app_id, (unsigned)screen_id, (unsigned)s_depth);
    return DECK_SDI_OK;
}

deck_sdi_err_t deck_bridge_ui_activity_pop(void)
{
    if (s_depth <= 1) return DECK_SDI_ERR_INVALID_ARG; /* never pop launcher */
    if (!deck_bridge_ui_lock(500)) return DECK_SDI_ERR_BUSY;

    deck_bridge_ui_activity_t *top  = &s_stack[s_depth - 1];
    deck_bridge_ui_activity_t *prev = &s_stack[s_depth - 2];

    invoke_lifecycle(top->cbs.on_pause, top, NULL);

    /* Load prev BEFORE destroying top — prevents disp->act_scr from
     * pointing at a deleted lv_obj. */
    if (prev->lvgl_screen) lv_scr_load((lv_obj_t *)prev->lvgl_screen);
    invoke_lifecycle(prev->cbs.on_resume, prev, NULL);

    invoke_lifecycle(top->cbs.on_destroy, top, NULL);
    if (top->lvgl_screen) lv_obj_del((lv_obj_t *)top->lvgl_screen);
    memset(top, 0, sizeof(*top));
    s_depth--;

    deck_bridge_ui_unlock();
    ESP_LOGI(TAG, "pop (depth=%u)", (unsigned)s_depth);
    return DECK_SDI_OK;
}

deck_sdi_err_t deck_bridge_ui_activity_pop_to_home(void)
{
    if (s_depth <= 1) return DECK_SDI_OK;
    if (!deck_bridge_ui_lock(500)) return DECK_SDI_ERR_BUSY;

    /* Load launcher first to avoid dangling act_scr. */
    if (s_stack[0].lvgl_screen) lv_scr_load((lv_obj_t *)s_stack[0].lvgl_screen);

    /* Destroy from top down (excluding launcher). */
    while (s_depth > 1) {
        deck_bridge_ui_activity_t *top = &s_stack[s_depth - 1];
        invoke_lifecycle(top->cbs.on_destroy, top, NULL);
        if (top->lvgl_screen) lv_obj_del((lv_obj_t *)top->lvgl_screen);
        memset(top, 0, sizeof(*top));
        s_depth--;
    }
    invoke_lifecycle(s_stack[0].cbs.on_resume, &s_stack[0], NULL);

    deck_bridge_ui_unlock();
    ESP_LOGI(TAG, "pop_to_home (depth=1)");
    return DECK_SDI_OK;
}

deck_bridge_ui_activity_t *deck_bridge_ui_activity_current(void)
{
    return s_depth > 0 ? &s_stack[s_depth - 1] : NULL;
}

size_t deck_bridge_ui_activity_depth(void) { return s_depth; }

deck_bridge_ui_activity_t *deck_bridge_ui_activity_at(size_t idx)
{
    if (idx >= s_depth) return NULL;
    return &s_stack[idx];
}

void deck_bridge_ui_activity_set_state(void *state)
{
    if (s_depth > 0) s_stack[s_depth - 1].state = state;
}

void deck_bridge_ui_activity_recreate_all(void)
{
    if (s_depth == 0) return;
    if (!deck_bridge_ui_lock(500)) return;

    for (size_t i = 0; i < s_depth; i++) {
        deck_bridge_ui_activity_t *a = &s_stack[i];
        invoke_lifecycle(a->cbs.on_destroy, a, NULL);
        if (a->lvgl_screen) lv_obj_del((lv_obj_t *)a->lvgl_screen);
        a->lvgl_screen = new_screen();
        a->state = NULL;
        invoke_lifecycle(a->cbs.on_create, a, NULL);
    }
    if (s_stack[s_depth - 1].lvgl_screen) {
        lv_scr_load((lv_obj_t *)s_stack[s_depth - 1].lvgl_screen);
    }
    invoke_lifecycle(s_stack[s_depth - 1].cbs.on_resume,
                      &s_stack[s_depth - 1], NULL);

    deck_bridge_ui_unlock();
    ESP_LOGI(TAG, "recreate_all (depth=%u)", (unsigned)s_depth);
}
