#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Download progress info (sent with EVT_DOWNLOAD_PROGRESS).
 */
typedef struct {
    uint32_t request_id;
    uint32_t bytes_received;
    uint32_t bytes_total;       /* 0 if unknown */
} svc_download_progress_t;

/**
 * @brief Initialize the downloader service.
 *        Creates the download task on Core 0.
 */
esp_err_t svc_downloader_init(void);

/**
 * @brief Enqueue a download.
 * @param url         HTTPS URL to download
 * @param dest_path   Local file path (e.g. "/sdcard/music/song.mp3")
 * @param request_id  Caller-assigned ID for tracking events
 * @return ESP_OK if queued
 */
esp_err_t svc_downloader_enqueue(const char *url, const char *dest_path,
                                  uint32_t request_id);

/**
 * @brief Cancel a pending or in-progress download.
 */
esp_err_t svc_downloader_cancel(uint32_t request_id);

#ifdef __cplusplus
}
#endif
