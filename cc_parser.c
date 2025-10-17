#include <ccombinator.h>

#include "internal.h"

#include <stddef.h>
#include <errno.h>
#include <uchar.h>

struct cc_parser *cc_retain(struct cc_parser *p) {
    // ignore NULL (simplifies error propagation)
    if(p)
        p->rc++;
        
    return p;
}

struct cc_parser *cc_release(struct cc_parser *p) {
    if(!p || --p->rc > 0)
        return p;

    parser_free(p);
    return NULL;
}

void cc_parser_copy(struct cc_parser *d, const struct cc_parser* s) {
    int d_rc = d->rc;
    memcpy(d, s, sizeof(struct cc_parser));
    d->rc = d_rc;
}

struct cc_parser *parser_allocate(void) {
    struct cc_parser *p = calloc(1, sizeof(struct cc_parser));
    if(!p)
        return NULL;

    p->rc = 1;
    return p;
}

// free the parser ignoring the refcount
void parser_free(struct cc_parser* p) {
    if(p->flags & PARSER_FLAG_RETAIN_INNER)
        goto free_data;

    switch(p->type) {
        case PARSER_EXPECT:
            cc_release(p->match.expect.inner);
            break;
        case PARSER_APPLY:
            cc_release(p->match.apply.inner);
            break;
        case PARSER_NOT:
        case PARSER_MANY:
        case PARSER_COUNT:
        case PARSER_MAYBE:
        case PARSER_LEAST:
        case PARSER_NOERROR:
        case PARSER_NORETURN:
            cc_release(p->match.unary.inner);
            break;
        case PARSER_AND:
        case PARSER_OR:
            for(unsigned i = 0; i < p->match.variadic.n; i++)
                cc_release(p->match.variadic.inner[i]);
            break;
        case PARSER_BIND:
            cc_release(p->match.bind.inner);
            break;
        case PARSER_MANY_UNTIL:
        case PARSER_CHAIN:
        case PARSER_POSTFIX:
            cc_release(p->match.binary.lhs);
            cc_release(p->match.binary.rhs);
            break;
        default:
            break;
    }

free_data:
    if(!(p->flags & PARSER_FLAG_FREE_DATA))
        goto free_self;

    switch(p->type) {
        case PARSER_STRING:
            free((char8_t*) p->match.str);
            break;
        case PARSER_FAIL:
            free((char*) p->match.msg);
            break;
        case PARSER_LIFT_VAL:
            free(p->match.lift.val);
            break;
        case PARSER_ANYOF:
        case PARSER_NONEOF:
        case PARSER_ONEOF:
            free((char32_t*) p->match.list.chars);
            break;
        case PARSER_EXPECT:
            free((char*) p->match.expect.what);
            break;
        case PARSER_AND:
        case PARSER_OR:
            free(p->match.variadic.inner);
            break;
        case PARSER_LOOKUP:
            free((char*) p->match.lookup);
            break;
        case PARSER_BIND:
            free((char*) p->match.bind.name);
            break;
        default:
            break;
    }
    
free_self:
    free(p);
}

struct cc_parser *cc_string(const char8_t *s) {
    if(!s) {
        errno = EINVAL;
        return NULL;
    }

    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->type = PARSER_STRING;
    p->match.str = s;

    return cc_expectf(p, "string \"%s\"", s);
}

struct cc_parser *cc_char(char32_t c) {
    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->type = PARSER_CHAR;
    p->match.ch = c;

    char8_t buf[CC_UTF8_ENCODE_PRINTABLE_MAX];
    utf8_encode_printable(p->match.ch, buf);
    return cc_expectf(p, "%s", (char*) buf);
}

struct cc_parser *cc_range(char32_t lo, char32_t hi) {
    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->type = PARSER_CHAR_RANGE;
    p->match.lo = lo;
    p->match.hi = hi;

    char8_t lo_buf[CC_UTF8_ENCODE_PRINTABLE_MAX], 
            hi_buf[CC_UTF8_ENCODE_PRINTABLE_MAX];
    utf8_encode_printable(lo, lo_buf);
    utf8_encode_printable(hi, hi_buf);
    return cc_expectf(p, "character in range %s - %s", lo_buf, hi_buf);
}

static struct cc_parser *char_arr_parser(const char32_t *chars, const char *what) {
    if(!chars || !what) {
        errno = EINVAL;
        return NULL;
    }

    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->match.list.chars = chars;

    while(chars[p->match.list.n])
        p->match.list.n++;

    struct string_buffer sb = STRING_BUFFER_INIT;
    int err;

    if((err = string_buffer_append(&sb, "%s of ", what)))
        goto cleanup;
    
    if(p->match.list.n == 0 && (err = string_buffer_append(&sb, "nothing")))
        goto cleanup;

    else if(p->match.list.n == 1 && (err = string_buffer_append(&sb, "'%s'", chars[0])))
        goto cleanup;

    else {
        for(size_t i = 0; i < p->match.list.n - 2; i++) {
            char8_t buf[CC_UTF8_ENCODE_PRINTABLE_MAX];
            utf8_encode_printable(chars[i], buf);
            if((err = string_buffer_append(&sb, "%s, ", buf)))
                goto cleanup;
        }

        char8_t lo_buf[CC_UTF8_ENCODE_PRINTABLE_MAX],
                hi_buf[CC_UTF8_ENCODE_PRINTABLE_MAX];
        utf8_encode_printable(chars[p->match.list.n - 2], hi_buf);
        utf8_encode_printable(chars[p->match.list.n - 1], lo_buf);
        if((err = string_buffer_append(&sb, "%s or %s", lo_buf, hi_buf)))
            goto cleanup;
    }

