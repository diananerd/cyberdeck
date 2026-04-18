#include "deck_loader.h"
#include "deck_parser.h"
#include "deck_intern.h"
#include "drivers/deck_sdi_info.h"

#include <stdarg.h>
#include <string.h>
#include <stdio.h>

static void set_err(deck_loader_t *l, deck_err_t code, uint8_t stage,
                    uint32_t line, uint32_t col, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));

static void set_err(deck_loader_t *l, deck_err_t code, uint8_t stage,
                    uint32_t line, uint32_t col, const char *fmt, ...)
{
    if (l->err != DECK_LOAD_OK) return;   /* keep earliest */
    l->err       = code;
    l->err_stage = stage;
    l->err_line  = line;
    l->err_col   = col;
    va_list ap; va_start(ap, fmt);
    vsnprintf(l->err_msg, sizeof(l->err_msg), fmt, ap);
    va_end(ap);
}

void deck_loader_init(deck_loader_t *l, deck_arena_t *arena)
{
    memset(l, 0, sizeof(*l));
    l->arena = arena;
    l->err   = DECK_LOAD_OK;
}

/* ================================================================
 * Stage 1 — metadata extraction from @app
 * ================================================================ */

/* Find the first top-level AST_APP in the module. */
static const ast_node_t *find_app(const ast_node_t *mod)
{
    if (!mod || mod->kind != AST_MODULE) return NULL;
    for (uint32_t i = 0; i < mod->as.module.items.len; i++) {
        const ast_node_t *it = mod->as.module.items.items[i];
        if (it && it->kind == AST_APP) return it;
    }
    return NULL;
}

static const ast_app_field_t *find_field(const ast_node_t *app, const char *name)
{
    if (!app || app->kind != AST_APP) return NULL;
    for (uint32_t i = 0; i < app->as.app.n_fields; i++) {
        if (app->as.app.fields[i].name &&
            strcmp(app->as.app.fields[i].name, name) == 0)
            return &app->as.app.fields[i];
    }
    return NULL;
}

static const char *as_str_or(const ast_node_t *v, const char *fallback)
{
    if (v && v->kind == AST_LIT_STR) return v->as.s;
    return fallback;
}

static int as_int_or(const ast_node_t *v, int fallback)
{
    if (v && v->kind == AST_LIT_INT) return (int)v->as.i;
    return fallback;
}

static void extract_app_metadata(deck_loader_t *l, const ast_node_t *app)
{
    l->app_id              = as_str_or(find_field(app, "id")      ? find_field(app, "id")->value      : NULL, NULL);
    l->app_name            = as_str_or(find_field(app, "name")    ? find_field(app, "name")->value    : NULL, NULL);
    l->app_version         = as_str_or(find_field(app, "version") ? find_field(app, "version")->value : NULL, NULL);
    l->edition             = as_int_or(find_field(app, "edition") ? find_field(app, "edition")->value : NULL, 0);
    l->required_deck_level = 1;   /* default if not specified */
    l->required_deck_os    = 1;

    const ast_app_field_t *req = find_field(app, "requires");
    if (req && req->value && req->value->kind == AST_APP) {
        const ast_app_field_t *dlf = find_field(req->value, "deck_level");
        const ast_app_field_t *dof = find_field(req->value, "deck_os");
        if (dlf) l->required_deck_level = as_int_or(dlf->value, 1);
        if (dof) l->required_deck_os    = as_int_or(dof->value, 1);
    }
}

/* ================================================================
 * Stage 2 — symbol resolution (transitions → state names)
 * ================================================================ */

static bool state_has_name(const ast_node_t *machine, const char *name)
{
    if (!machine || machine->kind != AST_MACHINE || !name) return false;
    for (uint32_t i = 0; i < machine->as.machine.states.len; i++) {
        const ast_node_t *st = machine->as.machine.states.items[i];
        if (st && st->kind == AST_STATE && st->as.state.name &&
            strcmp(st->as.state.name, name) == 0)
            return true;
    }
    return false;
}

static void check_transition_targets(deck_loader_t *l, const ast_node_t *machine)
{
    for (uint32_t i = 0; i < machine->as.machine.states.len; i++) {
        const ast_node_t *st = machine->as.machine.states.items[i];
        if (!st || st->kind != AST_STATE) continue;
        for (uint32_t j = 0; j < st->as.state.hooks.len; j++) {
            const ast_node_t *h = st->as.state.hooks.items[j];
            if (h && h->kind == AST_TRANSITION) {
                if (!state_has_name(machine, h->as.transition.target)) {
                    set_err(l, DECK_LOAD_UNRESOLVED_SYMBOL, 2,
                            h->line, h->col,
                            "transition target :%s not a state in machine '%s'",
                            h->as.transition.target,
                            machine->as.machine.name ? machine->as.machine.name : "?");
                    return;
                }
            }
        }
    }
}

