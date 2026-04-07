#include <ccombinator.h>
#include <stdint.h>

#include "internal.h"

#define EMIT(ir, i) do {                \
    if((err = ir_push_u8((ir), (i))))   \
        goto cleanup;                   \
    } while(0)

#define EMIT_PUSH(ir, v) do {           \
    EMIT(ir, IR_PUSH);                  \
    if((err = ir_push_u32((ir), (v))))  \
        goto cleanup;                   \
    } while(0)

#define EMIT_CALL(ir, v) do {           \
    EMIT(ir, IR_CALL);                  \
    if((err = ir_push_ptr((ir), (v))))  \
        goto cleanup;                   \
    } while(0)

#define EMIT_JUMP(ir, to) do {          \
    EMIT(ir, IR_JUMP);                  \
    if((err = ir_push_u32((ir), (to)))) \
        goto cleanup;                   \
    } while(0)

#define EMIT_COND_JUMP(ir, to, cond) do {   \
    EMIT(ir, IR_JUMP_##cond);               \
    if((err = ir_push_u32((ir), (to))))     \
        goto cleanup;                       \
    } while(0)

static int ir_reserve(struct cc_ir **ir, uint32_t capacity) {
    assert(*ir == NULL && "can only reserve on new ir vectors");
    if(!(*ir = malloc(IR_ALLOC_SIZE(IR_INIT_CAPACITY))))
        return errno;

    (*ir)->count = 0;
    (*ir)->capacity = capacity;
    return 0;
}

static int ir_push_u8(struct cc_ir **ir, uint8_t byte) {
    if(*ir == NULL)
        ir_reserve(ir, IR_INIT_CAPACITY);

    if((*ir)->count + 1 > (*ir)->capacity) {
        struct cc_ir *new = realloc((*ir), IR_ALLOC_SIZE((*ir)->capacity * 2));
        if(!new)
            return errno;

        *ir = new;
        (*ir)->capacity *= 2;
    }

    (*ir)->bytes[(*ir)->count++] = byte;
    return 0;
}

static int ir_push_u32(struct cc_ir **ir, uint32_t dword) {
    int err = 0;
    for(int i = sizeof(uint32_t) - 1; i >= 0 && !err; i--) {
        err = ir_push_u8(ir, (dword >> (i * 8)) & 0xff);
    }
    return err;
}

static int ir_push_ptr(struct cc_ir **ir, uintptr_t ptr) {
    int err = 0;
    for(int i = sizeof(uintptr_t) - 1; i >= 0 && !err; i--) {
        err = ir_push_u8(ir, (ptr >> (i * 8)) & 0xff);
    }
    return err;
}

static void apply_patches(struct cc_ir *ir, const uint32_t *patches, unsigned count, uint32_t value) {
    for(unsigned i = 0; i < count; i++)
        ir_write_u32(ir, patches[i], value);
}

static int generate_try(struct cc_ir **ir, struct cc_parser *inner, uint32_t *patch, bool noerror) {
    int err;

    if(noerror) {
        EMIT_PUSH(ir, 1u);
        EMIT(ir, IR_SET_NOERROR);
    }

    EMIT(ir, IR_SAVE_LOCATION);
    EMIT_CALL(ir, (uintptr_t) inner);

    if(noerror) {
        EMIT(ir, IR_SWAP);
        EMIT(ir, IR_SET_NOERROR);
        EMIT(ir, IR_POP);
    }

    patch[0] = (*ir)->count + 1;
    EMIT_COND_JUMP(ir, UINT32_MAX, IF_SUCCESS); // jump lsuccess
    EMIT(ir, IR_RESTORE_LOCATION);

    // lsuccess:
cleanup:
    return err;
}

static int generate_many_iter(struct cc_ir **ir, struct cc_parser *inner, uint32_t extra_iter) {
    int err;

    EMIT_PUSH(ir, 1u);
    EMIT(ir, IR_SET_NOERROR);

    EMIT_PUSH(ir, extra_iter); // iteration counter

    uint32_t lrepeat = (*ir)->count;
    EMIT(ir, IR_SAVE_LOCATION);
    EMIT(ir, IR_INCREMENT);
    EMIT_CALL(ir, (uintptr_t) inner);
    EMIT_COND_JUMP(ir, lrepeat, IF_SUCCESS);

    EMIT(ir, IR_RESTORE_LOCATION);
    EMIT(ir, IR_DECREMENT);

    EMIT(ir, IR_FOLD);

    EMIT(ir, IR_SET_NOERROR);
    EMIT(ir, IR_POP);

cleanup:
    return err;
}

