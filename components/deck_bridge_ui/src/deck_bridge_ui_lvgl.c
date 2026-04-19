/* deck_bridge_ui_lvgl — LVGL stack wiring.
 *
 * - LVGL is initialized once on first register.
 * - A FreeRTOS task on Core 1 calls lv_timer_handler() periodically.
 * - A second timer task on Core 1 ticks lv_tick_inc(5) every 5 ms.
 * - The display driver flushes via esp_lcd_panel_draw_bitmap on the
 *   panel handle held by the SDI display driver.
 * - The touch indev driver polls deck_sdi_touch_read.
 * - All non-LVGL-task code MUST take the recursive mutex via
 *   deck_bridge_ui_lock / _unlock before touching any lv_obj_*.
 */

#include "deck_bridge_ui.h"
#include "deck_bridge_ui_internal.h"

#include "drivers/deck_sdi_display.h"
#include "drivers/deck_sdi_touch.h"

#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"

#include <string.h>

static const char *TAG = "bridge_ui.lvgl";

#define LVGL_TASK_STACK_SIZE   (8 * 1024)
#define LVGL_TASK_PRIORITY     2
#define LVGL_TASK_CORE         1
#define LVGL_TICK_PERIOD_MS    5
#define LVGL_HANDLER_PERIOD_MS 10
/* Draw buffer = 1/10 of the panel; PSRAM-backed to avoid hammering DRAM.
 * Two banks for double-buffered partial flushing. */
#define LVGL_DRAW_BUF_LINES    48     /* 800 × 48 px × 2 bytes ≈ 75 KB per bank */

static SemaphoreHandle_t  s_ui_mutex = NULL;
static TaskHandle_t       s_lvgl_task = NULL;
static esp_timer_handle_t s_tick_timer = NULL;
static lv_disp_draw_buf_t s_disp_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_indev_drv;
static lv_disp_t         *s_disp = NULL;
static lv_color_t        *s_buf_a = NULL;
static lv_color_t        *s_buf_b = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static bool               s_inited  = false;

bool deck_bridge_ui_lock(uint32_t timeout_ms)
{
    if (!s_ui_mutex) return false;
    TickType_t to = (timeout_ms == 0)
                      ? portMAX_DELAY
                      : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_ui_mutex, to) == pdTRUE;
}

void deck_bridge_ui_unlock(void)
{
    if (s_ui_mutex) xSemaphoreGiveRecursive(s_ui_mutex);
}

bool deck_bridge_ui_lvgl_is_ready(void)
{
    return s_inited;
}

/* ---------- LVGL display flush ---------- */

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                           lv_color_t *color_map)
{
    if (s_panel) {
        esp_lcd_panel_draw_bitmap(s_panel,
                                   area->x1, area->y1,
                                   area->x2 + 1, area->y2 + 1,
                                   color_map);
    }
    lv_disp_flush_ready(drv);
}

/* ---------- LVGL touch read ---------- */

static void lvgl_indev_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    bool pressed = false;
    uint16_t x = 0, y = 0;
    if (deck_sdi_touch_read(&pressed, &x, &y) == DECK_SDI_OK && pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ---------- LVGL tick + handler tasks ---------- */

static void lv_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LVGL task running on core %d", xPortGetCoreID());
    while (1) {
        if (deck_bridge_ui_lock(0)) {
            uint32_t next_ms = lv_timer_handler();
            deck_bridge_ui_unlock();
            if (next_ms > 100) next_ms = 100;
            vTaskDelay(pdMS_TO_TICKS(next_ms));
        } else {
            vTaskDelay(pdMS_TO_TICKS(LVGL_HANDLER_PERIOD_MS));
        }
    }
}

