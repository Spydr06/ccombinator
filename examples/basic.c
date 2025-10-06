#include <ccombinator.h>

#include <string.h>
#include <stdlib.h>

static void *fold_ident(size_t n, void** p) {
    size_t total = 0;

    for(size_t i = 0; i < n; i++)
        total += strlen(p[i]);

    char8_t *s = malloc((total + 1) * sizeof(char8_t));
    size_t off = 0;

    for(size_t i = 0; i < n; i++) {
        size_t l = strlen(p[i]);
        memcpy(s + off, p[i], l);
        off += l;

        free(p[i]);
    }

    s[off] = '\0';
    return s;
}

int main() {
    struct cc_parser *alpha = cc_or(2, cc_range('a', 'z'), cc_range('A', 'Z'));
    struct cc_parser *digit = cc_range('0', '9');
    struct cc_parser *underscore = cc_char('_');

    struct cc_parser *p = cc_and(2, 
        fold_ident,
        cc_or(2, cc_retain(alpha), cc_retain(underscore)),
        cc_many(fold_ident, cc_or(3, alpha, digit, underscore))
    );

    struct cc_source *s = cc_string_source(u8"uint64_t");

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

