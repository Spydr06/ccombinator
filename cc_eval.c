#include <ccombinator.h>

#include "internal.h"

#include <alloca.h>
#include <assert.h>
#include <errno.h>
#include <locale.h>
#include <memory.h>
#include <stdint.h>
#include <stdio.h>

#define CC_STATE_FLAGS_DEFAULT  0x00
#define CC_STATE_FLAG_EOF       0x01
#define CC_STATE_FLAG_NOERR     0x02
#define CC_STATE_FLAG_NORETURN  0x04

struct cc_state {
    int flags;
    const struct cc_source *src;
    struct cc_location loc;

    struct dynarr scope;

    unsigned recursion_depth;
    unsigned max_recursion_depth;
};

struct cc_save {
    int flags;
    struct cc_location loc;

    size_t scope_len;
    unsigned recursion_depth;
};

#define PROP(s, x) do {                             \
/*        assert((s)->recursion_depth > 0);           \
        (s)->recursion_depth--;            */         \
        return (x);                                 \
    } while(0)

#define FAIL_WITH(e, s, x, c) do {                  \
        int err = new_error((e), (s), (x), (c));    \
        PROP(s, err ? -err : PARSE_FAILURE);        \
    } while(0)

#define SUCCESS(r, s, x) do {                       \
        if(!((s)->flags & CC_STATE_FLAG_NORETURN))  \
            (r) = (x);                              \
        PROP(s, PARSE_SUCCESS);                     \
    } while(0)

static inline bool set_flag(struct cc_state *s, int flag, bool state) {
    bool before = !!(s->flags & flag);

    if(state)
        s->flags |= flag;
    else
        s->flags &= ~flag;

    return before;
}

static inline char32_t peek_at(struct cc_state *s) {
    if(s->loc.byte_off >= s->src->buffer_size) {
        s->flags |= CC_STATE_FLAG_EOF;
        return EOF;
    }

    return utf8_first_cp(s->src->buffer + s->loc.byte_off);
}

static inline void state_free(struct cc_state *s) {
    free(s->scope.elems);
}

static inline int scope_push(struct cc_state *s, struct cc_parser *p) {
    assert(p->type == PARSER_BIND);
    return dynarr_append(&s->scope, (void*) p);
}

static inline struct cc_parser *scope_pop(struct cc_state *s) {
    assert(s->scope.len > 0);

    return s->scope.elems[--s->scope.len];
}

static struct cc_parser *scope_lookup(struct cc_state *s, const char *name) {
    // TODO: maybe use some sort of hashing to avoid strcmp?

    for(size_t i = s->scope.len; i > 0; i--) {
        const struct cc_parser *def = s->scope.elems[i - 1];

        if(strcmp(def->match.bind.name, name) == 0)
            return def->match.bind.inner;
    }

    return NULL;
}

static int new_error(struct cc_error *e, struct cc_state *s, const char *msg, bool copy) {
    if(s->flags & CC_STATE_FLAG_NOERR)
        return 0;

    if(!e)
        return EINVAL;

    memset(e, 0, sizeof(struct cc_error));

    e->loc = s->loc;
    e->received = peek_at(s);
    if(copy && !(e->failure = strdup(msg)))
        return errno;
    else if(!copy)
        e->failure = msg;

    return 0;
}

static int add_expected(struct cc_error *e, struct cc_state *s, const char *expected) {
    if(s->flags & CC_STATE_FLAG_NOERR || e->num_expected >= CC_ERR_MAX_EXPECTED)
        return 0; // cannot add anymore, don't error tho

    if(e->num_expected == 0) {
        e->filename = s->src->origin;
        e->loc = s->loc;
        e->received = peek_at(s);
    }

    char *expected_copy = strdup(expected);
    if(!expected_copy)
        return errno;

    e->expected[e->num_expected++] = expected_copy;

    return 0;
}

static inline char32_t advance_char(struct cc_state *s, char32_t ch) {
    s->loc.byte_off += utf8_cp_length(ch);

    if(ch == '\n') {
        s->loc.line++;
        s->loc.col = 1;
    }
    else {
        s->loc.col++;
    }

    return ch;
}

