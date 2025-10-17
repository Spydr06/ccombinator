/*
    ccombinator.h - A simple parser combinator library for C.

    See https://github.com/Spydr06/ccombinator for the source code and more information.
    The following license applies:

    The MIT License (MIT)

    Copyright (c) 2025 Spydr06

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
    THE SOFTWARE.
*/

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

/*
 * ccombinator types
 */

// regular structs used by ccombinator

typedef struct cc_error cc_error_t;
typedef struct cc_grammar cc_grammar_t;
typedef struct cc_location cc_location_t;
typedef struct cc_parser cc_parser_t;
typedef struct cc_result cc_result_t;
typedef struct cc_source cc_source_t;

// callback function types

typedef void *(*cc_lift_t)(void);
typedef void *(*cc_fold_t)(size_t, void**);
typedef void *(*cc_apply_t)(void*);
typedef void (*cc_dtor_t)(void*);

typedef struct cc_parser *(*cc_fix_t)(struct cc_parser*, void*);

// error handling

// default initial value of a cc_location struct
#define CC_LOCATION_DEFAULT ((struct cc_location){.col=1, .line=1, .byte_off=0})

// represents a location in a `struct cc_source`.
// `col` (column) and `line` are one-indexed. A new line starts after a received '\n' character.
// `byte_off` represents the raw byte offset into the input data.
struct cc_location {
    uint32_t col, line;
    size_t byte_off;
};

// maximum number of `expected` elements saved for the error report
#define CC_ERR_MAX_EXPECTED 16

// represents a parser error that is automatically generated from the provided parser tree
struct cc_error {
    struct cc_location loc;
    const char *filename;

    const char *failure;

    const char *expected[CC_ERR_MAX_EXPECTED];
    size_t num_expected;

    char32_t received;
};

// represents the result of a cc_parse() call.
// on a parsing error, `err` will point to a `cc_error` struct, which must be freed using `cc_err_free`.
// on success, the top-most parser's return value will be put into `out`.
struct cc_result {
    struct cc_error *err;
    void *out;
};

// converts an error report (`struct cc_error`) into a printable error message.
// the resulting string must be freed.
// on error, errno is set to the error code and NULL is returned.
char *cc_err_string(struct cc_error *e);

// prints an error report directly without turning it into a string before.
// returns an errno value on error and `0` on success.
int cc_err_print(struct cc_error *e);
int cc_err_fprint(struct cc_error *e, FILE *f);

// frees an error report struct
void cc_err_free(struct cc_error *e);

/*
 * Parsers
 *
 * a `cc_parser` represents the core data structure of this library.
 * since they are used in large quantities and in often complex relationships,
 * they are reference-counted to aid in memory management.
 *
 * the internal reference-count is intitialized to `1` on parser creation.
 * if it reaches `0`, the parser and its contained data is freed automatically.
 */

// increments the reference-count of the parser `p` and returns it
struct cc_parser *cc_release(struct cc_parser *p);

// decrements the reference-count of the parser `p` and returns it.
// if the reference-count reaches `0`, `p` is automatically freed and NULL returned.
struct cc_parser *cc_retain(struct cc_parser *p);

// copies the contents of parser `s` to `d` while preserving the individual reference-counts.
void cc_parser_copy(struct cc_parser *d, const struct cc_parser* s);

/*
 * Primitive Parsers -> cc_parser.c
 *
 * the following funtions construct primitive and otherwise useful basic parsers.
 * if an internal error occurrs, NULL is retuned and `errno` set to an error code.
 */

// matches any character except end of file (EOF).
struct cc_parser *cc_any(void);

// matches the given utf8-string exactly.
struct cc_parser *cc_string(const char8_t *s);

// matches the given utf32-character exactly.
struct cc_parser *cc_char(char32_t c);

// matches an utf32-character in the range of `lo` to `hi`.
struct cc_parser *cc_range(char32_t lo, char32_t hi);

// matches any utf32-character element in `chars`.
// `chars` must be NULL-terminated. A U"..." string may be used.
struct cc_parser *cc_anyof(const char32_t chars[]);

// similar to `cc_anyof`. matches any character which occurs exactly once in `chars`.
struct cc_parser *cc_oneof(const char32_t chars[]);

// similar to `cc_anyof`. matches any character not present in `chars`.
struct cc_parser *cc_noneof(const char32_t chars[]);

// matches any character, for which the function `f` returns a non-zero value.
struct cc_parser *cc_match(int (*f)(char32_t));

// matches the end of file.
struct cc_parser *cc_eof(void);

