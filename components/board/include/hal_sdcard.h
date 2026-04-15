#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_SDCARD_MOUNT_POINT  "/sdcard"

/**
 * @brief Mount the SD card at /sdcard via SPI.
 *        CS is controlled via CH422G EXIO4 (must keep USB_SEL=0).
 * @return ESP_OK on success, ESP_FAIL if no card or mount error
 */
esp_err_t hal_sdcard_mount(void);

/**
 * @brief Unmount the SD card.
 */
esp_err_t hal_sdcard_unmount(void);

/**
 * @brief Check if SD card is currently mounted.
 */
bool hal_sdcard_is_mounted(void);

/**
 * @brief Probe whether the SD card is physically present and accessible.
 *        Performs a lightweight statvfs call. Returns false if the card
 *        was removed since the last mount (even if s_mounted flag is true).
 */
bool hal_sdcard_probe(void);

/**
 * @brief Get total and used space in KB.
 */
esp_err_t hal_sdcard_get_space(uint32_t *total_kb, uint32_t *used_kb);

#ifdef __cplusplus
}
#endif
