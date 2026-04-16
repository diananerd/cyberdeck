/*
 * CyberDeck — Activity stack manager
 *
 * D1: on_create returns void* view_state.
 * D3: stack max=8, push fails instead of evicting when full.
 * D6: view_args_t passed to on_create; owned data freed by OS after on_create.
 * J3: view_cbs_t con void *app_data (L1) en todos los callbacks.
 * J5: app_data se obtiene de os_process_get(app_id) al pushear; se pasa
 *     a todos los callbacks. NULL si el proceso no está registrado.
 */

#include "ui_activity.h"
#include "os_process.h"
#include "ui_statusbar.h"
#include "ui_navbar.h"
#include "ui_theme.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "activity";

static activity_t              stack[ACTIVITY_STACK_MAX];
static uint8_t                 depth        = 0;
static bool                    s_nav_lock   = false;
static ui_activity_close_hook_fn s_close_hook = NULL;

void ui_activity_set_close_hook(ui_activity_close_hook_fn fn)
{
    s_close_hook = fn;
}

/* Fire the close hook for every unique app_id in ids[0..count-1].
 * Skips APP_ID_LAUNCHER — the launcher is never truly "closed". */
static void fire_close_hook(const app_id_t *ids, uint8_t count)
{
    if (!s_close_hook) return;
    for (uint8_t i = 0; i < count; i++) {
        if (ids[i] == APP_ID_LAUNCHER || ids[i] == APP_ID_INVALID) continue;
        s_close_hook(ids[i]);
    }
}

/* Add id to seen[] (max capacity) if not already present. */
static void track_id(app_id_t *seen, uint8_t *n, uint8_t max, app_id_t id)
{
    if (*n >= max) return;
    for (uint8_t i = 0; i < *n; i++) {
        if (seen[i] == id) return;
    }
    seen[(*n)++] = id;
}

/* ---------- Internal helpers ---------- */

static void apply_screen_padding(lv_obj_t *scr)
{
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
        a->cbs.on_destroy(a->screen, a->view_state, a->app_data);
    }
    lv_obj_del(a->screen);
    a->screen     = NULL;
    a->view_state = NULL;
    /* app_data NOT cleared here — owned by os_process_t until on_terminate */
    ESP_LOGD(TAG, "Destroyed activity app=%d scr=%d", (int)a->app_id, a->screen_id);
}

static void pause_top(void)
{
    if (depth == 0) return;
    activity_t *a = &stack[depth - 1];
    if (a->cbs.on_pause) {
        a->cbs.on_pause(a->screen, a->view_state, a->app_data);
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
        a->cbs.on_resume(a->screen, a->view_state, a->app_data);
    }
}

/* ---------- Public API ---------- */

void ui_activity_init(void)
{
    memset(stack, 0, sizeof(stack));
    depth = 0;
    ESP_LOGI(TAG, "Activity system initialized (max=%d)", ACTIVITY_STACK_MAX);
}

bool ui_activity_push(app_id_t app_id, uint8_t screen_id,
                      const view_cbs_t *cbs, const view_args_t *args)
{
    if (!cbs || !cbs->on_create) {
        ESP_LOGE(TAG, "Cannot push activity without on_create callback");
        return false;
    }

    /* D3: fail instead of silently evicting when stack is full */
    if (depth >= ACTIVITY_STACK_MAX) {
        ESP_LOGE(TAG, "Activity stack full (max=%d) — push rejected for app=%d",
                 ACTIVITY_STACK_MAX, (int)app_id);
        return false;
    }

    /* Pause current top */
    pause_top();

    /* J5: get app_data (L1) from process registry — NULL if not registered */
    os_process_t *proc = os_process_get(app_id);
    void *app_data = proc ? proc->app_data : NULL;

    /* Create new screen */
    lv_obj_t *scr = create_screen();
    activity_t *a = &stack[depth];
    memset(a, 0, sizeof(activity_t));
    a->app_id     = app_id;
    a->screen_id  = screen_id;
    a->screen     = scr;
    a->cbs        = *cbs;
    a->app_data   = app_data;
    a->view_state = NULL;

    depth++;

    /* on_create returns view_state* (L2) — measure heap delta */
    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    a->view_state = a->cbs.on_create(scr, args, app_data);
    size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    a->heap_used = (heap_before > heap_after) ? (heap_before - heap_after) : 0;

    /* Update process view count */
    if (proc) {
        os_process_update_counts(app_id, depth, proc->task_count);
    }

    /* D6: if args are owned, free the data buffer now */
    if (args && args->owned && args->data) {
        free(args->data);
    }

    /* Show it */
    lv_scr_load(scr);

    ESP_LOGI(TAG, "Pushed activity app=%d scr=%d (depth=%d)",
             (int)app_id, screen_id, depth);
    return true;
}

