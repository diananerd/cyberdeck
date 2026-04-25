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

/* Spec 02-deck-app §4A — @needs is a top-level sibling of @app.
 * The parser yields an AST_NEEDS node whose `as.app.fields` layout
 * mirrors AST_APP. */
static const ast_node_t *find_needs(const ast_node_t *mod)
{
    if (!mod || mod->kind != AST_MODULE) return NULL;
    for (uint32_t i = 0; i < mod->as.module.items.len; i++) {
        const ast_node_t *it = mod->as.module.items.items[i];
        if (it && it->kind == AST_NEEDS) return it;
    }
    return NULL;
}

/* Accepts AST_APP or AST_NEEDS — both share the ast_app_field_t layout. */
static const ast_app_field_t *find_field(const ast_node_t *node, const char *name)
{
    if (!node) return NULL;
    if (node->kind != AST_APP && node->kind != AST_NEEDS) return NULL;
    for (uint32_t i = 0; i < node->as.app.n_fields; i++) {
        if (node->as.app.fields[i].name &&
            strcmp(node->as.app.fields[i].name, name) == 0)
            return &node->as.app.fields[i];
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

    /* 02-deck-app §4A — @needs is a top-level sibling of @app. */
    const ast_node_t *req = find_needs(l->module);
    if (req) {
        const ast_app_field_t *dlf = find_field(req, "deck_level");
        const ast_app_field_t *dof = find_field(req, "deck_os");
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
                    set_err(l, DECK_LOAD_UNRESOLVED, 2,
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
        set_err(l, DECK_LOAD_TYPE, 3, 1, 1,
                "program has no @app declaration");
        return;
    }
    if (!l->app_id || !*l->app_id) {
        set_err(l, DECK_LOAD_TYPE, 3, app->line, app->col,
                "@app.id is required and must be a string");
        return;
    }
    if (!l->app_name) {
        set_err(l, DECK_LOAD_TYPE, 3, app->line, app->col,
                "@app.name is required and must be a string");
        return;
    }
    if (!l->app_version) {
        set_err(l, DECK_LOAD_TYPE, 3, app->line, app->col,
                "@app.version is required and must be a string");
        return;
    }
    if (l->edition == 0) {
        set_err(l, DECK_LOAD_TYPE, 3, app->line, app->col,
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
    { "list",   DL1_CAP_LIST   },
    { "map",    DL1_CAP_MAP    },
    { "bridge",  DL1_CAP_BRIDGE  },
    { "asset",   DL1_CAP_ASSET   },
    /* G3 — both casings registered so apps can write either Machine.send
     * (spec convention) or machine.send (lower-case). */
    { "Machine", DL1_CAP_MACHINE },
    { "machine", DL1_CAP_MACHINE },
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

/* Given a DOT node whose root ident is a capability name, mark it.
 * DL2 F22.2: dot chains on non-cap roots are now legal record/map field
 * accesses, so we only TRACK caps here — the actual capability-missing
 * error fires at CALL sites in walk_call_check_cap. */
static void handle_dot_chain(deck_loader_t *l, const ast_node_t *dot)
{
    (void)l;
    const ast_node_t *root = dot;
    while (root && root->kind == AST_DOT) root = root->as.dot.obj;
    if (!root || root->kind != AST_IDENT) return;
    deck_dl1_cap_t bit = lookup_cap(root->as.s);
    if (bit) l->capabilities_used |= (uint32_t)bit;
}

/* DL2 F22.2: when an AST_DOT appears as a CALL callee, the user is
 * dispatching to a capability builtin; an unknown cap-rooted chain at
 * that position is still an error. */
static void check_call_cap(deck_loader_t *l, const ast_node_t *call)
{
    const ast_node_t *fn = call->as.call.fn;
    if (!fn || fn->kind != AST_DOT) return;
    const ast_node_t *root = fn;
    while (root && root->kind == AST_DOT) root = root->as.dot.obj;
    if (!root || root->kind != AST_IDENT) return;
    if (lookup_cap(root->as.s)) return;
    set_err(l, DECK_LOAD_INCOMPATIBLE, 4, fn->line, fn->col,
            "unknown capability '%s' (allowed: math, text, bytes, log, time, system, nvs, fs, os, list, map, bridge, asset, Machine)",
            root->as.s ? root->as.s : "?");
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
            check_call_cap(l, n);
            if (l->err != DECK_LOAD_OK) return;
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
        case AST_LIT_LIST:
            walk_list(l, &n->as.list.items);
            break;
        case AST_LIT_TUPLE:
            walk_list(l, &n->as.tuple_lit.items);
            break;
        case AST_TUPLE_GET:
            walk_expr(l, n->as.tuple_get.obj);
            break;
        case AST_LIT_MAP:
            walk_list(l, &n->as.map_lit.keys);
            walk_list(l, &n->as.map_lit.vals);
            break;
        case AST_WITH:
            walk_expr(l, n->as.with_.base);
            walk_list(l, &n->as.with_.keys);
            walk_list(l, &n->as.with_.vals);
            break;
        default: break;
    }
}

/* DL2 F23 — collect declared @use names so fn !effect annotations can
 * be cross-checked. Post-concept-#9 the AST_USE node carries a list of
 * entries (spec 02-deck-app §4 block form); match the queried alias
 * against each entry's alias, module, or last dotted segment. */
static bool use_declared(const ast_node_t *mod, const char *alias)
{
    if (!alias) return false;
    for (uint32_t i = 0; i < mod->as.module.items.len; i++) {
        const ast_node_t *it = mod->as.module.items.items[i];
        if (!it || it->kind != AST_USE) continue;
        for (uint32_t j = 0; j < it->as.use.n_entries; j++) {
            const ast_use_entry_t *e = &it->as.use.entries[j];
            if (e->alias  && strcmp(e->alias,  alias) == 0) return true;
            if (e->module && strcmp(e->module, alias) == 0) return true;
            if (e->module) {
                const char *dot = strrchr(e->module, '.');
                if (dot && strcmp(dot + 1, alias) == 0) return true;
            }
        }
        /* Metadata stubs (parse_metadata_block / parse_opaque_block)
         * set the mirror `module` field without building an entries
         * array. Keep them matchable. */
        if (it->as.use.n_entries == 0 && it->as.use.module) {
            if (strcmp(it->as.use.module, alias) == 0) return true;
        }
    }
    /* Implicit caps that don't need @use (DL1 baseline). */
    static const char *IMPLICIT[] = {
        "math","text","bytes","log","time","system","nvs","fs","os","list","map","bridge","asset",NULL
    };
    for (const char **e = IMPLICIT; *e; e++) if (strcmp(*e, alias) == 0) return true;
    return false;
}

static void check_fn_effects(deck_loader_t *l, const ast_node_t *fn)
{
    for (uint32_t i = 0; i < fn->as.fndef.n_effects; i++) {
        const char *eff = fn->as.fndef.effects[i];
        if (!use_declared(l->module, eff)) {
            set_err(l, DECK_LOAD_INCOMPATIBLE, 4, fn->line, fn->col,
                    "fn '%s' declares !%s but no matching @use",
                    fn->as.fndef.name ? fn->as.fndef.name : "<anon>",
                    eff ? eff : "?");
            return;
        }
    }
}

static void stage4_capability_bind(deck_loader_t *l)
{
    for (uint32_t i = 0; i < l->module->as.module.items.len; i++) {
        const ast_node_t *it = l->module->as.module.items.items[i];
        if (!it) continue;
        /* Only walk executable items — skip @app header. */
        if (it->kind == AST_APP || it->kind == AST_USE) continue;
        if (it->kind == AST_FN_DEF) check_fn_effects(l, it);
        if (l->err != DECK_LOAD_OK) return;
        walk_expr(l, it);
        if (l->err != DECK_LOAD_OK) return;
    }
}

/* ================================================================
 * Stage 5 — pattern exhaustiveness
 * ================================================================ */

static bool match_has_wildcard(const ast_node_t *m)
{
    /* First pass: any arm with a top-level wildcard / ident (catch-all)
     * or with NO guard covering the entire variant space is "exhaustive"
     * enough for the current loader's purposes. */
    bool has_some_with_binder = false, has_none = false;
    bool has_ok = false, has_err = false;
    bool has_true = false, has_false = false;
    bool has_nil = false, has_cons = false;
    for (uint32_t i = 0; i < m->as.match.n_arms; i++) {
        const ast_node_t *pat = m->as.match.arms[i].pattern;
        const ast_node_t *guard = m->as.match.arms[i].guard;
        if (!pat) continue;
        /* Any unguarded catch-all arm makes the match exhaustive. */
        if (!guard) {
            if (pat->kind == AST_PAT_WILD) return true;
            if (pat->kind == AST_PAT_IDENT) return true;
        }
        /* Spec §8 — Optional `:some x` + `:none`, Result `:ok x` + `:err x`,
         * full `true`/`false` coverage, and list `[]` + `H :: T` are
         * exhaustive without wildcard. */
        if (pat->kind == AST_PAT_VARIANT && pat->as.pat_variant.ctor) {
            const char *c = pat->as.pat_variant.ctor;
            if (strcmp(c, "some") == 0) has_some_with_binder = true;
            else if (strcmp(c, "ok") == 0) has_ok = true;
            else if (strcmp(c, "err") == 0) has_err = true;
            else if (strcmp(c, "::") == 0) has_cons = true;
            else if (strcmp(c, "[]") == 0 && pat->as.pat_variant.n_subs == 0) has_nil = true;
            /* Spec §8.2 tuple pattern `(p1, …, pN)` — when every sub-
             * pattern is a binder (AST_PAT_IDENT) or wildcard, the arm
             * matches every tuple of arity N, which is universal
             * coverage for a scrutinee of that exact tuple shape. No
             * wildcard arm needed. */
            else if (strcmp(c, "(,)") == 0 && !guard) {
                bool all_binders = true;
                for (uint32_t k = 0; k < pat->as.pat_variant.n_subs; k++) {
                    ast_kind_t sk = pat->as.pat_variant.subs[k]->kind;
                    if (sk != AST_PAT_IDENT && sk != AST_PAT_WILD) {
                        all_binders = false; break;
                    }
                }
                if (all_binders) return true;
            }
        }
        if (pat->kind == AST_PAT_LIT && pat->as.pat_lit) {
            const ast_node_t *lit = pat->as.pat_lit;
            if (lit->kind == AST_LIT_ATOM && lit->as.s) {
                if (strcmp(lit->as.s, "none") == 0) has_none = true;
            }
            if (lit->kind == AST_LIT_BOOL) {
                if (lit->as.b) has_true = true; else has_false = true;
            }
        }
    }
    if (has_some_with_binder && has_none) return true;
    if (has_ok && has_err) return true;
    if (has_true && has_false) return true;
    if (has_nil && has_cons) return true;
    return false;
}

static void walk_check_match(deck_loader_t *l, const ast_node_t *n)
{
    if (!n || l->err != DECK_LOAD_OK) return;
    if (n->kind == AST_MATCH) {
        if (!match_has_wildcard(n)) {
            set_err(l, DECK_LOAD_EXHAUSTIVE, 5, n->line, n->col,
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

/* DL2 F23.5 — capabilities the runtime *advertises* (currently built-in
 * + DL2 names that the spec promises and apps can declare-require even
 * if the dispatch isn't wired yet). Apps that USE an unimplemented cap
 * still error at the dot-chain CALL check; @needs.capabilities is a
 * coarse declaration. */
static bool cap_advertised(const char *name)
{
    if (!name) return false;
    if (lookup_cap(name)) return true;
    static const char *DL2_PROMISED[] = {
        "http","wifi","crypto","ui","battery","security","tasks","display",
        "locale","notify","screen","api","cache","store",NULL
    };
    for (const char **e = DL2_PROMISED; *e; e++)
        if (strcmp(*e, name) == 0) return true;
    return false;
}

/* Stage 6b — SERVICES.md §13 canonical catalog. Every service ID an
 * app can declare in `@needs.services` must be on this list. Native
 * dispatch for a given ID may or may not be wired — at-call-time
 * resolution is a separate check (SDI registry lookup). `@needs` is
 * a coarse declaration that the runtime advertises the surface. */
static bool service_advertised(const char *id)
{
    if (!id) return false;
    static const char *SPEC_SERVICES[] = {
        /* Tier 1 — storage */
        "storage.fs", "storage.nvs", "storage.cache",
        /* Tier 2 — network */
        "network.http", "network.ws", "network.wifi", "network.bluetooth",
        /* Tier 3 — system */
        "system.platform", "system.apps", "system.power", "system.display",
        "system.audio", "system.security", "system.time", "system.locale",
        "system.ota", "system.url", "system.notify", "system.theme",
        "system.logs", "system.scheduler", "system.events",
        "system.intents", "system.services", "system.tasks",
        /* Tier 4 — high-level */
        "api.client", "media.image", "media.audio", "auth.oauth",
        "data.cache", "share.target",
        NULL,
    };
    for (const char **e = SPEC_SERVICES; *e; e++)
        if (strcmp(*e, id) == 0) return true;
    /* Tier 5 — sensors.<name> is an open namespace per SERVICES §13. */
    if (strncmp(id, "sensors.", 8) == 0 && id[8] != '\0') return true;
    return false;
}

static void check_required_services(deck_loader_t *l)
{
    const ast_node_t *req = find_needs(l->module);
    if (!req) return;
    const ast_app_field_t *svc = find_field(req, "services");
    if (!svc || !svc->value) return;
    const ast_node_t *block = svc->value;
    if (block->kind != AST_NEEDS) {
        set_err(l, DECK_LOAD_TYPE, 6, block->line, block->col,
                "@needs.services must be an indented block of "
                "`\"service.id\": \"version_range\"` entries (LANG §8)");
        return;
    }
    for (uint32_t i = 0; i < block->as.app.n_fields; i++) {
        const ast_app_field_t *f = &block->as.app.fields[i];
        const char *id = f->name;
        if (!id) {
            set_err(l, DECK_LOAD_TYPE, 6,
                    f->value ? f->value->line : 0,
                    f->value ? f->value->col  : 0,
                    "@needs.services entry missing service id");
            return;
        }
        if (!service_advertised(id)) {
            set_err(l, DECK_LOAD_INCOMPATIBLE, 6,
                    f->value ? f->value->line : 0,
                    f->value ? f->value->col  : 0,
                    "@needs.services lists '%s' which is not in the "
                    "SERVICES §13 catalog", id);
            return;
        }
    }
}

static void check_required_capabilities(deck_loader_t *l)
{
    /* Spec 02-deck-app §4A — capabilities: nested block of
     *   capability.path: "version_range"
     * The parser yields an AST_NEEDS child whose fields carry the
     * dotted capability names. */
    const ast_node_t *req = find_needs(l->module);
    if (!req) return;
    const ast_app_field_t *caps = find_field(req, "capabilities");
    if (!caps || !caps->value) return;
    const ast_node_t *block = caps->value;
    if (block->kind != AST_NEEDS) {
        set_err(l, DECK_LOAD_TYPE, 6, block->line, block->col,
                "@needs.capabilities must be an indented block of "
                "`name: \"version_range\"` entries (see 02-deck-app §4A)");
        return;
    }
    for (uint32_t i = 0; i < block->as.app.n_fields; i++) {
        const ast_app_field_t *f = &block->as.app.fields[i];
        const char *name = f->name;
        if (!name) {
            set_err(l, DECK_LOAD_TYPE, 6,
                    f->value ? f->value->line : 0,
                    f->value ? f->value->col  : 0,
                    "@needs.capabilities entry missing name");
            return;
        }
        if (!cap_advertised(name)) {
            set_err(l, DECK_LOAD_INCOMPATIBLE, 6,
                    f->value ? f->value->line : 0,
                    f->value ? f->value->col  : 0,
                    "@needs.capabilities lists '%s' which is not advertised",
                    name);
            return;
        }
    }
}

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
        set_err(l, DECK_LOAD_INCOMPATIBLE, 6, 1, 1,
                "app edition %d not supported (runtime: %d)",
                l->edition, runtime_ed);
        return;
    }
    if (l->required_deck_level < 1 || l->required_deck_level > 3) {
        set_err(l, DECK_LOAD_INCOMPATIBLE, 6, 1, 1,
                "@needs.deck_level %d is not a valid conformance level (1..3)",
                l->required_deck_level);
        return;
    }
    if (l->required_deck_level > runtime_level) {
        set_err(l, DECK_LOAD_INCOMPATIBLE, 6, 1, 1,
                "app requires deck_level %d; runtime provides %d",
                l->required_deck_level, runtime_level);
        return;
    }
    if (l->required_deck_os > runtime_os) {
        set_err(l, DECK_LOAD_INCOMPATIBLE, 6, 1, 1,
                "app requires deck_os %d; runtime provides %d",
                l->required_deck_os, runtime_os);
        return;
    }
    /* Cross-check: DL1-declared app must not use DL2+ capabilities. */
    /* All DL1 caps are DL1-or-higher, so this is a no-op at DL1 today.
     * Wiring left here for when DL2 caps are added (http, wifi, etc.). */

    /* DL2 F23.5 — verify @needs.capabilities list against runtime. */
    check_required_capabilities(l);
    /* Stage 6b — @needs.services catalog check. */
    check_required_services(l);
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
        set_err(l, deck_parser_err_code(&p) ? deck_parser_err_code(&p) : DECK_LOAD_PARSE,
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
