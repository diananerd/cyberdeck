#pragma once

/* deck_sdi — Service Driver Interface (v1.0)
 *
 * Contract between the Deck runtime and the hosting platform. A platform
 * implements a fixed set of drivers; each driver is a vtable + ctx
 * registered at boot. The runtime never touches platform primitives
 * directly — it looks up drivers by id and calls through the vtable.
 *
 * DL1 mandates five drivers: storage.nvs, storage.fs (read-only),
 * system.info, system.time (monotonic), system.shell (minimal).
 *
 * See deck-lang/12-deck-service-drivers.md for the spec.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DECK_SDI_OK = 0,
    DECK_SDI_ERR_NOT_FOUND,
    DECK_SDI_ERR_INVALID_ARG,
    DECK_SDI_ERR_NOT_SUPPORTED,
    DECK_SDI_ERR_NO_MEMORY,
    DECK_SDI_ERR_TIMEOUT,
    DECK_SDI_ERR_IO,
    DECK_SDI_ERR_ALREADY_EXISTS,
    DECK_SDI_ERR_BUSY,
    DECK_SDI_ERR_FAIL,
} deck_sdi_err_t;

const char *deck_sdi_strerror(deck_sdi_err_t err);

/* Stable numeric identifiers for DL1-mandatory drivers.
 * New drivers append to this enum (DL2+ drivers will follow DL1_MAX).
 * Never reorder.
 */
typedef enum {
    DECK_SDI_DRIVER_NVS = 0,
    DECK_SDI_DRIVER_FS,
    DECK_SDI_DRIVER_INFO,
    DECK_SDI_DRIVER_TIME,
    DECK_SDI_DRIVER_SHELL,
    /* --- DL1 end --- */
    DECK_SDI_DRIVER_WIFI,        /* network.wifi      (DL2) */
    DECK_SDI_DRIVER_HTTP,        /* network.http      (DL2) */
    DECK_SDI_DRIVER_BATTERY,     /* system.battery    (DL2) */
    DECK_SDI_DRIVER_SECURITY,    /* system.security   (DL2) */
    DECK_SDI_DRIVER_BRIDGE_UI,   /* bridge.ui         (DL2) */
    DECK_SDI_DRIVER_DISPLAY,     /* display.panel     (DL2) */
    DECK_SDI_DRIVER_TOUCH,       /* display.touch     (DL2) */
    /* --- DL2 end --- */
    DECK_SDI_DRIVER_MAX = 32,   /* slot cap; DL3 drivers land below this */
} deck_sdi_driver_id_t;

typedef struct {
    const char             *name;    /* canonical capability name, e.g. "storage.nvs" */
    deck_sdi_driver_id_t    id;
    const char             *version; /* semver, e.g. "1.0.0" */
    const void             *vtable;  /* driver-specific vtable struct; cast by user */
    void                   *ctx;     /* driver-specific state passed to every vtable fn */
} deck_sdi_driver_t;

#ifdef __cplusplus
}
#endif
