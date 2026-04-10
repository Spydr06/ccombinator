#include <ccombinator.h>

#include "internal.h"

#include <assert.h>

#define INIT_RULES_CAP 32

#define BNF_DEFINITION          cc_noreturn(cc_char('='))
#define BNF_CONCATENATION       cc_noreturn(cc_char(','))
#define BNF_TERMINATION         cc_noreturn(cc_char(';'))
#define BNF_ALTERNATION         cc_noreturn(cc_char('|'))
#define BNF_EXCEPTION           cc_noreturn(cc_char('-'))
#define BNF_ESCAPE              cc_noreturn(cc_char('\\'))
#define BNF_OPTIONAL_START      cc_noreturn(cc_char('['))
#define BNF_OPTIONAL_END        cc_noreturn(cc_char(']'))
#define BNF_REPETITION_START    cc_noreturn(cc_char('{'))
#define BNF_REPETITION_END      cc_noreturn(cc_char('}'))
#define BNF_GROUPING_START      cc_noreturn(cc_char('('))
#define BNF_GROUPING_END        cc_noreturn(cc_char(')'))
#define BNF_TERMINAL_A_START    cc_noreturn(cc_char('"'))
#define BNF_TERMINAL_A_END      cc_noreturn(cc_char('"'))
#define BNF_TERMINAL_B_START    cc_noreturn(cc_char('\''))
#define BNF_TERMINAL_B_END      cc_noreturn(cc_char('\''))
#define BNF_COMMENT_START       cc_noreturn(cc_string(u8"(*"))
#define BNF_COMMENT_END         cc_noreturn(cc_string(u8"*)"))
#define BNF_ACTION              cc_noreturn(cc_char('@'))

#define BNF_ESCAPE_CHARS U"rtnv\"\'\\"

#define BNF_PATTERN_PARSER "bnf-pattern"

static struct cc_parser *__bnf_parser;

struct bnf_parsers {
    struct cc_parser *ws;
    struct cc_parser *ident;
};

static int bnf_is_ident(char32_t c) {
    return cc_is_alphanum(c) || c == '-' || c == '_';
}

static struct cc_result bnf_decl(size_t n, void **vs) {
    assert(n == 5 && "bnf_decl() with n != 5");

    struct cc_grammar *g = vs[0];
    char *name = vs[1];
    struct cc_parser *p = vs[3];

    if(!p || !name)
        goto cleanup;

    int err;
    if((err = hashtable_set(&g->rules, name, p))) {
        struct cc_error *e = cc_errorf("multiple definitions of rule '%s'", name);
        cc_release(p);
        free(name);
        return cc_err(e);
    }

    return cc_ok(NULL);
cleanup:
    cc_release(p);
    free(name);
    return cc_ok(NULL);
}

static struct cc_parser *bnf_comment(void) {
    struct cc_parser *start = BNF_COMMENT_START;
    struct cc_parser *end = BNF_COMMENT_END;

    return cc_and(2, cc_fold_null, 
        start,
        cc_many_until(cc_fold_null, cc_any(), end)
    );
}

static inline struct cc_parser *bnf_token(struct cc_parser *a, struct cc_parser *ws) {
    return cc_seq(cc_fold_first, a, ws);
}

static struct cc_result bnf_escaped_char(size_t n, void **vs) {
    assert(n == 2 && "bnf_escaped_char() with n != 2");

    char32_t *c = vs[1];
    if(!c)
        return cc_ok(NULL);

    switch(c[0]) {
        case 'v':
        c[0] = '\v';
        return cc_ok(c);
    case 't':
        c[0] = '\t';
        return cc_ok(c);
    case 'n':
        c[0] = '\n';
        return cc_ok(c);
    case 'r':
        c[0] = '\r';
        return cc_ok(c);
    case '\"':
    case '\'':
    case '\\':
        return cc_ok(c);
    default:
        assert(false && "unknown escape char");
        unreachable();
    }
}

static struct cc_result bnf_terminal(size_t n, void **vs) {
    assert(n == 3 && "bnf_terminal() with n != 3");

    char8_t *term = vs[1];
    if(!term)
        return cc_ok(NULL);
    if(!utf8_single_char(term))
        return cc_ok(cc_free_data(cc_string(term)));
    
    struct cc_parser *p = cc_char(utf8_first_cp(term));
    free(term);
    return cc_ok(p);
}

static struct cc_result bnf_identifier(void *p) {
    if(!p)
        return cc_ok(NULL);

    return cc_ok(cc_free_data(cc_lookup((char*) p)));
}

