#ifndef CC_INTERNAL_H
#define CC_INTERNAL_H

#include <ccombinator.h>

#include <ctype.h>
#include <errno.h>
#include <memory.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <uchar.h>

#define QUOTE(a) #a

#define CC_VERISON_MAJOR 0
#define CC_VERSION_MINOR 1

#define CC_VERSION_STRING (QUOTE(CC_VERSION_MAJOR) "." QUOTE(CC_VERSION_MINOR))

#ifdef __GNUC__
    #define __internal __attribute__((visibility("hidden")))
#else
    #define __internal
#endif

#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define CC_UTF8_ENCODE_MAX (MB_CUR_MAX + 1)
#define CC_UTF8_ENCODE_PRINTABLE_MAX MAX(CC_UTF8_ENCODE_MAX, 16)

#define CC_UTF8_IS_CONT(x) (((x) & 0xc0) == 0x80)

enum parser_type : uint16_t {
    PARSER_UNDEFINED = 0,
    PARSER_EOF,
    PARSER_SOF,
    PARSER_ANY,
    PARSER_STRING,
    PARSER_CHAR,
    PARSER_CHAR_RANGE,
    PARSER_MATCH,
    PARSER_PASS,
    PARSER_FAIL,
    PARSER_LIFT,
    PARSER_LIFT_VAL,
    PARSER_ANYOF,
    PARSER_NONEOF,
    PARSER_ONEOF,

    PARSER_LOOKUP,
    PARSER_BIND,

    // combinators
    PARSER_EXPECT,
    PARSER_APPLY,
    PARSER_NOT,
    PARSER_AND,
    PARSER_OR,
    PARSER_MANY,
    PARSER_COUNT,
    PARSER_LEAST,
    PARSER_MAYBE,
};

enum parser_flags : uint16_t {
    PARSER_FLAG_FREE_DATA = 0x01,
    PARSER_FLAG_RETAIN_INNER = 0x02
};

struct cc_parser {
    enum parser_type type;
    enum parser_flags flags;

    uint32_t rc;

    cc_fold_t fold;

    union {
        char32_t ch;
        struct { char32_t lo, hi; };

        const char8_t *str;
        const char *msg;

        int (*matchfn)(char32_t);

        const char *lookup;

        struct {
            const char *name;
            struct cc_parser *inner;
        } bind;

        struct {
            cc_apply_t af;
            struct cc_parser *inner;
        } apply;

        struct {
            const char32_t *chars;
            size_t n;
        } list;

        union {
            cc_lift_t lf;
            void *val;
        } lift;

        struct {
            const char *what;
            struct cc_parser *inner;
        } expect;

        struct {
            unsigned n;
            struct cc_parser *inner;
        } unary;

        struct {
            unsigned n;
            struct cc_parser **inner;
        } variadic;
    } match;
};

struct cc_source {
    const char *origin;

    int fd;

    const char8_t *buffer;
    size_t buffer_size;

    int (*buffer_dtor)(const char8_t *buffer);
};

static inline char32_t utf8_first_cp(const uint8_t *s) {
    uint32_t k = s[0] ? __builtin_clz(~(s[0] << 24)) : 0;
    uint32_t mask = (1 << (8 - k)) - 1;
    uint32_t value = *s & mask;

    for(s++, k--; k > 0 && CC_UTF8_IS_CONT(s[0]); k--, s++) {
        value <<= 6;
        value += (*s & 0x3f);
    }

    return (char32_t) value;
} 

// TODO: replace with proper UTF-8 functions

static inline int utf8_is_whitespace(char32_t c) {
    return c <= 0xff && isspace(c);
}

static inline int utf8_is_blank(char32_t c) {
    return c <= 0xff && isblank(c);
}

static inline int utf8_is_print(char32_t c) {
    return c <= 0xff && isprint(c);
}

static inline int utf8_is_cntrl(char32_t c) {
    return c <= 0xff && iscntrl(c);
}

static inline int utf8_is_digit(char32_t c) {
    return c >= U'0' && c <= U'9';
}

static inline int utf8_is_hexdigit(char32_t c) {
    return c <= 0xff && isxdigit(c);
}