static void stage2_resolve(deck_loader_t *l)
{
    const ast_node_t *mod = l->module;
    for (uint32_t i = 0; i < mod->as.module.items.len; i++) {
        const ast_node_t *it = mod->as.module.items.items[i];
        if (it && it->kind == AST_MACHINE) check_transition_targets(l, it);
        if (l->err != DECK_LOAD_OK) return;
    }
}

/* ================================================================
 * Stage 3 — type / structure check on @app
 * ================================================================ */

static void stage3_type_check(deck_loader_t *l)
{
    const ast_node_t *app = find_app(l->module);
    if (!app) {
        set_err(l, DECK_LOAD_TYPE_ERROR, 3, 1, 1,
                "program has no @app declaration");
        return;
    }
    if (!l->app_id || !*l->app_id) {
        set_err(l, DECK_LOAD_TYPE_ERROR, 3, app->line, app->col,
                "@app.id is required and must be a string");
        return;
    }
    if (!l->app_name) {
        set_err(l, DECK_LOAD_TYPE_ERROR, 3, app->line, app->col,
                "@app.name is required and must be a string");
        return;
    }
    if (!l->app_version) {
        set_err(l, DECK_LOAD_TYPE_ERROR, 3, app->line, app->col,
                "@app.version is required and must be a string");
        return;
    }
    if (l->edition == 0) {
        set_err(l, DECK_LOAD_TYPE_ERROR, 3, app->line, app->col,
                "@app.edition is required and must be an int literal");
        return;
    }
}

/* ================================================================
 * Stage 4 — capability bind
 * ================================================================ */

typedef struct { const char *name; deck_dl1_cap_t bit; } cap_entry_t;

static const cap_entry_t DL1_CAPS[] = {
    { "math",   DL1_CAP_MATH   },
    { "text",   DL1_CAP_TEXT   },
    { "bytes",  DL1_CAP_BYTES  },
    { "log",    DL1_CAP_LOG    },
    { "time",   DL1_CAP_TIME   },
    { "system", DL1_CAP_SYSTEM },
    { "nvs",    DL1_CAP_NVS    },
    { "fs",     DL1_CAP_FS     },
    { "os",     DL1_CAP_OS     },
    { NULL, 0 },
};

static deck_dl1_cap_t lookup_cap(const char *name)
{
    if (!name) return 0;
    for (const cap_entry_t *e = DL1_CAPS; e->name; e++) {
        if (strcmp(e->name, name) == 0) return e->bit;
    }
    return 0;
}

static void walk_expr(deck_loader_t *l, const ast_node_t *n);

/* Given a DOT node whose root ident is a capability name, mark it. */
static void handle_dot_chain(deck_loader_t *l, const ast_node_t *dot)
{
    const ast_node_t *root = dot;
    while (root && root->kind == AST_DOT) root = root->as.dot.obj;
    if (!root || root->kind != AST_IDENT) return;
    deck_dl1_cap_t bit = lookup_cap(root->as.s);
    if (bit) {
        l->capabilities_used |= (uint32_t)bit;
    } else {
        /* Not a known capability and not a let-binding — flag only if
         * clearly a capability-style chain (length ≥ 2). */
        if (dot->kind == AST_DOT) {
            set_err(l, DECK_LOAD_CAPABILITY_MISSING, 4, dot->line, dot->col,
                    "unknown capability '%s' (DL1 allows: math, text, bytes, log, time, system, nvs, fs, os)",
                    root->as.s ? root->as.s : "?");
        }
    }
}

static void walk_list(deck_loader_t *l, const ast_list_t *list)
{
    for (uint32_t i = 0; i < list->len; i++) walk_expr(l, list->items[i]);
}

