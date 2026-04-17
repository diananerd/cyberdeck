#include "deck_runtime.h"
#include "deck_intern.h"

#include "esp_log.h"

static const char *TAG = "deck_runtime";

static void on_panic(deck_err_t code, const char *message)
{
    ESP_LOGE(TAG, "PANIC %s: %s", deck_err_name(code), message ? message : "");
}

deck_err_t deck_runtime_init(size_t heap_limit_bytes)
{
    deck_alloc_init(heap_limit_bytes, on_panic);
    deck_intern_init(64);
    ESP_LOGI(TAG, "initialized (heap_limit=%u bytes)", (unsigned)heap_limit_bytes);
    return DECK_RT_OK;
}

/* --- selftest ------------------------------------------------------
 * Exercises:
 *  1. immortal singletons — unit/true/false keep refcount 0 across retain/release
 *  2. int + float primitives — alloc + release balance used→0
 *  3. bytes — copy + release
 *  4. list — push items + release drops children
 *  5. tuple — fixed arity release drops children
 *  6. optional — none + some(inner) release drops inner
 *  7. stress — 1000 alloc/release cycles; used must return to baseline
 */

#define CHECK(cond, msg) do { if (!(cond)) { \
    ESP_LOGE(TAG, "selftest FAIL: %s", msg); return DECK_RT_ASSERTION_FAILED; \
} } while (0)

deck_err_t deck_runtime_selftest(void)
{
    size_t baseline = deck_alloc_used();
    size_t live_base = deck_alloc_live_values();

    /* 1. immortals */
    deck_retain(deck_unit());
    deck_release(deck_unit());
    CHECK(deck_unit()->refcount == 0, "unit refcount mutated");
    CHECK(deck_new_bool(true)  == deck_true(),  "true not singleton");
    CHECK(deck_new_bool(false) == deck_false(), "false not singleton");

    /* 2. int */
    deck_value_t *i = deck_new_int(42);
    CHECK(i && i->type == DECK_T_INT && i->as.i == 42, "int new/value");
    CHECK(i->refcount == 1, "int refcount init");
    deck_retain(i);
    CHECK(i->refcount == 2, "int retain");
    deck_release(i);
    CHECK(i->refcount == 1, "int release");
    deck_release(i);
    CHECK(deck_alloc_used() == baseline, "int freed to baseline");

    /* 3. float */
    deck_value_t *f = deck_new_float(3.14);
    CHECK(f && f->type == DECK_T_FLOAT && f->as.f > 3.13 && f->as.f < 3.15, "float");
    deck_release(f);

    /* 4. bytes */
    uint8_t buf[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    deck_value_t *b = deck_new_bytes(buf, 4);
    CHECK(b && b->type == DECK_T_BYTES && b->as.bytes.len == 4, "bytes");
    CHECK(b->as.bytes.buf[0] == 0xDE && b->as.bytes.buf[3] == 0xEF, "bytes content");
    deck_release(b);
    CHECK(deck_alloc_used() == baseline, "bytes freed");

    /* 5. list with 3 ints */
    deck_value_t *list = deck_new_list(0);
    CHECK(list, "list new");
    for (int n = 0; n < 3; n++) {
        deck_value_t *it = deck_new_int(n);
        deck_list_push(list, it);
        deck_release(it);   /* list retained it */
    }
    CHECK(list->as.list.len == 3, "list len");
    CHECK(list->as.list.items[2]->as.i == 2, "list item");
    deck_release(list);
    CHECK(deck_alloc_used() == baseline, "list + children freed");

    /* 6. tuple (i, f) */
    deck_value_t *ti = deck_new_int(7);
    deck_value_t *tf = deck_new_float(2.718);
    deck_value_t *items[2] = { ti, tf };
    deck_value_t *tup = deck_new_tuple(items, 2);
    deck_release(ti); deck_release(tf);
    CHECK(tup && tup->as.tuple.arity == 2, "tuple arity");
    CHECK(tup->as.tuple.items[0]->as.i == 7, "tuple item 0");
    deck_release(tup);
    CHECK(deck_alloc_used() == baseline, "tuple + children freed");

    /* 7. optional: none, some(int) */
    deck_value_t *none = deck_new_none();
    CHECK(none && none->as.opt.inner == NULL, "none");
    deck_release(none);
    deck_value_t *inner = deck_new_int(99);
    deck_value_t *some  = deck_new_some(inner);
    deck_release(inner);
    CHECK(some && some->as.opt.inner && some->as.opt.inner->as.i == 99, "some");
    deck_release(some);
    CHECK(deck_alloc_used() == baseline, "optional freed");

    /* 8. atoms + interning — same name returns same interned ptr */
    uint32_t intern_before = deck_intern_count();
    deck_value_t *a1 = deck_new_atom("ok");
    deck_value_t *a2 = deck_new_atom("ok");
    deck_value_t *a3 = deck_new_atom("error");
    CHECK(a1 && a2 && a3, "atom new");
    CHECK(a1->type == DECK_T_ATOM, "atom type");
    CHECK(a1->as.atom == a2->as.atom, "atom interning (same ptr)");
    CHECK(a1->as.atom != a3->as.atom, "different atoms distinct");
    CHECK(deck_intern_count() == intern_before + 2, "intern added 2 unique");
    deck_release(a1); deck_release(a2); deck_release(a3);
    CHECK(deck_alloc_used() == baseline, "atoms freed (intern retained)");

    /* 9. strings — same content interned */
    deck_value_t *s1 = deck_new_str_cstr("hello");
    deck_value_t *s2 = deck_new_str_cstr("hello");
    deck_value_t *s3 = deck_new_str_cstr("world");
    CHECK(s1 && s2 && s3, "str new");
    CHECK(s1->as.s.ptr == s2->as.s.ptr, "str interning");
    CHECK(s1->as.s.ptr != s3->as.s.ptr, "str distinct");
    CHECK(s1->as.s.len == 5, "str len");
    deck_release(s1); deck_release(s2); deck_release(s3);
    CHECK(deck_alloc_used() == baseline, "strs freed");

    /* 10. stress — 1000 cycles */
    for (int k = 0; k < 1000; k++) {
        deck_value_t *v = deck_new_int(k);
        deck_retain(v);
        deck_release(v);
        deck_release(v);
    }
    CHECK(deck_alloc_used() == baseline,      "stress used balance");
    CHECK(deck_alloc_live_values() == live_base, "stress live count");

    ESP_LOGI(TAG, "selftest: PASS (baseline=%u peak=%u live=%u intern_count=%u intern_bytes=%u)",
             (unsigned)baseline, (unsigned)deck_alloc_peak(),
             (unsigned)deck_alloc_live_values(),
             (unsigned)deck_intern_count(),
             (unsigned)deck_intern_bytes());
    return DECK_RT_OK;
}