static inline int utf8_is_octdigit(char32_t c) {
    return c >= U'0' && c < U'7';
}

static inline int utf8_is_alpha(char32_t c) {
    return c <= 0xff && isalpha(c);
}

static inline int utf8_is_lower(char32_t c) {
    return c <= 0xff && islower(c);
}

static inline int utf8_is_upper(char32_t c) {
    return c <= 0xff && isupper(c);
}

static inline int utf8_is_alphanum(char32_t c) {
    return c <= 0xff && isalnum(c);
}

static inline uint8_t utf8_cp_length(char32_t cp) {
    return 1u
        + (cp >= 0x80)
        + (cp >= 0x800)
        + (cp >= 0x10000);
}

static inline int utf8_encode(char32_t cp, char8_t dst[CC_UTF8_ENCODE_MAX]) {
    mbstate_t ps = {0};
    size_t l = c32rtomb((char*) dst, cp, &ps); 
    if(l == (size_t) -1)
        return errno;

    dst[l] = '\0';
    return 0;
}

static inline int utf8_encode_printable(char32_t cp, char8_t dst[CC_UTF8_ENCODE_PRINTABLE_MAX]) {
    int err = 0;

    switch(cp) {
    case EOF:
        strncpy((char*) dst, "<end of file>", CC_UTF8_ENCODE_PRINTABLE_MAX);
        break;

    case '\t':
        strncpy((char*) dst, "<tab>", CC_UTF8_ENCODE_PRINTABLE_MAX);
        break;

    case '\v':
        strncpy((char*) dst, "<vtab>", CC_UTF8_ENCODE_PRINTABLE_MAX);
        break;

    case '\n':
        strncpy((char*) dst, "<newline>", CC_UTF8_ENCODE_PRINTABLE_MAX);
        break;

    case '\r':
        strncpy((char*) dst, "<cr>", CC_UTF8_ENCODE_PRINTABLE_MAX);
        break;

    default:
        if(utf8_is_print(cp)) {
            char8_t buf[CC_UTF8_ENCODE_MAX];
            if((err = utf8_encode(cp, buf)))
                break;

            if(snprintf((char*) dst, CC_UTF8_ENCODE_PRINTABLE_MAX, "'%s'", (char*) buf) < 0)
                err = errno;
        }
        else if(snprintf((char*) dst, CC_UTF8_ENCODE_PRINTABLE_MAX, "<u+%04x>", (uint32_t) cp) < 0)
            err = errno;
    }

    return err;
}

#define DYNARR_INIT { 0, 0, NULL }
#define DYNARR_INIT_CAP 16

struct dynarr {
    size_t len;
    size_t cap;
    void **elems;
};

__internal int dynarr_append(struct dynarr *da, void *elem);
__internal void dynarr_free(struct dynarr *da);

#define STRING_BUFFER_INIT { 0, 0, NULL }
#define STRING_BUFFER_INIT_CAP 128

struct string_buffer {
    size_t len;
    size_t cap;
    char *buf;
};

__internal int string_buffer_append(struct string_buffer *sb, const char *fmt, ...);

static inline char *string_buffer_unwrap(struct string_buffer *sb) {
    if(!sb)
        return NULL;

    char *s = realloc(sb->buf, (sb->len + 1) * sizeof(char));
    if(!s)
        return sb->buf;

    sb->buf = s;
    sb->cap = sb->len;

    return s;
}

__internal struct cc_parser *parser_allocate(void);
__internal void parser_free(struct cc_parser*);

static inline char *vformat(const char *fmt, va_list ap) {
    va_list ap_copy;
    va_copy(ap_copy, ap);

    int size = vsnprintf(NULL, 0, fmt, ap_copy);

    va_end(ap_copy);
    if(size < 0)
        return NULL;

    char *s = malloc((size + 1) * sizeof(char));
    if(!s)
        return NULL;

    vsnprintf(s, size + 1, fmt, ap);
    return s;
}

CC_format_printf(1)
static inline char *format(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = vformat(fmt, ap);
    va_end(ap);
    return s;
}

struct cc_grammar {
    size_t n;
    struct cc_parser *ps[];
};

#endif /* CC_INTERNAL_H */