static int generate_many(struct cc_ir **ir, cc_fold_t f, struct cc_parser *inner) {
    int err;

    if(!f) {
        EMIT_PUSH(ir, 1u);
        EMIT(ir, IR_SET_NORETURN);
    }

    if((err = generate_many_iter(ir, inner, 0u)))
        goto cleanup;

    if(!f) {
        EMIT(ir, IR_SET_NORETURN);
        EMIT(ir, IR_POP);
        EMIT(ir, IR_NULL_RESULT);
    }

    EMIT_PUSH(ir, PARSE_SUCCESS);
cleanup:
    return err;
}

static int generate_many_until(struct cc_ir **ir, cc_fold_t f, struct cc_parser *inner, struct cc_parser *end) {
    int err;

    if(!f) {
        EMIT_PUSH(ir, 1u);
        EMIT(ir, IR_SET_NORETURN);
    }

    EMIT_PUSH(ir, 1u);

    uint32_t lrepeat = (*ir)->count;

    uint32_t lsuccess_patch[2];
    if((err = generate_try(ir, end, &lsuccess_patch[0], true)))
        goto cleanup;

    EMIT(ir, IR_INCREMENT);
    EMIT_CALL(ir, (uintptr_t) inner);
    EMIT_COND_JUMP(ir, lrepeat, IF_SUCCESS); // jump lrepeat

    // try the end parser again, but generate errors this time
    if((err = generate_try(ir, end, &lsuccess_patch[1], false)))
        goto cleanup;

    EMIT_PUSH(ir, PARSE_FAILURE);
    uint32_t lrestore_patch = (*ir)->count + 1;
    EMIT_JUMP(ir, UINT32_MAX); // jump lrestore
    
    EMIT_JUMP(ir, lrepeat);

    uint32_t lsuccess = (*ir)->count;
    apply_patches(*ir, lsuccess_patch, 2, lsuccess);

    EMIT(ir, IR_FOLD);
    EMIT_PUSH(ir, PARSE_SUCCESS);

    uint32_t lrestore = (*ir)->count;
    apply_patches(*ir, &lrestore_patch, 1, lrestore);

    if(!f) {
        EMIT(ir, IR_SWAP);
        EMIT(ir, IR_SET_NORETURN);
        EMIT(ir, IR_POP);
        EMIT(ir, IR_NULL_RESULT);
    }
cleanup:
    return err;
}

static int generate_count_linear(struct cc_ir **ir, struct cc_parser *inner, unsigned n, uint32_t *patch) {
    int err;

    for(unsigned i = 0; i < n; i++) {
        EMIT_CALL(ir, (uintptr_t) inner);
        EMIT(ir, IR_DUP);

        patch[i] = (*ir)->count + 1;
        EMIT_COND_JUMP(ir, UINT32_MAX, IF_FAILURE);
        EMIT(ir, IR_POP);
    }

cleanup:
    return err;
}

static int generate_count_iter(struct cc_ir **ir, struct cc_parser *inner, unsigned n, uint32_t *patch) {
    int err;

    EMIT_PUSH(ir, n); // iteration counter

    uint32_t lrepeat = (*ir)->count;
    EMIT(ir, IR_DECREMENT);

    EMIT_CALL(ir, (uintptr_t) inner);
    EMIT(ir, IR_DUP);

    patch[0] = (*ir)->count + 1;
    EMIT_COND_JUMP(ir, UINT32_MAX, IF_FAILURE);
    EMIT(ir, IR_POP);

    EMIT(ir, IR_DUP);
    EMIT_COND_JUMP(ir, lrepeat, IF_NONZERO);
    EMIT(ir, IR_POP);

cleanup:
    return err;
}

static int generate_count(struct cc_ir **ir, cc_fold_t f, struct cc_parser *inner, unsigned n) {
    int err;

    if(n == 0) {
        EMIT(ir, IR_NULL_RESULT);
        EMIT_PUSH(ir, PARSE_SUCCESS);
        goto cleanup;
    }

    if(!f) {
        EMIT_PUSH(ir, 1u);
        EMIT(ir, IR_SET_NORETURN);
    }

    uint32_t patch[IR_UNROLL_THRESHOLD];
    unsigned patch_count = 0;

    if(n < IR_UNROLL_THRESHOLD) {
        patch_count = n;
        if((err = generate_count_linear(ir, inner, n, patch)))
            goto cleanup;
    }
    else {
        patch_count = 1;
        if((err = generate_count_iter(ir, inner, n, patch)))
            goto cleanup;
    }

    EMIT_PUSH(ir, n);
    EMIT(ir, IR_FOLD);
    EMIT_PUSH(ir, PARSE_SUCCESS);

    uint32_t lrestore = (*ir)->count;
    apply_patches(*ir, patch, patch_count, lrestore);

    if(!f) {
        EMIT(ir, IR_SWAP);
        EMIT(ir, IR_SET_NORETURN);
        EMIT(ir, IR_POP);
        EMIT(ir, IR_NULL_RESULT);
    }
cleanup:
    return err;
}