bool ui_activity_pop(void)
{
    if (depth <= 1) {
        ESP_LOGD(TAG, "Cannot pop last activity (launcher)");
        return false;
    }

    activity_t old_top = stack[depth - 1];

    depth--;
    resume_top();

    if (old_top.cbs.on_destroy) {
        old_top.cbs.on_destroy(old_top.screen, old_top.view_state, old_top.app_data);
    }
    lv_obj_del(old_top.screen);

    memset(&stack[depth], 0, sizeof(activity_t));

    ESP_LOGI(TAG, "Popped, new depth=%d", depth);
    return true;
}

void ui_activity_set_nav_lock(bool locked)
{
    s_nav_lock = locked;
    ESP_LOGI(TAG, "Nav lock: %s", locked ? "ON" : "OFF");
}

void ui_activity_suspend_to_home(void)
{
    if (s_nav_lock) {
        ESP_LOGD(TAG, "suspend_to_home blocked (nav_lock)");
        return;
    }
    if (depth <= 1) return;  /* already at launcher */

    /* Pause the current top and hide it */
    activity_t *top = &stack[depth - 1];
    if (top->cbs.on_pause) {
        top->cbs.on_pause(top->screen, top->view_state, top->app_data);
    }
    lv_obj_add_flag(top->screen, LV_OBJ_FLAG_HIDDEN);

    /* Show launcher and call its on_resume */
    lv_obj_clear_flag(stack[0].screen, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(stack[0].screen);
    if (stack[0].cbs.on_resume) {
        stack[0].cbs.on_resume(stack[0].screen, stack[0].view_state, stack[0].app_data);
    }

    ESP_LOGI(TAG, "Suspended to home (depth=%d, apps in background)", depth);
}

void ui_activity_pop_to_home(void)
{
    if (s_nav_lock) {
        ESP_LOGD(TAG, "pop_to_home blocked (nav_lock)");
        return;
    }
    if (depth <= 1) {
        return;
    }

    /* Collect unique app_ids before destroying so we can fire the close hook. */
    app_id_t closed_ids[ACTIVITY_STACK_MAX];
    uint8_t  closed_count = 0;
    for (uint8_t i = 1; i < depth; i++) {
        track_id(closed_ids, &closed_count, ACTIVITY_STACK_MAX, stack[i].app_id);
    }

    lv_obj_clear_flag(stack[0].screen, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(stack[0].screen);

    while (depth > 1) {
        destroy_entry(&stack[depth - 1]);
        memset(&stack[depth - 1], 0, sizeof(activity_t));
        depth--;
    }

    if (stack[0].cbs.on_resume) {
        stack[0].cbs.on_resume(stack[0].screen, stack[0].view_state, stack[0].app_data);
    }

    fire_close_hook(closed_ids, closed_count);
    ESP_LOGI(TAG, "Popped to home, final depth=%d", depth);
}

bool ui_activity_raise(app_id_t app_id)
{
    /* Find the app (search from top to handle duplicates gracefully) */
    int idx = -1;
    for (int i = (int)depth - 1; i >= 1; i--) {
        if (stack[i].app_id == app_id) { idx = i; break; }
    }
    if (idx < 0) return false;              /* not in stack */
    if (idx == (int)depth - 1) return true; /* already on top */

    /* Collect app_ids ABOVE the target that belong to OTHER apps.
     * Sub-screens of the raised app itself are not "closing" — they're being
     * replaced by the root screen of the same app. */
    app_id_t closed_ids[ACTIVITY_STACK_MAX];
    uint8_t  closed_count = 0;
    for (int i = idx + 1; i < (int)depth; i++) {
        if (stack[i].app_id != app_id) {
            track_id(closed_ids, &closed_count, ACTIVITY_STACK_MAX, stack[i].app_id);
        }
    }

    /* Load the target screen first (safe before destroying those above it) */
    lv_obj_clear_flag(stack[idx].screen, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(stack[idx].screen);

    /* Destroy everything above the target */
    while ((int)depth - 1 > idx) {
        destroy_entry(&stack[depth - 1]);
        memset(&stack[depth - 1], 0, sizeof(activity_t));
        depth--;
    }

    /* Resume the raised app */
    if (stack[depth - 1].cbs.on_resume) {
        stack[depth - 1].cbs.on_resume(stack[depth - 1].screen, stack[depth - 1].view_state, stack[depth - 1].app_data);
    }

    fire_close_hook(closed_ids, closed_count);
    ESP_LOGI(TAG, "Raised app=%d to top (depth=%d)", (int)app_id, depth);
    return true;
}

uint8_t ui_activity_list(activity_info_t *buf, uint8_t max)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < depth && n < max; i++) {
        buf[n].app_id    = stack[i].app_id;
        buf[n].screen_id = stack[i].screen_id;
        buf[n].stack_idx = i;
        buf[n].heap_used = stack[i].heap_used;
        n++;
    }
    return n;
}

/* Async trampoline — runs on the LVGL task after the triggering event returns.
 * This avoids destroying our own screen while still inside an event callback. */
static void close_app_async(void *arg)
{
    app_id_t target = (app_id_t)(uintptr_t)arg;

    /* Find the target's stack index (skip launcher at 0) */
    int idx = -1;
    for (int i = 1; i < (int)depth; i++) {
        if (stack[i].app_id == target) { idx = i; break; }
    }
    if (idx < 0) return;  /* already gone */

    /* Collect unique app_ids that will be destroyed (target + anything above it). */
    app_id_t closed_ids[ACTIVITY_STACK_MAX];
    uint8_t  closed_count = 0;
    for (int i = idx; i < (int)depth; i++) {
        track_id(closed_ids, &closed_count, ACTIVITY_STACK_MAX, stack[i].app_id);
    }

    /* Load the screen that will become visible (just below target) */
    lv_obj_clear_flag(stack[idx - 1].screen, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(stack[idx - 1].screen);

    /* Destroy all entries from current top down to and including target */
    while ((int)depth > idx) {
        destroy_entry(&stack[depth - 1]);
        memset(&stack[depth - 1], 0, sizeof(activity_t));
        depth--;
    }

    /* Resume the new top */
    if (depth > 0 && stack[depth - 1].cbs.on_resume) {
        stack[depth - 1].cbs.on_resume(stack[depth - 1].screen, stack[depth - 1].view_state, stack[depth - 1].app_data);
    }

    /* Fire OS-level close hook for each unique app that was closed (H2).
     * Called AFTER on_destroy — safe to kill bg tasks here. */
    fire_close_hook(closed_ids, closed_count);

    ESP_LOGI(TAG, "Closed app=%d, new depth=%d", (int)target, depth);
}

bool ui_activity_close_app(app_id_t app_id)
{
    /* Verify it exists before scheduling */
    for (int i = 1; i < (int)depth; i++) {
        if (stack[i].app_id == app_id) {
            lv_async_call(close_app_async, (void *)(uintptr_t)app_id);
            return true;
        }
    }
    return false;
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

void ui_activity_recreate_all(void)
{
    for (int i = 0; i < depth; i++) {
        activity_t *a = &stack[i];
        if (!a->screen || !a->cbs.on_create) continue;

        if (a->cbs.on_destroy) {
            a->cbs.on_destroy(a->screen, a->view_state, a->app_data);
        }
        a->view_state = NULL;

        lv_obj_clean(a->screen);

        const cyberdeck_theme_t *t = ui_theme_get();
        lv_obj_set_style_bg_color(a->screen, t->bg_dark, 0);
        lv_obj_set_style_bg_opa(a->screen, LV_OPA_COVER, 0);
        apply_screen_padding(a->screen);

        /* D1: on_create returns new view_state* */
        a->view_state = a->cbs.on_create(a->screen, NULL, a->app_data);

        ESP_LOGD(TAG, "Recreated activity app=%d scr=%d", (int)a->app_id, a->screen_id);
    }

    if (depth > 0) {
        lv_scr_load(stack[depth - 1].screen);
    }

    ESP_LOGI(TAG, "All %d activities recreated for rotation", depth);
}
