#include "drivers/deck_sdi_fs.h"
#include "deck_sdi_registry.h"

#include "esp_spiffs.h"
#include "esp_log.h"

#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

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

static deck_sdi_err_t fs_write_impl(void *ctx, const char *path,
                                     const void *buf, size_t bytes)
{
    (void)ctx;
    if (!path || (!buf && bytes)) return DECK_SDI_ERR_INVALID_ARG;
    char full[128];
    if (to_vfs(path, full, sizeof(full)) >= sizeof(full))
        return DECK_SDI_ERR_INVALID_ARG;
    FILE *f = fopen(full, "wb");
    if (!f) return errno == ENOENT ? DECK_SDI_ERR_NOT_FOUND : DECK_SDI_ERR_IO;
    size_t n = bytes ? fwrite(buf, 1, bytes, f) : 0;
    int err = ferror(f);
    fclose(f);
    if (err || n != bytes) return DECK_SDI_ERR_IO;
    return DECK_SDI_OK;
}

static deck_sdi_err_t fs_create_impl(void *ctx, const char *path)
{
    (void)ctx;
    if (!path) return DECK_SDI_ERR_INVALID_ARG;
    char full[128];
    if (to_vfs(path, full, sizeof(full)) >= sizeof(full))
        return DECK_SDI_ERR_INVALID_ARG;
    struct stat st;
    if (stat(full, &st) == 0) return DECK_SDI_ERR_ALREADY_EXISTS;
    FILE *f = fopen(full, "wb");
    if (!f) return DECK_SDI_ERR_IO;
    fclose(f);
    return DECK_SDI_OK;
}

static deck_sdi_err_t fs_remove_impl(void *ctx, const char *path)
{
    (void)ctx;
    if (!path) return DECK_SDI_ERR_INVALID_ARG;
    char full[128];
    if (to_vfs(path, full, sizeof(full)) >= sizeof(full))
        return DECK_SDI_ERR_INVALID_ARG;
    struct stat st;
    if (stat(full, &st) != 0)
        return errno == ENOENT ? DECK_SDI_ERR_NOT_FOUND : DECK_SDI_ERR_IO;
    int rc = S_ISDIR(st.st_mode) ? rmdir(full) : unlink(full);
    if (rc != 0) return DECK_SDI_ERR_IO;
    return DECK_SDI_OK;
}

static deck_sdi_err_t fs_mkdir_impl(void *ctx, const char *path)
{
    (void)ctx;
    if (!path) return DECK_SDI_ERR_INVALID_ARG;
    char full[128];
    if (to_vfs(path, full, sizeof(full)) >= sizeof(full))
        return DECK_SDI_ERR_INVALID_ARG;
    if (mkdir(full, 0775) != 0) {
        if (errno == EEXIST) return DECK_SDI_ERR_ALREADY_EXISTS;
        if (errno == ENOENT) return DECK_SDI_ERR_NOT_FOUND;
        /* SPIFFS is flat — no real directories. Treat as not_supported
         * so callers can fall back to flat-name conventions. */
        if (errno == ENOTSUP || errno == ENOSYS) return DECK_SDI_ERR_NOT_SUPPORTED;
        return DECK_SDI_ERR_IO;
    }
    return DECK_SDI_OK;
}

