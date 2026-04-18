/* deck_dvc — DeckViewContent wire format encoder/decoder.
 *
 * Spec: deck-lang/11-deck-implementation.md §18.
 *
 * The encoder walks an in-memory tree (allocated in a render arena) and
 * emits a flat little-endian byte stream. The decoder reverses the
 * process into a fresh arena. Both ends fail closed on truncation,
 * unknown attr types, or self-inconsistent length fields.
 */

#include "deck_dvc.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "deck_dvc";

/* ---------- little-endian write helpers ---------- */

typedef struct {
    uint8_t  *buf;
    size_t    cap;
    size_t    pos;
    bool      overflow;
} writer_t;

static void w_u8 (writer_t *w, uint8_t v)
{
    if (w->pos + 1 > w->cap) { w->overflow = true; w->pos++; return; }
    w->buf[w->pos++] = v;
}
static void w_u16(writer_t *w, uint16_t v)
{
    if (w->pos + 2 > w->cap) { w->overflow = true; w->pos += 2; return; }
    w->buf[w->pos++] = (uint8_t)(v & 0xFF);
    w->buf[w->pos++] = (uint8_t)((v >> 8) & 0xFF);
}
static void w_u32(writer_t *w, uint32_t v)
{
    if (w->pos + 4 > w->cap) { w->overflow = true; w->pos += 4; return; }
    w->buf[w->pos++] = (uint8_t)(v & 0xFF);
    w->buf[w->pos++] = (uint8_t)((v >> 8) & 0xFF);
    w->buf[w->pos++] = (uint8_t)((v >> 16) & 0xFF);
    w->buf[w->pos++] = (uint8_t)((v >> 24) & 0xFF);
}
static void w_i64(writer_t *w, int64_t v) /* two's complement */
{
    uint64_t u = (uint64_t)v;
    if (w->pos + 8 > w->cap) { w->overflow = true; w->pos += 8; return; }
    for (int i = 0; i < 8; i++) {
        w->buf[w->pos++] = (uint8_t)((u >> (8 * i)) & 0xFF);
    }
}
static void w_f64(writer_t *w, double v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof(u));
    if (w->pos + 8 > w->cap) { w->overflow = true; w->pos += 8; return; }
    for (int i = 0; i < 8; i++) {
        w->buf[w->pos++] = (uint8_t)((u >> (8 * i)) & 0xFF);
    }
}
static void w_bytes(writer_t *w, const void *src, size_t n)
{
    if (w->pos + n > w->cap) { w->overflow = true; w->pos += n; return; }
    memcpy(w->buf + w->pos, src, n);
    w->pos += n;
}

static void w_lenstr_u32(writer_t *w, const char *s)
{
    if (!s) { w_u32(w, 0); return; }
    size_t n = strlen(s);
    w_u32(w, (uint32_t)n);
    w_bytes(w, s, n);
}
static void w_lenstr_u16(writer_t *w, const char *s)
{
    if (!s) { w_u16(w, 0); return; }
    size_t n = strlen(s);
    if (n > 0xFFFF) n = 0xFFFF;
    w_u16(w, (uint16_t)n);
    w_bytes(w, s, n);
}

/* ---------- builders ---------- */

deck_dvc_node_t *deck_dvc_node_new(deck_arena_t *arena, deck_dvc_type_t type)
{
    deck_dvc_node_t *n = deck_arena_zalloc(arena, sizeof(*n));
    if (!n) return NULL;
    n->type = (uint16_t)type;
    return n;
}

deck_dvc_node_t *deck_dvc_add_child(deck_arena_t *arena,
                                     deck_dvc_node_t *parent,
                                     deck_dvc_node_t *child)
{
    if (!parent || !child) return NULL;
    /* Realloc-by-copy: arenas don't free, so this leaks the old slot.
     * Renders are short-lived; arena reset reclaims everything. */
    size_t old_n = parent->child_count;
    deck_dvc_node_t **dst = deck_arena_alloc(arena,
                                              (old_n + 1) * sizeof(*dst));
    if (!dst) return NULL;
    if (old_n) memcpy(dst, parent->children, old_n * sizeof(*dst));
    dst[old_n] = child;
    parent->children = dst;
    parent->child_count = (uint16_t)(old_n + 1);
    return child;
}

