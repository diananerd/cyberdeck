/*
 * CyberDeck — Runtime State
 * Centralizes volatile device state updated by service events.
 * Apps read this rather than querying services directly.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     wifi_connected;
    char     wifi_ssid[33];
    int8_t   wifi_rssi;
    uint8_t  battery_pct;
    bool     time_synced;
    bool     bt_module_present;
    bool     bt_connected;
    char     bt_device_name[32];
    bool     audio_playing;
    char     audio_title[64];
    uint32_t audio_pos_ms;
    uint32_t audio_dur_ms;
    bool     sd_mounted;
    uint32_t sd_total_kb;
    uint32_t sd_used_kb;
} cyberdeck_state_t;

void                     app_state_init(void);
const cyberdeck_state_t *app_state_get(void);
void                     app_state_update_wifi(bool connected, const char *ssid, int8_t rssi);
void                     app_state_update_battery(uint8_t pct);
void                     app_state_update_sd(bool mounted);
void                     app_state_update_sd_space(uint32_t total_kb, uint32_t used_kb);
void                     app_state_set_bt(bool present, bool connected, const char *name);

#ifdef __cplusplus
}
#endif
