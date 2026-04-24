#include "deck_ast.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

ast_node_t *ast_new(deck_arena_t *a, ast_kind_t kind, uint32_t line, uint32_t col)
{
    ast_node_t *n = deck_arena_zalloc(a, sizeof(ast_node_t));
    if (!n) return NULL;
    n->kind = kind;
    n->line = line;
    n->col  = col;
    return n;
}

void ast_list_init(ast_list_t *l)
{
    if (!l) return;
    l->items = NULL; l->len = 0; l->cap = 0;
}

deck_err_t ast_list_push(deck_arena_t *a, ast_list_t *l, ast_node_t *n)
{
    if (!l) return DECK_LOAD_INTERNAL;
    if (l->len >= l->cap) {
        uint32_t new_cap = l->cap ? l->cap * 2 : 4;
        ast_node_t **buf = deck_arena_alloc(a, new_cap * sizeof(ast_node_t *));
        if (!buf) return DECK_LOAD_RESOURCE;
        if (l->items) memcpy(buf, l->items, l->len * sizeof(ast_node_t *));
        l->items = buf;
        l->cap   = new_cap;
    }
    l->items[l->len++] = n;
    return DECK_LOAD_OK;
}

const char *ast_kind_name(ast_kind_t k)
{
    switch (k) {
        case AST_LIT_INT:     return "int";
        case AST_LIT_FLOAT:   return "float";
        case AST_LIT_BOOL:    return "bool";
        case AST_LIT_STR:     return "str";
        case AST_LIT_ATOM:    return "atom";
        case AST_LIT_UNIT:    return "unit";
        case AST_LIT_NONE:    return "none";
        case AST_LIT_LIST:    return "list";
        case AST_LIT_TUPLE:   return "tuple";
        case AST_TUPLE_GET:   return "tuple_get";
        case AST_LIT_MAP:     return "map";
        case AST_WITH:        return "with";
        case AST_IDENT:       return "ident";
        case AST_BINOP:       return "binop";
        case AST_UNARY:       return "unary";
        case AST_CALL:        return "call";
        case AST_DOT:         return "dot";
        case AST_IF:          return "if";
        case AST_LET:         return "let";
        case AST_DO:          return "do";
        case AST_MATCH:       return "match";
        case AST_PAT_LIT:     return "pat_lit";
        case AST_PAT_WILD:    return "_";
        case AST_PAT_IDENT:   return "pat_ident";
        case AST_SEND:        return "send";
        case AST_TRANSITION:  return "transition";
        case AST_FN_DEF:      return "fn";
        case AST_APP:         return "app";
        case AST_APP_FIELD:   return "app_field";
        case AST_USE:         return "use";
        case AST_ON:          return "on";
        case AST_MACHINE:     return "machine";
        case AST_STATE:       return "state";
        case AST_STATE_HOOK:  return "state_hook";
        case AST_MACHINE_TRANSITION: return "machine_transition";
        case AST_CONTENT_BLOCK: return "content_block";
        case AST_CONTENT_ITEM:  return "content_item";
        case AST_MODULE:      return "module";
        case AST_TYPE_DEF:    return "type_def";
        case AST_ASSETS:      return "assets";
        case AST_NEEDS:       return "needs";
        case AST_MIGRATE:     return "migrate";
        case AST_GRANTS:      return "grants";
        case AST_CONFIG:      return "config";
        case AST_ERRORS:      return "errors";
        case AST_HANDLES:     return "handles";
        case AST_SERVICE:     return "service";
        case AST_TRY:         return "try";
        default:              return "?";
    }
}

const char *ast_binop_name(binop_t op)
{
    switch (op) {
        case BINOP_ADD:   return "+";
        case BINOP_SUB:   return "-";
        case BINOP_MUL:   return "*";
        case BINOP_DIV:   return "/";
        case BINOP_MOD:   return "%";
        case BINOP_POW:   return "**";
        case BINOP_LT:    return "<";
        case BINOP_LE:    return "<=";
        case BINOP_GT:    return ">";
        case BINOP_GE:    return ">=";
        case BINOP_EQ:    return "==";
        case BINOP_NE:    return "!=";
        case BINOP_AND:   return "&&";
        case BINOP_OR:    return "||";
        case BINOP_CONCAT:return "++";
        case BINOP_PIPE:  return "|>";
        case BINOP_PIPE_OPT: return "|>?";
        case BINOP_IS:    return "is";
        default:          return "?";
    }
}

const char *ast_unary_name(unary_t op)
{
    switch (op) {
        case UNARY_NEG: return "-";
        case UNARY_NOT: return "!";
        default:        return "?";
    }
}

/* ------ s-expr printer ------ */

typedef struct {
    char  *buf;
    size_t size;
    size_t pos;
    bool   truncated;
} sprinter_t;

static void sp_init(sprinter_t *p, char *buf, size_t size)
{
    p->buf = buf; p->size = size; p->pos = 0; p->truncated = false;
    if (size) buf[0] = '\0';
}

