#pragma once

/* network.http — outbound HTTP/HTTPS client.
 *
 * DL2 driver. Wraps esp_http_client behind a platform-neutral synchronous
 * vtable. Caller supplies a request struct + a writable response buffer;
 * driver fills body bytes (truncated to capacity) and status code.
 *
 * No streaming yet — full request body is in memory, full response body
 * (up to capacity) is collected into the caller's buffer. Streaming will
 * be added in DL3 alongside async dispatch.
 */

#include "deck_sdi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DECK_SDI_HTTP_GET = 0,
    DECK_SDI_HTTP_POST,
    DECK_SDI_HTTP_PUT,
    DECK_SDI_HTTP_DELETE,
    DECK_SDI_HTTP_PATCH,
    DECK_SDI_HTTP_HEAD,
} deck_sdi_http_method_t;

typedef struct {
    const char *name;   /* "Content-Type" */
    const char *value;  /* "application/json" */
} deck_sdi_http_header_t;

typedef struct {
    const char                  *url;        /* full url incl. scheme */
    deck_sdi_http_method_t       method;
    const deck_sdi_http_header_t *headers;   /* may be NULL */
    size_t                       header_count;
    const void                   *body;      /* may be NULL */
    size_t                       body_size;
    uint32_t                     timeout_ms; /* 0 → driver default */
} deck_sdi_http_request_t;

typedef struct {
    int      status_code;       /* HTTP status, e.g. 200, 404 */
    size_t   body_bytes;        /* bytes actually written into out_body */
    bool     truncated;         /* true if response > out_capacity */
} deck_sdi_http_response_t;

typedef struct {
    /* Issue request synchronously. Writes up to out_capacity bytes of
     * the response body into out_body. Sets *out_resp on success. */
    deck_sdi_err_t (*request)(void *ctx,
                              const deck_sdi_http_request_t *req,
                              void *out_body, size_t out_capacity,
                              deck_sdi_http_response_t *out_resp);
} deck_sdi_http_vtable_t;

/* esp_http_client implementation. */
deck_sdi_err_t deck_sdi_http_register_esp32(void);

/* High-level wrappers. */
deck_sdi_err_t deck_sdi_http_request(const deck_sdi_http_request_t *req,
                                      void *out_body, size_t out_capacity,
                                      deck_sdi_http_response_t *out_resp);

/* Convenience GET wrapper. */
deck_sdi_err_t deck_sdi_http_get(const char *url,
                                  void *out_body, size_t out_capacity,
                                  deck_sdi_http_response_t *out_resp);

/* Selftest: registers the driver only. Network calls require WiFi to
 * be connected — performs a NULL-arg validity probe instead. */
deck_sdi_err_t deck_sdi_http_selftest(void);

#ifdef __cplusplus
}
#endif