static deck_dvc_attr_t *upsert_attr(deck_arena_t *arena, deck_dvc_node_t *node,
                                     const char *atom)
{
    /* Replace existing? */
    for (uint16_t i = 0; i < node->attr_count; i++) {
        if (strcmp(node->attrs[i].atom, atom) == 0) return &node->attrs[i];
    }
    size_t old_n = node->attr_count;
    deck_dvc_attr_t *dst = deck_arena_alloc(arena,
                                             (old_n + 1) * sizeof(*dst));
    if (!dst) return NULL;
    if (old_n) memcpy(dst, node->attrs, old_n * sizeof(*dst));
    char *atom_dup = deck_arena_strdup(arena, atom);
    if (!atom_dup) return NULL;
    dst[old_n].atom = atom_dup;
    dst[old_n].type = DVC_ATTR_NONE;
    memset(&dst[old_n].value, 0, sizeof(dst[old_n].value));
    node->attrs = dst;
    node->attr_count = (uint16_t)(old_n + 1);
    return &dst[old_n];
}

deck_err_t deck_dvc_set_bool(deck_arena_t *arena, deck_dvc_node_t *node,
                              const char *atom, bool value)
{
    if (!arena || !node || !atom) return DECK_RT_INTERNAL;
    deck_dvc_attr_t *a = upsert_attr(arena, node, atom);
    if (!a) return DECK_RT_NO_MEMORY;
    a->type = DVC_ATTR_BOOL;
    a->value.b = value;
    return DECK_RT_OK;
}

deck_err_t deck_dvc_set_i64(deck_arena_t *arena, deck_dvc_node_t *node,
                             const char *atom, int64_t value)
{
    if (!arena || !node || !atom) return DECK_RT_INTERNAL;
    deck_dvc_attr_t *a = upsert_attr(arena, node, atom);
    if (!a) return DECK_RT_NO_MEMORY;
    a->type = DVC_ATTR_I64;
    a->value.i = value;
    return DECK_RT_OK;
}

deck_err_t deck_dvc_set_f64(deck_arena_t *arena, deck_dvc_node_t *node,
                             const char *atom, double value)
{
    if (!arena || !node || !atom) return DECK_RT_INTERNAL;
    deck_dvc_attr_t *a = upsert_attr(arena, node, atom);
    if (!a) return DECK_RT_NO_MEMORY;
    a->type = DVC_ATTR_F64;
    a->value.f = value;
    return DECK_RT_OK;
}

deck_err_t deck_dvc_set_str(deck_arena_t *arena, deck_dvc_node_t *node,
                             const char *atom, const char *value)
{
    if (!arena || !node || !atom) return DECK_RT_INTERNAL;
    deck_dvc_attr_t *a = upsert_attr(arena, node, atom);
    if (!a) return DECK_RT_NO_MEMORY;
    a->type = DVC_ATTR_STR;
    a->value.s = value ? deck_arena_strdup(arena, value) : "";
    if (value && !a->value.s) return DECK_RT_NO_MEMORY;
    return DECK_RT_OK;
}

deck_err_t deck_dvc_set_atom(deck_arena_t *arena, deck_dvc_node_t *node,
                              const char *atom, const char *atom_value)
{
    if (!arena || !node || !atom || !atom_value) return DECK_RT_INTERNAL;
    deck_dvc_attr_t *a = upsert_attr(arena, node, atom);
    if (!a) return DECK_RT_NO_MEMORY;
    a->type = DVC_ATTR_ATOM;
    a->value.s = deck_arena_strdup(arena, atom_value);
    if (!a->value.s) return DECK_RT_NO_MEMORY;
    return DECK_RT_OK;
}

