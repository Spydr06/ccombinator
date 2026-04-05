#include <ccombinator.h>

#include "internal.h"

#include <errno.h>
#include <assert.h>

static struct cc_parser *__re_parser;

#define RE_SEL_START '['
#define RE_SEL_END ']'
#define RE_GROUP_START '('
#define RE_GROUP_END ')'

#define RE_NEGATE_CHAR '^'
#define RE_CLASS_CHAR ':'
#define RE_RANGE_CHAR '-'
#define RE_QUANT_OPT '?'
#define RE_QUANT_LEAST1 '+'
#define RE_QUANT_MANY '*'
#define RE_ALTERNATIVE_CHAR '|'

#define RE_SOF_CHAR '^'
#define RE_EOF_CHAR '$'
#define RE_ANY_CHAR '.'

#define RE_ESCAPE_CHAR '\\'
#define RE_SPECIAL_CHARS U"\\()[]{}|?*+.^$"

#define RE_SEL_ALLOWED_CHARS U"(){}|?*.^$"

#define RE_CLASS_CHARS U"adDlsSuwWx"

static void *re_char(void *r) {
    if(!r) return NULL; // error propagation

    char32_t ch = utf8_first_cp(r);
    free(r);

    return cc_char(ch);
}

static int re_not_digit(char32_t c) {
    return !utf8_is_digit(c);
}

static int re_not_whitespace(char32_t c) {
    return !utf8_is_whitespace(c);
}

static int re_is_word(char32_t c) {
    return utf8_is_alphanum(c) || c == '_';
}

static int re_not_word(char32_t c) {
    return !re_is_word(c);
}

#define RE_POSIX_CLASSES_COUNT 12

static const struct {
    const char8_t *class;
    int (*match)(char32_t);
} re_posix_classes[RE_POSIX_CLASSES_COUNT] = {
    { u8"cntrl", utf8_is_cntrl },
    { u8"print", utf8_is_print },
    { u8"space", utf8_is_whitespace },
    { u8"blank", utf8_is_blank },
    { u8"graph", utf8_is_graph },
    { u8"punct", utf8_is_punct },
    { u8"alnum", utf8_is_alphanum },
    { u8"xdigit", utf8_is_hexdigit },
    { u8"digit", utf8_is_digit},
    { u8"alpha", utf8_is_alpha },
    { u8"lower", utf8_is_lower },
    { u8"upper", utf8_is_upper }
};

static void *re_posix_class(void *r) {
    if(!r) return NULL; // error propagation
    
    return cc_match((int (*)(char32_t)) r);
}

static void *re_class(void *r) {
    if(!r) return NULL; // error propagation

    char32_t esc = utf8_first_cp(r);
    free(r);

    switch(esc) {
        case 'a':
            return cc_alpha();
        case 'd':
            return cc_digit();
        case 'D':
            return parser_match(re_not_digit, "non-digit character");
        case 'l':
            return cc_lower();
        case 'u':
            return cc_upper();
        case 'w':
            return parser_match(re_is_word, "character in 'a' - 'z', 'A' - 'Z', '0' - '9' or '_'");
        case 'W':
            return parser_match(re_not_word, "character except 'a' - 'z', 'A' - 'Z', '0' - '9' or '_'");
        case 's':
            return cc_whitespace();
        case 'S':
            return parser_match(re_not_whitespace, "non-whitespace character");
        case 'x':
            return cc_hexdigit();
        default:
            assert(false && "invalid escape char");
            return NULL;
    }
}

static void *re_range(size_t n, void **r) {
    if(!r) return NULL; // error propagation

    assert(n == 3 && "re_range() only applicable on cc_and(3, ...)");

    if(!r[0] || !r[2]) {
        free(r[0]);
        free(r[2]);
        return NULL;
    }

    char32_t lo = utf8_first_cp(r[0]);
    char32_t hi = utf8_first_cp(r[2]);

    free(r[0]); 
    free(r[2]);
    return cc_range(lo, hi);
}

static void *re_eof(void *) {
    return cc_eof();
}

static void *re_sof(void *) {
    return cc_sof();
}

static void *re_any(void *) {
    return cc_any();
}