static void walk_expr(deck_loader_t *l, const ast_node_t *n)
{
    if (!n || l->err != DECK_LOAD_OK) return;
    switch (n->kind) {
        case AST_BINOP:
            walk_expr(l, n->as.binop.lhs);
            walk_expr(l, n->as.binop.rhs);
            break;
        case AST_UNARY:
            walk_expr(l, n->as.unary.expr);
            break;
        case AST_CALL:
            walk_expr(l, n->as.call.fn);
            walk_list(l, &n->as.call.args);
            break;
        case AST_DOT:
            handle_dot_chain(l, n);
            walk_expr(l, n->as.dot.obj);
            break;
        case AST_IF:
            walk_expr(l, n->as.if_.cond);
            walk_expr(l, n->as.if_.then_);
            walk_expr(l, n->as.if_.else_);
            break;
        case AST_LET:
            walk_expr(l, n->as.let.value);
            walk_expr(l, n->as.let.body);
            break;
        case AST_DO:
            walk_list(l, &n->as.do_.exprs);
            break;
        case AST_MATCH:
            walk_expr(l, n->as.match.scrut);
            for (uint32_t i = 0; i < n->as.match.n_arms; i++) {
                walk_expr(l, n->as.match.arms[i].guard);
                walk_expr(l, n->as.match.arms[i].body);
            }
            break;
        case AST_ON:
            walk_expr(l, n->as.on.body);
            break;
        case AST_STATE_HOOK:
            walk_expr(l, n->as.state_hook.body);
            break;
        case AST_STATE:
            walk_list(l, &n->as.state.hooks);
            break;
        case AST_MACHINE:
            walk_list(l, &n->as.machine.states);
            break;
        case AST_FN_DEF:
            walk_expr(l, n->as.fndef.body);
            break;
        default: break;
    }
}

static void stage4_capability_bind(deck_loader_t *l)
{
    for (uint32_t i = 0; i < l->module->as.module.items.len; i++) {
        const ast_node_t *it = l->module->as.module.items.items[i];
        if (!it) continue;
        /* Only walk executable items — skip @app header. */
        if (it->kind == AST_APP || it->kind == AST_USE) continue;
        walk_expr(l, it);
        if (l->err != DECK_LOAD_OK) return;
    }
}

/* ================================================================
 * Stage 5 — pattern exhaustiveness
 * ================================================================ */

static bool match_has_wildcard(const ast_node_t *m)
{
    for (uint32_t i = 0; i < m->as.match.n_arms; i++) {
        const ast_node_t *pat = m->as.match.arms[i].pattern;
        if (pat && pat->kind == AST_PAT_WILD) return true;
        /* A bare ident pattern (binds any value) also catches all. */
        if (pat && pat->kind == AST_PAT_IDENT) return true;
    }
    return false;
}

static void walk_check_match(deck_loader_t *l, const ast_node_t *n)
{
    if (!n || l->err != DECK_LOAD_OK) return;
    if (n->kind == AST_MATCH) {
        if (!match_has_wildcard(n)) {
            set_err(l, DECK_LOAD_PATTERN_NOT_EXHAUSTIVE, 5, n->line, n->col,
                    "match is not exhaustive: no wildcard arm");
            return;
        }
        walk_check_match(l, n->as.match.scrut);
        for (uint32_t i = 0; i < n->as.match.n_arms; i++) {
            walk_check_match(l, n->as.match.arms[i].guard);
            walk_check_match(l, n->as.match.arms[i].body);
        }
        return;
    }
    switch (n->kind) {
        case AST_BINOP:
            walk_check_match(l, n->as.binop.lhs);
            walk_check_match(l, n->as.binop.rhs); break;
        case AST_UNARY:    walk_check_match(l, n->as.unary.expr); break;
        case AST_CALL:
            walk_check_match(l, n->as.call.fn);
            for (uint32_t i = 0; i < n->as.call.args.len; i++)
                walk_check_match(l, n->as.call.args.items[i]);
            break;
        case AST_DOT:      walk_check_match(l, n->as.dot.obj); break;
        case AST_IF:
            walk_check_match(l, n->as.if_.cond);
            walk_check_match(l, n->as.if_.then_);
            walk_check_match(l, n->as.if_.else_); break;
        case AST_LET:
            walk_check_match(l, n->as.let.value);
            walk_check_match(l, n->as.let.body); break;
        case AST_DO:
            for (uint32_t i = 0; i < n->as.do_.exprs.len; i++)
                walk_check_match(l, n->as.do_.exprs.items[i]);
            break;
        case AST_ON:          walk_check_match(l, n->as.on.body); break;
        case AST_STATE_HOOK:  walk_check_match(l, n->as.state_hook.body); break;
        case AST_STATE:
            for (uint32_t i = 0; i < n->as.state.hooks.len; i++)
                walk_check_match(l, n->as.state.hooks.items[i]);
            break;
        case AST_MACHINE:
            for (uint32_t i = 0; i < n->as.machine.states.len; i++)
                walk_check_match(l, n->as.machine.states.items[i]);
            break;
        case AST_FN_DEF:
            walk_check_match(l, n->as.fndef.body);
            break;
        default: break;
    }
}

