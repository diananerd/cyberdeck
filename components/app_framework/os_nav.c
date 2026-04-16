/*
 * CyberDeck — OS Navigation API (D4)
 * Thin wrappers over ui_activity stack operations.
 */

#include "os_nav.h"
#include "ui_activity.h"
#include "esp_log.h"

static const char *TAG = "os_nav";

esp_err_t os_view_push(app_id_t app_id, uint8_t screen_id,
                       const view_cbs_t  *cbs,
                       const view_args_t *args)
{
    if (!ui_activity_push(app_id, screen_id, cbs, args)) {
        ESP_LOGE(TAG, "os_view_push failed (app=%u scr=%u)", (unsigned)app_id, screen_id);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void os_view_pop(void)
{
    ui_activity_pop();
}

void os_view_pop_to_root(void)
{
    /* Pop until we're at the root view of the current app (screen_id == 0).
     * This does NOT leave the app — it just unwinds its own sub-screen stack. */
    while (ui_activity_depth() > 1) {
        const activity_t *top = ui_activity_current();
        if (!top || top->screen_id == 0) break;
        if (!ui_activity_pop()) break;
    }
}

void os_view_home(void)
{
    ui_activity_suspend_to_home();
}