static void sp_putc(sprinter_t *p, char c)
{
    if (p->truncated) return;
    if (p->pos + 1 >= p->size) { p->truncated = true; return; }
    p->buf[p->pos++] = c;
    p->buf[p->pos]   = '\0';
}

static void sp_puts(sprinter_t *p, const char *s)
{
    while (s && *s && !p->truncated) sp_putc(p, *s++);
}

static void sp_printf(sprinter_t *p, const char *fmt, ...)
{
    if (p->truncated) return;
    char scratch[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(scratch, sizeof(scratch), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(scratch)) n = (int)sizeof(scratch) - 1;
    scratch[n] = '\0';
    sp_puts(p, scratch);
}

static void print_node(sprinter_t *p, const ast_node_t *n);

static void print_list(sprinter_t *p, const ast_list_t *l)
{
    for (uint32_t i = 0; i < l->len; i++) {
        sp_putc(p, ' ');
        print_node(p, l->items[i]);
    }
}

static void print_node(sprinter_t *p, const ast_node_t *n)
{
    if (!n) { sp_puts(p, "_"); return; }
    sp_putc(p, '(');
    sp_puts(p, ast_kind_name(n->kind));
    switch (n->kind) {
        case AST_LIT_INT:    sp_printf(p, " %lld", (long long)n->as.i); break;
        case AST_LIT_FLOAT:  sp_printf(p, " %g",   n->as.f); break;
        case AST_LIT_BOOL:   sp_puts(p, n->as.b ? " true" : " false"); break;
        case AST_LIT_STR:    sp_printf(p, " \"%s\"", n->as.s ? n->as.s : ""); break;
        case AST_LIT_ATOM:   sp_printf(p, " :%s", n->as.s ? n->as.s : ""); break;
        case AST_LIT_UNIT:
        case AST_LIT_NONE:   break;
        case AST_LIT_LIST:   print_list(p, &n->as.list.items); break;
        case AST_LIT_TUPLE:  print_list(p, &n->as.tuple_lit.items); break;
        case AST_TUPLE_GET:
            sp_putc(p, ' '); print_node(p, n->as.tuple_get.obj);
            sp_printf(p, " %u", (unsigned)n->as.tuple_get.idx);
            break;
        case AST_LIT_MAP:
            for (uint32_t i = 0; i < n->as.map_lit.keys.len; i++) {
                sp_puts(p, " (");
                print_node(p, n->as.map_lit.keys.items[i]);
                sp_putc(p, ' ');
                print_node(p, n->as.map_lit.vals.items[i]);
                sp_putc(p, ')');
            }
            break;
        case AST_IDENT:      sp_printf(p, " %s", n->as.s ? n->as.s : ""); break;
        case AST_BINOP:
            sp_printf(p, " %s", ast_binop_name(n->as.binop.op));
            sp_putc(p, ' '); print_node(p, n->as.binop.lhs);
            sp_putc(p, ' '); print_node(p, n->as.binop.rhs);
            break;
        case AST_UNARY:
            sp_printf(p, " %s", ast_unary_name(n->as.unary.op));
            sp_putc(p, ' '); print_node(p, n->as.unary.expr);
            break;
        case AST_CALL:
            sp_putc(p, ' '); print_node(p, n->as.call.fn);
            print_list(p, &n->as.call.args);
            break;
        case AST_DOT:
            sp_putc(p, ' '); print_node(p, n->as.dot.obj);
            sp_printf(p, " %s", n->as.dot.field ? n->as.dot.field : "");
            break;
        case AST_IF:
            sp_putc(p, ' '); print_node(p, n->as.if_.cond);
            sp_putc(p, ' '); print_node(p, n->as.if_.then_);
            sp_putc(p, ' '); print_node(p, n->as.if_.else_);
            break;
        case AST_LET:
            sp_printf(p, " %s", n->as.let.name ? n->as.let.name : "");
            sp_putc(p, ' '); print_node(p, n->as.let.value);
            sp_putc(p, ' '); print_node(p, n->as.let.body);
            break;
        case AST_DO:
            print_list(p, &n->as.do_.exprs);
            break;
        case AST_MATCH:
            sp_putc(p, ' '); print_node(p, n->as.match.scrut);
            for (uint32_t i = 0; i < n->as.match.n_arms; i++) {
                sp_puts(p, " (arm ");
                print_node(p, n->as.match.arms[i].pattern);
                if (n->as.match.arms[i].guard) {
                    sp_puts(p, " when "); print_node(p, n->as.match.arms[i].guard);
                }
                sp_putc(p, ' '); print_node(p, n->as.match.arms[i].body);
                sp_putc(p, ')');
            }
            break;
        case AST_PAT_LIT:    sp_putc(p, ' '); print_node(p, n->as.pat_lit); break;
        case AST_PAT_IDENT:  sp_printf(p, " %s", n->as.pat_ident ? n->as.pat_ident : ""); break;
        case AST_SEND:       sp_printf(p, " :%s", n->as.send.event ? n->as.send.event : ""); break;
        case AST_TRANSITION: sp_printf(p, " :%s", n->as.transition.target ? n->as.transition.target : ""); break;
        case AST_FN_DEF:
            sp_printf(p, " %s", n->as.fndef.name ? n->as.fndef.name : "?");
            sp_puts(p, " (params");
            for (uint32_t i = 0; i < n->as.fndef.n_params; i++)
                sp_printf(p, " %s", n->as.fndef.params[i] ? n->as.fndef.params[i] : "?");
            sp_putc(p, ')');
            sp_putc(p, ' '); print_node(p, n->as.fndef.body);
            break;
        case AST_APP:
        case AST_NEEDS:
            for (uint32_t i = 0; i < n->as.app.n_fields; i++) {
                sp_printf(p, " (%s ", n->as.app.fields[i].name);
                print_node(p, n->as.app.fields[i].value);
                sp_putc(p, ')');
            }
            break;
        case AST_USE:
            if (n->as.use.n_entries > 0) {
                for (uint32_t i = 0; i < n->as.use.n_entries; i++) {
                    const ast_use_entry_t *e = &n->as.use.entries[i];
                    sp_printf(p, " (%s%s%s%s)",
                              e->module ? e->module : "?",
                              e->alias  ? " as "    : "",
                              e->alias  ? e->alias  : "",
                              e->is_optional ? " optional" : "");
                }
            } else if (n->as.use.module) {
                sp_printf(p, " %s", n->as.use.module);
            }
            break;
        case AST_ON:
            sp_printf(p, " :%s", n->as.on.event ? n->as.on.event : "");
            if (n->as.on.n_params > 0) {
                sp_puts(p, " (");
                for (uint32_t i = 0; i < n->as.on.n_params; i++) {
                    if (i > 0) sp_putc(p, ' ');
                    sp_printf(p, "%s:", n->as.on.params[i].field);
                    sp_putc(p, ' ');
                    print_node(p, n->as.on.params[i].pattern);
                }
                sp_putc(p, ')');
            }
            sp_putc(p, ' ');
            print_node(p, n->as.on.body);
            break;
        case AST_MACHINE:
            sp_printf(p, " %s", n->as.machine.name ? n->as.machine.name : "");
            if (n->as.machine.initial_state)
                sp_printf(p, " (initial :%s)", n->as.machine.initial_state);
            print_list(p, &n->as.machine.states);
            break;
        case AST_STATE:
            sp_printf(p, " %s", n->as.state.name ? n->as.state.name : "");
            print_list(p, &n->as.state.hooks);
            break;
        case AST_STATE_HOOK:
            sp_printf(p, " %s ", n->as.state_hook.kind ? n->as.state_hook.kind : "?");
            print_node(p, n->as.state_hook.body);
            break;
        case AST_MODULE:
            print_list(p, &n->as.module.items);
            break;
        case AST_ASSETS:
            for (uint32_t i = 0; i < n->as.assets.n_entries; i++) {
                sp_printf(p, " (%s \"%s\")",
                          n->as.assets.names[i] ? n->as.assets.names[i] : "?",
                          n->as.assets.paths[i] ? n->as.assets.paths[i] : "");
            }
            break;
        case AST_MIGRATE:
            for (uint32_t i = 0; i < n->as.migrate.n_entries; i++) {
                sp_printf(p, " (from %lld ",
                          (long long)n->as.migrate.from_versions[i]);
                print_node(p, n->as.migrate.bodies[i]);
                sp_putc(p, ')');
            }
            break;
        case AST_CONTENT_BLOCK:
            print_list(p, &n->as.content_block.items);
            break;
        case AST_CONTENT_ITEM:
            if (n->as.content_item.kind)
                sp_printf(p, " :%s", n->as.content_item.kind);
            if (n->as.content_item.label)
                sp_printf(p, " \"%s\"", n->as.content_item.label);
            if (n->as.content_item.action_expr) {
                sp_putc(p, ' ');
                print_node(p, n->as.content_item.action_expr);
            }
            if (n->as.content_item.data_expr) {
                sp_puts(p, " (data ");
                print_node(p, n->as.content_item.data_expr);
                sp_putc(p, ')');
            }
            for (uint32_t i = 0; i < n->as.content_item.n_options; i++) {
                const ast_content_option_t *o = &n->as.content_item.options[i];
                sp_printf(p, " (%s ", o->key ? o->key : "?");
                print_node(p, o->value);
                sp_putc(p, ')');
            }
            if (n->as.content_item.item_binder) {
                sp_printf(p, " (item %s", n->as.content_item.item_binder);
                for (uint32_t i = 0; i < n->as.content_item.item_body.len; i++) {
                    sp_putc(p, ' ');
                    print_node(p, n->as.content_item.item_body.items[i]);
                }
                sp_putc(p, ')');
            }
            break;
        default: break;
    }
    sp_putc(p, ')');
}

size_t ast_print(const ast_node_t *n, char *out, size_t out_size)
{
    sprinter_t p; sp_init(&p, out, out_size);
    print_node(&p, n);
    return p.truncated ? 0 : p.pos;
}
