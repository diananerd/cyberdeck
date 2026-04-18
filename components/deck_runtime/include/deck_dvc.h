#pragma once

/* deck_dvc — DeckViewContent wire format.
 *
 * The runtime emits snapshots in this format; the bridge UI driver
 * decodes them into platform widgets. Both ends share this header.
 *
 * Spec: deck-lang/11-deck-implementation.md §18 + 10-deck-bridge-ui.md.
 *
 * F26.1 ships:
 *   - Type catalog (DvcType)
 *   - Standard attribute keys (DvcAttrKey)
 *   - In-memory tree (DvcNode/DvcAttr) over a render arena
 *   - Wire encoder / decoder (round-trippable)
 *
 * F26.2+ wires the decoder to LVGL.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "deck_arena.h"
#include "deck_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- wire envelope ---------- */

#define DECK_DVC_MAGIC        0xDC0Eu
#define DECK_DVC_VERSION      1

/* ---------- node type catalog (spec §18.3) ---------- */

typedef enum {
    DVC_EMPTY            = 0,
    DVC_FLOW             = 1,
    DVC_GROUP            = 2,
    DVC_COLUMN           = 3,
    DVC_ROW              = 4,
    DVC_GRID             = 5,
    DVC_LIST             = 6,
    DVC_LIST_ITEM        = 7,
    DVC_DATA_ROW         = 8,
    DVC_TEXT             = 9,
    DVC_PASSWORD         = 10,
    DVC_SWITCH           = 11,
    DVC_SLIDER           = 12,
    DVC_CHOICE           = 13,
    DVC_TRIGGER          = 14,
    DVC_NAVIGATE         = 15,
    DVC_TOGGLE           = 16,
    DVC_DATE_PICKER      = 17,
    DVC_PIN              = 18,
    DVC_LABEL            = 19,
    DVC_RICH_TEXT        = 20,
    DVC_MEDIA            = 21,
    DVC_PROGRESS         = 22,
    DVC_LOADING          = 23,
    DVC_TOAST            = 24,
    DVC_CONFIRM          = 25,
    DVC_SHARE            = 26,
    DVC_CHART            = 27,
    DVC_SPACER           = 28,
    DVC_DIVIDER          = 29,
    DVC_FORM             = 30,
    DVC_MARKDOWN         = 31,
    DVC_MARKDOWN_EDITOR  = 32,
    DVC_CUSTOM           = 33,
    /* 34..127 reserved for future first-party additions. */
    /* 128..255 reserved for third-party module-registered types. */
} deck_dvc_type_t;

/* ---------- attribute value types ---------- */

/* Subset of DeckType — wire-safe scalars only. No FN/STREAM/OPAQUE. */
typedef enum {
    DVC_ATTR_NONE   = 0,
    DVC_ATTR_BOOL   = 1,
    DVC_ATTR_I64    = 2,
    DVC_ATTR_F64    = 3,
    DVC_ATTR_STR    = 4,    /* UTF-8, NUL-terminated, length-prefixed on wire */
    DVC_ATTR_ATOM   = 5,    /* atom name as string on wire (the runtime interns it) */
    DVC_ATTR_LIST_STR = 6,  /* list of strings (e.g. CHOICE :options) */
} deck_dvc_attr_type_t;

typedef struct {
    const char *atom;           /* arena-owned, e.g. "title" (no leading colon) */
    deck_dvc_attr_type_t type;
    union {
        bool        b;
        int64_t     i;
        double      f;
        const char *s;          /* arena-owned */
        struct {
            const char **items; /* arena-owned array of arena-owned strings */
            uint16_t     count;
        } list_str;
    } value;
} deck_dvc_attr_t;

/* ---------- in-memory tree ---------- */

typedef struct deck_dvc_node {
    uint16_t                  type;          /* deck_dvc_type_t */
    uint16_t                  flags;         /* node-type-specific bitfield */
    uint32_t                  intent_id;     /* 0 = no intent */
    uint16_t                  attr_count;
    uint16_t                  child_count;
    deck_dvc_attr_t          *attrs;         /* arena-owned */
    struct deck_dvc_node    **children;      /* arena-owned array of pointers */
} deck_dvc_node_t;

