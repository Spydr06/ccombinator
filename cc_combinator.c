#include <ccombinator.h>

#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

struct cc_parser *cc_expect(struct cc_parser *a, const char *e) {
    struct cc_parser *p = parser_allocate();
    if(!p)
        goto failure;

    p->type = PARSER_EXPECT;
    p->match.expect.inner = a;
    p->match.expect.what = e;

    return p;
failure:
    cc_release(a);
    return NULL;
}

CC_format_printf(2)
struct cc_parser *cc_expectf(struct cc_parser *a, const char *fmt, ...) {
    if(!a) {
        errno = EINVAL;
        return NULL;
    }

    va_list ap;
    va_start(ap, fmt);
    char *s = vformat(fmt, ap);
    va_end(ap);

    if(!s)
        goto failure;

    struct cc_parser *p = parser_allocate();
    if(!p)
        goto failure;

    p->type = PARSER_EXPECT;
    p->match.expect.inner = a;
    p->match.expect.what = s;

    p->flags |= PARSER_FLAG_FREE_DATA;

    return p;
failure:
    cc_release(a);
    return NULL;
}

struct cc_parser *cc_apply(struct cc_parser *a, cc_apply_t f) {
    if(!a) {
        errno = EINVAL;
        goto failure;
    }

    struct cc_parser *p = parser_allocate();
    if(!p)
        goto failure;

    p->type = PARSER_APPLY;
    p->match.apply.af = f;
    p->match.apply.inner = a;

    return p;
failure:
    cc_release(a);
    return NULL;
}

struct cc_parser *cc_not(struct cc_parser* a) {
    if(!a) {
        errno = EINVAL;
        goto failure;
    }

    struct cc_parser *p = parser_allocate();
    if(!p)
        goto failure;

    p->type = PARSER_NOT;
    p->match.unary.inner = a;

    return p;
failure:
    cc_release(a);
    return NULL;
}

static struct cc_parser *variadic_parser(unsigned n, va_list ap) {
    va_list ap_copy;
    va_copy(ap_copy, ap);

    struct cc_parser **ps = malloc(n * sizeof(struct cc_parser*));
    if(!ps)
        return NULL;

    for(unsigned i = 0; i < n; i++) {
        struct cc_parser *a = va_arg(ap, struct cc_parser*);
        ps[i] = a;

        if(!a) {
            errno = EINVAL;
            goto failure;
        }
    }
    
    struct cc_parser *p = parser_allocate();
    if(!p)
        goto failure;

    p->flags |= PARSER_FLAG_FREE_DATA;
    p->match.variadic.n = n;
    p->match.variadic.inner = ps;

    va_end(ap_copy);
    return p;
failure:
    for(unsigned i = 0; i < n; i++) {
        struct cc_parser *a = va_arg(ap, struct cc_parser*);
        cc_release(a);
    }

    va_end(ap_copy);
    free(ps);
    return NULL;
}

struct cc_parser *cc_and(unsigned n, cc_fold_t f, ...) {
    va_list ap;
    va_start(ap, f);
    struct cc_parser *p = variadic_parser(n, ap);
    va_end(ap);

    if(!p)
        return NULL;

    p->fold = f;
    p->type = PARSER_AND;
    return p;
}

struct cc_parser *cc_or(unsigned n, ...) {
    va_list ap;
    va_start(ap, n);
    struct cc_parser *p = variadic_parser(n, ap);
    va_end(ap);

    if(!p)
        return NULL;

    p->type = PARSER_OR;
    return p;
}

static struct cc_parser *unary_parser(struct cc_parser *a) {
    if(!a) {
        errno = EINVAL;
        goto failure;
    }

    struct cc_parser *p = parser_allocate();
    if(!p)
        goto failure;

    p->match.unary.inner = a;

    return p;
failure:
    cc_release(a);
    return NULL;
}

static struct cc_parser *binary_parser(struct cc_parser *lhs, struct cc_parser *rhs) {
    if(!lhs || !rhs) {
        errno = EINVAL;
        goto failure;
    }

