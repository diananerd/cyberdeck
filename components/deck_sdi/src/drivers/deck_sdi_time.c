#include "drivers/deck_sdi_time.h"
#include "deck_sdi_registry.h"

#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <time.h>
#include <sys/time.h>

static const char *TAG = "sdi.time";

static bool s_wall_set = false;

static int64_t time_monotonic_us(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time();
}

static int64_t time_wall_epoch_s(void *ctx)
{
    (void)ctx;
    if (!s_wall_set) return 0;
    time_t now = 0;
    time(&now);
    return (int64_t)now;
}

static deck_sdi_err_t time_set_wall_epoch_s(void *ctx, int64_t epoch_s)
{
    (void)ctx;
    if (epoch_s < 0) return DECK_SDI_ERR_INVALID_ARG;
    struct timeval tv = { .tv_sec = (time_t)epoch_s, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) return DECK_SDI_ERR_IO;
    s_wall_set = true;
    return DECK_SDI_OK;
}

static bool time_wall_is_set(void *ctx)
{
    (void)ctx;
    return s_wall_set;
}

static const deck_sdi_time_vtable_t s_vtable = {
    .monotonic_us      = time_monotonic_us,
    .wall_epoch_s      = time_wall_epoch_s,
    .set_wall_epoch_s  = time_set_wall_epoch_s,
    .wall_is_set       = time_wall_is_set,
};

deck_sdi_err_t deck_sdi_time_register(void)
{
    const deck_sdi_driver_t driver = {
        .name    = "system.time",
        .id      = DECK_SDI_DRIVER_TIME,
        .version = "1.0.0",
        .vtable  = &s_vtable,
        .ctx     = NULL,
    };
    return deck_sdi_register(&driver);
}

/* ---------- wrappers ---------- */

static const deck_sdi_time_vtable_t *time_vt(void **ctx_out)
{
    const deck_sdi_driver_t *d = deck_sdi_lookup(DECK_SDI_DRIVER_TIME);
    if (!d) return NULL;
    if (ctx_out) *ctx_out = d->ctx;
    return (const deck_sdi_time_vtable_t *)d->vtable;
}

int64_t deck_sdi_time_monotonic_us(void)
{ void *c; const deck_sdi_time_vtable_t *v = time_vt(&c); return v ? v->monotonic_us(c) : 0; }
int64_t deck_sdi_time_wall_epoch_s(void)
{ void *c; const deck_sdi_time_vtable_t *v = time_vt(&c); return v ? v->wall_epoch_s(c) : 0; }
deck_sdi_err_t deck_sdi_time_set_wall_epoch_s(int64_t e)
{ void *c; const deck_sdi_time_vtable_t *v = time_vt(&c); return v ? v->set_wall_epoch_s(c, e) : DECK_SDI_ERR_NOT_FOUND; }
bool deck_sdi_time_wall_is_set(void)
{ void *c; const deck_sdi_time_vtable_t *v = time_vt(&c); return v ? v->wall_is_set(c) : false; }

/* ---------- selftest ---------- */

deck_sdi_err_t deck_sdi_time_selftest(void)
{
    int64_t t0 = deck_sdi_time_monotonic_us();
    vTaskDelay(pdMS_TO_TICKS(50));
    int64_t t1 = deck_sdi_time_monotonic_us();
    int64_t dt = t1 - t0;

    /* Expect ~50ms = ~50000us. Allow 40..200 ms tolerance for scheduler jitter. */
    if (dt < 40000 || dt > 200000) {
        ESP_LOGE(TAG, "monotonic delta out of range: %lldus", (long long)dt);
        return DECK_SDI_ERR_FAIL;
    }
    if (t1 <= t0) {
        ESP_LOGE(TAG, "monotonic not strictly increasing: t0=%lld t1=%lld",
                 (long long)t0, (long long)t1);
        return DECK_SDI_ERR_FAIL;
    }

    ESP_LOGI(TAG, "selftest: PASS (dt=%lldus after 50ms vTaskDelay, wall_set=%d)",
             (long long)dt, (int)deck_sdi_time_wall_is_set());
    return DECK_SDI_OK;
}