static void *re_sel(size_t n, void **r) {
    if(!r) return NULL;
    if(n == 1) return r[0];
    if(n == 2) return cc_either(r[0], r[1]);

    assert(n > 0 && "zero-size selection");

    struct cc_parser **ps = malloc(n * sizeof(struct cc_parser*)); // FIXME: do without copying
    memcpy(ps, r, n * sizeof(struct cc_parser*));

    return cc_free_data(cc_orv((unsigned) n, ps));
}

static void *re_negated_sel(size_t n, void **r) {
    if(!r) return NULL;

    assert(n > 0 && "zero-size selection");

    struct cc_parser *or;
    if(n == 1) or = r[0];
    else if(n == 2) or = cc_either(r[0], r[1]);
    else {
        struct cc_parser **ps = malloc(n * sizeof(struct cc_parser*));
        if(!ps)
            return NULL;
    
        memcpy(ps, r, n *sizeof(struct cc_parser*));
        or = cc_free_data(cc_orv((unsigned) n, ps));
    }

    return cc_seq(cc_fold_last, cc_not(or), cc_any());
}

static void *re_seq(size_t n, void **r) {
    if(!r) return NULL;
    if(n == 0) cc_pass();
    if(n == 1) return r[0];
    if(n == 2) return cc_either(r[0], r[1]);

    struct cc_parser **ps = malloc(n * sizeof(struct cc_parser*));
    memcpy(ps, r, n * sizeof(struct cc_parser*));

    return cc_free_data(cc_andv((unsigned) n, cc_fold_concat, ps));
}

static void *re_apply_qop_many(void *) {
    return (void *) RE_QUANT_MANY;
}

static void *re_apply_qop_least1(void *) {
    return (void *) RE_QUANT_LEAST1;
}

static void *re_apply_qop_opt(void *) {
    return (void *) RE_QUANT_OPT;
}

static void *re_quantify(size_t n, void **r) {
    if(n == 0) return NULL; // error propagation
    if(n == 1) return r[0];

    assert(n >= 2 && "re_quantor() cannot be applied to n < 2");

    struct cc_parser *p = r[0];
    if(!p)
        return NULL; // error propagation

    for(size_t i = 1; i < n; i++) {
        char32_t qop = (char32_t) (uintptr_t) r[i];

        switch(qop) {
        case RE_QUANT_MANY:
            p = cc_many(cc_fold_concat, p);
            break;
        
        case RE_QUANT_LEAST1:
            p = cc_least(1, cc_fold_concat, p);
            break;

        case RE_QUANT_OPT:
            p = cc_maybe(p);
            break;
        }
    }

    return (void*) p;
}

static void *re_alt(size_t n, void **r) {
    if(!r) return NULL;
    if(n == 1) return r[0];

    assert(n % 2 == 1 && "re_alt() cannot be applied to even n");

    struct cc_parser **ps = malloc((n + 1) / 2 * sizeof(struct cc_parser*));
    if(!ps)
        return NULL;

    ps[0] = r[0];

    for(size_t i = 1; i < n; i += 2) {
        ps[(i + 1) / 2] = r[i + 1];
    }

    return cc_free_data(cc_orv((n + 1) / 2, ps));
}

static struct cc_parser *re_expr_fix(struct cc_parser *self, void*) {
    struct cc_parser *escaped = cc_apply(cc_seq(cc_fold_last, cc_noreturn(cc_char(RE_ESCAPE_CHAR)), cc_anyof(RE_SPECIAL_CHARS)), re_char);
    struct cc_parser *class = cc_apply(cc_seq(cc_fold_last, cc_noreturn(cc_char(RE_ESCAPE_CHAR)), cc_anyof(RE_CLASS_CHARS)), re_class);

    struct cc_parser **posix_classes = malloc(RE_POSIX_CLASSES_COUNT * sizeof(struct cc_parser*));
    if(!posix_classes)
        return NULL;

    for(int i = 0; i < RE_POSIX_CLASSES_COUNT; i++) {
        posix_classes[i] = cc_apply(cc_seq(cc_fold_last, cc_noreturn(cc_string(re_posix_classes[i].class)), cc_lift_val((void*) re_posix_classes[i].match)), re_posix_class);
    }

