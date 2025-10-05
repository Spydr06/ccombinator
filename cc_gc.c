#include <ccombinator.h>

#include "internal.h"

#include <stdlib.h>
#include <memory.h>

struct cc_parser *gc_allocate_parser(void) {
    // FIXME: implement gc
    void *ptr = malloc(sizeof(struct cc_parser));

    memset(ptr, 0, sizeof(struct cc_parser));

    return ptr;
}

void gc_run(void) {

}
