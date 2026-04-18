#include "drivers/deck_sdi_time.h"
#include "deck_sdi_registry.h"

#include "esp_timer.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <time.h>
#include <sys/time.h>
#include <string.h>

static const char *TAG = "sdi.time";

static bool s_wall_set    = false;
static bool s_sntp_active = false;

static void sntp_synced_cb(struct timeval *tv)
{
    if (!tv) return;
    s_wall_set = true;
    ESP_LOGI(TAG, "SNTP sync: epoch=%lld", (long long)tv->tv_sec);
}

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

static deck_sdi_err_t time_sntp_start_impl(void *ctx, const char *server)
{
    (void)ctx;
    if (s_sntp_active) return DECK_SDI_OK;
    if (!server || !*server) server = "pool.ntp.org";

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, server);
    sntp_set_time_sync_notification_cb(sntp_synced_cb);
    esp_sntp_init();
    s_sntp_active = true;
    ESP_LOGI(TAG, "SNTP started (server=%s)", server);
    return DECK_SDI_OK;
}

static deck_sdi_err_t time_sntp_stop_impl(void *ctx)
{
    (void)ctx;
    if (!s_sntp_active) return DECK_SDI_OK;
    esp_sntp_stop();
    s_sntp_active = false;
    return DECK_SDI_OK;
}

static const deck_sdi_time_vtable_t s_vtable = {
    .monotonic_us      = time_monotonic_us,
    .wall_epoch_s      = time_wall_epoch_s,
    .set_wall_epoch_s  = time_set_wall_epoch_s,
    .wall_is_set       = time_wall_is_set,
    .sntp_start        = time_sntp_start_impl,
    .sntp_stop         = time_sntp_stop_impl,
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

deck_sdi_err_t deck_sdi_time_sntp_start(const char *server)
{
    void *c; const deck_sdi_time_vtable_t *v = time_vt(&c);
    if (!v) return DECK_SDI_ERR_NOT_FOUND;
    if (!v->sntp_start) return DECK_SDI_ERR_NOT_SUPPORTED;
    return v->sntp_start(c, server);
}

deck_sdi_err_t deck_sdi_time_sntp_stop(void)
{
    void *c; const deck_sdi_time_vtable_t *v = time_vt(&c);
    if (!v) return DECK_SDI_ERR_NOT_FOUND;
    if (!v->sntp_stop) return DECK_SDI_ERR_NOT_SUPPORTED;
    return v->sntp_stop(c);
}

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
