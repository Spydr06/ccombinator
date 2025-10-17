#include <ccombinator.h>

#include "internal.h"

#include <errno.h>
#include <memory.h>

#include <assert.h>
#include <locale.h>

#define CC_STATE_FLAGS_DEFAULT  0x00
#define CC_STATE_FLAG_EOF       0x01
#define CC_STATE_FLAG_NOERR     0x02
#define CC_STATE_FLAG_NORETURN  0x04

struct cc_state {
    int flags;
    const struct cc_source *src;
    struct cc_location loc;

    struct dynarr scope;

    unsigned recursion_depth;
    unsigned max_recursion_depth;
};

struct cc_save {
    int flags;
    struct cc_location loc;

    size_t scope_len;
    unsigned recursion_depth;
};

#define PARSE_SUCCESS 1
#define PARSE_FAILURE 0

#define PROP(s, x) do {                             \
        assert((s)->recursion_depth > 0);           \
        (s)->recursion_depth--;                     \
        return (x);                                 \
    } while(0)

#define FAIL_WITH(e, s, x) do {                     \
        int err = new_error((e), (s), (x));         \
        PROP(s, err ? -err : PARSE_FAILURE);        \
    } while(0)

#define SUCCESS(r, s, x) do {                       \
        if(!((s)->flags & CC_STATE_FLAG_NORETURN))  \
            (r) = (x);                              \
        PROP(s, PARSE_SUCCESS);                     \
    } while(0)

static int run_parser(struct cc_state *s, const struct cc_parser *p, void **r, struct cc_error *e);

static inline bool set_flag(struct cc_state *s, int flag, bool state) {
    bool before = !!(s->flags & flag);

    if(state)
        s->flags |= flag;
    else
        s->flags &= ~flag;

    return before;
}

static inline char32_t peek_at(struct cc_state *s) {
    if(s->loc.byte_off >= s->src->buffer_size) {
        s->flags |= CC_STATE_FLAG_EOF;
        return EOF;
    }

    return utf8_first_cp(s->src->buffer + s->loc.byte_off);
}

static inline struct cc_save state_save(const struct cc_state *s) {
    return (struct cc_save){
        .loc = s->loc,
        .flags = s->flags,
        .scope_len = s->scope.len,
        .recursion_depth = s->recursion_depth
    };
}

static inline void state_restore(struct cc_state *s, const struct cc_save *save) {
    assert(s->scope.len >= save->scope_len);
    assert(s->recursion_depth == save->recursion_depth); // safety check

    s->loc = save->loc;
    s->flags = save->flags;
    s->scope.len = save->scope_len;
}

static inline void state_free(struct cc_state *s) {
    free(s->scope.elems);
}

static inline int scope_push(struct cc_state *s, const struct cc_parser *p) {
    assert(p->type == PARSER_BIND);
    return dynarr_append(&s->scope, (void*) p);
}

static inline const struct cc_parser *scope_pop(struct cc_state *s) {
    assert(s->scope.len > 0);

    return s->scope.elems[--s->scope.len];
}

static const struct cc_parser *scope_lookup(struct cc_state *s, const char *name) {
    // TODO: maybe use some sort of hashing to avoid strcmp?

    for(size_t i = s->scope.len; i > 0; i--) {
        const struct cc_parser *def = s->scope.elems[i - 1];

        if(strcmp(def->match.bind.name, name) == 0)
            return def->match.bind.inner;
    }

    return NULL;
}

static int new_error(struct cc_error *e, struct cc_state *s, const char *msg) {
    if(s->flags & CC_STATE_FLAG_NOERR)
        return 0;

    if(!e)
        return EINVAL;

    memset(e, 0, sizeof(struct cc_error));

    e->loc = s->loc;
    e->received = peek_at(s);
    if(!(e->failure = strdup(msg)))
        return errno;

    return 0;
}

