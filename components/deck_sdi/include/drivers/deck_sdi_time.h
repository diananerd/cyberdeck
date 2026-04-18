#pragma once

/* system.time — time primitives.
 *
 * Mandatory at DL1 for monotonic time (boot-relative). Wall clock is
 * optional at DL1 (not all platforms have an RTC). NTP sync is DL2+.
 *
 * See deck-lang/03-deck-os.md §time.
 */

#include "deck_sdi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Microseconds since boot. Strictly monotonic. */
    int64_t (*monotonic_us)(void *ctx);

    /* Wall-clock UNIX epoch seconds. Returns 0 if no wall clock set. */
    int64_t (*wall_epoch_s)(void *ctx);

    /* Set wall clock (seconds since UNIX epoch). */
    deck_sdi_err_t (*set_wall_epoch_s)(void *ctx, int64_t epoch_s);

    /* true if wall clock is considered set/accurate. */
    bool (*wall_is_set)(void *ctx);

    /* DL2+ — start asynchronous SNTP sync against the configured pool.
     * Idempotent. Caller polls wall_is_set() to see when sync lands.
     * NULL when the platform has no SNTP support. */
    deck_sdi_err_t (*sntp_start)(void *ctx, const char *server);

    /* DL2+ — stop SNTP sync (frees client). NULL when unsupported. */
    deck_sdi_err_t (*sntp_stop)(void *ctx);
} deck_sdi_time_vtable_t;

deck_sdi_err_t deck_sdi_time_register(void);

int64_t        deck_sdi_time_monotonic_us(void);
int64_t        deck_sdi_time_wall_epoch_s(void);
deck_sdi_err_t deck_sdi_time_set_wall_epoch_s(int64_t epoch_s);
bool           deck_sdi_time_wall_is_set(void);

/* SNTP wrappers. server may be NULL for the platform default
 * ("pool.ntp.org" on the ESP-IDF impl). */
deck_sdi_err_t deck_sdi_time_sntp_start(const char *server);
deck_sdi_err_t deck_sdi_time_sntp_stop(void);

/* Selftest: monotonic_us moves forward across a vTaskDelay. */
deck_sdi_err_t deck_sdi_time_selftest(void);

#ifdef __cplusplus
}
#endif