static int allocate_string(char8_t **s, size_t n) {
    if(!(*s = calloc(n + 1, sizeof(char8_t))))
        return -errno;

    return 0;
}

static int char_result(struct cc_state *s, char8_t **r, char32_t ch) {
    if(s->flags & CC_STATE_FLAG_NORETURN)
        return 0;

    int err = allocate_string(r, utf8_cp_length(ch));
    if(err)
        return err;

    utf8_encode(ch, *r);
    return 0;
}

static int string_result(struct cc_state *s, char8_t **r, const char8_t *str) {
    if(s->flags & CC_STATE_FLAG_NORETURN)
        return 0;

    size_t len = strlen((const char*) str);
    int err = allocate_string(r, len);
    if(err)
        return err;

    memcpy(*r, str, len * sizeof(char));
    return 0;
}

static int match_char(struct cc_state *s, char32_t ch, void **r) {
    char32_t next = peek_at(s);
    if((s->flags & CC_STATE_FLAG_EOF) || next != ch)
        return PARSE_FAILURE;

    advance_char(s, ch);

    int err;
    if(r != NULL && (err = char_result(s, (char8_t**) r, ch)))
        return err;

    return PARSE_SUCCESS;
}

static int match_char_func(struct cc_state *s, int(*f)(char32_t), void **r) {
    char32_t next = peek_at(s);
    if((s->flags & CC_STATE_FLAG_EOF) || !f(next))
        return PARSE_FAILURE;

    advance_char(s, next);

    int err;
    if(r != NULL && (err = char_result(s, (char8_t**) r, next)))
        return err;
    
    return PARSE_SUCCESS;
}

static int match_eof(struct cc_state *s, void **r) {
    *r = NULL;
    peek_at(s);
    return (s->flags & CC_STATE_FLAG_EOF) ? PARSE_SUCCESS : PARSE_FAILURE;
}

static int match_sof(struct cc_state *s, void **r) {
    *r = NULL;
    return s->loc.byte_off == 0 ? PARSE_SUCCESS : PARSE_FAILURE;
}

static int match_any(struct cc_state *s, void **r) {
    char32_t next = peek_at(s);
    if(s->flags & CC_STATE_FLAG_EOF)
        return PARSE_FAILURE;

    advance_char(s, next);

    int err;
    if((err = char_result(s, (char8_t**) r, next)))
        return err;

    return PARSE_SUCCESS;
}

static int match_range(struct cc_state *s, char32_t lo, char32_t hi, void **r) {
    char32_t next = peek_at(s);
    if((s->flags & CC_STATE_FLAG_EOF) || next < lo || next > hi)
        return PARSE_FAILURE;

    advance_char(s, next);

    int err;
    if((err = char_result(s, (char8_t**) r, next)))
        return err;

    return PARSE_SUCCESS;
}

static int match_oneof(struct cc_state *s, const char32_t *chars, size_t n, void **r) {
    char32_t next = peek_at(s);
    if(s->flags & CC_STATE_FLAG_EOF)
        return PARSE_FAILURE;

    bool one_found = false;
    for(size_t i = 0; i < n; i++) {
        if(next == chars[n]) {
            if(one_found)
                return PARSE_FAILURE;
            one_found = true;
        }
    }
    
    if(!one_found)
        return PARSE_FAILURE;

    advance_char(s, next);

    int err;
    if((err = char_result(s, (char8_t**) r, next)))
        return err;

    return PARSE_SUCCESS;
}

static int match_anyof(struct cc_state *s, const char32_t *chars, size_t n, void **r) {
    char32_t next = peek_at(s);
    if(s->flags & CC_STATE_FLAG_EOF)
        return PARSE_FAILURE;

    for(size_t i = 0; i < n; i++) {
        if(next == chars[n]) {
            advance_char(s, next);

            int err;
            if((err = char_result(s, (char8_t**) r, next)))
                return err;

            return PARSE_SUCCESS;
        }
    }

    return PARSE_FAILURE;
}