static int add_expected(struct cc_error *e, struct cc_state *s, const char *expected) {
    if(s->flags & CC_STATE_FLAG_NOERR || e->num_expected >= CC_ERR_MAX_EXPECTED)
        return 0; // cannot add anymore, don't error tho

    if(e->num_expected == 0) {
        e->filename = s->src->origin;
        e->loc = s->loc;
        e->received = peek_at(s);
    }

    char *expected_copy = strdup(expected);
    if(!expected_copy)
        return errno;

    e->expected[e->num_expected++] = expected_copy;

    return 0;
}

static inline char32_t advance_char(struct cc_state *s, char32_t ch) {
    s->loc.byte_off += utf8_cp_length(ch);

    if(ch == '\n') {
        s->loc.line++;
        s->loc.col = 1;
    }
    else {
        s->loc.col++;
    }

    return ch;
}

static int allocate_string(char8_t **s, size_t n) {
    if(!(*s = calloc(n + 1, sizeof(char8_t))))
        return -errno;

    return 0;
}

static int char_result(struct cc_state *s, char8_t **r, char32_t ch) {
    if(s->flags & CC_STATE_FLAG_NORETURN)
        return 0;

    int err = allocate_string(r, utf8_cp_length(ch));
    if(err)
        return err;

    utf8_encode(ch, *r);
    return 0;
}

static int string_result(struct cc_state *s, char8_t **r, const char8_t *str) {
    if(s->flags & CC_STATE_FLAG_NORETURN)
        return 0;

    size_t len = strlen((const char*) str);
    int err = allocate_string(r, len);
    if(err)
        return err;

    memcpy(*r, str, len * sizeof(char));
    return 0;
}

static int match_char(struct cc_state *s, char32_t ch, void **r) {
    char32_t next = peek_at(s);
    if((s->flags & CC_STATE_FLAG_EOF) || next != ch)
        return PARSE_FAILURE;

    advance_char(s, ch);

    int err;
    if(r != NULL && (err = char_result(s, (char8_t**) r, ch)))
        return err;

    return PARSE_SUCCESS;
}

static int match_char_func(struct cc_state *s, int(*f)(char32_t), void **r) {
    char32_t next = peek_at(s);
    if((s->flags & CC_STATE_FLAG_EOF) || !f(next))
        return PARSE_FAILURE;

    advance_char(s, next);

    int err;
    if(r != NULL && (err = char_result(s, (char8_t**) r, next)))
        return err;
    
    return PARSE_SUCCESS;
}

static int match_eof(struct cc_state *s, void **r) {
    *r = NULL;
    peek_at(s);
    return (s->flags & CC_STATE_FLAG_EOF) ? PARSE_SUCCESS : PARSE_FAILURE;
}

static int match_sof(struct cc_state *s, void **r) {
    *r = NULL;
    return s->loc.byte_off == 0 ? PARSE_SUCCESS : PARSE_FAILURE;
}

static int match_any(struct cc_state *s, void **r) {
    char32_t next = peek_at(s);
    if(s->flags & CC_STATE_FLAG_EOF)
        return PARSE_FAILURE;

    advance_char(s, next);

    int err;
    if((err = char_result(s, (char8_t**) r, next)))
        return err;

    return PARSE_SUCCESS;
}

static int match_range(struct cc_state *s, char32_t lo, char32_t hi, void **r) {
    char32_t next = peek_at(s);
    if((s->flags & CC_STATE_FLAG_EOF) || next < lo || next > hi)
        return PARSE_FAILURE;

    advance_char(s, next);

    int err;
    if((err = char_result(s, (char8_t**) r, next)))
        return err;

    return PARSE_SUCCESS;
}

static int match_oneof(struct cc_state *s, const char32_t *chars, size_t n, void **r) {
    char32_t next = peek_at(s);
    if(s->flags & CC_STATE_FLAG_EOF)
        return PARSE_FAILURE;

    bool one_found = false;
    for(size_t i = 0; i < n; i++) {
        if(next == chars[n]) {
            if(one_found)
                return PARSE_FAILURE;
            one_found = true;
        }
    }
    
    if(!one_found)
        return PARSE_FAILURE;

    advance_char(s, next);

    int err;
    if((err = char_result(s, (char8_t**) r, next)))
        return err;

    return PARSE_SUCCESS;
}