static const deck_sdi_fs_vtable_t s_vtable = {
    .read   = fs_read_impl,
    .exists = fs_exists_impl,
    .list   = fs_list_impl,
    .write  = fs_write_impl,
    .create = fs_create_impl,
    .remove = fs_remove_impl,
    .mkdir  = fs_mkdir_impl,
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

deck_sdi_err_t deck_sdi_fs_write(const char *path, const void *buf, size_t bytes)
{
    void *ctx; const deck_sdi_fs_vtable_t *vt = fs_vt(&ctx);
    if (!vt) return DECK_SDI_ERR_NOT_FOUND;
    if (!vt->write) return DECK_SDI_ERR_NOT_SUPPORTED;
    return vt->write(ctx, path, buf, bytes);
}

deck_sdi_err_t deck_sdi_fs_create(const char *path)
{
    void *ctx; const deck_sdi_fs_vtable_t *vt = fs_vt(&ctx);
    if (!vt) return DECK_SDI_ERR_NOT_FOUND;
    if (!vt->create) return DECK_SDI_ERR_NOT_SUPPORTED;
    return vt->create(ctx, path);
}

deck_sdi_err_t deck_sdi_fs_remove(const char *path)
{
    void *ctx; const deck_sdi_fs_vtable_t *vt = fs_vt(&ctx);
    if (!vt) return DECK_SDI_ERR_NOT_FOUND;
    if (!vt->remove) return DECK_SDI_ERR_NOT_SUPPORTED;
    return vt->remove(ctx, path);
}

deck_sdi_err_t deck_sdi_fs_mkdir(const char *path)
{
    void *ctx; const deck_sdi_fs_vtable_t *vt = fs_vt(&ctx);
    if (!vt) return DECK_SDI_ERR_NOT_FOUND;
    if (!vt->mkdir) return DECK_SDI_ERR_NOT_SUPPORTED;
    return vt->mkdir(ctx, path);
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

    /* DL2 round-trip: write → read → remove. SPIFFS is flat; mkdir may
     * report NOT_SUPPORTED — which is acceptable for this driver. */
    const char *scratch = "/__sdi_fs_test__.bin";
    /* Best-effort cleanup from a previous aborted run. */
    (void)deck_sdi_fs_remove(scratch);

    const char payload[] = "deck-fs-roundtrip\n";
    const size_t plen = sizeof(payload) - 1;

    r = deck_sdi_fs_write(scratch, payload, plen);
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "write failed: %s", deck_sdi_strerror(r));
        return r;
    }

    bool is_dir = true;
    r = deck_sdi_fs_exists(scratch, &is_dir);
    if (r != DECK_SDI_OK || is_dir) {
        ESP_LOGE(TAG, "exists after write failed: r=%s dir=%d",
                 deck_sdi_strerror(r), is_dir);
        (void)deck_sdi_fs_remove(scratch);
        return DECK_SDI_ERR_FAIL;
    }

    char buf[64] = {0};
    size_t n = sizeof(buf);
    r = deck_sdi_fs_read(scratch, buf, &n);
    if (r != DECK_SDI_OK || n != plen || memcmp(buf, payload, plen) != 0) {
        ESP_LOGE(TAG, "read mismatch: r=%s n=%u expected=%u",
                 deck_sdi_strerror(r), (unsigned)n, (unsigned)plen);
        (void)deck_sdi_fs_remove(scratch);
        return DECK_SDI_ERR_FAIL;
    }

    /* create on existing path must return ALREADY_EXISTS. */
    r = deck_sdi_fs_create(scratch);
    if (r != DECK_SDI_ERR_ALREADY_EXISTS) {
        ESP_LOGE(TAG, "create on existing path: expected ALREADY_EXISTS, got %s",
                 deck_sdi_strerror(r));
        (void)deck_sdi_fs_remove(scratch);
        return DECK_SDI_ERR_FAIL;
    }

    r = deck_sdi_fs_remove(scratch);
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "remove failed: %s", deck_sdi_strerror(r));
        return r;
    }
    if (deck_sdi_fs_exists(scratch, NULL) != DECK_SDI_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "scratch still exists after remove");
        return DECK_SDI_ERR_FAIL;
    }

    /* mkdir probe: SPIFFS likely returns NOT_SUPPORTED, which is fine. */
    deck_sdi_err_t mk = deck_sdi_fs_mkdir("/__sdi_fs_dir__");
    bool mkdir_ok = (mk == DECK_SDI_OK);
    if (mkdir_ok) (void)deck_sdi_fs_remove("/__sdi_fs_dir__");

    ESP_LOGI(TAG,
        "selftest: PASS (list=%d, RW round-trip OK, mkdir=%s)",
        cc.count, mkdir_ok ? "ok" : deck_sdi_strerror(mk));
    return DECK_SDI_OK;
}