static struct cc_result bnf_optional(size_t n, void **vs) {
    assert(n == 3 && "bnf_optional() with n != 3");

    struct cc_parser *inner = vs[1];
    
    return cc_ok(cc_maybe(inner));
}

static struct cc_result bnf_repetition(size_t n, void **vs) {
    assert(n == 5 && "bnf_repetition() with n != 5");

    struct cc_hashtable *actions = vs[0];
    char *action_name = vs[1];
    struct cc_parser *inner = vs[3];

    if(!inner) {
        free(action_name);
        return cc_ok(NULL);
    }

    if(!action_name)
        return cc_ok(cc_many(NULL, inner));

    struct cc_action *action = hashtable_get(actions, action_name);
    if(!action) {
        struct cc_error *e = cc_errorf("undefined action '@%s'", action_name);
        free(action_name);
        cc_release(inner);
        return cc_err(e);
    }

    if(action->type != CC_ACTION_FOLD) {
        struct cc_error *e = cc_errorf("action '@%s' is not of type 'fold', got '%s'", action_name, action_to_string(action->type));
        free(action_name);
        cc_release(inner);
        return cc_err(e);
    }

    return cc_ok(cc_many(action->fold, inner));
}

static struct cc_result bnf_application(size_t n, void **vs) {
    assert(n == 5 && "bnf_application() with n != 5");

    struct cc_hashtable *actions = vs[0];
    char *action_name = vs[1];
    struct cc_parser *inner = vs[3];

    if(!inner) {
        free(action_name);
        return cc_ok(NULL);
    }

    if(!action_name)
        return cc_ok(inner);

    struct cc_action *action = hashtable_get(actions, action_name);
    if(!action) {
        struct cc_error *e = cc_errorf("undefined action '@%s'", action_name);
        free(action_name);
        cc_release(inner);
        return cc_err(e);
    }

    if(action->type != CC_ACTION_APPLY) {
        struct cc_error *e = cc_errorf("action '@%s' is not of type 'apply', got '%s'", action_name, action_to_string(action->type));
        free(action_name);
        cc_release(inner);
        return cc_err(e);
    }

    return cc_ok(cc_apply(inner, action->apply));
}

static struct cc_result bnf_lift(size_t n, void **vs) {
    assert(n == 2 && "bnf_lift() with n != 2");

    struct cc_hashtable *actions = vs[0];
    char *action_name = vs[1];

    if(!action_name)
        return cc_ok(NULL);

    struct cc_action *action = hashtable_get(actions, action_name);
    if(!action) {
        struct cc_error *e = cc_errorf("undefined action '@%s'", action_name);
        free(action_name);
        return cc_err(e);
    }

    switch(action->type) {
    case CC_ACTION_LIFT:
        return cc_ok(cc_lift(action->lift));
    case CC_ACTION_VALUE:
        return cc_ok(cc_lift_val(action->value));
    case CC_ACTION_MATCH:
        return cc_ok(cc_match(action->match));
    default:
        struct cc_error *e = cc_errorf("action '@%s' is not of type 'lift' or 'value', got '%s'", action_name, action_to_string(action->type));
        free(action_name);
        return cc_err(e);
    }
}

static struct cc_result bnf_exception(size_t n, void **vs) {
    assert(n == 2 && "bnf_exception() with n != 2");

    struct cc_parser *lhs = vs[0], *rhs = vs[1];
    if(!rhs)
        return cc_ok(lhs);
    if(!lhs)
        return cc_ok(NULL);

    return cc_ok(cc_seq(cc_fold_last, cc_not(rhs), lhs));
}

static struct cc_result bnf_concatenation(size_t n, void **vs) {
    assert(n % 2 == 1);

    if(n == 1) return cc_ok(vs[0]);
    if(n == 3) return cc_ok(cc_seq(NULL, vs[0], vs[2]));
    
    struct cc_parser **ps = malloc(((n + 1) / 2) * sizeof(struct cc_parser*));
    if(!ps) {
        for(size_t i = 0; i < n; i += 2)
            cc_release(vs[i]);
        return cc_ok(NULL);
    }

    for(size_t i = 0; i < n; i += 2)
        ps[i / 2] = vs[i];
    
    return cc_ok(cc_free_data(cc_andv((n + 1) / 2, NULL, ps)));
}

static struct cc_result bnf_fold_concatenation(size_t n, void **vs) {
    assert(n == 3 && "bnf_fold_concatenation() with n != 3");

    struct cc_hashtable *actions = vs[0];
    char *action_name = vs[1];
    struct cc_parser *inner = vs[2];

    if(!action_name)
        return cc_ok(inner);
    
