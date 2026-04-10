#include <ccombinator.h>

#include <stdlib.h>
#include <locale.h>
#include <string.h>
#include <errno.h>

int main() {
    setlocale(LC_ALL, "");

    struct cc_error *err;
    struct cc_parser *re = cc_regex(u8"[a-zA-Z_][a-zA-Z0-9_]*!?$", &err);

    if(!re) {
        if(err) {
            cc_err_fprint(err, stderr);
            cc_err_free(err);
        }
        else
            fprintf(stderr, "regex error: %s\n", strerror(errno));

        return EXIT_FAILURE;
    }

    int res = cc_matches(u8"uint64_t!", re, &err);

    if(res == CC_NOMATCH) {
        cc_err_print(err);
        cc_err_free(err);
    }
    else if(res < 0) {
        fprintf(stderr, "parse error: %s\n", strerror(errno));
    }
    else {
        printf("match!\n");
    }

    return EXIT_SUCCESS;
}