/* ---------- node + attr builders (over a render arena) ---------- */

deck_dvc_node_t *deck_dvc_node_new(deck_arena_t *arena, deck_dvc_type_t type);

/* Append a child node. Reallocates the parent's children pointer array
 * inside the arena. Returns child for chaining, NULL on OOM. */
deck_dvc_node_t *deck_dvc_add_child(deck_arena_t *arena,
                                     deck_dvc_node_t *parent,
                                     deck_dvc_node_t *child);

/* Set an attribute on `node`. atom may be a literal string ("title"); it
 * is duplicated into the arena. For string values, the value is also
 * duplicated. Replaces an existing attribute with the same atom. */
deck_err_t deck_dvc_set_bool (deck_arena_t *arena, deck_dvc_node_t *node,
                              const char *atom, bool value);
deck_err_t deck_dvc_set_i64  (deck_arena_t *arena, deck_dvc_node_t *node,
                              const char *atom, int64_t value);
deck_err_t deck_dvc_set_f64  (deck_arena_t *arena, deck_dvc_node_t *node,
                              const char *atom, double value);
deck_err_t deck_dvc_set_str  (deck_arena_t *arena, deck_dvc_node_t *node,
                              const char *atom, const char *value);
deck_err_t deck_dvc_set_atom (deck_arena_t *arena, deck_dvc_node_t *node,
                              const char *atom, const char *atom_value);
deck_err_t deck_dvc_set_list_str(deck_arena_t *arena, deck_dvc_node_t *node,
                                  const char *atom,
                                  const char *const *items, uint16_t count);

/* Lookup helper. Returns NULL if not found. */
const deck_dvc_attr_t *deck_dvc_find_attr(const deck_dvc_node_t *node,
                                           const char *atom);

/* ---------- wire envelope ---------- */

/* Encode a DVC tree to a flat byte buffer.
 *
 * Layout (all little-endian, packed):
 *   u16 magic           = 0xDC0E
 *   u8  version         = 1
 *   u8  flags           = 0 (reserved)
 *   u32 root_offset     = bytes from start to root node
 *   nodes...            packed depth-first
 *
 * Each node (at offset `root_offset` and recursively):
 *   u16 type
 *   u16 flags
 *   u32 intent_id
 *   u16 attr_count
 *   u16 child_count
 *   <attr_count attrs>
 *   <child_count children inline, depth-first>
 *
 * Each attr:
 *   u16 atom_len        u8[atom_len] atom_bytes (no NUL)
 *   u8  attr_type       (deck_dvc_attr_type_t)
 *   <value bytes>
 *     BOOL:    u8
 *     I64:     i64
 *     F64:     f64
 *     STR/ATOM: u32 len  u8[len] bytes (no NUL)
 *     LIST_STR: u16 count  [u32 len  u8[len] bytes] × count
 *
 * Returns:
 *   DECK_RT_OK              on success; *out_len gets bytes written.
 *   DECK_RT_ERR_RUNTIME if cap is too small (sets *out_len to required size).
 *   DECK_RT_ERR_RUNTIME for any internal inconsistency.
 */
deck_err_t deck_dvc_encode(const deck_dvc_node_t *root,
                           void *out_buf, size_t cap,
                           size_t *out_len);

/* Decode bytes back into a tree allocated in `arena`. *out_root is set
 * to the new root node. On error, the arena is not freed; caller may
 * reset it. */
deck_err_t deck_dvc_decode(const void *bytes, size_t len,
                           deck_arena_t *arena,
                           deck_dvc_node_t **out_root);

/* Compare two trees structurally (type+flags+intent+attrs+children).
 * Returns 0 on equal, nonzero on mismatch. Used by selftests. */
int deck_dvc_tree_equal(const deck_dvc_node_t *a, const deck_dvc_node_t *b);

/* Selftest: build a small tree, encode → decode → equal. */
deck_err_t deck_dvc_selftest(void);

#ifdef __cplusplus
}
#endif