    struct cc_action *action = hashtable_get(actions, action_name);
    if(!action) {
        struct cc_error *e = cc_errorf("undefined action '@%s'", action_name);
        free(action_name);
        return cc_err(e);
    }

    if(action->type != CC_ACTION_FOLD) {
        struct cc_error *e = cc_errorf("action '@%s' is not of type 'fold', got '%s'", action_name, action_to_string(action->type));
        free(action_name);
        cc_release(inner);
        return cc_err(e);
    }

    if(inner->type != PARSER_AND && inner->type != PARSER_SEQ)
        return cc_ok(cc_and(1, action->fold, inner));
    
    inner->fold = action->fold;
    return cc_ok(inner);
}

static struct cc_result bnf_alternation(size_t n, void **vs) {
    assert(n % 2 == 1);

    if(n == 1) return cc_ok(vs[0]);
    if(n == 3) return cc_ok(cc_either(vs[0], vs[2]));
    
    struct cc_parser **ps = malloc(((n + 1) / 2) * sizeof(struct cc_parser*));
    if(!ps) {
        for(size_t i = 0; i < n; i += 2)
            cc_release(vs[i]);
        return cc_ok(NULL);
    }

    for(size_t i = 0; i < n; i += 2)
        ps[i / 2] = vs[i];

    return cc_ok(cc_free_data(cc_orv((n + 1) / 2, ps)));
}

static struct cc_parser *bnf_pattern(struct cc_parser *self, void *p) {
    struct bnf_parsers *ps = p;

    struct cc_parser *ch_a = cc_either(
        cc_noneof(U"\\\"\r\t\n\v"),
        cc_seq(bnf_escaped_char, BNF_ESCAPE, cc_anyof(BNF_ESCAPE_CHARS))
    );

    struct cc_parser *ch_b = cc_either(
        cc_noneof(U"\\\'\r\t\n\v"),
        cc_seq(bnf_escaped_char, BNF_ESCAPE, cc_anyof(BNF_ESCAPE_CHARS))
    );

    // "..."
    struct cc_parser *terminal_a = bnf_token(cc_and(3,
            bnf_terminal,
            BNF_TERMINAL_A_START,
            cc_least(1, cc_fold_concat, ch_a),
            BNF_TERMINAL_A_END
        ), cc_retain(ps->ws));

    // '...'
    struct cc_parser *terminal_b = bnf_token(cc_and(3,
            bnf_terminal,
            BNF_TERMINAL_B_START,
            cc_least(1, cc_fold_concat, ch_b),
            BNF_TERMINAL_B_END
        ), cc_retain(ps->ws));

    struct cc_parser *terminal = cc_either(terminal_a, terminal_b);

    // @action
    struct cc_parser *action = bnf_token(
        cc_seq(
            cc_fold_last,
            BNF_ACTION,
            cc_retain(ps->ident)
        ), cc_retain(ps->ws));

    // ( ... ), @action(...) (-> cc_apply_t)
    struct cc_parser *group = cc_and(
        5,
        bnf_application,
        cc_lookup("action-table"),
        cc_maybe(cc_retain(action)),
        bnf_token(BNF_GROUPING_START, cc_retain(ps->ws)),
        self,
        bnf_token(BNF_GROUPING_END, cc_retain(ps->ws))
    );

    // [ ... ]
    struct cc_parser *opt = cc_and(
        3,
        bnf_optional,
        bnf_token(BNF_OPTIONAL_START, cc_retain(ps->ws)),
        self,
        bnf_token(BNF_OPTIONAL_END, cc_retain(ps->ws))
    );

    // { ... }, @action{...} (-> cc_fold_t)
    struct cc_parser *rept = cc_and(
        5,
        bnf_repetition,
        cc_lookup("action-table"),
        cc_maybe(cc_retain(action)),
        bnf_token(BNF_REPETITION_START, cc_retain(ps->ws)),
        self,
        bnf_token(BNF_REPETITION_END, cc_retain(ps->ws))
    );

    // @action (-> cc_lift_t)
    struct cc_parser *lift = cc_seq(
        bnf_lift,
        cc_lookup("action-table"),
        cc_retain(action)
    );

    struct cc_parser *term = cc_or(
        6,
        terminal,
        cc_apply(cc_retain(ps->ident), bnf_identifier),
        group,
        opt,
        rept,
        lift
    );

    // x or x - y
    struct cc_parser *factor = cc_seq(
        bnf_exception,
        cc_retain(term),
        cc_maybe(cc_seq(cc_fold_last, bnf_token(BNF_EXCEPTION, cc_retain(ps->ws)), term))
    );

