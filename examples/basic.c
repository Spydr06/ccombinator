#include <ccombinator.h>

#include <string.h>
#include <stdlib.h>
#include <locale.h>

int main() {
    setlocale(LC_ALL, "");

    struct cc_parser *alpha = cc_or(2, cc_range('a', 'z'), cc_range('A', 'Z'));
    struct cc_parser *digit = cc_range('0', '9');
    struct cc_parser *underscore = cc_char('_');

    struct cc_parser *p = cc_and(4, 
        cc_fold_concat,
        cc_or(2, cc_retain(alpha), cc_retain(underscore)),
        cc_many(cc_fold_concat, cc_or(3, alpha, digit, underscore)),
        cc_maybe(cc_char('!')),
        cc_eof()
    );

    struct cc_source *s = cc_string_source(u8"uint64_t!");

    struct cc_result r;
    int err = cc_parse(s, p, &r);
    if(err)
        fprintf(stderr, "failed parsing: %s\n", strerror(err));

    if(r.err) {
        cc_err_print(r.err);
        cc_err_free(r.err);
    }

    printf("parse result: %s\n", (char*) r.out);
    free(r.out);

    cc_close(s);
    return 0;
}

