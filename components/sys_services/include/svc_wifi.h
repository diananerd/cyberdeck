#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Max scan results */
#define SVC_WIFI_SCAN_MAX   16

/**
 * @brief Initialize WiFi STA mode.
 *        Does NOT connect — call svc_wifi_connect() or svc_wifi_auto_connect().
 */
esp_err_t svc_wifi_init(void);

/**
 * @brief Connect to a specific AP.
 * @param ssid SSID (null-terminated)
 * @param pass Password (null-terminated, empty for open)
 */
esp_err_t svc_wifi_connect(const char *ssid, const char *pass);

/**
 * @brief Disconnect from current AP.
 */
esp_err_t svc_wifi_disconnect(void);

/**
 * @brief Auto-connect using stored WiFi settings.
 */
esp_err_t svc_wifi_auto_connect(void);

/**
 * @brief Start an AP scan. Results available after EVT_WIFI_SCAN_DONE.
 */
esp_err_t svc_wifi_start_scan(void);

/**
 * @brief Get scan results after scan completes.
 * @param results Output array (caller provides)
 * @param count   In: array size, Out: number of results
 */
esp_err_t svc_wifi_get_scan_results(wifi_ap_record_t *results, uint16_t *count);

/**
 * @brief Check if WiFi is currently connected.
 */
bool svc_wifi_is_connected(void);

/**
 * @brief Get current RSSI.
 * @return RSSI in dBm, or 0 if not connected
 */
int8_t svc_wifi_get_rssi(void);

/**
 * @brief Get the connected SSID.
 * @param buf   Output buffer
 * @param len   Buffer size (at least 33 bytes)
 */
esp_err_t svc_wifi_get_ssid(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