static int match_noneof(struct cc_state *s, const char32_t *chars, size_t n, void **r) {
    char32_t next = peek_at(s);
    if(s->flags & CC_STATE_FLAG_EOF)
        return PARSE_FAILURE;

    for(size_t i = 0; i < n; i++) {
        if(next == chars[i])
            return PARSE_FAILURE;
    }

    advance_char(s, next);

    int err;
    if((err = char_result(s, (char8_t**) r, next)))
        return err;

    return PARSE_SUCCESS;
}

static int match_string(struct cc_state *s, const char8_t *str, void **r) {
    struct cc_state save = *s;

    for(size_t i = 0; str[i];) {
        char32_t ch = utf8_first_cp(str + i);
        if(!match_char(s, ch, NULL)) {
            *s = save;
            return PARSE_FAILURE;
        }

        i += utf8_cp_length(ch);
    }

    int err;
    if((err = string_result(s, (char8_t**) r, str)))
        return err;

    return PARSE_SUCCESS;
}

static int call_terminal(struct cc_state *s, const struct cc_parser *p, void **r, struct cc_error *e) {
    if(!s || !p)
        return EINVAL;

    if(!(s->flags & CC_STATE_FLAG_NORETURN)) {
        if(!r)
            return EINVAL;
        *r = NULL;
    }

    switch(p->type) {
        case PARSER_FAIL:       FAIL_WITH(e, s, (char*) p->match.msg, true);

        case PARSER_PASS:       SUCCESS(r, s, NULL);
        
        case PARSER_LOCATION: {
            struct cc_location *loc = malloc(sizeof(struct cc_location));
            if(!loc)
                PROP(s, -errno);

            memcpy(loc, &s->loc, sizeof(struct cc_location));
            SUCCESS(r, s, (void*) loc);
        }

        case PARSER_LIFT:       SUCCESS(r, s, p->match.lift.lf());
        case PARSER_LIFT_VAL:   SUCCESS(r, s, p->match.lift.val);

        case PARSER_EOF:        PROP(s, match_eof(s, r));
        case PARSER_SOF:        PROP(s, match_sof(s, r));
        case PARSER_ANY:        PROP(s, match_any(s, r));
        case PARSER_CHAR:       PROP(s, match_char(s, p->match.ch, r));
        case PARSER_CHAR_RANGE: PROP(s, match_range(s, p->match.lo, p->match.hi, r));
        case PARSER_MATCH:      PROP(s, match_char_func(s, p->match.matchfn, r));

        case PARSER_ONEOF:      PROP(s, match_oneof(s, p->match.list.chars, p->match.list.n, r));
        case PARSER_ANYOF:      PROP(s, match_anyof(s, p->match.list.chars, p->match.list.n, r));
        case PARSER_NONEOF:     PROP(s, match_noneof(s, p->match.list.chars, p->match.list.n, r));

        case PARSER_STRING:     PROP(s, match_string(s, p->match.str, r));

        default:                FAIL_WITH(e, s, format("undefined parser %d", p->type), false);
    }
}

struct data_stack {
    size_t count;
    size_t capacity;
    uint32_t *data;    
};

#define DATA_STACK_INIT {0, 0, NULL}
#define DATA_STACK_INIT_CAP 256

static int data_push(struct data_stack *st, uint32_t v) {
    assert(st != NULL);

    if(st->count + 1 > st->capacity) {
        size_t new_capacity = MAX(st->capacity * 2, DATA_STACK_INIT_CAP);
        uint32_t *new = realloc(st->data, new_capacity * sizeof(uint32_t));
        if(!new)
            return errno;

        st->data = new;
        st->capacity = new_capacity;
    }

    st->data[st->count++] = v;
    return 0;
}

static uint32_t data_pop(struct data_stack *st) {
    assert(st && st->count > 0);

    return st->data[--st->count];
}

struct call {
    struct cc_parser *parser;
    uint32_t ip;
    uint32_t sp;
    uint32_t rp; // result pointer
    struct cc_location loc;
};

struct call_stack {
    size_t capacity;
    size_t count;
    struct call *items;
};

