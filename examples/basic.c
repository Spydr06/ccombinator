#include <ccombinator.h>

#include <string.h>

int main() {
    struct cc_parser *p = cc_count(NULL, 4, cc_string(u8"hello"));

    struct cc_source *s = cc_string_source(u8"hellohellohello");

    struct cc_result r;
    int err = cc_parse(s, p, &r);
    if(err)
        fprintf(stderr, "failed parsing: %s\n", strerror(err));

    if(r.err) {
        cc_err_print(r.err);
        cc_err_free(r.err);
    }

    cc_close(s);
    return 0;
}

