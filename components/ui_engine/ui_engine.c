/*
 * CyberDeck — LVGL porting layer
 * Adapted from Waveshare ESP32-S3-Touch-LCD-4.3 example (lvgl_port.c)
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"
#include "os_task.h"
#include "ui_engine.h"

static const char *TAG = "ui_engine";
static SemaphoreHandle_t lvgl_mux;
static TaskHandle_t lvgl_task_handle = NULL;
static lv_disp_t *s_disp = NULL;
static uint8_t s_rotation = 0;  /* 0=landscape, 1=portrait */

/* --- Flush callbacks (anti-tearing) --- */

#if UI_ENGINE_AVOID_TEAR_ENABLE && UI_ENGINE_DIRECT_MODE && (UI_ENGINE_ROTATION_DEGREE == 0)

static void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;

    if (lv_disp_flush_is_last(drv)) {
        esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
        ulTaskNotifyValueClear(NULL, ULONG_MAX);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    lv_disp_flush_ready(drv);
}

#elif UI_ENGINE_AVOID_TEAR_ENABLE && UI_ENGINE_FULL_REFRESH && (UI_ENGINE_LCD_RGB_BUFFER_NUMS == 2)

static void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;

    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
    ulTaskNotifyValueClear(NULL, ULONG_MAX);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    lv_disp_flush_ready(drv);
}

#elif UI_ENGINE_AVOID_TEAR_ENABLE && UI_ENGINE_FULL_REFRESH && (UI_ENGINE_LCD_RGB_BUFFER_NUMS == 3) && (UI_ENGINE_ROTATION_DEGREE == 0)

static void *rgb_last_buf = NULL;
static void *rgb_next_buf = NULL;
static void *flush_next_buf = NULL;

static void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;

    drv->draw_buf->buf1 = color_map;
    drv->draw_buf->buf2 = flush_next_buf;
    flush_next_buf = color_map;

    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
    rgb_next_buf = color_map;

    lv_disp_flush_ready(drv);
}

#else

static void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;

    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
    lv_disp_flush_ready(drv);
}

#endif /* flush callbacks */

/* --- Display init --- */

static lv_disp_t *display_init(esp_lcd_panel_handle_t panel_handle)
{
    static lv_disp_draw_buf_t disp_buf = { 0 };
    static lv_disp_drv_t disp_drv = { 0 };

    void *buf1 = NULL;
    void *buf2 = NULL;
    int buffer_size = 0;

#if UI_ENGINE_AVOID_TEAR_ENABLE
    buffer_size = UI_ENGINE_H_RES * UI_ENGINE_V_RES;
#if (UI_ENGINE_LCD_RGB_BUFFER_NUMS == 3) && (UI_ENGINE_ROTATION_DEGREE == 0) && UI_ENGINE_FULL_REFRESH
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 3, &rgb_last_buf, &buf1, &buf2));
    rgb_next_buf = rgb_last_buf;
    flush_next_buf = buf2;
#else
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &buf1, &buf2));
#endif
#else
    buffer_size = UI_ENGINE_H_RES * CONFIG_EXAMPLE_LVGL_PORT_BUF_HEIGHT;
#if CONFIG_EXAMPLE_LVGL_PORT_BUF_PSRAM
    buf1 = heap_caps_malloc(buffer_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
#else
    buf1 = heap_caps_malloc(buffer_size * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
    assert(buf1);
    ESP_LOGI(TAG, "LVGL buffer size: %dKB", buffer_size * sizeof(lv_color_t) / 1024);
#endif

    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buffer_size);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = UI_ENGINE_H_RES;
    disp_drv.ver_res = UI_ENGINE_V_RES;
    disp_drv.flush_cb = flush_callback;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;

#if UI_ENGINE_FULL_REFRESH
    disp_drv.full_refresh = 1;
#elif UI_ENGINE_DIRECT_MODE
    disp_drv.direct_mode = 1;
#endif

    /* Enable software rotation support */
    disp_drv.sw_rotate = 1;
    disp_drv.rotated = LV_DISP_ROT_NONE;

    return lv_disp_drv_register(&disp_drv);
}

/* --- Touch input --- */