deck_sdi_err_t deck_bridge_ui_lvgl_init(void)
{
    if (s_inited) return DECK_SDI_OK;

    deck_sdi_err_t r = deck_sdi_display_init();
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "display init: %s", deck_sdi_strerror(r));
        return r;
    }
    r = deck_sdi_touch_init();
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "touch init: %s", deck_sdi_strerror(r));
        return r;
    }

    /* Pull the panel handle from the SDI display driver context. We
     * don't expose it through the SDI vtable today; reach in through
     * the global owned by deck_sdi_display_esp32.c. The bridge_ui +
     * SDI-display bond is reference-platform-specific; on a different
     * platform the bridge_ui implementation is also different. */
    extern esp_lcd_panel_handle_t deck_sdi_display_panel_handle(void);
    s_panel = deck_sdi_display_panel_handle();
    if (!s_panel) {
        ESP_LOGE(TAG, "display panel handle NULL — display not initialized");
        return DECK_SDI_ERR_FAIL;
    }

    s_ui_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_ui_mutex) return DECK_SDI_ERR_NO_MEMORY;

    lv_init();

    uint16_t W = deck_sdi_display_width();
    uint16_t H = deck_sdi_display_height();
    if (W == 0 || H == 0) {
        ESP_LOGE(TAG, "implausible panel dims %ux%u", W, H);
        return DECK_SDI_ERR_FAIL;
    }

    /* Two PSRAM buffers, partial-render. */
    size_t buf_pixels = (size_t)W * LVGL_DRAW_BUF_LINES;
    size_t buf_bytes  = buf_pixels * sizeof(lv_color_t);
    s_buf_a = heap_caps_aligned_alloc(64, buf_bytes,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_buf_b = heap_caps_aligned_alloc(64, buf_bytes,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf_a || !s_buf_b) {
        ESP_LOGE(TAG, "draw buffer alloc failed (%u bytes each)",
                 (unsigned)buf_bytes);
        return DECK_SDI_ERR_NO_MEMORY;
    }
    lv_disp_draw_buf_init(&s_disp_buf, s_buf_a, s_buf_b, buf_pixels);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = W;
    s_disp_drv.ver_res  = H;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.draw_buf = &s_disp_buf;
    s_disp_drv.full_refresh = false;
    /* NOTE: sw_rotate=1 would be required for LVGL to rotate the
     * framebuffer, but the default LVGL sw-rotate path expects a full
     * screen-size draw buffer; we only allocate 48 lines (partial
     * refresh). Enabling sw_rotate with partial buffers triggers boot
     * crashes in LVGL 8.4. The indev coordinate transform (which reads
     * disp->driver->rotated) works regardless of sw_rotate — so touch
     * rotates correctly as long as `lv_disp_set_rotation` runs. */
    s_disp = lv_disp_drv_register(&s_disp_drv);
    if (!s_disp) {
        ESP_LOGE(TAG, "lv_disp_drv_register failed");
        return DECK_SDI_ERR_FAIL;
    }

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type    = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = lvgl_indev_read_cb;
    if (!lv_indev_drv_register(&s_indev_drv)) {
        ESP_LOGE(TAG, "lv_indev_drv_register failed");
        return DECK_SDI_ERR_FAIL;
    }

    /* Tick source — esp_timer fires lv_tick_inc every 5 ms. */
    const esp_timer_create_args_t tick_args = {
        .callback = lv_tick_cb,
        .name     = "lv_tick",
    };
    if (esp_timer_create(&tick_args, &s_tick_timer) != ESP_OK ||
        esp_timer_start_periodic(s_tick_timer,
                                  LVGL_TICK_PERIOD_MS * 1000ULL) != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer init failed");
        return DECK_SDI_ERR_FAIL;
    }

    /* LVGL task on Core 1 (pinned away from the WiFi/event-loop core). */
    BaseType_t br = xTaskCreatePinnedToCore(lvgl_task, "lvgl",
                                              LVGL_TASK_STACK_SIZE, NULL,
                                              LVGL_TASK_PRIORITY, &s_lvgl_task,
                                              LVGL_TASK_CORE);
    if (br != pdPASS) {
        ESP_LOGE(TAG, "LVGL task create failed");
        return DECK_SDI_ERR_FAIL;
    }

    /* Paint a single black background so the panel isn't garbage. */
    if (deck_bridge_ui_lock(200)) {
        lv_obj_t *scr = lv_scr_act();
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
        deck_bridge_ui_unlock();
    }

    s_inited = true;
    ESP_LOGI(TAG, "LVGL init OK (%ux%u, draw_buf=%uKB×2 PSRAM, task on core %d)",
             W, H, (unsigned)(buf_bytes / 1024), LVGL_TASK_CORE);
    return DECK_SDI_OK;
}