    struct cc_parser *p = parser_allocate();
    if(!p)
        goto failure;

    p->match.binary.lhs = lhs;
    p->match.binary.rhs = rhs;

    return p;
failure:
    cc_release(lhs);
    cc_release(rhs);
    return NULL;
}

struct cc_parser *cc_many(cc_fold_t f, struct cc_parser *a) {
    struct cc_parser *p = unary_parser(a);
    if(!p)
        return NULL;

    p->fold = f;
    p->type = PARSER_MANY;

    return p;
}

struct cc_parser *cc_many_until(cc_fold_t f, struct cc_parser *a, struct cc_parser *end) {
    struct cc_parser *p = binary_parser(a, end);
    if(!p)
        return NULL;

    p->fold = f;
    p->type = PARSER_MANY_UNTIL;

    return p;
}

struct cc_parser *cc_count(unsigned n, cc_fold_t f, struct cc_parser *a) {
    struct cc_parser *p = unary_parser(a);
    if(!p)
        return NULL;

    p->fold = f;
    p->type = PARSER_COUNT;
    p->match.unary.n = n;

    return p;
}

struct cc_parser *cc_least(unsigned n, cc_fold_t f, struct cc_parser *a) {
    struct cc_parser *p = unary_parser(a);
    if(!p)
        return NULL;

    p->fold = f;
    p->type = PARSER_LEAST;
    p->match.unary.n = n;

    return p;
}

struct cc_parser *cc_maybe(struct cc_parser *a) {
    struct cc_parser *p = unary_parser(a);
    if(!p)
        return NULL;

    p->type = PARSER_MAYBE;
    return p;
}

struct cc_parser *cc_chain(cc_fold_t f, struct cc_parser *a, struct cc_parser *op) {
    struct cc_parser *p = binary_parser(a, op);
    if(!p)
        return NULL;

    p->type = PARSER_CHAIN;
    p->fold = f;

    return p;
}

struct cc_parser *cc_postfix(cc_fold_t f, struct cc_parser *a, struct cc_parser *op) {
    struct cc_parser *p = binary_parser(a, op);
    if(!p)
        return NULL;

    p->type = PARSER_POSTFIX;
    p->fold = f;

    return p;
}

struct cc_parser *cc_token(struct cc_parser *a) {
    if(!a) {
        errno = EINVAL;
        return NULL;
    }

    struct cc_parser *ws = cc_many(NULL, cc_noreturn(cc_whitespace()));
    if(!ws)
        return NULL;

    return cc_and(3, cc_fold_middle, cc_retain(ws), a, ws);
}

struct cc_parser *cc_fix(cc_fix_t f, void *userp) {
    struct cc_parser *placeholder = parser_allocate();
    if(!placeholder)
        return NULL;

    errno = 0;
    struct cc_parser *real = f(placeholder, userp);
    if(!real) {
        if(!errno)
            errno = EINVAL;
        return NULL;
    } 

    cc_parser_copy(placeholder, real);

    real->flags |= PARSER_FLAG_RETAIN_INNER;
    real->flags &= ~PARSER_FLAG_FREE_DATA;

    parser_free(real);

    return placeholder;
}

struct cc_parser *cc_noreturn(struct cc_parser *a) {
    struct cc_parser *p = unary_parser(a);
    if(!p)
        return NULL;

    p->type = PARSER_NORETURN;

    return p;
}

struct cc_parser *cc_noerror(struct cc_parser *a) {
    struct cc_parser *p = unary_parser(a);
    if(!p)
        return NULL;

    p->type = PARSER_NOERROR;

    return p;
}

struct cc_parser *cc_between(struct cc_parser *s, struct cc_parser *a, struct cc_parser *e) {
    if(!s || !a || !e)
        return NULL;

    return cc_and(3, cc_fold_middle, cc_noreturn(s), a, cc_noreturn(e));
}

