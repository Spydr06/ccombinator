#include <ccombinator.h>

#include <assert.h>
#include <stdlib.h>
#include <locale.h>
#include <string.h>
#include <errno.h>

#define _(x) x

static struct cc_result fold_assign(size_t n, void **vs) {
    assert(n == 5);
    printf("assing '%s' <- '%s'\n", (char*) vs[0], (char*) vs[4]);

    free(vs[0]);
    free(vs[2]);
    free(vs[4]);
    return cc_ok(NULL);
}

int main() {
    setlocale(LC_ALL, "");

    struct cc_action *actions = CC_ACTIONS(
        cc_action_match("isspace", cc_is_whitespace),
        cc_action_match("isalpha", cc_is_alpha),
        cc_action_match("isdigit", cc_is_digit),
        cc_action_fold("concat", cc_fold_concat),
        cc_action_fold("assign", fold_assign),
        cc_action_fold("null", cc_fold_null) // used to force result evaluation, since @assign has a side-effect
    );

    struct cc_error *err;
    struct cc_grammar *g = cc_bnf(_(u8"                                     \
program = @null: S, 'program', S, ident, S, 'begin',                        \n\
                    @null{ @null: S, assign, S, ';' }, S, 'end', '.', S;    \n\
assign  = @assign:  ident, S, ':=', S, number;                              \n\
ident   = @concat:  @isalpha, @concat { @isalpha | @isdigit | '_' };        \n\
number  = @concat:  [ '-' ], @isdigit, { @isdigit };                        \n\
                                                                            \n\
(* whitespace: *)                                                           \n\
S = { @isspace };                                                           \n\
"), actions, &err);

    if(!g) {
        fprintf(stderr, "bnf error: ");
        if(err) {
            cc_err_fprint(err, stderr);
            cc_err_free(err);
        }
        else
            fprintf(stderr, "%s\n", strerror(errno));

        return EXIT_FAILURE;
    }

    struct cc_parser *p = cc_rule(g, "program");
    struct cc_source *s = cc_string_source(_(u8"\
program Test begin  \n\
    foo := 69;      \n\
end.                \n\
"));

    struct cc_result r;
    cc_parse(s, p, &r);

    if(r.err) {
        cc_err_fprint(r.err, stderr);
        cc_err_free(r.err);

        return EXIT_FAILURE;
    }
    else {
        printf("result: %p\n", r.out);
        free(r.out);
    }

    cc_close(s);
    cc_grammar_free(g);

    return EXIT_SUCCESS;
}

