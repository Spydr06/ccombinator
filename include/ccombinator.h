#ifndef CCOMBINATOR_H
#define CCOMBINATOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <uchar.h>
#include <stdio.h>
#include <stdint.h>

#ifdef __GNUC__
    #define CC_format_printf(i) __attribute__((format(printf, (i), (i)+1)))
#else
    #define CC_format_printf(i)
#endif

// base types

struct cc_parser;
struct cc_source;

typedef void *(*cc_lift_t)(void);

typedef void *(*cc_fold_t)(size_t, void**);

typedef struct cc_parser cc_parser_t;
typedef struct cc_source cc_source_t;
typedef struct cc_location cc_location_t;
typedef struct cc_error cc_error_t;
typedef struct cc_result cc_result_t;

// error handling

#define CC_LOCATION_DEFAULT ((struct cc_location){.col=1, .line=1, .byte_off=0})

struct cc_location {
    uint32_t col, line;
    size_t byte_off;
};

#define CC_ERR_FREE_SELF            0x01
#define CC_ERR_FREE_FAILURE         0x02
#define CC_ERR_FREE_EXPECTED_ELEMS  0x04
#define CC_ERR_MAX_EXPECTED 16

struct cc_error {
    struct cc_location loc;
    const char *filename;

    const char *failure;

    const char *expected[CC_ERR_MAX_EXPECTED];
    size_t num_expected;

    char32_t received;

    int flags;
};

struct cc_result {
    struct cc_error *err;
    void *out;
};

char *cc_err_string(struct cc_error *e);

int cc_err_print(struct cc_error *e);
int cc_err_fprint(struct cc_error *e, FILE *f);

void cc_err_free(struct cc_error *e);

// parsers

struct cc_parser *cc_any(void);

struct cc_parser *cc_string(const char8_t *s);
struct cc_parser *cc_char(char32_t c);

struct cc_parser *cc_range(char32_t lo, char32_t hi);

struct cc_parser *cc_anyof(const char32_t chars[]);
struct cc_parser *cc_oneof(const char32_t chars[]);
struct cc_parser *cc_noneof(const char32_t chars[]);
struct cc_parser *cc_match(int (*f)(char32_t));

struct cc_parser *cc_eof(void);
struct cc_parser *cc_any(void);

struct cc_parser *cc_whitespace(void);
struct cc_parser *cc_blank(void);

struct cc_parser *cc_newline(void);
struct cc_parser *cc_tab(void);

struct cc_parser *cc_digit(void);
struct cc_parser *cc_hexdigit(void);
struct cc_parser *cc_octdigit(void);

struct cc_parser *cc_alpha(void);
struct cc_parser *cc_lower(void);
struct cc_parser *cc_upper(void);
struct cc_parser *cc_underscore(void);
struct cc_parser *cc_aplhanum(void);

struct cc_parser *cc_pass(void);
struct cc_parser *cc_fail(const char *e);

CC_format_printf(1)
struct cc_parser *cc_failf(const char *fmt, ...);

struct cc_parser *cc_lift(cc_lift_t lf);
struct cc_parser *cc_lift_val(void *val);

// combinators

struct cc_parser *cc_expect(struct cc_parser *p, const char *e);

CC_format_printf(2)
struct cc_parser *cc_expectf(struct cc_parser *p, const char *fmt, ...);

struct cc_parser *cc_not(struct cc_parser* p);

struct cc_parser *cc_and(cc_fold_t f, unsigned n, ...);
struct cc_parser *cc_or(unsigned n, ...);

struct cc_parser *cc_many(cc_fold_t f, struct cc_parser *p);
struct cc_parser *cc_count(cc_fold_t f, unsigned n, struct cc_parser *p);
struct cc_parser *cc_maybe(struct cc_parser *p);

// source

struct cc_source *cc_string_source(const char8_t *s);
struct cc_source *cc_nstring_source(const char8_t *s, size_t n);

struct cc_source *cc_open(const char *filename);
int cc_close(struct cc_source *s);

int cc_parse(const struct cc_source *s, const struct cc_parser *p, struct cc_result *r);

// versioning

const char *cc_version(void);
int cc_version_major(void);
int cc_version_minor(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CCOMBINATOR_H */

