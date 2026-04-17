#pragma once

/* deck_sdi — Service Driver Interface (v1.0)
 *
 * Populated in F1. Each DL1-mandatory driver (storage.nvs, storage.fs-ro,
 * system.info, system.time, system.shell) lands as its own header under
 * include/drivers/ with a vtable + a thin ESP32 impl in src/drivers/.
 */
