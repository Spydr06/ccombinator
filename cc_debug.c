#include <ccombinator.h>

#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdarg.h>

#define INDENT_WIDTH 2

#define E(s) [(s)] = #s

static const char *parser_type_string_table[] = {
    E(PARSER_UNDEFINED),
    E(PARSER_EOF),
    E(PARSER_SOF),
    E(PARSER_ANY),
    E(PARSER_STRING),
    E(PARSER_CHAR),
    E(PARSER_CHAR_RANGE),
    E(PARSER_MATCH),
    E(PARSER_PASS),
    E(PARSER_FAIL),
    E(PARSER_LIFT),
    E(PARSER_LIFT_VAL),
    E(PARSER_ANYOF),
    E(PARSER_NONEOF),
    E(PARSER_ONEOF),
    E(PARSER_EXPECT),
    E(PARSER_APPLY),
    E(PARSER_NOT),
    E(PARSER_AND),
    E(PARSER_OR),
    E(PARSER_MANY),
    E(PARSER_MANY_UNTIL),
    E(PARSER_COUNT),
    E(PARSER_LEAST),
    E(PARSER_MAYBE),
    E(PARSER_CHAIN),
    E(PARSER_POSTFIX),
    E(PARSER_LOCATION),
    E(PARSER_NORETURN),
    E(PARSER_NOERROR),
    E(PARSER_LOOKUP),
    E(PARSER_BIND),
};

CC_format_printf(3)
static int print_indented(FILE *f, size_t d, const char *fmt, ...) {
    for(size_t i = 0; i < d * INDENT_WIDTH; i++) {
        if(fputc(' ', f) < 0)
            return errno;
    }

    va_list ap;
    va_start(ap, fmt);
    if(vfprintf(f, fmt, ap) < 0)
        return errno;
    va_end(ap);

    return 0;
}

static int dump_parser(struct cc_parser *p, FILE *f, size_t d) {
    if(!p || !f)
        return EINVAL;

    int err;

    const char *name = p->type < PARSER_TYPE_MAX ? parser_type_string_table[p->type] : "<invalid>";
    if((err = print_indented(f, d, "-> %s:\n", name)))
        return err;

    d++;

    switch(p->type) {
    case PARSER_STRING:
        err = print_indented(f, d, "string = \"%s\"\n", p->match.str);
        break;

    case PARSER_CHAR: {
        char8_t buf[CC_UTF8_ENCODE_PRINTABLE_MAX];
        utf8_encode_printable(p->match.ch, buf);
        err = print_indented(f, d, "char = %s\n", (char*) buf);
    } break;

    case PARSER_CHAR_RANGE: {
        char8_t lo[CC_UTF8_ENCODE_PRINTABLE_MAX],
            hi[CC_UTF8_ENCODE_PRINTABLE_MAX];
        utf8_encode_printable(p->match.lo, lo);
        utf8_encode_printable(p->match.hi, hi);
        if((err = print_indented(f, d, "low = %s\n", (char*) lo)))
            break;
        err = print_indented(f, d, "high = %s\n", (char*) hi);
    } break;

    case PARSER_MATCH: {
        union {
            int (*f)(char32_t);
            void *p;
        } u;
        u.f = p->match.matchfn;
        err = print_indented(f, d, "func = <%p>\n", u.p);
        break;
    }

    case PARSER_FAIL:
        err = print_indented(f, d, "message = \"%s\"\n", p->match.msg);
        break;

    case PARSER_LIFT: {
        union {
            cc_lift_t f;
            void *p;
        } u;
        u.f = p->match.lift.lf;
        err = print_indented(f, d, "func = <%p>\n", u.p);
    } break;

    case PARSER_LIFT_VAL:
        err = print_indented(f, d, "value = <%p>\n", p->match.lift.val);
        break;

    case PARSER_ANYOF:
    case PARSER_NONEOF:
    case PARSER_ONEOF: {
        if((err = print_indented(f, d, "chars = [")))
            break;

        char8_t buf[CC_UTF8_ENCODE_PRINTABLE_MAX];
        for(unsigned i = 0; i < p->match.list.n; i++) {
            utf8_encode_printable(p->match.list.chars[i], buf);

            if(fprintf(f, "%s, ", buf) < 0) {
                err = errno;
                break;
            }    
        }

        err = print_indented(f, d, "]\n");
    } break;

    case PARSER_EXPECT:
        break;

    default:
    }

    return err;
}

int cc_debug_dump(struct cc_parser *p) {
    return cc_debug_fdump(p, stderr);
}

int cc_debug_fdump(struct cc_parser *p, FILE *f) {
    return dump_parser(p, f, 0);
}

