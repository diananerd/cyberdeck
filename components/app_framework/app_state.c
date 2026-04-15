/*
 * S3 Cyber-Deck — Runtime state store
 */

#include "app_state.h"
#include <string.h>

static cyberdeck_state_t s_state;

void app_state_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
}

const cyberdeck_state_t *app_state_get(void)
{
    return &s_state;
}

void app_state_update_wifi(bool connected, const char *ssid, int8_t rssi)
{
    s_state.wifi_connected = connected;
    if (ssid) {
        strncpy(s_state.wifi_ssid, ssid, sizeof(s_state.wifi_ssid) - 1);
    } else {
        s_state.wifi_ssid[0] = '\0';
    }
    s_state.wifi_rssi = rssi;
}

void app_state_update_battery(uint8_t pct)
{
    s_state.battery_pct = pct;
}

void app_state_update_sd(bool mounted)
{
    s_state.sd_mounted = mounted;
    if (!mounted) {
        s_state.sd_total_kb = 0;
        s_state.sd_used_kb  = 0;
    }
}

void app_state_update_sd_space(uint32_t total_kb, uint32_t used_kb)
{
    s_state.sd_total_kb = total_kb;
    s_state.sd_used_kb  = used_kb;
}

void app_state_set_bt(bool present, bool connected, const char *name)
{
    s_state.bt_module_present = present;
    s_state.bt_connected      = connected;
    if (name) {
        strncpy(s_state.bt_device_name, name, sizeof(s_state.bt_device_name) - 1);
    } else {
        s_state.bt_device_name[0] = '\0';
    }
}