static int generate_least(struct cc_ir **ir, cc_fold_t f, struct cc_parser *inner, unsigned n) {
    int err; 

    if(!f) {
        EMIT_PUSH(ir, 1u);
        EMIT(ir, IR_SET_NORETURN);
    }

    uint32_t patch[IR_UNROLL_THRESHOLD];
    unsigned patch_count = 0;

    if(n > 0) {
        if(n < IR_UNROLL_THRESHOLD) {
            patch_count = n;
            if((err = generate_count_linear(ir, inner, n, patch)))
                goto cleanup;
        }
        else {
            patch_count = 1;
            if((err = generate_count_iter(ir, inner, n, patch)))
                goto cleanup;
        }
    }

    if((err = generate_many_iter(ir, inner, n)))
        goto cleanup;

    EMIT_PUSH(ir, PARSE_SUCCESS);

    uint32_t lrestore = (*ir)->count;
    apply_patches(*ir, patch, patch_count, lrestore);

    if(!f) {
        EMIT(ir, IR_SWAP);
        EMIT(ir, IR_SET_NORETURN);
        EMIT(ir, IR_POP);
        EMIT(ir, IR_NULL_RESULT);
    }
cleanup:
    return err;
}

static int generate_maybe(struct cc_ir **ir, struct cc_parser *inner) {
    uint32_t patch;
    int err = generate_try(ir, inner, &patch, true);

    EMIT(ir, IR_NULL_RESULT);

    uint32_t lsuccess = (*ir)->count;
    apply_patches(*ir, &patch, 1, lsuccess);

    EMIT_PUSH(ir, PARSE_SUCCESS);
cleanup:
    return err;
}

static int generate_chain(struct cc_ir **ir, cc_fold_t f, struct cc_parser *lr, struct cc_parser *op) {
    int err = 0;
    uint32_t lrestore_patch[2];

    if(!f) {
        EMIT_PUSH(ir, 1u);
        EMIT(ir, IR_SET_NORETURN);
    }

    EMIT_CALL(ir, (uintptr_t) lr);
    EMIT(ir, IR_DUP);
    lrestore_patch[0] = (*ir)->count + 1;
    EMIT_COND_JUMP(ir, UINT32_MAX, IF_FAILURE);
    EMIT(ir, IR_POP);

    EMIT_PUSH(ir, 1u); // iteration counter

    uint32_t lrepeat = (*ir)->count;

    EMIT(ir, IR_SAVE_LOCATION);
    EMIT_PUSH(ir, 1u);
    EMIT(ir, IR_SET_NOERROR);

    EMIT_CALL(ir, (uintptr_t) op);
    EMIT(ir, IR_SWAP);
    EMIT(ir, IR_SET_NOERROR);
    EMIT(ir, IR_POP);

    uint32_t lbreak_patch = (*ir)->count + 1;
    EMIT_COND_JUMP(ir, UINT32_MAX, IF_FAILURE); // jump lbreak
    
    EMIT_CALL(ir, (uintptr_t) lr);
    EMIT(ir, IR_DUP);
    lrestore_patch[1] = (*ir)->count + 1;
    EMIT_COND_JUMP(ir, UINT32_MAX, IF_FAILURE);

    // opreand parser successful
    EMIT(ir, IR_POP);

    // increment iteration counter for operator and operand
    EMIT(ir, IR_INCREMENT);
    EMIT(ir, IR_INCREMENT);
    EMIT_JUMP(ir, lrepeat);

    uint32_t lbreak = (*ir)->count;
    apply_patches(*ir, &lbreak_patch, 1, lbreak);

    // operator parser failed (chain is complete)
    EMIT(ir, IR_RESTORE_LOCATION);

    EMIT(ir, IR_FOLD);
    EMIT_PUSH(ir, PARSE_SUCCESS);

    uint32_t lrestore = (*ir)->count;
    apply_patches(*ir, lrestore_patch, 2, lrestore);

    if(!f) {
        // lrestore:
        EMIT(ir, IR_SWAP);
        EMIT(ir, IR_SET_NORETURN);
        EMIT(ir, IR_POP);
        EMIT(ir, IR_NULL_RESULT);
    }
cleanup:
    return err;
}