deck_err_t deck_dvc_set_list_str(deck_arena_t *arena, deck_dvc_node_t *node,
                                  const char *atom,
                                  const char *const *items, uint16_t count)
{
    if (!arena || !node || !atom) return DECK_RT_INTERNAL;
    if (count > 0 && !items)      return DECK_RT_INTERNAL;
    deck_dvc_attr_t *a = upsert_attr(arena, node, atom);
    if (!a) return DECK_RT_NO_MEMORY;

    const char **dup = NULL;
    if (count) {
        dup = deck_arena_alloc(arena, count * sizeof(*dup));
        if (!dup) return DECK_RT_NO_MEMORY;
        for (uint16_t i = 0; i < count; i++) {
            if (!items[i]) return DECK_RT_INTERNAL;
            dup[i] = deck_arena_strdup(arena, items[i]);
            if (!dup[i]) return DECK_RT_NO_MEMORY;
        }
    }
    a->type = DVC_ATTR_LIST_STR;
    a->value.list_str.items = dup;
    a->value.list_str.count = count;
    return DECK_RT_OK;
}

const deck_dvc_attr_t *deck_dvc_find_attr(const deck_dvc_node_t *node,
                                           const char *atom)
{
    if (!node || !atom) return NULL;
    for (uint16_t i = 0; i < node->attr_count; i++) {
        if (strcmp(node->attrs[i].atom, atom) == 0) return &node->attrs[i];
    }
    return NULL;
}

/* ---------- encoder ---------- */

static void encode_attr(writer_t *w, const deck_dvc_attr_t *a)
{
    w_lenstr_u16(w, a->atom);
    w_u8(w, (uint8_t)a->type);
    switch (a->type) {
        case DVC_ATTR_NONE: break;
        case DVC_ATTR_BOOL: w_u8(w, a->value.b ? 1 : 0); break;
        case DVC_ATTR_I64:  w_i64(w, a->value.i); break;
        case DVC_ATTR_F64:  w_f64(w, a->value.f); break;
        case DVC_ATTR_STR:
        case DVC_ATTR_ATOM: w_lenstr_u32(w, a->value.s); break;
        case DVC_ATTR_LIST_STR:
            w_u16(w, a->value.list_str.count);
            for (uint16_t i = 0; i < a->value.list_str.count; i++) {
                w_lenstr_u32(w, a->value.list_str.items[i]);
            }
            break;
        default: break;
    }
}

static void encode_node(writer_t *w, const deck_dvc_node_t *n)
{
    if (!n) {
        /* Encode an empty placeholder. */
        w_u16(w, DVC_EMPTY); w_u16(w, 0); w_u32(w, 0);
        w_u16(w, 0); w_u16(w, 0);
        return;
    }
    w_u16(w, n->type);
    w_u16(w, n->flags);
    w_u32(w, n->intent_id);
    w_u16(w, n->attr_count);
    w_u16(w, n->child_count);
    for (uint16_t i = 0; i < n->attr_count; i++) encode_attr(w, &n->attrs[i]);
    for (uint16_t i = 0; i < n->child_count; i++) encode_node(w, n->children[i]);
}

deck_err_t deck_dvc_encode(const deck_dvc_node_t *root,
                           void *out_buf, size_t cap,
                           size_t *out_len)
{
    if (!out_len) return DECK_RT_INTERNAL;
    writer_t w = { .buf = out_buf, .cap = out_buf ? cap : 0, .pos = 0 };

    /* Envelope header. */
    w_u16(&w, DECK_DVC_MAGIC);
    w_u8 (&w, DECK_DVC_VERSION);
    w_u8 (&w, 0);          /* flags */
    /* root_offset is fixed — header is 8 bytes total. */
    w_u32(&w, 8);
    encode_node(&w, root);

    *out_len = w.pos;
    if (w.overflow) return DECK_RT_NO_MEMORY;
    return DECK_RT_OK;
}

/* ---------- decoder ---------- */

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    bool           bad;
} reader_t;

