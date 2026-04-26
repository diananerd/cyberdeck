#include "drivers/deck_sdi_info.h"
#include "deck_sdi_registry.h"

#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "sdi.info";

/* Runtime claims. Bumped in lockstep with the component version.
 * deck_level promotes to 2 once F25-F29 are all on the platform. */
#define DECK_RUNTIME_VERSION     "0.3.0"
/* CyberDeck reference implementation now claims DL3 conformance:
 * 16/16 content kinds rendered, 11/11 UI services wired (lockscreen
 * exposed), DL3 stream tick scheduler live (15/15 ops + tick canary
 * verified). See REPORTS.md / spec BRIDGE §59. */
#define DECK_LEVEL               3
#define DECK_OS_SURFACE          2
/* Spec edition advertised by the runtime. The five pillar specs in
 * deck-lang/ are tagged "Edition: 2027" — runtime aligns. Apps still
 * may declare older editions; the loader rejects mismatched values
 * (deck_loader.c:stage6_compat). All shipped reference apps have been
 * updated to 2027 in the same sweep. */
#define DECK_EDITION             2027

/* MAC-derived device id, computed once on first access. */
static char s_device_id[13] = {0};
static const char *s_current_app = NULL;

static void ensure_device_id(void)
{
    if (s_device_id[0]) return;
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_device_id, sizeof(s_device_id),
                 "%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        strcpy(s_device_id, "000000000000");
    }
}

static const char *info_device_id(void *ctx)   { (void)ctx; ensure_device_id(); return s_device_id; }
static size_t      info_free_heap(void *ctx)   { (void)ctx; return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
static int         info_deck_level(void *ctx)  { (void)ctx; return DECK_LEVEL; }
static int         info_deck_os(void *ctx)     { (void)ctx; return DECK_OS_SURFACE; }
static const char *info_runtime(void *ctx)     { (void)ctx; return DECK_RUNTIME_VERSION; }
static int         info_edition(void *ctx)     { (void)ctx; return DECK_EDITION; }
static const char *info_current_app(void *ctx) { (void)ctx; return s_current_app; }

static const deck_sdi_info_vtable_t s_vtable = {
    .device_id       = info_device_id,
    .free_heap       = info_free_heap,
    .deck_level      = info_deck_level,
    .deck_os         = info_deck_os,
    .runtime_version = info_runtime,
    .edition         = info_edition,
    .current_app_id  = info_current_app,
};

deck_sdi_err_t deck_sdi_info_register(void)
{
    const deck_sdi_driver_t driver = {
        .name    = "system.info",
        .id      = DECK_SDI_DRIVER_INFO,
        .version = "1.0.0",
        .vtable  = &s_vtable,
        .ctx     = NULL,
    };
    return deck_sdi_register(&driver);
}

/* ---------- wrappers ---------- */

static const deck_sdi_info_vtable_t *info_vt(void **ctx_out)
{
    const deck_sdi_driver_t *d = deck_sdi_lookup(DECK_SDI_DRIVER_INFO);
    if (!d) return NULL;
    if (ctx_out) *ctx_out = d->ctx;
    return (const deck_sdi_info_vtable_t *)d->vtable;
}

const char *deck_sdi_info_device_id(void)
{ void *c; const deck_sdi_info_vtable_t *v = info_vt(&c); return v ? v->device_id(c) : NULL; }
size_t deck_sdi_info_free_heap(void)
{ void *c; const deck_sdi_info_vtable_t *v = info_vt(&c); return v ? v->free_heap(c) : 0; }
int deck_sdi_info_deck_level(void)
{ void *c; const deck_sdi_info_vtable_t *v = info_vt(&c); return v ? v->deck_level(c) : 0; }
int deck_sdi_info_deck_os(void)
{ void *c; const deck_sdi_info_vtable_t *v = info_vt(&c); return v ? v->deck_os(c) : 0; }
const char *deck_sdi_info_runtime_version(void)
{ void *c; const deck_sdi_info_vtable_t *v = info_vt(&c); return v ? v->runtime_version(c) : NULL; }
int deck_sdi_info_edition(void)
{ void *c; const deck_sdi_info_vtable_t *v = info_vt(&c); return v ? v->edition(c) : 0; }
const char *deck_sdi_info_current_app_id(void)
{ void *c; const deck_sdi_info_vtable_t *v = info_vt(&c); return v ? v->current_app_id(c) : NULL; }

/* ---------- selftest ---------- */

deck_sdi_err_t deck_sdi_info_selftest(void)
{
    const char *did = deck_sdi_info_device_id();
    if (!did || strlen(did) != 12) {
        ESP_LOGE(TAG, "device_id invalid: %s", did ? did : "(null)");
        return DECK_SDI_ERR_FAIL;
    }
    size_t heap = deck_sdi_info_free_heap();
    if (heap == 0) { ESP_LOGE(TAG, "free_heap=0 unexpected"); return DECK_SDI_ERR_FAIL; }
    int lvl = deck_sdi_info_deck_level();
    if (lvl != DECK_LEVEL) { ESP_LOGE(TAG, "deck_level=%d want %d", lvl, DECK_LEVEL); return DECK_SDI_ERR_FAIL; }
    int os = deck_sdi_info_deck_os();
    if (os != DECK_OS_SURFACE) { ESP_LOGE(TAG, "deck_os=%d want %d", os, DECK_OS_SURFACE); return DECK_SDI_ERR_FAIL; }
    const char *rv = deck_sdi_info_runtime_version();
    if (!rv) { ESP_LOGE(TAG, "runtime_version null"); return DECK_SDI_ERR_FAIL; }
    int ed = deck_sdi_info_edition();
    if (ed != DECK_EDITION) { ESP_LOGE(TAG, "edition=%d want %d", ed, DECK_EDITION); return DECK_SDI_ERR_FAIL; }
    /* current_app_id may legitimately be NULL before F8. */
    const char *app = deck_sdi_info_current_app_id();

    ESP_LOGI(TAG, "selftest: PASS (device=%s level=%d os=%d runtime=%s edition=%d heap=%u app=%s)",
             did, lvl, os, rv, ed, (unsigned)heap, app ? app : "(none)");
    return DECK_SDI_OK;
}
