#include "internal.h"

__internal struct cc_lazy_value *lazy_value(struct cc_location loc, void *value) {
    struct cc_lazy_value *lazy = malloc(sizeof(struct cc_lazy_value));
    if(!lazy)
        return NULL;

    lazy->lazy.type = LAZY_VALUE;
    lazy->lazy.location = loc;
    lazy->value = value;

    return lazy;
}

__internal struct cc_lazy_inline *lazy_inline(struct cc_location loc, void *value, size_t size) {
    struct cc_lazy_inline *lazy = malloc(sizeof(struct cc_lazy_inline) + size);
    if(!lazy)
        return NULL;

    lazy->lazy.type = LAZY_INLINE;
    lazy->lazy.location = loc;
    memcpy(lazy->value, value, size);

    return lazy;
}

__internal struct cc_lazy_char *lazy_char(struct cc_location loc, char32_t ch) {
    struct cc_lazy_char *lazy = malloc(sizeof(struct cc_lazy_char));
    if(!lazy)
        return NULL;

    lazy->lazy.type = LAZY_CHAR;
    lazy->lazy.location = loc;
    lazy->ch = ch;

    return lazy;
}

__internal struct cc_lazy_terminal *lazy_terminal(struct cc_location loc, struct cc_parser *p) {
    struct cc_lazy_terminal *lazy = malloc(sizeof(struct cc_lazy_terminal));
    if(!lazy)
        return NULL;

    lazy->lazy.type = LAZY_TERMINAL;
    lazy->lazy.location = loc;
    lazy->p = cc_retain(p);

    return lazy;
}

__internal struct cc_lazy_lift *lazy_lift(struct cc_location loc, cc_lift_t lift) {
    struct cc_lazy_lift *lazy = malloc(sizeof(struct cc_lazy_lift));
    if(!lazy)
        return NULL;

    lazy->lazy.type = LAZY_LIFT;
    lazy->lazy.location = loc;
    lazy->lift = lift;

    return lazy;
}

__internal struct cc_lazy_fold *lazy_fold(struct cc_location loc, cc_fold_t fold, unsigned n, struct cc_lazy *values[]) {
    struct cc_lazy_fold *lazy = malloc(sizeof(struct cc_lazy_fold) + n * sizeof(struct cc_lazy*));
    if(!lazy)
        return NULL;

    assert(fold != NULL);

    lazy->lazy.type = LAZY_FOLD;
    lazy->lazy.location = loc;
    lazy->fold = fold;
    lazy->n = n;

    memcpy(lazy->values, values, n * sizeof(struct cc_lazy*));

    return lazy;
}

__internal struct cc_lazy_apply *lazy_apply(struct cc_location loc, cc_apply_t apply, struct cc_lazy *value) {
    struct cc_lazy_apply *lazy = malloc(sizeof(struct cc_lazy_apply));
    if(!lazy)
        return NULL;

    assert(apply != NULL);

    lazy->lazy.type = LAZY_APPLY;
    lazy->lazy.location = loc;
    lazy->apply = apply;
    lazy->value = value;

    return lazy;
}

static void lazy_free_value(struct cc_lazy_value *lazy) {
    free(lazy);
}

static void lazy_free_inline(struct cc_lazy_inline *lazy) {
    free(lazy);
}

static void lazy_free_char(struct cc_lazy_char *lazy) {
    free(lazy);
}

static void lazy_free_terminal(struct cc_lazy_terminal *lazy) {
    cc_release(lazy->p);
    free(lazy);
}

static void lazy_free_lift(struct cc_lazy_lift *lazy) {
    free(lazy);
}

static int lazy_free_fold(struct cc_lazy_fold *lazy, struct result_stack *stack) {
    for(unsigned i = 0; i < lazy->n; i++) {
        int err = result_push(stack, lazy->values[i]);
        if(err)
            return err;
    }

    free(lazy);
    return 0;
}

static int lazy_free_apply(struct cc_lazy_apply *lazy, struct result_stack *stack) {
    int err = result_push(stack, lazy->value);
    free(lazy);
    return err;
}

__internal int lazy_free(struct cc_lazy *lazy, struct result_stack *stack) {
    if(!lazy)
        return 0;
    if(!stack)
        return EINVAL;

    int err;
    size_t stack_before = stack->count;

    if((err = result_push(stack, lazy)))
        goto cleanup;

    while(stack->count > stack_before) {
        if(!(lazy = result_pop(stack)))
            continue;

        switch(lazy->type) {
        case LAZY_VALUE:
            lazy_free_value(LAZY_DOWNCAST(lazy, struct cc_lazy_value));
            break;
        case LAZY_INLINE:
            lazy_free_inline(LAZY_DOWNCAST(lazy, struct cc_lazy_inline));
            break;
        case LAZY_CHAR:
            lazy_free_char(LAZY_DOWNCAST(lazy, struct cc_lazy_char));
            break;
        case LAZY_TERMINAL:
            lazy_free_terminal(LAZY_DOWNCAST(lazy, struct cc_lazy_terminal));
            break;
        case LAZY_LIFT:
            lazy_free_lift(LAZY_DOWNCAST(lazy, struct cc_lazy_lift));
            break;
        case LAZY_FOLD:
            if((err = lazy_free_fold(LAZY_DOWNCAST(lazy, struct cc_lazy_fold), stack)))
                goto cleanup;
            break;
        case LAZY_APPLY:
            if((err = lazy_free_apply(LAZY_DOWNCAST(lazy, struct cc_lazy_apply), stack)))
                goto cleanup;
            break;
        default:
            fprintf(stderr, "%d\n", lazy->type);
            assert(false && "unknown lazy type");
            unreachable();
        }
    }

cleanup:
    stack->count = stack_before;
    return err;
}

__internal void *lazy_eval(struct cc_lazy *lazy) {
    if(!lazy)
        return NULL;

    return NULL;    
}