static int match_anyof(struct cc_state *s, const char32_t *chars, size_t n, void **r) {
    char32_t next = peek_at(s);
    if(s->flags & CC_STATE_FLAG_EOF)
        return PARSE_FAILURE;

    for(size_t i = 0; i < n; i++) {
        if(next == chars[n]) {
            advance_char(s, next);

            int err;
            if((err = char_result(s, (char8_t**) r, next)))
                return err;

            return PARSE_SUCCESS;
        }
    }

    return PARSE_FAILURE;
}

static int match_noneof(struct cc_state *s, const char32_t *chars, size_t n, void **r) {
    char32_t next = peek_at(s);
    if(s->flags & CC_STATE_FLAG_EOF)
        return PARSE_FAILURE;

    for(size_t i = 0; i < n; i++) {
        if(next == chars[i])
            return PARSE_FAILURE;
    }

    advance_char(s, next);

    int err;
    if((err = char_result(s, (char8_t**) r, next)))
        return err;

    return PARSE_SUCCESS;
}

static int match_string(struct cc_state *s, const char8_t *str, void **r) {
    struct cc_state save = *s;

    for(size_t i = 0; str[i];) {
        char32_t ch = utf8_first_cp(str + i);
        if(!match_char(s, ch, NULL)) {
            *s = save;
            return PARSE_FAILURE;
        }

        i += utf8_cp_length(ch);
    }

    int err;
    if((err = string_result(s, (char8_t**) r, str)))
        return err;

    return PARSE_SUCCESS;
}

static int combine_many(struct cc_state *s, const struct cc_parser *p, void **r) {
    struct dynarr values = DYNARR_INIT;

    bool noerr_before = set_flag(s, CC_STATE_FLAG_NOERR, true);
    bool noret_before = !!(s->flags & CC_STATE_FLAG_NORETURN);
    bool noret = noret_before;
    if(!p->fold)
        set_flag(s, CC_STATE_FLAG_NORETURN, noret = true);

    int res, err;

    for(;;) {
        void *val = NULL;
        struct cc_save save = state_save(s);

        res = run_parser(s, p->match.unary.inner, noret ? NULL : &val, NULL);
        if(res == PARSE_FAILURE) {
            state_restore(s, &save);
            break;
        }
        if(res < 0)
            goto cleanup;

        if(!noret && (err = dynarr_append(&values, val))) {
            res = -err;
            goto cleanup;
        }
    }

    if(!noret && p->fold)
        *r = p->fold(values.len, values.elems);
 
cleanup:
    set_flag(s, CC_STATE_FLAG_NOERR, noerr_before);
    set_flag(s, CC_STATE_FLAG_NORETURN, noret_before);

    dynarr_free(&values);
    return PARSE_SUCCESS;
}