static void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)indev_drv->user_data;
    uint16_t x, y;
    uint8_t cnt = 0;

    esp_lcd_touch_read_data(tp);
    bool pressed = esp_lcd_touch_get_coordinates(tp, &x, &y, NULL, &cnt, 1);

    if (pressed && cnt > 0) {
        /* Pass raw panel coordinates — LVGL sw_rotate (LV_DISP_ROT_90) handles
         * the portrait coordinate transform internally via lv_indev.c.
         * Do NOT transform here; doing so would double-transform and break touch. */
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static lv_indev_t *indev_init(esp_lcd_touch_handle_t tp)
{
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touchpad_read;
    indev_drv.user_data = tp;
    return lv_indev_drv_register(&indev_drv);
}

/* --- Tick --- */

static void tick_increment(void *arg)
{
    lv_tick_inc(UI_ENGINE_TICK_PERIOD_MS);
}

static esp_err_t tick_init(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = &tick_increment,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    return esp_timer_start_periodic(timer, UI_ENGINE_TICK_PERIOD_MS * 1000);
}

/* --- LVGL task --- */

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started on core %d", xPortGetCoreID());

    uint32_t task_delay_ms = UI_ENGINE_TASK_MAX_DELAY_MS;
    while (1) {
        if (ui_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            ui_unlock();
        }
        if (task_delay_ms > UI_ENGINE_TASK_MAX_DELAY_MS) {
            task_delay_ms = UI_ENGINE_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < UI_ENGINE_TASK_MIN_DELAY_MS) {
            task_delay_ms = UI_ENGINE_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

/* --- Public API --- */

esp_err_t ui_engine_init(esp_lcd_panel_handle_t lcd_handle, esp_lcd_touch_handle_t tp_handle)
{
    lv_init();
    ESP_ERROR_CHECK(tick_init());

    lv_disp_t *disp = display_init(lcd_handle);
    assert(disp);
    s_disp = disp;

    if (tp_handle) {
        lv_indev_t *indev = indev_init(tp_handle);
        assert(indev);
    }

    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    assert(lvgl_mux);

    ESP_LOGI(TAG, "Creating LVGL task (stack=%d, prio=%d, core=%d)",
             UI_ENGINE_TASK_STACK_SIZE, UI_ENGINE_TASK_PRIORITY, UI_ENGINE_TASK_CORE);

    os_task_config_t cfg = {
        .name       = "lvgl",
        .fn         = lvgl_task,
        .arg        = NULL,
        .stack_size = UI_ENGINE_TASK_STACK_SIZE,
        .priority   = UI_ENGINE_TASK_PRIORITY,
        .core       = UI_ENGINE_TASK_CORE,
        .owner      = OS_OWNER_SYSTEM,
    };
    esp_err_t ret = os_task_create(&cfg, &lvgl_task_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LVGL task: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool ui_lock(int timeout_ms)
{
    assert(lvgl_mux && "ui_engine_init must be called first");
    const TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lvgl_mux, ticks) == pdTRUE;
}

void ui_unlock(void)
{
    assert(lvgl_mux && "ui_engine_init must be called first");
    xSemaphoreGiveRecursive(lvgl_mux);
}

void ui_engine_set_rotation(uint8_t rotation)
{
    if (!s_disp) return;
    s_rotation = rotation;
    if (rotation == 1) {
        lv_disp_set_rotation(s_disp, LV_DISP_ROT_90);
    } else {
        lv_disp_set_rotation(s_disp, LV_DISP_ROT_NONE);
    }
    ESP_LOGI(TAG, "Display rotation set to %s", rotation ? "portrait" : "landscape");
}

bool ui_engine_notify_vsync(void)
{
    BaseType_t need_yield = pdFALSE;
#if UI_ENGINE_AVOID_TEAR_ENABLE && UI_ENGINE_FULL_REFRESH && (UI_ENGINE_LCD_RGB_BUFFER_NUMS == 3) && (UI_ENGINE_ROTATION_DEGREE == 0)
    if (rgb_next_buf != rgb_last_buf) {
        flush_next_buf = rgb_last_buf;
        rgb_last_buf = rgb_next_buf;
    }
#elif UI_ENGINE_AVOID_TEAR_ENABLE
    if (lvgl_task_handle) {
        xTaskNotifyFromISR(lvgl_task_handle, ULONG_MAX, eNoAction, &need_yield);
    }
#endif
    return (need_yield == pdTRUE);
}
