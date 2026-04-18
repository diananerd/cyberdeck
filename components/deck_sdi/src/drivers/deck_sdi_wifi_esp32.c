#include "drivers/deck_sdi_wifi.h"
#include "deck_sdi_registry.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <string.h>

static const char *TAG = "sdi.wifi";

/* Internal state machine. */
static deck_sdi_wifi_state_t s_state    = DECK_SDI_WIFI_DISCONNECTED;
static esp_netif_t          *s_netif    = NULL;
static EventGroupHandle_t    s_evt      = NULL;
static int8_t                s_last_rssi = 0;
static char                  s_ip[DECK_SDI_WIFI_IP_MAX] = {0};
static bool                  s_inited   = false;

#define EVT_BIT_CONNECTED  BIT0
#define EVT_BIT_FAILED     BIT1
#define EVT_BIT_GOT_IP     BIT2
#define EVT_BIT_DISCONN    BIT3

static deck_sdi_wifi_auth_t map_auth(wifi_auth_mode_t a)
{
    switch (a) {
        case WIFI_AUTH_OPEN:           return DECK_SDI_WIFI_AUTH_OPEN;
        case WIFI_AUTH_WEP:            return DECK_SDI_WIFI_AUTH_WEP;
        case WIFI_AUTH_WPA_PSK:        return DECK_SDI_WIFI_AUTH_WPA;
        case WIFI_AUTH_WPA2_PSK:       return DECK_SDI_WIFI_AUTH_WPA2;
        case WIFI_AUTH_WPA_WPA2_PSK:   return DECK_SDI_WIFI_AUTH_WPA_WPA2;
        case WIFI_AUTH_WPA3_PSK:       return DECK_SDI_WIFI_AUTH_WPA3;
        default:                       return DECK_SDI_WIFI_AUTH_OTHER;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started");
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                s_state = DECK_SDI_WIFI_DISCONNECTED;
                s_ip[0] = '\0';
                xEventGroupSetBits(s_evt, EVT_BIT_DISCONN | EVT_BIT_FAILED);
                break;
            case WIFI_EVENT_STA_CONNECTED:
                /* Wait for IP before declaring CONNECTED. */
                xEventGroupSetBits(s_evt, EVT_BIT_CONNECTED);
                break;
            default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_state = DECK_SDI_WIFI_CONNECTED;
        xEventGroupSetBits(s_evt, EVT_BIT_GOT_IP);
        ESP_LOGI(TAG, "got IP %s", s_ip);
    }
}

static deck_sdi_err_t wifi_init_impl(void *ctx)
{
    (void)ctx;
    if (s_inited) return DECK_SDI_OK;

    s_evt = xEventGroupCreate();
    if (!s_evt) return DECK_SDI_ERR_NO_MEMORY;

    /* esp_event_loop and netif may already be initialized by the host —
     * tolerate ESP_ERR_INVALID_STATE. */
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif_init: %s", esp_err_to_name(e));
        return DECK_SDI_ERR_IO;
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event_loop_create: %s", esp_err_to_name(e));
        return DECK_SDI_ERR_IO;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) {
        ESP_LOGE(TAG, "netif_sta create failed");
        return DECK_SDI_ERR_IO;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "wifi_init: %s", esp_err_to_name(e));
        return DECK_SDI_ERR_IO;
    }

    e = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             wifi_event_handler, NULL, NULL);
    if (e != ESP_OK) return DECK_SDI_ERR_IO;
    e = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             wifi_event_handler, NULL, NULL);
    if (e != ESP_OK) return DECK_SDI_ERR_IO;

    e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) return DECK_SDI_ERR_IO;
    e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (e != ESP_OK) return DECK_SDI_ERR_IO;
    e = esp_wifi_start();
    if (e != ESP_OK) return DECK_SDI_ERR_IO;

    s_state = DECK_SDI_WIFI_DISCONNECTED;
    s_inited = true;
    ESP_LOGI(TAG, "init OK");
    return DECK_SDI_OK;
}

