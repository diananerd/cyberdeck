#include "drivers/deck_sdi_http.h"
#include "deck_sdi_registry.h"

#include "esp_http_client.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "sdi.http";

#define HTTP_DEFAULT_TIMEOUT_MS  10000

typedef struct {
    char    *out_buf;
    size_t   out_capacity;
    size_t   bytes_written;
    bool     truncated;
} http_collect_t;

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data && evt->data) {
        http_collect_t *c = (http_collect_t *)evt->user_data;
        size_t avail = c->out_capacity - c->bytes_written;
        if (avail == 0) {
            c->truncated = true;
            return ESP_OK;
        }
        size_t to_copy = (size_t)evt->data_len < avail
                            ? (size_t)evt->data_len
                            : avail;
        memcpy(c->out_buf + c->bytes_written, evt->data, to_copy);
        c->bytes_written += to_copy;
        if ((size_t)evt->data_len > to_copy) c->truncated = true;
    }
    return ESP_OK;
}

static esp_http_client_method_t map_method(deck_sdi_http_method_t m)
{
    switch (m) {
        case DECK_SDI_HTTP_GET:    return HTTP_METHOD_GET;
        case DECK_SDI_HTTP_POST:   return HTTP_METHOD_POST;
        case DECK_SDI_HTTP_PUT:    return HTTP_METHOD_PUT;
        case DECK_SDI_HTTP_DELETE: return HTTP_METHOD_DELETE;
        case DECK_SDI_HTTP_PATCH:  return HTTP_METHOD_PATCH;
        case DECK_SDI_HTTP_HEAD:   return HTTP_METHOD_HEAD;
        default:                   return HTTP_METHOD_GET;
    }
}

static deck_sdi_err_t http_request_impl(void *ctx,
                                         const deck_sdi_http_request_t *req,
                                         void *out_body, size_t out_capacity,
                                         deck_sdi_http_response_t *out_resp)
{
    (void)ctx;
    if (!req || !req->url || !out_resp) return DECK_SDI_ERR_INVALID_ARG;
    if (out_capacity > 0 && !out_body)  return DECK_SDI_ERR_INVALID_ARG;

    http_collect_t collect = {
        .out_buf      = (char *)out_body,
        .out_capacity = out_capacity,
        .bytes_written = 0,
        .truncated     = false,
    };

    esp_http_client_config_t cfg = {
        .url            = req->url,
        .method         = map_method(req->method),
        .timeout_ms     = req->timeout_ms ? (int)req->timeout_ms
                                          : HTTP_DEFAULT_TIMEOUT_MS,
        .event_handler  = http_evt,
        .user_data      = &collect,
        .crt_bundle_attach = NULL, /* TLS bundle is opt-in; caller may set later */
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return DECK_SDI_ERR_NO_MEMORY;

    if (req->headers && req->header_count) {
        for (size_t i = 0; i < req->header_count; i++) {
            const deck_sdi_http_header_t *h = &req->headers[i];
            if (!h->name || !h->value) continue;
            esp_http_client_set_header(client, h->name, h->value);
        }
    }

    if (req->body && req->body_size) {
        esp_http_client_set_post_field(client,
                                        (const char *)req->body,
                                        (int)req->body_size);
    }

    esp_err_t e = esp_http_client_perform(client);
    deck_sdi_err_t rv = DECK_SDI_OK;
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "perform: %s", esp_err_to_name(e));
        if (e == ESP_ERR_HTTP_EAGAIN || e == ESP_ERR_TIMEOUT)
            rv = DECK_SDI_ERR_TIMEOUT;
        else
            rv = DECK_SDI_ERR_IO;
    }

    out_resp->status_code = esp_http_client_get_status_code(client);
    out_resp->body_bytes  = collect.bytes_written;
    out_resp->truncated   = collect.truncated;

    esp_http_client_cleanup(client);
    return rv;
}

static const deck_sdi_http_vtable_t s_vtable = {
    .request = http_request_impl,
};

deck_sdi_err_t deck_sdi_http_register_esp32(void)
{
    const deck_sdi_driver_t driver = {
        .name    = "network.http",
        .id      = DECK_SDI_DRIVER_HTTP,
        .version = "1.0.0",
        .vtable  = &s_vtable,
        .ctx     = NULL,
    };
    return deck_sdi_register(&driver);
}

/* ---------- wrappers ---------- */

static const deck_sdi_http_vtable_t *http_vt(void **ctx_out)
{
    const deck_sdi_driver_t *d = deck_sdi_lookup(DECK_SDI_DRIVER_HTTP);
    if (!d) return NULL;
    if (ctx_out) *ctx_out = d->ctx;
    return (const deck_sdi_http_vtable_t *)d->vtable;
}

deck_sdi_err_t deck_sdi_http_request(const deck_sdi_http_request_t *req,
                                      void *out_body, size_t out_capacity,
                                      deck_sdi_http_response_t *out_resp)
{
    void *c; const deck_sdi_http_vtable_t *v = http_vt(&c);
    if (!v) return DECK_SDI_ERR_NOT_FOUND;
    return v->request(c, req, out_body, out_capacity, out_resp);
}

deck_sdi_err_t deck_sdi_http_get(const char *url,
                                  void *out_body, size_t out_capacity,
                                  deck_sdi_http_response_t *out_resp)
{
    deck_sdi_http_request_t req = {
        .url     = url,
        .method  = DECK_SDI_HTTP_GET,
    };
    return deck_sdi_http_request(&req, out_body, out_capacity, out_resp);
}

/* ---------- selftest ---------- */

deck_sdi_err_t deck_sdi_http_selftest(void)
{
    /* Verify driver is registered + arg validation. Real network call
     * is deferred to a runtime-driven test (requires WiFi association). */
    deck_sdi_http_response_t resp;
    deck_sdi_err_t r = deck_sdi_http_request(NULL, NULL, 0, &resp);
    if (r != DECK_SDI_ERR_INVALID_ARG) {
        ESP_LOGE(TAG, "expected INVALID_ARG on NULL req, got %s",
                 deck_sdi_strerror(r));
        return DECK_SDI_ERR_FAIL;
    }
    ESP_LOGI(TAG, "selftest: PASS (registered + arg validation OK; "
                  "network call deferred to runtime)");
    return DECK_SDI_OK;
}