/* ---------- rotation ---------- */

static deck_bridge_ui_rotation_t s_rotation = DECK_BRIDGE_UI_ROT_0;

/* Worker that actually applies the rotation + recreates activities.
 * Scheduled via lv_async_call so it runs on a clean LVGL tick *after*
 * the triggering event handler has returned. Otherwise recreate_all
 * deletes the LVGL screen (and widget tree) whose own event callback
 * we're currently inside — classic use-after-free crash surfaced by
 * tapping a rotation button in Settings. */
static void rotation_apply_async(void *user_data)
{
    deck_bridge_ui_rotation_t rot = (deck_bridge_ui_rotation_t)(uintptr_t)user_data;
    if (!s_inited || !s_disp) return;
    if (!deck_bridge_ui_lock(500)) {
        ESP_LOGW(TAG, "rotation: lock timeout");
        return;
    }
    lv_disp_rot_t lv_rot;
    switch (rot) {
        default:
        case DECK_BRIDGE_UI_ROT_0:   lv_rot = LV_DISP_ROT_NONE; break;
        case DECK_BRIDGE_UI_ROT_90:  lv_rot = LV_DISP_ROT_90;   break;
        case DECK_BRIDGE_UI_ROT_180: lv_rot = LV_DISP_ROT_180;  break;
        case DECK_BRIDGE_UI_ROT_270: lv_rot = LV_DISP_ROT_270;  break;
    }
    /* Software rotation — frame buffer is rotated by LVGL itself (we
     * don't touch the panel's hardware rotation; the RGB peripheral is
     * fixed at 0°). */
    lv_disp_set_rotation(s_disp, lv_rot);
    s_rotation = rot;

    /* Chrome (statusbar + navbar) lives on lv_layer_top with a fixed
     * width baked in at init time from hor_res. After rotation the
     * display dimensions swap; resize + re-align so the chrome spans
     * the new horizontal axis. */
    deck_bridge_ui_statusbar_relayout();
    deck_bridge_ui_navbar_relayout();

    deck_bridge_ui_unlock();

    /* Rebuild every activity for the new dimensions. recreate_all takes
     * the lock itself. Safe here because (a) we're already on the LVGL
     * task via lv_async_call so the triggering event handler has returned,
     * and (b) adapters that cache state (e.g. deck_shell_deck_apps) now
     * look their data up by act->app_id instead of relying on a->state
     * surviving the NULL reset inside recreate_all. */
    deck_bridge_ui_activity_recreate_all();
    ESP_LOGI(TAG, "rotation set to %d", (int)rot);
}

deck_sdi_err_t deck_bridge_ui_set_rotation(deck_bridge_ui_rotation_t rot)
{
    if (!s_inited || !s_disp) return DECK_SDI_ERR_FAIL;
    if (rot > DECK_BRIDGE_UI_ROT_270) return DECK_SDI_ERR_INVALID_ARG;
    /* Defer the work — callers may be inside an LVGL event cb whose
     * widget lives on the screen we're about to destroy. */
    lv_async_call(rotation_apply_async, (void *)(uintptr_t)rot);
    return DECK_SDI_OK;
}

deck_bridge_ui_rotation_t deck_bridge_ui_get_rotation(void)
{
    return s_rotation;
}
