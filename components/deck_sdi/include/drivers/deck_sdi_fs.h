#pragma once

/* storage.fs — filesystem surface.
 *
 * Mandatory at DL1 as read-only. DL2+ adds write, create, delete, mkdir.
 *
 * Paths are POSIX-style relative to the driver's mount root, with a
 * leading slash (e.g. "/apps/hello/main.deck"). Platform implementations
 * map these to the underlying filesystem path.
 *
 * See deck-lang/05-deck-os-api.md §3 for the capability contract.
 */

#include "deck_sdi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Max bytes returned by a single list() entry. Long paths are truncated. */
#define DECK_SDI_FS_NAME_MAX    64

typedef struct {
    /* Read up to out_size bytes from path into out. *io_bytes is both:
     * input — max bytes to read; output — bytes actually read. */
    deck_sdi_err_t (*read)(void *ctx, const char *path,
                           void *out, size_t *io_bytes);

    /* Existence probe. Returns OK if the path exists (file or dir),
     * NOT_FOUND otherwise. *is_dir_out set on OK if non-NULL. */
    deck_sdi_err_t (*exists)(void *ctx, const char *path, bool *is_dir_out);

    /* Iterate entries in a directory. cb is called once per entry.
     * cb returns false to stop early. Returns OK on full iteration
     * or NOT_FOUND if path is not a directory. */
    deck_sdi_err_t (*list)(void *ctx, const char *path,
                           bool (*cb)(const char *name, bool is_dir, void *user),
                           void *user);

    /* DL2+ writable surface. NULL → DECK_SDI_ERR_NOT_SUPPORTED at the
     * wrapper layer (DL1 read-only platforms). */

    /* Write `bytes` of `buf` to `path`, fully truncating any prior content.
     * Creates the file if absent. Parent dir must exist. */
    deck_sdi_err_t (*write)(void *ctx, const char *path,
                            const void *buf, size_t bytes);

    /* Create an empty file. ERR_ALREADY_EXISTS if path exists. */
    deck_sdi_err_t (*create)(void *ctx, const char *path);

    /* Delete file (or empty directory). NOT_FOUND if absent. */
    deck_sdi_err_t (*remove)(void *ctx, const char *path);

    /* Create a directory. ERR_ALREADY_EXISTS if path exists. Parent dir
     * must already exist (no recursive mkdir at SDI surface). */
    deck_sdi_err_t (*mkdir)(void *ctx, const char *path);
} deck_sdi_fs_vtable_t;

/* Platform registration — SPIFFS impl backed by the "apps" partition. */
deck_sdi_err_t deck_sdi_fs_register_spiffs(void);

/* High-level wrappers. */
deck_sdi_err_t deck_sdi_fs_read(const char *path, void *out, size_t *io_bytes);
deck_sdi_err_t deck_sdi_fs_exists(const char *path, bool *is_dir_out);
deck_sdi_err_t deck_sdi_fs_list(const char *path,
                                bool (*cb)(const char *name, bool is_dir, void *user),
                                void *user);

/* DL2+ writable wrappers. Return DECK_SDI_ERR_NOT_SUPPORTED when the
 * underlying driver does not implement the op. */
deck_sdi_err_t deck_sdi_fs_write(const char *path, const void *buf, size_t bytes);
deck_sdi_err_t deck_sdi_fs_create(const char *path);
deck_sdi_err_t deck_sdi_fs_remove(const char *path);
deck_sdi_err_t deck_sdi_fs_mkdir(const char *path);

/* Selftest: list("/") + exists on a known-missing path returns NOT_FOUND.
 * If the driver advertises writable ops, also exercises mkdir → write →
 * read → remove round-trip in a temporary scratch directory. */
deck_sdi_err_t deck_sdi_fs_selftest(void);

#ifdef __cplusplus
}
#endif
