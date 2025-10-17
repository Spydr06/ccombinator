#include <ccombinator.h>

#include <string.h>
#include <stdlib.h>
#include <locale.h>

void *read_int(void *r) {
    if(!r)
        return 0;

    intptr_t n = atol(r); // safe here, since `r` only consists of digits
    free(r);
    return (void*) n;
}

void *calc_negate(void *r) {
    intptr_t i = (intptr_t) r; 
    return (void*) -i;
}

void *calc_prod(size_t n, void **r) {
    intptr_t p = (intptr_t) r[0];

    for(size_t i = 1; i < n; i += 2) {
        if(((char32_t**) r)[i][0] == '/')
            p /= (intptr_t) r[i + 1];
        else
            p *= (intptr_t) r[i + 1];

        free(r[i]);
    }

    return (void*) p;
}

void *calc_sum(size_t n, void **r) {
    intptr_t s = (intptr_t) r[0];

    for(size_t i = 1; i < n; i += 2) {
        if(((char32_t**) r)[i][0] == '-')
            s -= (intptr_t) r[i + 1];
        else
            s += (intptr_t) r[i + 1];

        free(r[i]);
    }

    return (void*) s;
}

struct cc_parser *term_parser(struct cc_parser *self, void*) {
    struct cc_parser *number = cc_apply(cc_least(1, cc_fold_concat, cc_digit()), read_int);
    
    struct cc_parser *negate = cc_apply(cc_and(2, cc_fold_last, 
        cc_noreturn(cc_char('-')),
        cc_retain(number)
    ), calc_negate);

    struct cc_parser *parens = cc_between(cc_char('('), self, cc_char(')'));

    struct cc_parser *unary = cc_or(3, negate, number, parens);

    struct cc_parser *prod = cc_chain(calc_prod, unary, cc_or(2, cc_char('*'), cc_char('/')));
    struct cc_parser *sum = cc_chain(calc_sum, prod, cc_or(2, cc_char('+'), cc_char('-')));

    return sum;
}

int main() {
    setlocale(LC_ALL, "");

    struct cc_source *s = cc_string_source(u8"2+2*(16/4-2)");
    struct cc_parser *term = cc_fix(term_parser, NULL);

    struct cc_result r;
    int err = cc_parse(s, term, &r);
    if(err)
        fprintf(stderr, "failed parsing: %s\n", strerror(err));

    if(r.err) {
        cc_err_print(r.err);
        cc_err_free(r.err);
    }

    intptr_t res = (intptr_t) r.out;
    printf("parse result: %ld\n", res);

    cc_close(s);
    return 0;
}

