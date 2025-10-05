#include <ccombinator.h>

#include "internal.h"

#include <errno.h>
#include <memory.h>

#include <assert.h>

#define CC_STATE_FLAGS_DEFAULT 0x00
#define CC_STATE_FLAG_EOF 0x01

struct cc_state {
    int flags;
    const struct cc_source *src;
    struct cc_location loc;
};

struct cc_save {
    struct cc_state state;
};

#define PARSE_SUCCESS 1
#define PARSE_FAILURE 0

#define FAIL_WITH(e, s, x) do {             \
        int err = new_error((e), (s), (x)); \
        return err ? -err : PARSE_FAILURE;  \
    } while(0)

#define SUCCESS(r, x) do {                  \
        (r) = (x);                          \
        return PARSE_SUCCESS;               \
    } while(0)

#define PRIMITIVE(x) do {                           \
        return (x) ? PARSE_SUCCESS : PARSE_FAILURE; \
    } while(0)

static int run_parser(struct cc_state *s, const struct cc_parser *p, void **r, struct cc_error *e);

static inline char32_t peek_at(struct cc_state *s) {
    if(s->loc.byte_off > s->src->buffer_size) {
        s->flags |= CC_STATE_FLAG_EOF;
        return EOF;
    }

    return utf8_first_cp(s->src->buffer + s->loc.byte_off);
}

static inline struct cc_save state_save(const struct cc_state *s) {
    return (struct cc_save){
        .state = *s
    };
}

static inline void state_restore(struct cc_state *s, const struct cc_save *save) {
    *s = save->state;
}

static int new_error(struct cc_error *e, struct cc_state *s, const char *msg) {
    if(!e)
        return EINVAL;

    memset(e, 0, sizeof(struct cc_error));

    e->loc = s->loc;
    e->received = peek_at(s);
    e->failure = msg;

    return 0;
}

/*static int new_expect(struct cc_error* e, struct cc_state *s, const char *const *expected, size_t n) {
    if(!e)
        return EINVAL;

    memset(e, 0, sizeof(struct cc_error));

    e->loc = s->loc;
    e->received = peek_at(s);

    e->num_expected = n;
    e->expected = malloc(sizeof(const char*) * n);

    memcpy(e->expected, expected, sizeof(const char*) * n);

    e->flags |= CC_ERR_FREE_EXPECTED;

    return 0;
}*/

static int add_expected(struct cc_error *e, struct cc_state *s, const char *expected) {
    if(e->num_expected >= CC_ERR_MAX_EXPECTED)
        return 0; // cannot add anymore, don't error tho

    if(e->num_expected == 0) {
        e->filename = s->src->origin;
        e->loc = s->loc;
        e->received = peek_at(s);
    }

    e->expected[e->num_expected++] = expected;

    return 0;
}

/*CC_format_printf(3)
static int new_errorf(struct cc_error *e, struct cc_state *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = vformat(fmt, ap);
    va_end(ap);

    if(!msg)
        return errno;

    new_error(e, s, msg);

    e->flags |= CC_ERR_FREE_FAILURE;

    return 0;
}*/

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

static bool match_char(struct cc_state *s, char32_t ch) {
    char32_t next = peek_at(s);
    if((s->flags & CC_STATE_FLAG_EOF) || next != ch)
        return false;

    advance_char(s, ch);

    return true;
}

static bool match_eof(struct cc_state *s) {
    peek_at(s);
    return !!(s->flags & CC_STATE_FLAG_EOF);
}

static bool match_any(struct cc_state *s) {
    char32_t next = peek_at(s);
    if(s->flags & CC_STATE_FLAG_EOF)
        return false;

    advance_char(s, next);

    return true;
}

static bool match_range(struct cc_state *s, char32_t lo, char32_t hi) {
    char32_t next = peek_at(s);
    if((s->flags & CC_STATE_FLAG_EOF) || next < lo || next > hi)
        return false;

    advance_char(s, next);

    return true;
}

static bool match_oneof(struct cc_state *s, const char32_t *chars, size_t n) {
    char32_t next = peek_at(s);
    if(s->flags & CC_STATE_FLAG_EOF)
        return false;

    bool one_found = false;
    for(size_t i = 0; i < n; i++) {
        if(next == chars[n]) {
            if(one_found)
                return false;
            one_found = true;
        }
    }
    
    if(!one_found)
        return false;

    advance_char(s, next);

    return true;
}

static bool match_anyof(struct cc_state *s, const char32_t *chars, size_t n) {
    char32_t next = peek_at(s);
    if(s->flags & CC_STATE_FLAG_EOF)
        return false;

    for(size_t i = 0; i < n; i++) {
        if(next == chars[n]) {
            advance_char(s, next);
            return true;
        }
    }

    return false;
}