static void stage5_exhaustiveness(deck_loader_t *l)
{
    for (uint32_t i = 0; i < l->module->as.module.items.len; i++) {
        walk_check_match(l, l->module->as.module.items.items[i]);
        if (l->err != DECK_LOAD_OK) return;
    }
}

/* ================================================================
 * Stage 6 — compat check
 * ================================================================ */

static void stage6_compat(deck_loader_t *l)
{
    int runtime_level = deck_sdi_info_deck_level();
    int runtime_os    = deck_sdi_info_deck_os();
    int runtime_ed    = deck_sdi_info_edition();
    if (runtime_level == 0) runtime_level = 1;
    if (runtime_os    == 0) runtime_os    = 1;
    if (runtime_ed    == 0) runtime_ed    = 2026;

    if (l->edition != runtime_ed) {
        /* Runtime supports a single edition at a time; accept exact match only. */
        set_err(l, DECK_LOAD_INCOMPATIBLE_EDITION, 6, 1, 1,
                "app edition %d not supported (runtime: %d)",
                l->edition, runtime_ed);
        return;
    }
    if (l->required_deck_level < 1 || l->required_deck_level > 3) {
        set_err(l, DECK_LOAD_LEVEL_UNKNOWN, 6, 1, 1,
                "@requires.deck_level %d is not a valid conformance level (1..3)",
                l->required_deck_level);
        return;
    }
    if (l->required_deck_level > runtime_level) {
        set_err(l, DECK_LOAD_LEVEL_BELOW_REQUIRED, 6, 1, 1,
                "app requires deck_level %d; runtime provides %d",
                l->required_deck_level, runtime_level);
        return;
    }
    if (l->required_deck_os > runtime_os) {
        set_err(l, DECK_LOAD_INCOMPATIBLE_SURFACE, 6, 1, 1,
                "app requires deck_os %d; runtime provides %d",
                l->required_deck_os, runtime_os);
        return;
    }
    /* Cross-check: DL1-declared app must not use DL2+ capabilities. */
    /* All DL1 caps are DL1-or-higher, so this is a no-op at DL1 today.
     * Wiring left here for when DL2 caps are added (http, wifi, etc.). */
}

/* ================================================================
 * Driver
 * ================================================================ */

deck_err_t deck_loader_load(deck_loader_t *l, const char *src, uint32_t len)
{
    if (!l || !l->arena || !src) return DECK_LOAD_INTERNAL;

    /* Stage 0 + 1 — lex + parse via deck_parser. */
    deck_parser_t p;
    deck_parser_init(&p, l->arena, src, len);
    ast_node_t *mod = deck_parser_parse_module(&p);
    if (!mod || deck_parser_err_code(&p) != DECK_LOAD_OK) {
        set_err(l, deck_parser_err_code(&p) ? deck_parser_err_code(&p) : DECK_LOAD_PARSE_ERROR,
                1,
                deck_parser_err_line(&p), deck_parser_err_col(&p),
                "%s", deck_parser_err_msg(&p) ? deck_parser_err_msg(&p) : "parse failed");
        return l->err;
    }
    l->module = mod;

    /* Extract metadata from @app (needed for stages 3 + 6). */
    const ast_node_t *app = find_app(mod);
    if (app) extract_app_metadata(l, app);

    /* Stage 3 — @app structure */
    stage3_type_check(l); if (l->err != DECK_LOAD_OK) return l->err;

    /* Stage 2 — symbol resolution (after we know module is well-formed) */
    stage2_resolve(l);    if (l->err != DECK_LOAD_OK) return l->err;

    /* Stage 4 — capability bind */
    stage4_capability_bind(l); if (l->err != DECK_LOAD_OK) return l->err;

    /* Stage 5 — pattern exhaustiveness */
    stage5_exhaustiveness(l); if (l->err != DECK_LOAD_OK) return l->err;

    /* Stage 6 — compat */
    stage6_compat(l);     if (l->err != DECK_LOAD_OK) return l->err;

    /* Stages 7 (reserved), 8 (freeze — arena is already immutable), 9 (linkage — stub)
     * are no-ops at DL1 F4; stage 9 activates @app.entry in F5/F7. */
    return DECK_LOAD_OK;
}