static int combine_many_until(struct cc_state *s, const struct cc_parser *p, void **r, struct cc_error *e) {
    struct dynarr values = DYNARR_INIT;
    int res, err;

    struct cc_parser *a = p->match.binary.lhs;
    struct cc_parser *end = p->match.binary.rhs;

    bool noret_before = !!(s->flags & CC_STATE_FLAG_NORETURN);
    bool noret = noret_before;
    if(!p->fold)
        set_flag(s, CC_STATE_FLAG_NORETURN, noret = true);

    for(;;) {
        void *val;

        {
            bool noerr_before = set_flag(s, CC_STATE_FLAG_NOERR, true);
            struct cc_save save = state_save(s);

            if((res = run_parser(s, end, noret ? NULL : &val, NULL)) < 0)
                goto cleanup; 

            set_flag(s, CC_STATE_FLAG_NOERR, noerr_before);
            if(res == PARSE_SUCCESS) {
                if(!noret && (err = dynarr_append(&values, val))) {
                    res = -err;
                    goto cleanup;
                }
                break;
            }

            state_restore(s, &save);
        }
        
        {
            bool noerr_before = set_flag(s, CC_STATE_FLAG_NOERR, true);
            struct cc_save save = state_save(s);

            if((res = run_parser(s, a, noret ? NULL : &val, NULL)) < 0)
                goto cleanup; 

            set_flag(s, CC_STATE_FLAG_NOERR, noerr_before);
            if(res == PARSE_SUCCESS) {
                if(!noret && (err = dynarr_append(&values, val))) {
                    res = -err;
                    goto cleanup;
                }
                continue;
            }

            state_restore(s, &save);
        }

        if((res = run_parser(s, end, noret ? NULL : &val, e)) < 0)
            goto cleanup;
        
        break;
    }

    if(!noret)
        *r = p->fold(values.len, values.elems);

cleanup:
    set_flag(s, CC_STATE_FLAG_NORETURN, noret_before);
    dynarr_free(&values);
    return res;
}

static int combine_count(struct cc_state *s, const struct cc_parser *p, void **r, struct cc_error *e) { 
    unsigned num_values = p->match.unary.n;
    void *values[num_values]; // FIXME: use dynarr on larger n to avoid stackoverflows
    
    bool noret_before = !!(s->flags & CC_STATE_FLAG_NORETURN);
    bool noret = noret_before;
    if(!p->fold)
        set_flag(s, CC_STATE_FLAG_NORETURN, noret = true);

    int res;
    for(unsigned i = 0; i < num_values; i++) {
        res = run_parser(s, p->match.unary.inner, noret ? NULL : &values[i], e);
        if(res == PARSE_FAILURE || res < 0)
            goto cleanup;
    }

    if(!noret)
        *r = p->fold(num_values, values);

    res = PARSE_SUCCESS;
cleanup:
    set_flag(s, CC_STATE_FLAG_NORETURN, noret_before);
    return res;
}

static int combine_least(struct cc_state *s, const struct cc_parser *p, void **r, struct cc_error *e) {
    struct dynarr values = DYNARR_INIT;

    int res;

    bool noerr_before = !!(s->flags & CC_STATE_FLAG_NOERR);
    bool noret_before = !!(s->flags & CC_STATE_FLAG_NORETURN);
    bool noret = noret_before;
    if(!p->fold)
        set_flag(s, CC_STATE_FLAG_NORETURN, noret = true);

    for(unsigned i = 0;; i++) {
        bool required = i < p->match.unary.n;
        if(!required)
            set_flag(s, CC_STATE_FLAG_NOERR, true);

        void *val = NULL;
        struct cc_save save = state_save(s);

        res = run_parser(s, p->match.unary.inner, noret ? NULL : &val, required ? e : NULL);
        if(res == PARSE_FAILURE) {
            state_restore(s, &save);
            if(!required)
                break;

            goto cleanup;
        }
        if(res < 0)
            goto cleanup;

        int err;
        if(!noret && (err = dynarr_append(&values, val))) {
            res = -err;
            goto cleanup;
        }
    }

    res = PARSE_SUCCESS;

    if(!noret)
        *r = p->fold(values.len, values.elems);
 
cleanup:
    set_flag(s, CC_STATE_FLAG_NORETURN, noret_before);
    set_flag(s, CC_STATE_FLAG_NOERR, noerr_before);

    dynarr_free(&values);
    return res;
}

static int combine_not(struct cc_state *s, const struct cc_parser *p) {
    struct cc_save save = state_save(s);

    bool noerr_before = set_flag(s, CC_STATE_FLAG_NOERR, true);
    bool noret_before = set_flag(s, CC_STATE_FLAG_NORETURN, true);

    int res = run_parser(s, p->match.unary.inner, NULL, NULL);

    set_flag(s, CC_STATE_FLAG_NORETURN, noret_before);
    set_flag(s, CC_STATE_FLAG_NOERR, noerr_before);

    if(res < 0)
        return res;

    if(res == PARSE_SUCCESS) {
        state_restore(s, &save);
        return PARSE_FAILURE;
    }

    return PARSE_SUCCESS;
}