static int generate_postfix(struct cc_ir **ir, cc_fold_t f, struct cc_parser *lhs, struct cc_parser *rhs) {
    int err = 0;

    if(!f) {
        EMIT_PUSH(ir, 1u);
        EMIT(ir, IR_SET_NORETURN);
    }

    EMIT_CALL(ir, (uintptr_t) lhs);
    EMIT(ir, IR_DUP);
    uint32_t patch = (*ir)->count + 1;
    EMIT_COND_JUMP(ir, UINT32_MAX, IF_FAILURE);
    EMIT(ir, IR_POP);

    if((err = generate_many_iter(ir, rhs, 1u)))
        goto cleanup;

    EMIT_PUSH(ir, PARSE_SUCCESS);

    uint32_t lrestore = (*ir)->count;
    apply_patches(*ir, &patch, 1, lrestore);

    if(!f) {
        // lrestore:
        EMIT(ir, IR_SWAP);
        EMIT(ir, IR_SET_NORETURN);
        EMIT(ir, IR_POP);
        EMIT(ir, IR_NULL_RESULT);
    }

cleanup:
    return err;
}

static int generate_sequenced(struct cc_ir **ir, cc_fold_t f, struct cc_parser *const *inner, unsigned n) {
    int err = 0;
    uint32_t patch[n];

    if(!f) {
        EMIT_PUSH(ir, 1u);
        EMIT(ir, IR_SET_NORETURN);
    }

    for(unsigned i = 0; i < n; i++) {
        EMIT_CALL(ir, (uintptr_t) inner[i]);
        EMIT(ir, IR_DUP);

        patch[i] = (*ir)->count + 1;
        EMIT_COND_JUMP(ir, UINT32_MAX, IF_FAILURE);
        EMIT(ir, IR_POP);
    }

    EMIT_PUSH(ir, n);
    EMIT(ir, IR_FOLD);
    EMIT_PUSH(ir, PARSE_SUCCESS);

    uint32_t lrestore = (*ir)->count;
    apply_patches(*ir, patch, n, lrestore);

    if(!f) {
    // lrestore:
        EMIT(ir, IR_SWAP);
        EMIT(ir, IR_SET_NORETURN);
        EMIT(ir, IR_POP);
        EMIT(ir, IR_NULL_RESULT);
    }

cleanup:
    return err;
}

static int generate_variants(struct cc_ir **ir, struct cc_parser *const *inner, unsigned n) {
    int err = 0;
    uint32_t patch[n];

    for(unsigned i = 0; i < n; i++) {
        EMIT(ir, i ? IR_RESTORE_LOCATION : IR_SAVE_LOCATION);
        EMIT_CALL(ir, (uintptr_t) inner[i]);
        EMIT(ir, IR_DUP);

        patch[i] = (*ir)->count + 1;
        EMIT_COND_JUMP(ir, UINT32_MAX, IF_SUCCESS);
        EMIT(ir, IR_POP);
    }

    EMIT_PUSH(ir, PARSE_FAILURE);

    uint32_t lbreak = (*ir)->count;
    apply_patches(*ir, patch, n, lbreak);
cleanup:
    return err;
}

static int generate_not(struct cc_ir **ir, struct cc_parser *inner) {
    int err;

    EMIT(ir, IR_SAVE_LOCATION);

    EMIT_PUSH(ir, 1u);
    EMIT(ir, IR_SET_NORETURN);
    EMIT_PUSH(ir, 1u);
    EMIT(ir, IR_SET_NOERROR);

    EMIT_CALL(ir, (uintptr_t) inner);

    EMIT(ir, IR_SWAP);
    EMIT(ir, IR_SET_NOERROR);
    EMIT(ir, IR_POP);
    EMIT(ir, IR_SWAP);
    EMIT(ir, IR_SET_NORETURN);
    EMIT(ir, IR_POP);

    EMIT(ir, IR_NULL_RESULT);
    EMIT(ir, IR_NEGATE);
    EMIT(ir, IR_DUP);
    
    uint32_t patch = (*ir)->count + 1;
    EMIT_COND_JUMP(ir, UINT32_MAX, IF_SUCCESS);
    EMIT(ir, IR_RESTORE_LOCATION); 

    uint32_t lend = (*ir)->count;
    apply_patches(*ir, &patch, 1, lend);
cleanup:
    return err;
}

