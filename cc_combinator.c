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

struct cc_parser *cc_many(cc_fold_t f, struct cc_parser *a) {
    struct cc_parser *p = unary_parser(a);
    if(!p)
        return NULL;

    p->fold = f;
    p->type = PARSER_MANY;
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

struct cc_parser *cc_maybe(struct cc_parser *a) {
    struct cc_parser *p = unary_parser(a);
    if(!p)
        return NULL;

    p->type = PARSER_MAYBE;
    return p;
}

