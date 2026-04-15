/*
 * CyberDeck — OS Storage API (F2)
 */

#include "os_storage.h"
#include "hal_sdcard.h"
#include "esp_log.h"
#include <sys/stat.h>
#include <string.h>
#include <errno.h>

static const char *TAG = "os_storage";

/* -------------------------------------------------------------------------
 * Internal registry — maps app_id to a custom base directory.
 * Built-in apps that don't register fall back to /sdcard/apps/<id>.
 * Dynamic SD apps register their actual path via os_app_discover_sd.
 * ------------------------------------------------------------------------- */

#define OS_STORAGE_MAX_ENTRIES  32
#define OS_STORAGE_DIR_MAXLEN   72  /* enough for /sdcard/apps/<64-char-name> */

typedef struct {
    app_id_t id;
    char     base[OS_STORAGE_DIR_MAXLEN];
} storage_entry_t;

static storage_entry_t s_entries[OS_STORAGE_MAX_ENTRIES];
static int             s_count = 0;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t os_storage_init(void)
{
    s_count = 0;
    return ESP_OK;
}

esp_err_t os_storage_register(app_id_t id, const char *base_dir)
{
    if (!base_dir) return ESP_ERR_INVALID_ARG;

    /* Update existing entry */
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].id == id) {
            strncpy(s_entries[i].base, base_dir, OS_STORAGE_DIR_MAXLEN - 1);
            s_entries[i].base[OS_STORAGE_DIR_MAXLEN - 1] = '\0';
            return ESP_OK;
        }
    }

    if (s_count >= OS_STORAGE_MAX_ENTRIES) {
        ESP_LOGE(TAG, "register: table full (max %d)", OS_STORAGE_MAX_ENTRIES);
        return ESP_ERR_NO_MEM;
    }

    s_entries[s_count].id = id;
    strncpy(s_entries[s_count].base, base_dir, OS_STORAGE_DIR_MAXLEN - 1);
    s_entries[s_count].base[OS_STORAGE_DIR_MAXLEN - 1] = '\0';
    s_count++;
    return ESP_OK;
}

const char *os_storage_dir(app_id_t id)
{
    if (!hal_sdcard_is_mounted()) return NULL;

    /* Look up registered override */
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].id == id) {
            mkdir(s_entries[i].base, 0755);  /* no-op if already exists */
            return s_entries[i].base;
        }
    }

    /* Numeric fallback: /sdcard/apps/<id> */
    static char s_fallback[OS_STORAGE_DIR_MAXLEN];
    snprintf(s_fallback, sizeof(s_fallback),
             HAL_SDCARD_MOUNT_POINT "/apps/%u", (unsigned)id);
    mkdir(s_fallback, 0755);
    return s_fallback;
}

const char *os_storage_path(app_id_t id, const char *name, char *buf, size_t len)
{
    const char *dir = os_storage_dir(id);
    if (!dir || !buf || !name || len == 0) return NULL;
    snprintf(buf, len, "%s/%s", dir, name);
    return buf;
}

FILE *os_storage_fopen(app_id_t id, const char *rel, const char *mode)
{
    char path[96];
    if (!os_storage_path(id, rel, path, sizeof(path))) return NULL;
    FILE *f = fopen(path, mode);
    if (!f) {
        ESP_LOGD(TAG, "fopen(%s, %s): %d", path, mode, errno);
    }
    return f;
}

esp_err_t os_storage_read(app_id_t id, const char *name, void *buf, size_t *len)
{
    if (!buf || !len) return ESP_ERR_INVALID_ARG;

    FILE *f = os_storage_fopen(id, name, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    size_t n = fread(buf, 1, *len, f);
    fclose(f);
    *len = n;
    return ESP_OK;
}

esp_err_t os_storage_write(app_id_t id, const char *name, const void *data, size_t len)
{
    if (!data) return ESP_ERR_INVALID_ARG;

    FILE *f = os_storage_fopen(id, name, "wb");
    if (!f) return ESP_FAIL;

    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return (n == len) ? ESP_OK : ESP_FAIL;
}

bool os_storage_exists(app_id_t id, const char *name)
{
    char path[96];
    if (!os_storage_path(id, name, path, sizeof(path))) return false;
    struct stat st;
    return (stat(path, &st) == 0);
}

esp_err_t os_storage_delete(app_id_t id, const char *name)
{
    char path[96];
    if (!os_storage_path(id, name, path, sizeof(path))) {
        return ESP_ERR_INVALID_STATE;
    }
    if (remove(path) != 0) {
        ESP_LOGE(TAG, "delete(%s): %d", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}