// matches the start of file.
struct cc_parser *cc_sof(void);

// matches any whitespace character (' ', '\n', '\t', '\v')
struct cc_parser *cc_whitespace(void);

// matches any blank character
struct cc_parser *cc_blank(void);

// matches the newline ('\n') character. equal to `cc_char('\n')`.
struct cc_parser *cc_newline(void);

// matches the tab ('\t') character. equal to `cc_char('\t')`.
struct cc_parser *cc_tab(void);

// matches a digit character ('0' to '9').
struct cc_parser *cc_digit(void);

// matches a hexadecimal digit character ('0' to '9', 'a' to 'f', 'A' to 'F').
struct cc_parser *cc_hexdigit(void);

// matches an octal digit character ('0' to '7').
struct cc_parser *cc_octdigit(void);

// matches an alphabetical character;
struct cc_parser *cc_alpha(void);

// matches a lower-case character.
struct cc_parser *cc_lower(void);

// matches an upper-case character.
struct cc_parser *cc_upper(void);

// matches an underscore ('_') character.
struct cc_parser *cc_underscore(void);

// matches an alphanumerical character. equal to `cc_or(2, cc_digit(), cc_alpha())`.
struct cc_parser *cc_aplhanum(void);

// always succeeds without consuming input.
struct cc_parser *cc_pass(void);

// fails with the error message `e`.
struct cc_parser *cc_fail(const char *e);

// fails with a formatted error message
CC_format_printf(1)
struct cc_parser *cc_failf(const char *fmt, ...);

// always succeeds and returns the given value
struct cc_parser *cc_lift(cc_lift_t lf);
struct cc_parser *cc_lift_val(void *val);

// always succeeds and returns a copy of the current parser location.
// this copy must be freed manually.
struct cc_parser *cc_location(void);

// looks up and returns a parser by its binding `name`.
// fails if `name` is not a known binding.
struct cc_parser *cc_lookup(const char *name);

// defines a binding `name` to the parser `a`.
struct cc_parser *cc_bind(const char *name, struct cc_parser *a);

/*
 * Parser Combinators
 *
 * the follwing functions construct parser combinators that take in other parsers.
 * all arguments of type `struct cc_parser*` consume a reference,
 * so all parsers passed must have at least a reference count of `1`.
 */

// runs parser `p` and forwards its return value on success.
// if `p` fails, an error `e` is produced.
struct cc_parser *cc_expect(struct cc_parser *p, const char *e);

// runs parser `p` and forwards its return value on success.
// if `p` fails, a formatted error is produced.
CC_format_printf(2)
struct cc_parser *cc_expectf(struct cc_parser *p, const char *fmt, ...);

// applies a function `f` to the return value of `p`, if p succeeds.
struct cc_parser *cc_apply(struct cc_parser *p, cc_apply_t f);

// succeeds, if parser `p` fails and fails, if `p` succeeds.
// no error messages are generated.
struct cc_parser *cc_not(struct cc_parser* p);

// runs `n` parsers in sequence. on success, the results are combined using the folding function `f`.
struct cc_parser *cc_and(unsigned n, cc_fold_t f, ...);
struct cc_parser *cc_andv(unsigned n, cc_fold_t f, struct cc_parser **ps);

// checks `n` parsers and returns the result of the first succeeding parser.
// fails if no parser succeeds.
struct cc_parser *cc_or(unsigned n, ...);
struct cc_parser *cc_orv(unsigned n, struct cc_parser **ps);

// runs parser `p` zero or more times in sequence until `p` fails.
// parser results are combined using the folding function `f`.
struct cc_parser *cc_many(cc_fold_t f, struct cc_parser *p);

// runs parser `p` until parser `end` succeeds.
// parser results are combined using the folding function `f`.
struct cc_parser *cc_many_until(cc_fold_t f, struct cc_parser *a, struct cc_parser *end);

// runs parser `p` exactly `n` times in sequence.
// parser results are combined using the folding function `f`.
struct cc_parser *cc_count(unsigned n, cc_fold_t f, struct cc_parser *p);

// runs parser `p` exactly `n` or more times in sequence until `p` fails.
// parser results are combined using the folding function `f`.
struct cc_parser *cc_least(unsigned n, cc_fold_t f, struct cc_parser *p);

// runs parser `a` and a sequence of parsers `op`, `p` until `op` fails.
// parser results mimic the form [`p` `op` `p` `op` ... `p`] and are combined using the folding function f 
// if any `op` parser succeeded. otherwise, the result of the first `p` parser is returned directly.
struct cc_parser *cc_chain(cc_fold_t f, struct cc_parser *a, struct cc_parser *op);