    // x , y , ...
    struct cc_parser *concat = cc_chain(
        bnf_concatenation,
        factor,
        bnf_token(BNF_CONCATENATION, cc_retain(ps->ws))
    );

    // @action: x, y, ... (-> cc_fold_t)
    struct cc_parser *fold_concat = cc_and(
        3,
        bnf_fold_concatenation,
        cc_lookup("action-table"),
        cc_maybe(cc_seq(cc_fold_first, action, bnf_token(cc_char(':'), cc_retain(ps->ws)))),
        concat
    );

    // x | y | ...
    struct cc_parser *alt = cc_chain(
        bnf_alternation,
        fold_concat,
        bnf_token(BNF_ALTERNATION, cc_retain(ps->ws))
    );

    return alt;
}

static void bnf_state_release(void) {
    if(!__bnf_parser)
        return;

    cc_release(__bnf_parser);
    __bnf_parser = NULL;
}

static inline struct cc_parser *bnf_parser(void) {
    if(__bnf_parser)
        return cc_retain(__bnf_parser);

    struct cc_parser *ws = cc_many(NULL, cc_either(cc_whitespace(), bnf_comment()));
    struct cc_parser *ident = bnf_token(
        cc_seq(
            cc_fold_concat,
            cc_either(cc_aplhanum(), cc_char('_')),
            cc_many(cc_fold_concat, cc_match(bnf_is_ident))
        ), cc_retain(ws));

    struct bnf_parsers ps = {.ws = ws, .ident = ident};
    struct cc_parser *pattern = cc_fix(bnf_pattern, &ps);

    // foo = <term>;
    struct cc_parser *decl = cc_and(5, bnf_decl, 
        cc_lookup("grammar"),
        ident,
        cc_noreturn(bnf_token(BNF_DEFINITION, cc_retain(ws))),
        pattern,
        cc_noreturn(bnf_token(BNF_TERMINATION, cc_retain(ws)))
    );

    __bnf_parser = cc_seq(
        cc_fold_last,
        ws,
        cc_many_until(cc_fold_null, decl, cc_eof())
    );

    atexit(bnf_state_release);

    return cc_retain(__bnf_parser);
}

static int actions_table(struct cc_hashtable *t, const struct cc_action actions[]) {
    memset(t, 0, sizeof(struct cc_hashtable));

    if(actions == NULL)
        return 0;

    size_t count;
    for(count = 0; actions[count].type != CC_ACTION_NULL; count++);

    int err;
    if((err = hashtable_init(t, count * 2)))
        return err;

    for(size_t i = 0; i < count; i++) {
        if((err = hashtable_set(t, actions[i].name, (void*) &actions[i]))) {
            hashtable_free(t);
            return errno;
        }
    }

    return 0;
}

struct cc_grammar *cc_bnf_from(const struct cc_source *bnf_source, const struct cc_action actions[], struct cc_error **e) {
    if(!bnf_source || !e) {
        errno = EINVAL;
        return NULL;
    }

    struct cc_parser *bnf = bnf_parser();
    if(!bnf)
        return NULL;

    struct cc_grammar *g = grammar_init(INIT_RULES_CAP);
    if(!g) {
        cc_release(bnf);
        return NULL;
    }

    struct cc_hashtable action_table;
    if((errno = actions_table(&action_table, actions))) {
        cc_release(bnf);
        cc_grammar_free(g);
        return NULL;
    }

    struct cc_parser *wrapped_bnf = 
            cc_bind("grammar",      cc_lift_val(g), 
            cc_bind("action-table", cc_lift_val(&action_table),
        bnf));
    if(!wrapped_bnf)
        return NULL;

    struct cc_result r = {0};
    int err = cc_parse(bnf_source, wrapped_bnf, &r);

    hashtable_free(&action_table);

    if(err || (*e = r.err)) {
        cc_grammar_free(g);
        errno = err;
        return NULL;
    }

    return g;
}

struct cc_grammar *cc_bnf(const char8_t *bnf, const struct cc_action actions[], struct cc_error **e) {
    if(!bnf || !e) {
        errno = EINVAL;
        return NULL;
    }

    struct cc_source *bnf_source = cc_string_source(bnf);
    if(!bnf_source)
        return NULL;

    struct cc_grammar *g = cc_bnf_from(bnf_source, actions, e);
    if(!g) {
        cc_close(bnf_source);
        return NULL;
    }

    if((errno = cc_close(bnf_source))) {
        cc_grammar_free(g);
        return NULL;
    }

    return g;
}