static uint8_t r_u8(reader_t *r)
{
    if (r->bad || r->pos + 1 > r->len) { r->bad = true; return 0; }
    return r->buf[r->pos++];
}
static uint16_t r_u16(reader_t *r)
{
    if (r->bad || r->pos + 2 > r->len) { r->bad = true; return 0; }
    uint16_t v = (uint16_t)r->buf[r->pos]
               | ((uint16_t)r->buf[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}
static uint32_t r_u32(reader_t *r)
{
    if (r->bad || r->pos + 4 > r->len) { r->bad = true; return 0; }
    uint32_t v = (uint32_t)r->buf[r->pos]
               | ((uint32_t)r->buf[r->pos + 1] << 8)
               | ((uint32_t)r->buf[r->pos + 2] << 16)
               | ((uint32_t)r->buf[r->pos + 3] << 24);
    r->pos += 4;
    return v;
}
static int64_t r_i64(reader_t *r)
{
    if (r->bad || r->pos + 8 > r->len) { r->bad = true; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)r->buf[r->pos + i]) << (8 * i);
    r->pos += 8;
    return (int64_t)v;
}
static double r_f64(reader_t *r)
{
    int64_t i = r_i64(r);
    double f;
    memcpy(&f, &i, sizeof(f));
    return f;
}
static char *r_lenstr_u32(reader_t *r, deck_arena_t *arena)
{
    uint32_t n = r_u32(r);
    if (r->bad) return NULL;
    if (r->pos + n > r->len) { r->bad = true; return NULL; }
    char *s = deck_arena_alloc(arena, n + 1);
    if (!s) { r->bad = true; return NULL; }
    if (n) memcpy(s, r->buf + r->pos, n);
    s[n] = '\0';
    r->pos += n;
    return s;
}
static char *r_lenstr_u16(reader_t *r, deck_arena_t *arena)
{
    uint16_t n = r_u16(r);
    if (r->bad) return NULL;
    if (r->pos + n > r->len) { r->bad = true; return NULL; }
    char *s = deck_arena_alloc(arena, (size_t)n + 1);
    if (!s) { r->bad = true; return NULL; }
    if (n) memcpy(s, r->buf + r->pos, n);
    s[n] = '\0';
    r->pos += n;
    return s;
}

static deck_dvc_node_t *decode_node(reader_t *r, deck_arena_t *arena);

static bool decode_attr(reader_t *r, deck_arena_t *arena, deck_dvc_attr_t *out)
{
    char *atom = r_lenstr_u16(r, arena);
    if (r->bad || !atom) return false;
    uint8_t t = r_u8(r);
    if (r->bad) return false;
    out->atom = atom;
    out->type = (deck_dvc_attr_type_t)t;
    switch ((deck_dvc_attr_type_t)t) {
        case DVC_ATTR_NONE: break;
        case DVC_ATTR_BOOL: out->value.b = (r_u8(r) != 0); break;
        case DVC_ATTR_I64:  out->value.i = r_i64(r); break;
        case DVC_ATTR_F64:  out->value.f = r_f64(r); break;
        case DVC_ATTR_STR:
        case DVC_ATTR_ATOM:
            out->value.s = r_lenstr_u32(r, arena);
            break;
        case DVC_ATTR_LIST_STR: {
            uint16_t count = r_u16(r);
            const char **items = NULL;
            if (count) {
                items = deck_arena_alloc(arena, count * sizeof(*items));
                if (!items) { r->bad = true; return false; }
                for (uint16_t i = 0; i < count; i++) {
                    items[i] = r_lenstr_u32(r, arena);
                    if (r->bad) return false;
                }
            }
            out->value.list_str.items = items;
            out->value.list_str.count = count;
            break;
        }
        default:
            /* Unknown attr type — fail closed. */
            r->bad = true;
            return false;
    }
    return !r->bad;
}

static deck_dvc_node_t *decode_node(reader_t *r, deck_arena_t *arena)
{
    deck_dvc_node_t *n = deck_arena_zalloc(arena, sizeof(*n));
    if (!n) { r->bad = true; return NULL; }
    n->type        = r_u16(r);
    n->flags       = r_u16(r);
    n->intent_id   = r_u32(r);
    n->attr_count  = r_u16(r);
    n->child_count = r_u16(r);
    if (r->bad) return NULL;

    if (n->attr_count) {
        n->attrs = deck_arena_zalloc(arena, n->attr_count * sizeof(*n->attrs));
        if (!n->attrs) { r->bad = true; return NULL; }
        for (uint16_t i = 0; i < n->attr_count; i++) {
            if (!decode_attr(r, arena, &n->attrs[i])) return NULL;
        }
    }
    if (n->child_count) {
        n->children = deck_arena_zalloc(arena, n->child_count * sizeof(*n->children));
        if (!n->children) { r->bad = true; return NULL; }
        for (uint16_t i = 0; i < n->child_count; i++) {
            n->children[i] = decode_node(r, arena);
            if (!n->children[i]) return NULL;
        }
    }
    return n;
}

deck_err_t deck_dvc_decode(const void *bytes, size_t len,
                           deck_arena_t *arena,
                           deck_dvc_node_t **out_root)
{
    if (!bytes || !arena || !out_root) return DECK_RT_INTERNAL;
    *out_root = NULL;

    reader_t r = { .buf = bytes, .len = len, .pos = 0 };
    uint16_t magic = r_u16(&r);
    if (magic != DECK_DVC_MAGIC) {
        ESP_LOGW(TAG, "decode: bad magic 0x%04x (expected 0x%04x)",
                 magic, DECK_DVC_MAGIC);
        return DECK_RT_INTERNAL;
    }
    uint8_t version = r_u8(&r);
    if (version != DECK_DVC_VERSION) {
        ESP_LOGW(TAG, "decode: bad version %u (expected %u)",
                 version, DECK_DVC_VERSION);
        return DECK_RT_INTERNAL;
    }
    (void)r_u8(&r);                /* flags reserved */
    uint32_t root_off = r_u32(&r);
    if (r.bad || root_off >= len) return DECK_RT_INTERNAL;
    r.pos = root_off;

    deck_dvc_node_t *root = decode_node(&r, arena);
    if (r.bad || !root) return DECK_RT_INTERNAL;
    *out_root = root;
    return DECK_RT_OK;
}

/* ---------- structural compare ---------- */

static int attr_equal(const deck_dvc_attr_t *a, const deck_dvc_attr_t *b)
{
    if (strcmp(a->atom, b->atom) != 0) return 1;
    if (a->type != b->type)            return 2;
    switch (a->type) {
        case DVC_ATTR_NONE: return 0;
        case DVC_ATTR_BOOL: return a->value.b != b->value.b;
        case DVC_ATTR_I64:  return a->value.i != b->value.i;
        case DVC_ATTR_F64:  return a->value.f != b->value.f;
        case DVC_ATTR_STR:
        case DVC_ATTR_ATOM:
            if (!a->value.s && !b->value.s) return 0;
            if (!a->value.s || !b->value.s) return 3;
            return strcmp(a->value.s, b->value.s) != 0;
        case DVC_ATTR_LIST_STR:
            if (a->value.list_str.count != b->value.list_str.count) return 4;
            for (uint16_t i = 0; i < a->value.list_str.count; i++) {
                if (strcmp(a->value.list_str.items[i],
                           b->value.list_str.items[i]) != 0) return 5;
            }
            return 0;
    }
    return 99;
}

int deck_dvc_tree_equal(const deck_dvc_node_t *a, const deck_dvc_node_t *b)
{
    if (!a && !b) return 0;
    if (!a || !b) return 1;
    if (a->type != b->type)             return 10;
    if (a->flags != b->flags)           return 11;
    if (a->intent_id != b->intent_id)   return 12;
    if (a->attr_count != b->attr_count) return 13;
    if (a->child_count != b->child_count) return 14;
    for (uint16_t i = 0; i < a->attr_count; i++) {
        int rc = attr_equal(&a->attrs[i], &b->attrs[i]);
        if (rc) return 100 + rc;
    }
    for (uint16_t i = 0; i < a->child_count; i++) {
        int rc = deck_dvc_tree_equal(a->children[i], b->children[i]);
        if (rc) return rc;
    }
    return 0;
}

/* ---------- selftest ---------- */

deck_err_t deck_dvc_selftest(void)
{
    deck_arena_t arena = {0};
    deck_arena_init(&arena, 0);

    /* Build a tree that exercises every attr type. */
    deck_dvc_node_t *root = deck_dvc_node_new(&arena, DVC_GROUP);
    if (!root) goto fail_oom;
    deck_dvc_set_str (&arena, root, "title", "DL2 SELFTEST");
    deck_dvc_set_bool(&arena, root, "selected", true);

    deck_dvc_node_t *label = deck_dvc_node_new(&arena, DVC_LABEL);
    deck_dvc_add_child(&arena, root, label);
    deck_dvc_set_str (&arena, label, "value", "Hello world");

    deck_dvc_node_t *trig = deck_dvc_node_new(&arena, DVC_TRIGGER);
    trig->intent_id = 42;
    deck_dvc_add_child(&arena, root, trig);
    deck_dvc_set_str (&arena, trig, "label", "INCREMENT");
    deck_dvc_set_atom(&arena, trig, "variant", "primary");
    deck_dvc_set_i64 (&arena, trig, "count", -7);
    deck_dvc_set_f64 (&arena, trig, "ratio", 1.5);

    deck_dvc_node_t *choice = deck_dvc_node_new(&arena, DVC_CHOICE);
    deck_dvc_add_child(&arena, root, choice);
    const char *opts[] = { "alpha", "beta", "gamma" };
    deck_dvc_set_list_str(&arena, choice, "options", opts, 3);

    /* Encode → decode → compare. */
    size_t need = 0;
    deck_err_t r = deck_dvc_encode(root, NULL, 0, &need);
    if (r != DECK_RT_NO_MEMORY) {
        ESP_LOGE(TAG, "size probe expected NO_MEMORY, got %d", (int)r);
        deck_arena_reset(&arena);
        return DECK_RT_INTERNAL;
    }
    if (need == 0) {
        ESP_LOGE(TAG, "size probe returned 0");
        deck_arena_reset(&arena);
        return DECK_RT_INTERNAL;
    }

    uint8_t *buf = deck_arena_alloc(&arena, need);
    if (!buf) { ESP_LOGE(TAG, "alloc %u failed", (unsigned)need); goto fail_oom; }

    size_t wrote = 0;
    r = deck_dvc_encode(root, buf, need, &wrote);
    if (r != DECK_RT_OK || wrote != need) {
        ESP_LOGE(TAG, "encode: r=%d wrote=%u need=%u",
                 (int)r, (unsigned)wrote, (unsigned)need);
        deck_arena_reset(&arena);
        return DECK_RT_INTERNAL;
    }

    deck_arena_t arena2 = {0};
    deck_arena_init(&arena2, 0);
    deck_dvc_node_t *root2 = NULL;
    r = deck_dvc_decode(buf, wrote, &arena2, &root2);
    if (r != DECK_RT_OK || !root2) {
        ESP_LOGE(TAG, "decode: r=%d", (int)r);
        deck_arena_reset(&arena);
        deck_arena_reset(&arena2);
        return DECK_RT_INTERNAL;
    }

    int diff = deck_dvc_tree_equal(root, root2);
    if (diff != 0) {
        ESP_LOGE(TAG, "tree_equal: diff=%d", diff);
        deck_arena_reset(&arena);
        deck_arena_reset(&arena2);
        return DECK_RT_INTERNAL;
    }

    /* Bad magic must reject. */
    uint8_t corrupted[16] = {0};
    memcpy(corrupted, buf, sizeof(corrupted) < wrote ? sizeof(corrupted) : wrote);
    corrupted[0] = 0xFF;
    deck_dvc_node_t *bad = NULL;
    r = deck_dvc_decode(corrupted, wrote, &arena2, &bad);
    if (r == DECK_RT_OK) {
        ESP_LOGE(TAG, "decode of bad-magic should have failed");
        deck_arena_reset(&arena);
        deck_arena_reset(&arena2);
        return DECK_RT_INTERNAL;
    }

    ESP_LOGI(TAG, "selftest: PASS (round-trip %u bytes, 4 nodes, 8 attrs, "
                  "rejects bad magic)",
             (unsigned)wrote);
    deck_arena_reset(&arena);
    deck_arena_reset(&arena2);
    return DECK_RT_OK;

fail_oom:
    deck_arena_reset(&arena);
    return DECK_RT_NO_MEMORY;
}
