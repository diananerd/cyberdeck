#pragma once

/* deck_loader — 10-stage DL1 load pipeline.
 *
 * Given `.deck` source, produces either:
 *   (a) a frozen AST + extracted metadata the evaluator (F5+) can run, or
 *   (b) a structured error identifying the failing stage + code + line:col.
 *
 * Stages (see deck-lang/04-deck-runtime.md):
 *   0. lexical pass
 *   1. parse + AST construction
 *   2. resolve symbols (module-local: transition targets vs state names)
 *   3. type check (DL1: @app shape + field types)
 *   4. capability bind (walk AST, enforce DL1 capability set)
 *   5. pattern exhaustiveness (match arms have a wildcard catch-all)
 *   6. compat check (edition / deck_level / deck_os / runtime semver)
 *   7. reserved — no-op at DL1
 *   8. freeze module (arena-allocated AST is already immutable)
 *   9. linkage — stub at F4; populated in F5/F7 when eval + machine exist
 *
 * Errors use deck_err_t (DECK_LOAD_*) from deck_error.h.
 */

#include "deck_ast.h"
#include "deck_arena.h"
#include "deck_error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DECK_LOADER_MSG_SIZE  192

typedef enum {
    DL1_CAP_MATH      = 1u << 0,
    DL1_CAP_TEXT      = 1u << 1,
    DL1_CAP_BYTES     = 1u << 2,
    DL1_CAP_LOG       = 1u << 3,
    DL1_CAP_TIME      = 1u << 4,
    DL1_CAP_SYSTEM    = 1u << 5,   /* system.info etc. */
    DL1_CAP_NVS       = 1u << 6,
    DL1_CAP_FS        = 1u << 7,
    DL1_CAP_OS        = 1u << 8,   /* os.resume/suspend/terminate */
    DL1_CAP_LIST      = 1u << 9,   /* DL2 F21.4 — list.len/head/get */
} deck_dl1_cap_t;

typedef struct {
    deck_arena_t     *arena;       /* caller-owned; AST lives here */
    const ast_node_t *module;      /* produced by stage 1 */

    /* Extracted app metadata (filled after stage 3). */
    const char *app_id;
    const char *app_name;
    const char *app_version;
    int         edition;
    int         required_deck_level;
    int         required_deck_os;

    /* Capabilities observed by stage 4 (bitset of DL1_CAP_*). */
    uint32_t    capabilities_used;

    /* Error state. err==DECK_LOAD_OK when the load succeeded. */
    deck_err_t  err;
    uint8_t     err_stage;
    uint32_t    err_line;
    uint32_t    err_col;
    char        err_msg[DECK_LOADER_MSG_SIZE];
} deck_loader_t;

/* Initialize. arena must outlive the loader; will hold AST nodes. */
void deck_loader_init(deck_loader_t *l, deck_arena_t *arena);

/* Run all 10 stages over src. Returns DECK_LOAD_OK on success.
 * On failure, l->err is the error code and l->err_stage identifies the
 * stage that rejected it. */
deck_err_t deck_loader_load(deck_loader_t *l, const char *src, uint32_t len);

/* Selftest — 20+ apps (valid and invalid) exercising every error class. */
deck_err_t deck_loader_run_selftest(void);

#ifdef __cplusplus
}
#endif
