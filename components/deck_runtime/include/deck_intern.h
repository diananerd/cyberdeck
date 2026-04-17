#pragma once

/* deck_intern — string interning table.
 *
 * All atoms and string literals pass through here so that comparison
 * is pointer-equality and there is no duplicate storage for the same
 * literal (e.g. capability names, record field names, atoms like
 * :ok / :error repeated across thousands of match arms).
 *
 * Open-addressed hash table with linear probing, FNV-1a hash, power-of-two
 * capacity. Strings are copied into a heap-owned buffer on first intern;
 * the returned const char * is stable for the lifetime of the table and
 * always NUL-terminated.
 *
 * Intern storage is separate from deck_alloc tracking — it does not count
 * against the DL1 heap hard-limit (it represents long-lived program data
 * loaded once per app). The spec treats intern usage as load-time cost.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the intern table. Capacity is rounded up to power of 2.
 * Safe to call multiple times; subsequent calls are no-ops. */
void deck_intern_init(uint32_t initial_cap);

/* Reset — frees all interned strings. Only safe when no live atoms/strs
 * reference them (i.e. between apps, or before shutdown). */
void deck_intern_reset(void);

/* Intern a range. Returns a stable NUL-terminated pointer.
 * NULL on allocation failure. */
const char *deck_intern(const char *s, uint32_t len);

/* Convenience for NUL-terminated input. */
const char *deck_intern_cstr(const char *s);

/* Number of unique strings currently in the table. */
uint32_t deck_intern_count(void);

/* Total bytes held by the intern table (struct overhead + string buffers). */
size_t deck_intern_bytes(void);

#ifdef __cplusplus
}
#endif