    struct cc_parser *posix_class = cc_and(5,
        cc_fold_middle,
        cc_noreturn(cc_char(RE_SEL_START)),
        cc_noreturn(cc_char(RE_CLASS_CHAR)),
        cc_free_data(cc_orv(RE_POSIX_CLASSES_COUNT, posix_classes)), 
        cc_noreturn(cc_char(RE_CLASS_CHAR)),
        cc_noreturn(cc_char(RE_SEL_END))
    );

    struct cc_parser *sym = cc_or(7,
        cc_apply(cc_noneof(RE_SPECIAL_CHARS), re_char),
        escaped,
        class,
        posix_class,
        cc_apply(cc_noreturn(cc_char(RE_EOF_CHAR)), re_eof),
        cc_apply(cc_noreturn(cc_char(RE_SOF_CHAR)), re_sof),
        cc_apply(cc_noreturn(cc_char(RE_ANY_CHAR)), re_any)
    );

    struct cc_parser *range = cc_and(3,
        re_range,
        cc_noneof(RE_SPECIAL_CHARS),
        cc_noreturn(cc_char(RE_RANGE_CHAR)),
        cc_noneof(RE_SPECIAL_CHARS)
    );

    struct cc_parser *opt = cc_or(3,
        range,
        cc_retain(sym),
        cc_anyof(RE_SEL_ALLOWED_CHARS)
    );

    struct cc_parser *sel = cc_and(3,
        cc_fold_middle,
        cc_noreturn(cc_char(RE_SEL_START)),
        cc_either(
            cc_seq(cc_fold_last, cc_noreturn(cc_char(RE_NEGATE_CHAR)), cc_least(1, re_negated_sel, cc_retain(opt))),
            cc_least(1, re_sel, opt)
        ),
        cc_noreturn(cc_char(RE_SEL_END))
    );

    struct cc_parser *group = cc_and(3,
        cc_fold_middle,
        cc_noreturn(cc_char(RE_GROUP_START)),
        self,
        cc_noreturn(cc_char(RE_GROUP_END))
    );

    struct cc_parser *exp = cc_or(3, sym, group, sel);

    struct cc_parser* qops = cc_or(3,
        cc_apply(cc_noreturn(cc_char(RE_QUANT_MANY)), re_apply_qop_many),
        cc_apply(cc_noreturn(cc_char(RE_QUANT_LEAST1)), re_apply_qop_least1),
        cc_apply(cc_noreturn(cc_char(RE_QUANT_OPT)), re_apply_qop_opt)
    );

    struct cc_parser *quantified = cc_postfix(re_quantify, exp, qops);

    struct cc_parser *seq = cc_many(re_seq, quantified);

    struct cc_parser *alt = cc_chain(
        re_alt,
        seq,
        cc_noreturn(cc_char(RE_ALTERNATIVE_CHAR))
    );

    return alt;
}

static void regex_state_release(void) {
    if(!__re_parser)
        return;

    cc_release(__re_parser);
    __re_parser = NULL;
}


static inline struct cc_parser *re_parser(void) {
    if(__re_parser)
        return cc_retain(__re_parser);

    __re_parser = cc_and(2, cc_fold_first,
        cc_fix(re_expr_fix, NULL),
        cc_eof()
    );

    atexit(regex_state_release);

    return cc_retain(__re_parser);
}

struct cc_parser *cc_regex_from(const struct cc_source *re_source, struct cc_error **e) {
    struct cc_parser *re = re_parser();
    if(!re)
        return NULL;

    struct cc_result r;
    int res = cc_parse(re_source, re, &r);
    if(res < 0) {
        errno = -res;
        return NULL;
    }

    *e = r.err;
    return r.out;
}

struct cc_parser *cc_regex(const char8_t *re, struct cc_error **e) {
    if(!re || !e) {
        errno = EINVAL;
        return NULL;
    }

    struct cc_source *re_source = cc_string_source(re);
    if(!re_source)
        return NULL;

    struct cc_parser *p = cc_regex_from(re_source, e);
    if(!p) {
        cc_close(re_source);
        return NULL;
    }

    if((errno = cc_close(re_source))) {
        cc_release(p);
        return NULL;
    }

    return p;
}