static deck_sdi_err_t wifi_scan_impl(void *ctx,
                                      deck_sdi_wifi_scan_cb_t cb, void *user)
{
    (void)ctx;
    if (!cb)              return DECK_SDI_ERR_INVALID_ARG;
    if (!s_inited)        return DECK_SDI_ERR_FAIL;

    deck_sdi_wifi_state_t prev = s_state;
    s_state = DECK_SDI_WIFI_SCANNING;

    wifi_scan_config_t scan_cfg = {0};
    esp_err_t e = esp_wifi_scan_start(&scan_cfg, true /* block */);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "scan_start: %s", esp_err_to_name(e));
        s_state = prev;
        return DECK_SDI_ERR_IO;
    }

    uint16_t n = 0;
    if (esp_wifi_scan_get_ap_num(&n) != ESP_OK) {
        s_state = prev;
        return DECK_SDI_ERR_IO;
    }
    if (n == 0) {
        s_state = prev;
        return DECK_SDI_OK;
    }

    /* Cap to a sane upper bound to avoid very large allocations. */
    if (n > 32) n = 32;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) { s_state = prev; return DECK_SDI_ERR_NO_MEMORY; }

    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        free(recs);
        s_state = prev;
        return DECK_SDI_ERR_IO;
    }

    for (uint16_t i = 0; i < n; i++) {
        deck_sdi_wifi_ap_t ap = {0};
        strncpy(ap.ssid, (char *)recs[i].ssid, DECK_SDI_WIFI_SSID_MAX);
        ap.ssid[DECK_SDI_WIFI_SSID_MAX] = '\0';
        ap.rssi    = recs[i].rssi;
        ap.channel = recs[i].primary;
        ap.auth    = map_auth(recs[i].authmode);
        if (!cb(&ap, user)) break;
    }

    free(recs);
    s_state = prev;
    return DECK_SDI_OK;
}

static deck_sdi_err_t wifi_connect_impl(void *ctx, const char *ssid,
                                         const char *password,
                                         uint32_t timeout_ms)
{
    (void)ctx;
    if (!ssid || !*ssid)  return DECK_SDI_ERR_INVALID_ARG;
    if (!s_inited)        return DECK_SDI_ERR_FAIL;

    xEventGroupClearBits(s_evt,
                         EVT_BIT_CONNECTED | EVT_BIT_FAILED |
                         EVT_BIT_GOT_IP    | EVT_BIT_DISCONN);

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password) {
        strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    }
    cfg.sta.threshold.authmode = password && *password
                                   ? WIFI_AUTH_WPA2_PSK
                                   : WIFI_AUTH_OPEN;

    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (e != ESP_OK) return DECK_SDI_ERR_IO;

    s_state = DECK_SDI_WIFI_CONNECTING;
    e = esp_wifi_connect();
    if (e != ESP_OK) {
        s_state = DECK_SDI_WIFI_FAILED;
        return DECK_SDI_ERR_IO;
    }

    EventBits_t bits = xEventGroupWaitBits(s_evt,
                                            EVT_BIT_GOT_IP | EVT_BIT_FAILED,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(timeout_ms));
    if (bits & EVT_BIT_GOT_IP) {
        s_state = DECK_SDI_WIFI_CONNECTED;
        return DECK_SDI_OK;
    }
    s_state = DECK_SDI_WIFI_FAILED;
    return (bits & EVT_BIT_FAILED) ? DECK_SDI_ERR_FAIL : DECK_SDI_ERR_TIMEOUT;
}

static deck_sdi_err_t wifi_disconnect_impl(void *ctx)
{
    (void)ctx;
    if (!s_inited) return DECK_SDI_OK;
    esp_err_t e = esp_wifi_disconnect();
    s_state = DECK_SDI_WIFI_DISCONNECTED;
    s_ip[0] = '\0';
    if (e != ESP_OK && e != ESP_ERR_WIFI_NOT_CONNECT) return DECK_SDI_ERR_IO;
    return DECK_SDI_OK;
}

static deck_sdi_wifi_state_t wifi_status_impl(void *ctx)
{
    (void)ctx;
    return s_state;
}