static bool match_noneof(struct cc_state *s, const char32_t *chars, size_t n) {
    char32_t next = peek_at(s);
    if(s->flags & CC_STATE_FLAG_EOF)
        return false;

    for(size_t i = 0; i < n; i++) {
        if(next == chars[n])
            return false;
    }

    advance_char(s, next);

    return true;
}

static bool match_string(struct cc_state *s, const char8_t *str) {
    struct cc_state save = *s;

    for(size_t i = 0; str[i];) {
        char32_t ch = utf8_first_cp(str + i);
        if(!match_char(s, ch)) {
            *s = save;
            return false;
        }

        i += utf8_cp_length(ch);
    }

    return true;
}

static int combine_many(struct cc_state  *s, const struct cc_parser *p, void **r) {
    struct dynarr values = DYNARR_INIT;

    for(;;) {
        void *val = NULL;
        struct cc_save save = state_save(s);
        struct cc_error e;
        memset(&e, 0, sizeof(struct cc_error));

        int res = run_parser(s, p->match.unary.inner, &val, &e);
        if(res == PARSE_FAILURE) {
            state_restore(s, &save);
            cc_err_free(&e);
            break;
        }
        if(res < 0) {
            dynarr_free(&values);
            return res;
        }

        int err = dynarr_append(&values, val);
        if(err) {
            dynarr_free(&values);
            return -err;
        }
    }

    if(p->fold)
        *r = p->fold(values.len, values.elems);

    dynarr_free(&values);
    return PARSE_SUCCESS;
}

static int combine_count(struct cc_state *s, const struct cc_parser *p, void **r, struct cc_error *e) { 
    unsigned num_values = p->match.unary.n;
    void *values[num_values]; // FIXME: use dynarr on larger n to avoid stackoverflows
    
    for(unsigned i = 0; i < num_values; i++) {
        int res = run_parser(s, p->match.unary.inner, &values[i], e);
        if(res == PARSE_FAILURE || res < 0)
            return res;
    }

    if(p->fold)
        *r = p->fold(num_values, values);

    return PARSE_SUCCESS;
}

static int run_parser(struct cc_state *s, const struct cc_parser *p, void **r, struct cc_error *e) {
    assert(s && p && r && e);

    switch(p->type) {
        case PARSER_FAIL:       FAIL_WITH(e, s, (char*) p->match.msg);

        case PARSER_PASS:       SUCCESS(r, NULL);

        case PARSER_LIFT:       SUCCESS(r, p->match.lift.lf());
        case PARSER_LIFT_VAL:   SUCCESS(r, p->match.lift.val);

        case PARSER_EOF:        PRIMITIVE(match_eof(s));
        case PARSER_ANY:        PRIMITIVE(match_any(s));
        case PARSER_CHAR:       PRIMITIVE(match_char(s, p->match.ch));
        case PARSER_CHAR_RANGE: PRIMITIVE(match_range(s, p->match.lo, p->match.hi));

        case PARSER_ONEOF:      PRIMITIVE(match_oneof(s, p->match.list.chars, p->match.list.n));
        case PARSER_ANYOF:      PRIMITIVE(match_anyof(s, p->match.list.chars, p->match.list.n));
        case PARSER_NONEOF:     PRIMITIVE(match_noneof(s, p->match.list.chars, p->match.list.n));

        case PARSER_STRING:     PRIMITIVE(match_string(s, p->match.str));

        case PARSER_MANY:       return combine_many(s, p, r);
        case PARSER_COUNT:      return combine_count(s, p, r, e);

        case PARSER_EXPECT: {
            assert(p->match.expect.inner);

            int res = run_parser(s, p->match.expect.inner, r, e);
            if(res == PARSE_SUCCESS || res < 0)
                return res;
            
            int err = add_expected(e, s, p->match.expect.what);
            if(err)
                return -err;

            return PARSE_FAILURE;
        }

        case PARSER_UNDEFINED:
        default:                FAIL_WITH(e, s, "undefined parser");
    }
}

int cc_parse(const struct cc_source *src, const struct cc_parser *p, struct cc_result *r) {
    if(!src || !p || !r)
        return EINVAL;
    
    struct cc_state s = {
        .flags = CC_STATE_FLAGS_DEFAULT,
        .loc = CC_LOCATION_DEFAULT,
        .src = src
    };

    memset(r, 0, sizeof(struct cc_result));

    r->err = malloc(sizeof(struct cc_error));
    if(!r->err)
        return errno;
    
    memset(r->err, 0, sizeof(struct cc_error));
    r->err->flags |= CC_ERR_FREE_SELF;

    int res = run_parser(&s, p, &r->out, r->err);

    if(res < 0) {
        // libc error, ideally this should never happen
        free(r->err);
        return -res;
    }

    if(res == PARSE_SUCCESS) {
        free(r->err);
        r->err = NULL;
    }

    return 0;
}

