/*
 * CyberDeck — SD App Manifest Parser (G1)
 *
 * Uses cJSON (ESP-IDF "json" component) to parse manifest.json.
 * All output strings go into sd_manifest_t's own inline buffers.
 */

#include "os_manifest.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "os_manifest";

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static uint8_t parse_permissions(const cJSON *arr)
{
    uint8_t bits = 0;
    if (!cJSON_IsArray(arr)) return bits;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsString(item)) continue;
        const char *p = item->valuestring;
        if      (strcmp(p, "wifi")     == 0) bits |= APP_PERM_WIFI;
        else if (strcmp(p, "sd")       == 0) bits |= APP_PERM_SD;
        else if (strcmp(p, "network")  == 0) bits |= APP_PERM_NETWORK;
        else if (strcmp(p, "settings") == 0) bits |= APP_PERM_SETTINGS;
        else ESP_LOGW(TAG, "Unknown permission: %s", p);
    }
    return bits;
}

/* Read entire file into a heap-allocated buffer (caller frees).
 * Returns NULL on error. Caps size at 4096 bytes. */
static char *read_file_alloc(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    if (st.st_size > 4096) {
        ESP_LOGE(TAG, "%s too large (%ld B)", path, (long)st.st_size);
        return NULL;
    }
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *buf = (char *)malloc((size_t)st.st_size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Safely copy a cJSON string value into a fixed buffer. */
static void copy_str(char *dst, size_t dst_size, const cJSON *item)
{
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(dst, item->valuestring, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t os_manifest_parse(const char *path, sd_manifest_t *out)
{
    if (!path || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->id = APP_ID_INVALID;

    /* ---- Read file ---- */
    char *buf = read_file_alloc(path);
    if (!buf) {
        ESP_LOGW(TAG, "Cannot read: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    /* ---- Parse JSON ---- */
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        const char *err = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "JSON parse error near: %.32s", err ? err : "?");
        return ESP_ERR_NO_MEM;
    }

    /* ---- Required: name ---- */
    const cJSON *name_j = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name_j) || !name_j->valuestring || !name_j->valuestring[0]) {
        ESP_LOGE(TAG, "%s: missing 'name'", path);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    copy_str(out->name, sizeof(out->name), name_j);

    /* ---- Optional: id ---- */
    const cJSON *id_j = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsNumber(id_j) && (int)id_j->valuedouble >= APP_ID_DYNAMIC_BASE) {
        out->id = (app_id_t)(unsigned)id_j->valuedouble;
    }

    /* ---- icon ---- */
    copy_str(out->icon, sizeof(out->icon),
             cJSON_GetObjectItemCaseSensitive(root, "icon"));
    if (!out->icon[0]) {
        /* Default icon: first 2 chars of name */
        strncpy(out->icon, out->name, 2);
        out->icon[2] = '\0';
    }

    /* ---- type ---- */
    const cJSON *type_j = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type_j) && strcmp(type_j->valuestring, "builtin") == 0) {
        out->type = APP_TYPE_BUILTIN;
    } else {
        out->type = APP_TYPE_SCRIPT;
    }

    /* ---- script fields ---- */
    copy_str(out->runtime, sizeof(out->runtime),
             cJSON_GetObjectItemCaseSensitive(root, "runtime"));
    copy_str(out->entry,   sizeof(out->entry),
             cJSON_GetObjectItemCaseSensitive(root, "entry"));

    /* ---- permissions ---- */
    out->permissions = parse_permissions(
        cJSON_GetObjectItemCaseSensitive(root, "permissions"));

    /* ---- version ---- */
    copy_str(out->version, sizeof(out->version),
             cJSON_GetObjectItemCaseSensitive(root, "version"));

    /* ---- min_os_api ---- */
    const cJSON *api_j = cJSON_GetObjectItemCaseSensitive(root, "min_os_api");
    if (cJSON_IsNumber(api_j)) {
        out->min_os_api = (uint16_t)(unsigned)api_j->valuedouble;
    }

    cJSON_Delete(root);

    ESP_LOGD(TAG, "Parsed %s: name=%s id=%u type=%d rt=%s",
             path, out->name, (unsigned)out->id, out->type, out->runtime);
    return ESP_OK;
}