// runs parser `a` once and a sequence of parser `op`.
// parser results mimic the form [`p` `op` `op` ... `op`] and are combined using the folding function f
// if any `op` parser succeeded. otherwise, the result of the first `p` parser is returned directly.
struct cc_parser *cc_postfix(cc_fold_t f, struct cc_parser *a, struct cc_parser *op);

// attempts to parse any number of whitespace, then a and then any number of whitespace again.
// similar to `cc_and(3, cc_fold_middle, cc_many(cc_whitespace()), a, cc_many(cc_whitespace()))`
struct cc_parser *cc_token(struct cc_parser *a);

// attempts to run parser `p` and always succeeds.
struct cc_parser *cc_maybe(struct cc_parser *p);

// constructs a recursive parser from the fixpoint-function `f`.
// `p` can be used to pass addition arguments to `f`.
struct cc_parser *cc_fix(cc_fix_t f, void *p);

// temporarily disables return value generation for the parser `p`.
// this can reduce the memory footprint of the parser.
// this causes lift, fold, apply and other passed callback functions in `p` and decendants not to be called.
// this flag is set automatically by combinators taking in folding functions when `NULL` is passed.
struct cc_parser *cc_noreturn(struct cc_parser *p);

// temporarily disables error report generation for the parser `p`.
// this can reduce the memory footprint of the parser.
struct cc_parser *cc_noerror(struct cc_parser *p);

/*
 * Folding functions
 *
 * common folding functions included for convenience
 */

// expects all elements of `r` to be utf8-strings.
// contats all elements to a single string while freeing the input strings.
void *cc_fold_concat(size_t n, void **r);

// retuns the first element of `r` while freeing all remaining elements.
void *cc_fold_first(size_t n, void **r);

// returns the middle element of `r` while freeing all remaining elements.
void *cc_fold_middle(size_t n, void **r);

// returns the last element of `r` while freeing all remaining elements.
void *cc_fold_last(size_t n, void **r);

// frees all elements of `r` and returns NULL.
void *cc_fold_null(size_t n, void **r);

// frees `r` and returns NULL. 
void *cc_apply_free(void *r);

/*
 * Regular Expressions
 *
 * the following functions aid constructing a parser by supplying a regular expression.
 */

struct cc_parser *cc_regex_from(const struct cc_source *s, struct cc_error **e);
struct cc_parser *cc_regex(const char8_t *re, struct cc_error **e);

void cc_regex_state_release(void);

/*
 * Backus-Naur Form (BNF)
 *
 * the following functions aid constructing a parser by supplying a grammar
 * in an Extended Backus-Naur form.
 */

struct cc_grammar *cc_bnf_from(const struct cc_source *s, struct cc_error **e);
struct cc_grammar *cc_bnf(const char8_t *s, struct cc_error **e);

void cc_bnf_state_release(void);

/*
 * Grammar
 *
 * a `cc_grammar` represents a list of named `cc_parser`s
 */

// returns a parser with the name `name` in the grammar `g`.
// if no such parser is found, NULL is returned.
struct cc_parser *cc_parser_by_name(const struct cc_grammar *g, const char *name);

// frees a grammar object.
// the reference-counts of all contained parsers is decreased.
void cc_grammar_free(struct cc_grammar *g);

/*
 * Source
 */

// construct a `cc_source` object out of an utf8-string.
struct cc_source *cc_string_source(const char8_t *s);
struct cc_source *cc_nstring_source(const char8_t *s, size_t n);

// construct a `cc_source` from a file read from `filename`.
struct cc_source *cc_open(const char *filename);

// frees a `cc_source` and closes all associated IO objects
int cc_close(struct cc_source *s);

// sets the maximum recursion depth for parsers running on `s`.
// if `max` is `0`, recursion depth testing is disabled.
struct cc_source *cc_max_recursion(struct cc_source *s, unsigned max);

// main parsing function:
//
// attempts to parse the `cc_source` s using the parser `p`.
// if an internal error occurred, a non-zero ERRNO value is returned.
// otherwise, `cc_parse` returns `0` and `r` contains the parsing result as either a value or an error report.
int cc_parse(const struct cc_source *s, struct cc_parser *p, struct cc_result *r);

/*
 * Versioning
 */

// gets the version string of the ccombinator library
const char *cc_version(void);

// gets the major version component in numerical form
int cc_version_major(void);

// gets the minor version component in numerical form
int cc_version_minor(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CCOMBINATOR_H */