__internal const char *ir_str_opcode(enum cc_ir_opcode opcode) {
    switch(opcode) {
        case IR_PUSH:
            return "push";
        case IR_POP:
            return "pop";
        case IR_SWAP:
            return "swap";
        case IR_DUP:
            return "dup";
        case IR_NEGATE:
            return "negate";
        case IR_SET_NOERROR:
            return "set_noerror";
        case IR_SET_NORETURN:
            return "set_noreturn";
        case IR_CALL:
            return "call";
        case IR_RETURN:
            return "return";
        case IR_FOLD:
            return "fold";
        case IR_APPLY:
            return "apply";
        case IR_EXPECT:
            return "expect";
        case IR_PUSH_BINDING:
            return "push_binding";
        case IR_POP_BINDING:
            return "pop_binding";
        case IR_NULL_RESULT:
            return "null_result";
        case IR_POP_RESULT:
            return "pop_result";
        case IR_JUMP:
            return "jump";
        case IR_JUMP_IF_NONZERO:
            return "if_nonzero";
        case IR_JUMP_IF_SUCCESS:
            return "if_success";
        case IR_JUMP_IF_FAILURE:
            return "if_failure";
        case IR_SAVE_LOCATION:
            return "save_location";
        case IR_RESTORE_LOCATION:
            return "restore_location";
        case IR_INCREMENT:
            return "increment";
        case IR_DECREMENT:
            return "decrement";
        default:
            return NULL;
    }
}

__internal int ir_dump(const struct cc_ir *ir, FILE *f) {
    if(!ir) {
        fprintf(f, "No IR generated.\n");
        return 0;
    }

    fprintf(f, "==== ir ====\n");

    uint32_t ip = 0;
    while(ip < ir->count) {
        const char *opcode = ir_str_opcode(ir->bytes[ip]);
        if(opcode)
            fprintf(f, "  %02x | %s", ip, opcode);
        else
            fprintf(f, "  %02x | Unknown opcode <%02hhx>", ip, ir->bytes[ip]);

        switch(ir->bytes[ip++]) {
        case IR_PUSH:
            uint32_t c = ir_read_u32(ir, ip);
            ip += sizeof(uint32_t);
            fprintf(f, " %u", c);
            break;

        case IR_CALL:
            uintptr_t p = ir_read_ptr(ir, ip);
            ip += sizeof(uintptr_t);
            fprintf(f, " <%p>", (void*) p);
            break;

        case IR_JUMP:
        case IR_JUMP_IF_NONZERO:
        case IR_JUMP_IF_SUCCESS:
        case IR_JUMP_IF_FAILURE:
            c = ir_read_u32(ir, ip);
            ip += sizeof(uint32_t);
            fprintf(f, " <%04x>", c);
            break;
        default:
            break;
        }

        fprintf(f, "\n");
    }

    return 0;
}

__internal void lazy_debug_dump(const struct cc_lazy *lazy, FILE *f) {
    if(!lazy) {
        fprintf(f, "<null>");
        return;
    }

    char8_t ch_buf[CC_UTF8_ENCODE_PRINTABLE_MAX];

    switch(lazy->type) {
    case LAZY_VALUE:
        fprintf(f, "<%p>", LAZY_DOWNCAST(lazy, struct cc_lazy_value)->value);
        break;
    case LAZY_INLINE:
        fprintf(f, "<%p>", LAZY_DOWNCAST(lazy, struct cc_lazy_inline)->value);
        break;
    case LAZY_CHAR:
        utf8_encode_printable(LAZY_DOWNCAST(lazy, struct cc_lazy_char)->ch, ch_buf);
        fprintf(f, "%s", ch_buf);
        break;
    case LAZY_TERMINAL:
        fprintf(f, "terminal(%s, %p)", ch_buf, (void*) LAZY_DOWNCAST(lazy, struct cc_lazy_terminal)->p);
        break;
    case LAZY_LIFT:
        fprintf(f, "lift(%p)", (void*) LAZY_DOWNCAST(lazy, struct cc_lazy_lift)->lift);
        break;
    case LAZY_FOLD:
        struct cc_lazy_fold *fold = LAZY_DOWNCAST(lazy, struct cc_lazy_fold);
        fprintf(f, "fold[%p](", (void*) fold->fold);

        for(unsigned i = 0; i < fold->n; i++) {
            lazy_debug_dump(fold->values[i], f);

            fprintf(f, "%s", i + 1 == fold->n ? ")" : ", ");
        }
        break;
    case LAZY_APPLY:
        struct cc_lazy_apply *apply = LAZY_DOWNCAST(lazy, struct cc_lazy_apply);

        fprintf(f, "apply[%p](", (void*) apply->apply);
        lazy_debug_dump(apply->value, f);
        fprintf(f, ")");
        break;
    default:
        fprintf(f, "<unknown>");
    }
}