static int combine_maybe(struct cc_state *s, const struct cc_parser *p, void **r) {
    struct cc_save save = state_save(s);

    bool noerr_before = set_flag(s, CC_STATE_FLAG_NOERR, true);

    int res = run_parser(s, p->match.unary.inner, r, NULL);
    if(res < 0)
        return res;

    set_flag(s, CC_STATE_FLAG_NOERR, noerr_before);

    if(res == PARSE_FAILURE) {
        if(!(s->flags & CC_STATE_FLAG_NORETURN))
            *r = NULL;
        state_restore(s, &save);
    }

    return PARSE_SUCCESS;
}

static int combine_chain(struct cc_state *s, const struct cc_parser *p, void **r, struct cc_error *e) {
    struct dynarr values = DYNARR_INIT;
    
    struct cc_parser *a = p->match.binary.lhs;
    struct cc_parser *op = p->match.binary.rhs;

    bool noret_before = !!(s->flags & CC_STATE_FLAG_NORETURN);
    bool noret = noret_before;
    if(!p->fold)
        set_flag(s, CC_STATE_FLAG_NORETURN, noret = true);

    void *val;
    int res = run_parser(s, a, noret ? NULL : &val, e);
    if(res != PARSE_SUCCESS)
        goto cleanup;

    int err;
    if(!noret && (err = dynarr_append(&values, val))) {
        res = -err;
        goto cleanup;
    }

    for(;;) {
        struct cc_save save = state_save(s);
        bool noerr_before = set_flag(s, CC_STATE_FLAG_NOERR, true);

        if((res = run_parser(s, op, noret ? NULL : &val, NULL)) < 0)
            goto cleanup;

        set_flag(s, CC_STATE_FLAG_NOERR, noerr_before);
        if(res == PARSE_FAILURE) {
            state_restore(s, &save);
            break;
        }

        if(!noret && (err = dynarr_append(&values, val))) {
            res = -err;
            goto cleanup;
        }

        if((res = run_parser(s, a, noret ? NULL : &val, e)) != PARSE_SUCCESS)
            goto cleanup;

        if(!noret && (err = dynarr_append(&values, val))) {
            res = -err;
            goto cleanup;
        }
    }

    if(!noret) {
        assert(values.len > 0);

        if(values.len > 1)
            *r = p->fold(values.len, values.elems);
        else
            *r = values.elems[0];
    }

    res = PARSE_SUCCESS;
cleanup:
    set_flag(s, CC_STATE_FLAG_NORETURN, noret_before);
    dynarr_free(&values);
    return res;
}

static int combine_postfix(struct cc_state *s, const struct cc_parser *p, void **r, struct cc_error *e) {
    struct dynarr values = DYNARR_INIT;

    struct cc_parser *a = p->match.binary.lhs;
    struct cc_parser *op = p->match.binary.rhs;

    bool noret_before = !!(s->flags & CC_STATE_FLAG_NORETURN);
    bool noret = noret_before;
    if(!p->fold)
        set_flag(s, CC_STATE_FLAG_NORETURN, noret = true);

    void *val;
    int res = run_parser(s, a, noret ? NULL : &val, e);
    if(res != PARSE_SUCCESS)
        goto cleanup;

    if(!noret)
        dynarr_append(&values, val);

    for(;;) {
        struct cc_save save = state_save(s);
        bool noerr_before = set_flag(s, CC_STATE_FLAG_NOERR, true);

        if((res = run_parser(s, op, noret ? NULL : &val, NULL)) < 0)
            goto cleanup;

        set_flag(s, CC_STATE_FLAG_NOERR, noerr_before);
        if(res == PARSE_FAILURE) {
            state_restore(s, &save);
            break;
        }

        int err;
        if(!noret && (err = dynarr_append(&values, val))) {
            res = -err;
            goto cleanup;
        }
    }

    if(!noret) {
        if(values.len > 1)
            *r = p->fold(values.len, values.elems);
        else
            *r = values.elems[0];
    }

    res = PARSE_SUCCESS;
cleanup:
    set_flag(s, CC_STATE_FLAG_NORETURN, noret_before);
    dynarr_free(&values);
    return res;
}