    struct cc_parser *ex = cc_expect(p, string_buffer_unwrap(&sb));
    if(!ex) {
        err = errno;
        goto cleanup;
    }

    ex->flags |= PARSER_FLAG_FREE_DATA;
    ex->match.expect.inner = p;
    
    return ex;
cleanup:
    parser_free(p);
    // TODO: free p
    free(sb.buf);
    errno = err;
    return NULL;
}

struct cc_parser *cc_anyof(const char32_t chars[]) {
    struct cc_parser *p = char_arr_parser(chars, "any");
    if(!p)
        return NULL;

    p->match.expect.inner->type = PARSER_ANYOF;

    return p;
}

struct cc_parser *cc_oneof(const char32_t chars[]) {
    struct cc_parser *p = char_arr_parser(chars, "one");
    if(!p)
        return NULL;

    p->match.expect.inner->type = PARSER_ONEOF;

    return p;
}

struct cc_parser *cc_noneof(const char32_t chars[]) {
    struct cc_parser *p = char_arr_parser(chars, "none");
    if(!p)
        return NULL;

    p->match.expect.inner->type = PARSER_NONEOF;

    return p;
}

__internal struct cc_parser *parser_match(int (*f)(char32_t), const char *what) {
    if(!f) {
        errno = EINVAL;
        return NULL;
    }

    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->type = PARSER_MATCH;
    p->match.matchfn = f;

    return cc_expect(p, what);
}

struct cc_parser *cc_match(int (*f)(char32_t)) {
    if(!f) {
        errno = EINVAL;
        return NULL;
    }

    // ISO C does not allow directly casting from a function ptr to void*
    union {
        int (*f)(char32_t);
        void *p;
    } conv;
    conv.f = f;

    char *what = format("character matching function <%p>", conv.p);
    if(!what)
        return NULL;

    struct cc_parser *p = parser_match(f, what);
    if(!p)
        return NULL;

    p->flags |= PARSER_FLAG_FREE_DATA;

    return p;
}

struct cc_parser *cc_eof(void) {
    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->type = PARSER_EOF;

    return cc_expect(p, "end of file");
}

struct cc_parser *cc_sof(void) {
    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->type = PARSER_SOF;

    return cc_expect(p, "start of file");
}

struct cc_parser *cc_any(void) {
    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->type = PARSER_ANY;

    return cc_expect(p, "any character");
}

struct cc_parser *cc_whitespace(void) {
    return parser_match(utf8_is_whitespace, "whitespace character");
}

struct cc_parser *cc_blank(void) {
    return parser_match(utf8_is_blank, "blank character");
}

struct cc_parser *cc_newline(void) {
    return cc_char(U'\n');
}

struct cc_parser *cc_tab(void) {
    return cc_char(U'\t');
}

struct cc_parser *cc_digit(void) {
    return parser_match(utf8_is_digit, "digit");
}

struct cc_parser *cc_hexdigit(void) {
    return parser_match(utf8_is_hexdigit, "hexadecimal digit");
}

struct cc_parser *cc_octdigit(void) {
    return parser_match(utf8_is_octdigit, "octal digit");
}

struct cc_parser *cc_alpha(void) {
    return parser_match(utf8_is_alpha, "alphabetical character");
}

struct cc_parser *cc_lower(void) {
    return parser_match(utf8_is_lower, "lower-case character");
}

struct cc_parser *cc_upper(void) {
    return parser_match(utf8_is_upper, "upper-case character");
}

struct cc_parser *cc_underscore(void) {
    return cc_char(U'_');
}

struct cc_parser *cc_aplhanum(void) {
    return parser_match(utf8_is_alphanum, "alphanumeric character");
}

struct cc_parser *cc_pass(void) {
    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->type = PARSER_PASS;

    return p;
}

struct cc_parser *cc_fail(const char *e) {
    if(!e) {
        errno = EINVAL;
        return NULL;
    }

    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->type = PARSER_FAIL;
    p->match.msg = e;

    return p;
}

CC_format_printf(1)
struct cc_parser *cc_failf(const char *fmt, ...) {
    if(!fmt) {
        errno = EINVAL;
        return NULL;
    }

    va_list ap;
    va_start(ap, fmt);
    char *e = vformat(fmt, ap);
    va_end(ap);

    if(!e)
        return NULL;

    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->type = PARSER_FAIL;
    p->match.msg = e;

    p->flags |= PARSER_FLAG_FREE_DATA;

    return p;
}

struct cc_parser *cc_lift(cc_lift_t lf) {
    if(!lf) {
        errno = EINVAL;
        return NULL;
    }

    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->type = PARSER_LIFT;
    p->match.lift.lf = lf;

    return p;
}

struct cc_parser *cc_lift_val(void *val) {
    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->type = PARSER_LIFT;
    p->match.lift.val = val;

    return p;
}

struct cc_parser *cc_location(void) {
    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->type = PARSER_LOCATION;

    return p;
}

struct cc_parser *cc_lookup(const char *name) {
    if(!name) {
        errno = EINVAL;
        return NULL;
    }

    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->type = PARSER_LOOKUP;
    p->match.lookup = name;

    return p;
}

struct cc_parser *cc_bind(const char *name, struct cc_parser *a) {
    if(!name || !a) {
        errno = EINVAL;
        return NULL;
    }

    struct cc_parser *p = parser_allocate();
    if(!p)
        return NULL;

    p->type = PARSER_BIND;
    p->match.bind.name = name;
    p->match.bind.inner = a;

    return p;
}