#define CALL_STACK_INIT {0, 0, NULL}
#define CALL_STACK_INIT_CAP 128

static int call_push(struct call_stack *st, struct call call) {
    if(st->count + 1 > st->capacity) {
        size_t new_capacity = MAX(st->capacity * 2, CALL_STACK_INIT_CAP);
        void *new = realloc(st->items, new_capacity * sizeof(struct call));
        if(!new)
            return errno;

        st->items = new;
        st->capacity = new_capacity;
    }

    st->items[st->count++] = call;
    return 0;
}

static struct call call_pop(struct call_stack *st) {
    assert(st->count > 0);
    return st->items[--st->count];
}

struct result_stack {
    size_t capacity;
    size_t count;
    void **items;
};

#define RESULT_STACK_INIT {0, 0, NULL}
#define RESULT_STACK_INIT_CAP 256

static int result_push(struct result_stack *st, void* v) {
    if(st->count + 1 > st->capacity) {
        size_t new_capacity = MAX(st->capacity * 2, RESULT_STACK_INIT_CAP);
        void *new = realloc(st->items, new_capacity * sizeof(void*));
        if(!new)
            return errno;

        st->items = new;
        st->capacity = new_capacity;
    }

    st->items[st->count++] = v;
    return 0;
}

static void *result_pop(struct result_stack *st) {
    assert(st->count > 0);
    return st->items[--st->count];
}

/* static void dump_results(struct result_stack *st) {
    if(st->count == 0)
        fprintf(stderr, "No results.\n");

    for(size_t i = 0; i < st->count; i++) {
        fprintf(stderr, "  %2zu) %p\n", i, st->items[i]);
    }
} */