static int combine_and(struct cc_state *s, const struct cc_parser *p, void **r, struct cc_error *e) {
    unsigned num_values = p->match.variadic.n;
    void *values[num_values]; // FIXME: use dynarr on larger n

    bool noret_before = !!(s->flags & CC_STATE_FLAG_NORETURN);
    bool noret = noret_before;
    if(!p->fold)
        set_flag(s, CC_STATE_FLAG_NORETURN, noret = true);
        
    int res;
    for(unsigned i = 0; i < num_values; i++) {
        res = run_parser(s, p->match.variadic.inner[i], noret ? NULL : &values[i], e);
        if(res == PARSE_FAILURE || res < 0)
            goto cleanup;
    }

    if(!noret && p->fold)
        *r = p->fold(num_values, values);
    
    res = PARSE_SUCCESS;
cleanup:
    set_flag(s, CC_STATE_FLAG_NORETURN, noret_before);
    return res;
}

static int combine_or(struct cc_state *s, const struct cc_parser *p, void **r, struct cc_error *e) {
    unsigned num_values = p->match.variadic.n;

    for(unsigned i = 0; i < num_values; i++) {
        int res = run_parser(s, p->match.variadic.inner[i], r, e);
        if(res == PARSE_SUCCESS || res < 0)
            return res;
    }

    return PARSE_FAILURE;
}

