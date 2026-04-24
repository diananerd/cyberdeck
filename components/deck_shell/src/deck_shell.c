#include "deck_shell.h"
#include "deck_interp.h"
#include "drivers/deck_sdi_fs.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "deck_shell";

/* Capture the first .deck entry via the fs list callback. */
typedef struct { char name[128]; bool found; } find_ctx_t;

static bool first_deck_cb(const char *name, bool is_dir, void *user)
{
    find_ctx_t *fc = user;
    if (!name) return true;
    ESP_LOGI(TAG, "  entry: \"%s\" dir=%d", name, (int)is_dir);
    if (is_dir) return true;
    size_t n = strlen(name);
    if (n > 5 && strcmp(name + n - 5, ".deck") == 0) {
        snprintf(fc->name, sizeof(fc->name), "%s", name);
        fc->found = true;
        return false;
    }
    return true;
}

deck_err_t deck_shell_boot(void)
{
    ESP_LOGI(TAG, "booting DL1 shell");

    find_ctx_t fc = { .found = false };
    /* SPIFFS is flat — all apps live at the mount root. */
    deck_sdi_err_t lr = deck_sdi_fs_list("/", first_deck_cb, &fc);
    if (lr != DECK_SDI_OK) {
        ESP_LOGW(TAG, "fs.list failed (%s) — skipping shell",
                 lr == DECK_SDI_ERR_NOT_FOUND ? "not_found" : "io");
        return DECK_RT_OK;
    }
    if (!fc.found) {
        ESP_LOGW(TAG, "no .deck file bundled in apps partition");
        return DECK_RT_OK;
    }

    char path[160];
    snprintf(path, sizeof(path), "/%s", fc.name);

    /* Read the .deck source into a heap buffer. 32 KB cap is ample
     * for DL1 programs (spec 16 §4.9 targets ≤ 48 KB per loaded app).
     * Fall back to PSRAM when internal is tight — by the time DL1 shell
     * runs, the DL2 persistent app slots have already consumed most of
     * the internal heap and a 32KB internal alloc can fail even though
     * several MB of PSRAM are free. The source buffer is only read, not
     * interpreted from, so PSRAM access speed is fine. */
    const size_t READ_CAP = 32 * 1024;
    char *buf = heap_caps_malloc(READ_CAP, MALLOC_CAP_INTERNAL);
    if (!buf) buf = heap_caps_malloc(READ_CAP, MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGE(TAG, "oom reading app"); return DECK_RT_NO_MEMORY; }
    size_t n = READ_CAP - 1;
    deck_sdi_err_t rr = deck_sdi_fs_read(path, buf, &n);
    if (rr != DECK_SDI_OK) {
        ESP_LOGE(TAG, "fs.read %s failed: %s", path, deck_sdi_strerror(rr));
        heap_caps_free(buf);
        return DECK_LOAD_LEX;
    }
    buf[n] = '\0';
    ESP_LOGI(TAG, "launching %s (%u bytes)", path, (unsigned)n);

    deck_err_t rc = deck_runtime_run_on_launch(buf, (uint32_t)n);
    heap_caps_free(buf);

    if (rc == DECK_RT_OK || rc == DECK_LOAD_OK) {
        ESP_LOGI(TAG, "app completed cleanly");
    } else {
        ESP_LOGE(TAG, "app ended with error: %s", deck_err_name(rc));
    }
    return rc;
}
