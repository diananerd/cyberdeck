#include "drivers/deck_sdi_fs.h"
#include "deck_sdi_registry.h"

#include "esp_spiffs.h"
#include "esp_log.h"

#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

static const char *TAG = "sdi.fs";

/* Partition label in partitions.csv and mount point under VFS. */
#define FS_PART_LABEL     "apps"
#define FS_MOUNT_POINT    "/deck"

static bool s_mounted = false;

static deck_sdi_err_t mount_partition(void)
{
    if (s_mounted) return DECK_SDI_OK;
    esp_vfs_spiffs_conf_t cfg = {
        .base_path       = FS_MOUNT_POINT,
        .partition_label = FS_PART_LABEL,
        .max_files       = 8,
        .format_if_mount_failed = true,
    };
    esp_err_t e = esp_vfs_spiffs_register(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(e));
        return DECK_SDI_ERR_IO;
    }
    size_t total = 0, used = 0;
    if (esp_spiffs_info(FS_PART_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "mounted at %s — %u/%u bytes used",
                 FS_MOUNT_POINT, (unsigned)used, (unsigned)total);
    }
    s_mounted = true;
    return DECK_SDI_OK;
}

/* Build the VFS path for a user-visible path. "/foo/bar" → "/deck/foo/bar".
 * "/" → "/deck". Caller supplies a buffer; returns required length.
 */
static size_t to_vfs(const char *path, char *buf, size_t buf_size)
{
    if (!path || !*path) path = "/";
    return (size_t)snprintf(buf, buf_size, "%s%s",
                            FS_MOUNT_POINT,
                            path[0] == '/' ? path : "/");
}

static deck_sdi_err_t fs_read_impl(void *ctx, const char *path,
                                    void *out, size_t *io_bytes)
{
    (void)ctx;
    if (!path || !out || !io_bytes) return DECK_SDI_ERR_INVALID_ARG;
    char full[128];
    if (to_vfs(path, full, sizeof(full)) >= sizeof(full))
        return DECK_SDI_ERR_INVALID_ARG;
    FILE *f = fopen(full, "rb");
    if (!f) return errno == ENOENT ? DECK_SDI_ERR_NOT_FOUND : DECK_SDI_ERR_IO;
    size_t n = fread(out, 1, *io_bytes, f);
    int err = ferror(f);
    fclose(f);
    if (err) return DECK_SDI_ERR_IO;
    *io_bytes = n;
    return DECK_SDI_OK;
}

static deck_sdi_err_t fs_exists_impl(void *ctx, const char *path,
                                      bool *is_dir_out)
{
    (void)ctx;
    if (!path) return DECK_SDI_ERR_INVALID_ARG;
    char full[128];
    if (to_vfs(path, full, sizeof(full)) >= sizeof(full))
        return DECK_SDI_ERR_INVALID_ARG;
    struct stat st;
    if (stat(full, &st) != 0)
        return errno == ENOENT ? DECK_SDI_ERR_NOT_FOUND : DECK_SDI_ERR_IO;
    if (is_dir_out) *is_dir_out = S_ISDIR(st.st_mode);
    return DECK_SDI_OK;
}

static deck_sdi_err_t fs_list_impl(void *ctx, const char *path,
                                    bool (*cb)(const char *, bool, void *),
                                    void *user)
{
    (void)ctx;
    if (!path || !cb) return DECK_SDI_ERR_INVALID_ARG;
    char full[128];
    if (to_vfs(path, full, sizeof(full)) >= sizeof(full))
        return DECK_SDI_ERR_INVALID_ARG;
    DIR *d = opendir(full);
    if (!d) return errno == ENOENT ? DECK_SDI_ERR_NOT_FOUND : DECK_SDI_ERR_IO;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        bool is_dir = (e->d_type == DT_DIR);
        if (!cb(e->d_name, is_dir, user)) break;
    }
    closedir(d);
    return DECK_SDI_OK;
}

static const deck_sdi_fs_vtable_t s_vtable = {
    .read   = fs_read_impl,
    .exists = fs_exists_impl,
    .list   = fs_list_impl,
};

deck_sdi_err_t deck_sdi_fs_register_spiffs(void)
{
    deck_sdi_err_t m = mount_partition();
    if (m != DECK_SDI_OK) return m;
    const deck_sdi_driver_t driver = {
        .name    = "storage.fs",
        .id      = DECK_SDI_DRIVER_FS,
        .version = "1.0.0",
        .vtable  = &s_vtable,
        .ctx     = NULL,
    };
    return deck_sdi_register(&driver);
}

/* ---------- wrappers ---------- */

static const deck_sdi_fs_vtable_t *fs_vt(void **ctx_out)
{
    const deck_sdi_driver_t *d = deck_sdi_lookup(DECK_SDI_DRIVER_FS);
    if (!d) return NULL;
    if (ctx_out) *ctx_out = d->ctx;
    return (const deck_sdi_fs_vtable_t *)d->vtable;
}

deck_sdi_err_t deck_sdi_fs_read(const char *path, void *out, size_t *io_bytes)
{
    void *ctx; const deck_sdi_fs_vtable_t *vt = fs_vt(&ctx);
    if (!vt) return DECK_SDI_ERR_NOT_FOUND;
    return vt->read(ctx, path, out, io_bytes);
}

deck_sdi_err_t deck_sdi_fs_exists(const char *path, bool *is_dir_out)
{
    void *ctx; const deck_sdi_fs_vtable_t *vt = fs_vt(&ctx);
    if (!vt) return DECK_SDI_ERR_NOT_FOUND;
    return vt->exists(ctx, path, is_dir_out);
}

deck_sdi_err_t deck_sdi_fs_list(const char *path,
                                bool (*cb)(const char *, bool, void *),
                                void *user)
{
    void *ctx; const deck_sdi_fs_vtable_t *vt = fs_vt(&ctx);
    if (!vt) return DECK_SDI_ERR_NOT_FOUND;
    return vt->list(ctx, path, cb, user);
}

/* ---------- selftest ---------- */

typedef struct { int count; } count_ctx_t;
static bool count_cb(const char *name, bool is_dir, void *user)
{
    (void)name; (void)is_dir;
    count_ctx_t *c = user;
    c->count++;
    return true;
}

deck_sdi_err_t deck_sdi_fs_selftest(void)
{
    /* list("/") must succeed. Count reported for visibility. */
    count_ctx_t cc = {0};
    deck_sdi_err_t r = deck_sdi_fs_list("/", count_cb, &cc);
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "list(/) failed: %s", deck_sdi_strerror(r));
        return r;
    }

    /* exists on a known-missing path must return NOT_FOUND. */
    r = deck_sdi_fs_exists("/__nope__", NULL);
    if (r != DECK_SDI_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "expected not_found on /__nope__, got %s",
                 deck_sdi_strerror(r));
        return DECK_SDI_ERR_FAIL;
    }

    ESP_LOGI(TAG, "selftest: PASS (list root: %d entries, missing path → not_found)",
             cc.count);
    return DECK_SDI_OK;
}