static int run_parser(struct cc_state *s, const struct cc_parser *p, void **r, struct cc_error *e) {
    if(!s || !p)
        return EINVAL;

    if(!(s->flags & CC_STATE_FLAG_NORETURN)) {
        if(!r)
            return EINVAL;
        *r = NULL;
    }

    if(s->max_recursion_depth && ++s->recursion_depth > s->max_recursion_depth)
        FAIL_WITH(e, s, format("maximum recursion depth of `%u` reached", s->max_recursion_depth)); 

    switch(p->type) {
        case PARSER_FAIL:       FAIL_WITH(e, s, (char*) p->match.msg);

        case PARSER_PASS:       SUCCESS(r, s, NULL);
        
        case PARSER_LOCATION: {
            struct cc_location *loc = malloc(sizeof(struct cc_location));
            if(!loc)
                PROP(s, -errno);

            memcpy(loc, &s->loc, sizeof(struct cc_location));
            SUCCESS(r, s, (void*) loc);
        }

        case PARSER_LIFT:       SUCCESS(r, s, p->match.lift.lf());
        case PARSER_LIFT_VAL:   SUCCESS(r, s, p->match.lift.val);

        case PARSER_EOF:        PROP(s, match_eof(s, r));
        case PARSER_SOF:        PROP(s, match_sof(s, r));
        case PARSER_ANY:        PROP(s, match_any(s, r));
        case PARSER_CHAR:       PROP(s, match_char(s, p->match.ch, r));
        case PARSER_CHAR_RANGE: PROP(s, match_range(s, p->match.lo, p->match.hi, r));
        case PARSER_MATCH:      PROP(s, match_char_func(s, p->match.matchfn, r));

        case PARSER_ONEOF:      PROP(s, match_oneof(s, p->match.list.chars, p->match.list.n, r));
        case PARSER_ANYOF:      PROP(s, match_anyof(s, p->match.list.chars, p->match.list.n, r));
        case PARSER_NONEOF:     PROP(s, match_noneof(s, p->match.list.chars, p->match.list.n, r));

        case PARSER_STRING:     PROP(s, match_string(s, p->match.str, r));

        case PARSER_MANY:       PROP(s, combine_many(s, p, r));
        case PARSER_MANY_UNTIL: PROP(s, combine_many_until(s, p, r, e));

        case PARSER_COUNT:      PROP(s, combine_count(s, p, r, e));
        case PARSER_LEAST:      PROP(s, combine_least(s, p, r, e));

        case PARSER_MAYBE:      PROP(s, combine_maybe(s, p, r));

        case PARSER_CHAIN:      PROP(s, combine_chain(s, p, r, e));
        case PARSER_POSTFIX:    PROP(s, combine_postfix(s, p, r, e));

        case PARSER_AND:        PROP(s, combine_and(s, p, r, e));
        case PARSER_OR:         PROP(s, combine_or(s, p, r, e));

        case PARSER_NOT:        PROP(s, combine_not(s, p));

        case PARSER_EXPECT: {
            assert(p->match.expect.inner);

            int res = run_parser(s, p->match.expect.inner, r, e);
            if(res == PARSE_SUCCESS || res < 0)
                PROP(s, res);
            
            int err = add_expected(e, s, p->match.expect.what);
            if(err)
                PROP(s, -err);

            PROP(s, PARSE_FAILURE);
        }

        case PARSER_APPLY: {
            int res = run_parser(s, p->match.expect.inner, r, e);
            if(res != PARSE_SUCCESS)
                PROP(s, res);

            if(!(s->flags & CC_STATE_FLAG_NORETURN) && p->match.apply.af)
                *r = p->match.apply.af(*r);

            PROP(s, res);
        }

        case PARSER_NORETURN: {
            bool noreturn_before = set_flag(s, CC_STATE_FLAG_NORETURN, true) ;

            int res = run_parser(s, p->match.unary.inner, NULL, e);

            set_flag(s, CC_STATE_FLAG_NORETURN, noreturn_before);
            PROP(s, res);
        }

        case PARSER_NOERROR: {
            bool noerr_before = set_flag(s, CC_STATE_FLAG_NOERR, true);

            int res = run_parser(s, p->match.unary.inner, r, NULL);

            set_flag(s, CC_STATE_FLAG_NOERR, noerr_before);
            PROP(s, res);
        }

        case PARSER_BIND: {
            int err = scope_push(s, p);
            if(err)
                PROP(s, -err);

            int res = run_parser(s, p->match.bind.inner, r, e);
            
            if(scope_pop(s) != p)
                assert(false);

            PROP(s, res);
        }

        case PARSER_LOOKUP: {
            const struct cc_parser *found = scope_lookup(s, p->match.lookup);
            if(!found)
                FAIL_WITH(e, s, format("undefined parser \"%s\"", p->match.lookup));

            PROP(s, run_parser(s, found, r, e));
        }

        case PARSER_UNDEFINED:
        default:                FAIL_WITH(e, s, format("undefined parser %d", p->type));
    }
}

int cc_parse(const struct cc_source *src, struct cc_parser *p, struct cc_result *r) {
    if(r)
        memset(r, 0, sizeof(struct cc_result));
    
    int err = 0;
    
    if(!src || !p || !r) {
        err = EINVAL;
        goto cleanup;
    }
    
    struct cc_state s = {
        .flags = CC_STATE_FLAGS_DEFAULT,
        .loc = CC_LOCATION_DEFAULT,
        .src = src,
        .scope = DYNARR_INIT,
        .max_recursion_depth = src->max_recursion
    };
    
    if(!(r->err = malloc(sizeof(struct cc_error)))) {
        err = errno;
        goto cleanup;
    }
    
    memset(r->err, 0, sizeof(struct cc_error));

    int res = run_parser(&s, p, &r->out, r->err);

    if(res < 0) {
        // libc error, ideally this should never happen
        free(r->err);
        err = -res;
        goto cleanup;
    }

    if(res == PARSE_SUCCESS) {
        free(r->err);
        r->err = NULL;
    }

cleanup:
    state_free(&s);
    cc_release(p);
    return err;
}