static deck_sdi_err_t wifi_get_ip_impl(void *ctx, char *out_buf, size_t out_size)
{
    (void)ctx;
    if (!out_buf || out_size == 0) return DECK_SDI_ERR_INVALID_ARG;
    strncpy(out_buf, s_ip, out_size - 1);
    out_buf[out_size - 1] = '\0';
    return DECK_SDI_OK;
}

static int8_t wifi_rssi_impl(void *ctx)
{
    (void)ctx;
    if (s_state != DECK_SDI_WIFI_CONNECTED) return 0;
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        s_last_rssi = info.rssi;
    }
    return s_last_rssi;
}

static const deck_sdi_wifi_vtable_t s_vtable = {
    .init       = wifi_init_impl,
    .scan       = wifi_scan_impl,
    .connect    = wifi_connect_impl,
    .disconnect = wifi_disconnect_impl,
    .status     = wifi_status_impl,
    .get_ip     = wifi_get_ip_impl,
    .rssi       = wifi_rssi_impl,
};

deck_sdi_err_t deck_sdi_wifi_register_esp32(void)
{
    const deck_sdi_driver_t driver = {
        .name    = "network.wifi",
        .id      = DECK_SDI_DRIVER_WIFI,
        .version = "1.0.0",
        .vtable  = &s_vtable,
        .ctx     = NULL,
    };
    return deck_sdi_register(&driver);
}

/* ---------- wrappers ---------- */

static const deck_sdi_wifi_vtable_t *wifi_vt(void **ctx_out)
{
    const deck_sdi_driver_t *d = deck_sdi_lookup(DECK_SDI_DRIVER_WIFI);
    if (!d) return NULL;
    if (ctx_out) *ctx_out = d->ctx;
    return (const deck_sdi_wifi_vtable_t *)d->vtable;
}

deck_sdi_err_t deck_sdi_wifi_init(void)
{ void *c; const deck_sdi_wifi_vtable_t *v = wifi_vt(&c);
  return v ? v->init(c) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_err_t deck_sdi_wifi_scan(deck_sdi_wifi_scan_cb_t cb, void *user)
{ void *c; const deck_sdi_wifi_vtable_t *v = wifi_vt(&c);
  return v ? v->scan(c, cb, user) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_err_t deck_sdi_wifi_connect(const char *ssid, const char *pass, uint32_t to)
{ void *c; const deck_sdi_wifi_vtable_t *v = wifi_vt(&c);
  return v ? v->connect(c, ssid, pass, to) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_err_t deck_sdi_wifi_disconnect(void)
{ void *c; const deck_sdi_wifi_vtable_t *v = wifi_vt(&c);
  return v ? v->disconnect(c) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_wifi_state_t deck_sdi_wifi_status(void)
{ void *c; const deck_sdi_wifi_vtable_t *v = wifi_vt(&c);
  return v ? v->status(c) : DECK_SDI_WIFI_DISCONNECTED; }

deck_sdi_err_t deck_sdi_wifi_get_ip(char *out_buf, size_t out_size)
{ void *c; const deck_sdi_wifi_vtable_t *v = wifi_vt(&c);
  return v ? v->get_ip(c, out_buf, out_size) : DECK_SDI_ERR_NOT_FOUND; }

int8_t deck_sdi_wifi_rssi(void)
{ void *c; const deck_sdi_wifi_vtable_t *v = wifi_vt(&c);
  return v ? v->rssi(c) : 0; }

/* ---------- selftest ---------- */

deck_sdi_err_t deck_sdi_wifi_selftest(void)
{
    deck_sdi_err_t r = deck_sdi_wifi_init();
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "init failed: %s", deck_sdi_strerror(r));
        return r;
    }
    deck_sdi_wifi_state_t st = deck_sdi_wifi_status();
    if (st != DECK_SDI_WIFI_DISCONNECTED) {
        ESP_LOGE(TAG, "expected DISCONNECTED after init, got %d", (int)st);
        return DECK_SDI_ERR_FAIL;
    }
    ESP_LOGI(TAG, "selftest: PASS (init OK, state=DISCONNECTED, no scan/connect attempted)");
    return DECK_SDI_OK;
}