static int eval(struct cc_state *s, struct cc_parser *p, struct cc_result *r) {
    struct data_stack data_stack = DATA_STACK_INIT;
    struct result_stack result_stack = RESULT_STACK_INIT;
    struct call_stack call_stack = CALL_STACK_INIT;

    int err;

    if((err = call_push(&call_stack, (struct call){
        .parser = p,
        .ip = 0,
        .sp = 0,
        .rp = 0
    })))
        goto cleanup;

    uint32_t call_success = PARSE_SUCCESS;

    while(call_stack.count > 0) {
        struct call *t = &call_stack.items[call_stack.count - 1];
        call_success = PARSE_SUCCESS;

        // resolve parser lookups directly
        while(t->parser->type == PARSER_LOOKUP) {
            // TODO: filter out infinite recursion

            struct cc_parser *found = scope_lookup(s, t->parser->match.lookup);
            if(!found) {
                if((err = new_error(r->err, s, format("undefined parser \"%s\"", t->parser->match.lookup), false)))
                    goto cleanup;
                
                call_success = PARSE_FAILURE;
                goto do_return;
            }

            t->parser = found;
        }
        
        // check if the current parser a terminal parser
        if(!is_combinator(t->parser->type)) {
            // interpret the parser directly without generating ir first
            void *call_result = NULL;
            call_success = call_terminal(s, t->parser, &call_result, r->err);
            if(err < 0) {
                err = -call_success;
                goto cleanup;
            }

            if(call_success == PARSE_SUCCESS && !(s->flags & CC_STATE_FLAG_NORETURN) && (err = result_push(&result_stack, call_result)))
                goto cleanup;

            goto do_return;
        }

        // compile the parser to IR if needed
        if(!t->parser->ir && (err = cc_compile(t->parser)))
            goto cleanup;

        assert(t->parser->ir && "no IR present even though it should be");


        // automatically return on IR end
        if(t->ip >= t->parser->ir->count) {
            call_success = data_pop(&data_stack);
            goto do_return;
        }

        uint32_t v;

        // interpret the next IR opcode
        // fprintf(stderr, "%04x: <%02hhx> %s\n", t->ip, t->parser->ir->bytes[t->ip], ir_str_opcode(t->parser->ir->bytes[t->ip]));
        switch(t->parser->ir->bytes[t->ip++]) {
        case IR_PUSH:
            assert(t->parser->ir->count - t->ip >= sizeof(uint32_t));

            v = ir_read_u32(t->parser->ir, t->ip);
            t->ip += sizeof(uint32_t);

            if((err = data_push(&data_stack, v)))
                goto cleanup;
            continue;

        case IR_POP:
            data_pop(&data_stack);
            continue;

        case IR_SWAP:
            assert(data_stack.count >= 2);
            v = data_stack.data[data_stack.count - 1];
            data_stack.data[data_stack.count - 1] = data_stack.data[data_stack.count - 2];
            data_stack.data[data_stack.count - 2] = v;
            continue;

        case IR_DUP:
            assert(data_stack.count >= 1);
            v = data_stack.data[data_stack.count - 1];

            if((err = data_push(&data_stack, v)))
                goto cleanup;
            continue;

        case IR_NEGATE:
            assert(data_stack.count >= 1);
            data_stack.data[data_stack.count - 1] = !data_stack.data[data_stack.count - 1];
            continue;

        case IR_INCREMENT:
            assert(data_stack.count >= 1);
            data_stack.data[data_stack.count - 1]++; // TODO: range check
            continue;

        case IR_DECREMENT:
            assert(data_stack.count >= 1);
            data_stack.data[data_stack.count - 1]--; // TODO: range check
            continue;

        case IR_SAVE_LOCATION:
            t->loc = s->loc;
            continue;

        case IR_RESTORE_LOCATION:
            s->loc = t->loc;
            continue;

        case IR_SET_NORETURN:
            assert(data_stack.count >= 1);

            v = data_stack.data[data_stack.count - 1];
            data_stack.data[data_stack.count - 1] = set_flag(s, CC_STATE_FLAG_NORETURN, v);
            continue;

        case IR_SET_NOERROR:
            assert(data_stack.count >= 1);

            v = data_stack.data[data_stack.count - 1];
            data_stack.data[data_stack.count - 1] = set_flag(s, CC_STATE_FLAG_NOERR, v);
            continue;

        case IR_CALL:
            assert(t->parser->ir->count - t->ip >= sizeof(uintptr_t));
            
            if((err = call_push(&call_stack, (struct call){
                .parser = (struct cc_parser*) ir_read_ptr(t->parser->ir, t->ip), // call destination
                .sp = data_stack.count,     // save stack pointer
                .rp = result_stack.count,   // save result pointer
                .ip = 0,                    // initial instruction pointer
            })))
                goto cleanup;
            t->ip += sizeof(uintptr_t);
            continue;

        case IR_RETURN:
        do_return:
            assert(data_stack.count >= t->sp);

            if(s->flags & CC_STATE_FLAG_NORETURN || !call_success) {
                assert(result_stack.count >= t->rp);
                result_stack.count = t->rp;
            }
            else {
                assert(result_stack.count == t->rp + 1);
            }

            data_stack.count = t->sp;   // restore stack pointer
            call_pop(&call_stack);      // return to caller
                
            if((err = data_push(&data_stack, call_success)))
                goto cleanup;
            continue;

        case IR_FOLD:
            uint32_t n = data_pop(&data_stack);
            if(!(s->flags & CC_STATE_FLAG_NORETURN)) {
                assert(result_stack.count >= n);

                result_stack.count -= n;
                void *fold_result = t->parser->fold ?
                    t->parser->fold(n, result_stack.items + result_stack.count)
                    : NULL;

                if((err = result_push(&result_stack, fold_result)))
                    goto cleanup;
            }
            continue;

        case IR_APPLY:
            assert(t->parser->type == PARSER_APPLY);
            if(!(s->flags & CC_STATE_FLAG_NORETURN)) {
                assert(result_stack.count > 0);

                void *result_top = result_stack.items[result_stack.count - 1];
                result_stack.items[result_stack.count - 1] = t->parser->match.apply.af(result_top);
            }
            continue;

        case IR_EXPECT:
            assert(t->parser->type == PARSER_EXPECT);
            if((err = add_expected(r->err, s, t->parser->match.expect.what)))
                goto cleanup;
            continue;

        case IR_PUSH_BINDING:
            assert(t->parser->type == PARSER_BIND);
            if((err = scope_push(s, t->parser)))
                goto cleanup;
            continue;

        case IR_POP_BINDING:
            assert(t->parser->type == PARSER_BIND);
            if(scope_pop(s) != t->parser)
                assert(false && "scope stack corrupt");
            continue;

        case IR_NULL_RESULT:
            if(!(s->flags & CC_STATE_FLAG_NORETURN) && (err = result_push(&result_stack, NULL)))
                goto cleanup;
            continue;

        case IR_POP_RESULT:
            if(!(s->flags & CC_STATE_FLAG_NORETURN))
                result_pop(&result_stack);
            continue;

        case IR_JUMP:
            assert(t->parser->ir->count - t->ip >= sizeof(uint32_t));

            t->ip = ir_read_u32(t->parser->ir, t->ip);

            assert(t->ip != UINT32_MAX && "unpatched jump target");
            continue;

        case IR_JUMP_IF_NONZERO:
            assert(t->parser->ir->count - t->ip >= sizeof(uint32_t));

            if(data_pop(&data_stack) != 0)
                t->ip = ir_read_u32(t->parser->ir, t->ip);
            else
                t->ip += sizeof(uint32_t);

            assert(t->ip != UINT32_MAX && "unpatched jump target");
            continue;

        case IR_JUMP_IF_SUCCESS:
            assert(t->parser->ir->count - t->ip >= sizeof(uint32_t));

            if(data_pop(&data_stack) != PARSE_FAILURE)
                t->ip = ir_read_u32(t->parser->ir, t->ip);
            else
                t->ip += sizeof(uint32_t);

            assert(t->ip != UINT32_MAX && "unpatched jump target");
            continue;

        case IR_JUMP_IF_FAILURE:
            assert(t->parser->ir->count - t->ip >= sizeof(uint32_t));

            if(data_pop(&data_stack) == PARSE_FAILURE)
                t->ip = ir_read_u32(t->parser->ir, t->ip);
            else
                t->ip += sizeof(uint32_t);

            assert(t->ip != UINT32_MAX && "unpatched jump target");
            continue;

        default:
            ir_dump(t->parser->ir, stderr);
            if((err = new_error(r->err, s, format("undefined opcode <%02hhx> at <%04x>", t->parser->ir->bytes[t->ip - 1], t->ip - 1), false)))
                goto cleanup;
            call_success = PARSE_FAILURE;
            goto do_return;
        }
    }

    assert(data_stack.count >= 1);
    call_success = data_pop(&data_stack);

    assert((call_success != PARSE_SUCCESS || result_stack.count <= 1) && "too many items left on result stack");
    r->out = call_success == PARSE_SUCCESS && result_stack.count ? result_stack.items[0] : NULL;
cleanup:
    if(data_stack.data)
        free(data_stack.data);
    if(call_stack.items)
        free(call_stack.items);
    if(result_stack.items)
        free(result_stack.items);
    return err ? -err : call_success;
}

int cc_parse(const struct cc_source *src, struct cc_parser *p, struct cc_result *r) {
    if(r)
        memset(r, 0, sizeof(struct cc_result));
    
    int err = 0;
    
    if(!src || !p || !r) {
        err = EINVAL;
        goto cleanup;
    }

    struct cc_state s = {
        .flags = CC_STATE_FLAGS_DEFAULT,
        .loc = CC_LOCATION_DEFAULT,
        .src = src,
        .scope = DYNARR_INIT,
        .max_recursion_depth = src->max_recursion
    };
    
    if(!(r->err = malloc(sizeof(struct cc_error)))) {
        err = errno;
        goto cleanup;
    }
    
    memset(r->err, 0, sizeof(struct cc_error));

    int res = eval(&s, p, r);

    if(res < 0) {
        // resource error, ideally this should never happen
        free(r->err);
        err = -res;
        goto cleanup;
    }

    if(res == PARSE_SUCCESS) {
        cc_err_free(r->err);
        r->err = NULL;
    }

cleanup:
    state_free(&s);
    cc_release(p);
    return err;
}