__internal int cc_compile(struct cc_parser *p) {
    if(!p)
        return EINVAL;
    if(p->ir || !is_combinator(p->type))
        return 0; // already compiled

    int err = 0;
    switch(p->type) {
    case PARSER_MANY:
        err = generate_many(&p->ir, p->fold, p->match.unary.inner);
        break;

    case PARSER_MANY_UNTIL:
        err = generate_many_until(&p->ir, p->fold, p->match.binary.lhs, p->match.binary.rhs);
        break;

    case PARSER_COUNT:
        err = generate_count(&p->ir, p->fold, p->match.unary.inner, p->match.unary.n);
        break;

    case PARSER_LEAST:
        err = generate_least(&p->ir, p->fold, p->match.unary.inner, p->match.unary.n);
        break;

    case PARSER_MAYBE:
        err = generate_maybe(&p->ir, p->match.unary.inner);
        break;

    case PARSER_CHAIN:
        err = generate_chain(&p->ir, p->fold, p->match.binary.lhs, p->match.binary.rhs);
        break;

    case PARSER_POSTFIX:
        err = generate_postfix(&p->ir, p->fold, p->match.binary.lhs, p->match.binary.rhs);
        break;

    case PARSER_SEQ:
        err = generate_sequenced(&p->ir, p->fold, (struct cc_parser* const[]){
            p->match.binary.lhs,
            p->match.binary.rhs
        }, 2);
        break;

    case PARSER_AND:
        err = generate_sequenced(&p->ir, p->fold, p->match.variadic.inner, p->match.variadic.n);
        break;

    case PARSER_EITHER:
        err = generate_variants(&p->ir, (struct cc_parser* const[]){
            p->match.binary.lhs,
            p->match.binary.rhs
        }, 2);
        break;

    case PARSER_OR:
        err = generate_variants(&p->ir, p->match.variadic.inner, p->match.variadic.n);
        break;

    case PARSER_NOT:
        err = generate_not(&p->ir, p->match.unary.inner);
        break;

    case PARSER_EXPECT: {
        EMIT_CALL(&p->ir, (uintptr_t) p->match.expect.inner);
        EMIT(&p->ir, IR_DUP);

        uint32_t patch = p->ir->count + 1;
        EMIT_COND_JUMP(&p->ir, UINT32_MAX, IF_SUCCESS);
        EMIT(&p->ir, IR_EXPECT);

        uint32_t lend = p->ir->count;
        apply_patches(p->ir, &patch, 1, lend);
    } break;

    case PARSER_APPLY: {
        EMIT_CALL(&p->ir, (uintptr_t) p->match.apply.inner);
        EMIT(&p->ir, IR_DUP);

        uint32_t lend = p->ir->count + 2 + sizeof(uint32_t);
        EMIT_COND_JUMP(&p->ir, lend, IF_FAILURE);
        EMIT(&p->ir, IR_APPLY);
    } break;

    case PARSER_NORETURN:
        EMIT_PUSH(&p->ir, 1u);
        EMIT(&p->ir, IR_SET_NORETURN);
        EMIT_CALL(&p->ir, (uintptr_t) p->match.bind.inner);
        EMIT(&p->ir, IR_SWAP);
        EMIT(&p->ir, IR_SET_NORETURN);
        EMIT(&p->ir, IR_POP);
        EMIT(&p->ir, IR_NULL_RESULT);
        break;

    case PARSER_NOERROR:
        EMIT_PUSH(&p->ir, 1u);
        EMIT(&p->ir, IR_SET_NOERROR); 
        EMIT_CALL(&p->ir, (uintptr_t) p->match.bind.inner);
        EMIT(&p->ir, IR_SWAP);
        EMIT(&p->ir, IR_SET_NOERROR);
        EMIT(&p->ir, IR_POP);
        break;

    case PARSER_BIND:
        EMIT(&p->ir, IR_PUSH_BINDING);
        EMIT_CALL(&p->ir, (uintptr_t) p->match.bind.inner);
        EMIT(&p->ir, IR_POP_BINDING);
        break;

    default:
        assert(false && "unknown parser combinator type");
        unreachable();
    }

    return 0;
cleanup:
    free(p->ir);
    p->ir = NULL;
    return err;
}

