/*
 * S3 Cyber-Deck — OS Core: Deferred execution
 *
 * os_defer usa esp_timer one-shot. Cada llamada crea y destruye su propio timer —
 * adecuado para usos ocasionales. No usar en hot paths (crear muchos timers por segundo).
 *
 * os_ui_post delega en lv_async_call, que encola el callback para el siguiente tick
 * del LVGL task. lv_async_call es thread-safe y no requiere mutex externo.
 */

#include "os_defer.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdlib.h>

static const char *TAG = "os_defer";

/* ---- os_defer ---- */

typedef struct {
    os_fn_t              fn;
    void                *arg;
    esp_timer_handle_t   timer;
} defer_ctx_t;

static void defer_timer_cb(void *arg)
{
    defer_ctx_t *ctx = (defer_ctx_t *)arg;
    ctx->fn(ctx->arg);
    esp_timer_delete(ctx->timer);
    free(ctx);
}

esp_err_t os_defer(os_fn_t fn, void *arg, uint32_t delay_ms)
{
    if (!fn) return ESP_ERR_INVALID_ARG;

    defer_ctx_t *ctx = malloc(sizeof(defer_ctx_t));
    if (!ctx) return ESP_ERR_NO_MEM;

    ctx->fn  = fn;
    ctx->arg = arg;

    esp_timer_create_args_t timer_args = {
        .callback        = defer_timer_cb,
        .arg             = ctx,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "os_defer",
    };

    esp_err_t ret = esp_timer_create(&timer_args, &ctx->timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create defer timer: %s", esp_err_to_name(ret));
        free(ctx);
        return ret;
    }

    ret = esp_timer_start_once(ctx->timer, (uint64_t)delay_ms * 1000ULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start defer timer: %s", esp_err_to_name(ret));
        esp_timer_delete(ctx->timer);
        free(ctx);
        return ret;
    }

    return ESP_OK;
}

/* ---- os_ui_post ---- */

void os_ui_post(os_fn_t fn, void *arg)
{
    /* lv_async_call signature: void (*cb)(void *arg)
     * Coincide exactamente con os_fn_t — no se necesita wrapper. */
    lv_async_call((lv_async_cb_t)fn, arg);
}
