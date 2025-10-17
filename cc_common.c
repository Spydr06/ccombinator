#include <ccombinator.h>

#include "internal.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/mman.h>

int cc_version_minor(void) {
    return CC_VERSION_MINOR;
}

int cc_version_major(void) {
    return CC_VERISON_MAJOR;
}

const char *cc_version(void) {
    return CC_VERSION_STRING;
}

char *cc_err_string(struct cc_error *e) {
    struct string_buffer sb = STRING_BUFFER_INIT;
    int err;

    char8_t buf[CC_UTF8_ENCODE_PRINTABLE_MAX];

    if(e->failure) {
        if(e->filename && (err = string_buffer_append(&sb, "%s: ", e->filename)))
            goto cleanup;

        if((err = string_buffer_append(&sb, "error: %s\n", e->failure)))
            goto cleanup;

        goto finish;
    }

    if(e->num_expected > 0) {
        if(e->filename && (err = string_buffer_append(&sb, "%s:", e->filename)))
            goto cleanup;

        if((err = string_buffer_append(&sb, "%u:%u: error: expected ", e->loc.line, e->loc.col)))
            goto cleanup;

        if(e->num_expected == 0 && (err = string_buffer_append(&sb, "nothing")))
            goto cleanup;

        else if(e->num_expected == 1 && (err = string_buffer_append(&sb, "%s", e->expected[0])))
            goto cleanup;

        else if(e->num_expected >= 2) {
            for(size_t i = 0; i < e->num_expected - 2; i++) {
                if((err = string_buffer_append(&sb, "%s, ", e->expected[i])))
                    goto cleanup;
            }

            if((err = string_buffer_append(&sb, "%s or %s", e->expected[e->num_expected - 2], e->expected[e->num_expected - 1])))
                goto cleanup;
        }
    }

    if((err = string_buffer_append(&sb, " at ")))
        goto cleanup;

    if((err = utf8_encode_printable(e->received, buf)))
        goto cleanup;

    if((err = string_buffer_append(&sb, (char*) buf)))
        goto cleanup;

finish:
    return string_buffer_unwrap(&sb);
cleanup:
    free(sb.buf);
    errno = err;
    return NULL;
}

int cc_err_print(struct cc_error *e) {
    return cc_err_fprint(e, stderr);
}

int cc_err_fprint(struct cc_error *e, FILE *f) {
    char *msg = cc_err_string(e);
    if(!msg)
        return errno;

    if(fprintf(f, "%s\n", msg) < 0) {
        free(msg);
        return errno;
    }

    free(msg);
    return 0;
}

void cc_err_free(struct cc_error *e) {
    free((void*) e->failure);

    for(size_t i = 0; i < e->num_expected; i++)
        free((void*) e->expected[i]);

    free(e);
}

int dynarr_append(struct dynarr *da, void *elem) {
    if(!da)
        return EINVAL;

    if(da->len + 1 > da->cap) {
        size_t new_cap = MAX(da->cap * 2, DYNARR_INIT_CAP);

        void *new_elems = realloc(da->elems, new_cap * sizeof(void*));
        if(!new_elems)
            return errno;

        da->elems = new_elems;
        da->cap = new_cap;
    }

    da->elems[da->len++] = elem;

    return 0;
}

void dynarr_free(struct dynarr *da) {
    free(da->elems);
    memset(da, 0, sizeof(struct dynarr));
}

int string_buffer_append(struct string_buffer *sb, const char *fmt, ...) {
    if(!sb || !fmt)
        return EINVAL;
    
    va_list ap, ap_copy;
    va_start(ap, fmt);

    va_copy(ap_copy, ap);
    int slen = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);

    if(slen < 0)
        return errno;

    if(sb->len + slen > sb->cap) {
        size_t new_cap = MAX(sb->cap, STRING_BUFFER_INIT_CAP);

        while(new_cap < sb->len + slen)
            new_cap *= 2;

        void *new_buf = realloc(sb->buf, (new_cap + 1) * sizeof(char));
        if(!new_buf)
            return errno;

        sb->buf = new_buf;
        sb->cap = new_cap;
    }

    int slen2 = vsprintf(sb->buf + sb->len, fmt, ap);
    assert(slen == slen2);
    va_end(ap);

    sb->len += slen;

    return 0;
}

static int free_memstream(struct cc_source *s) {
    if(munmap((void*) s->buffer, s->buffer_size) < 0)
        return -1;

    s->buffer = NULL;
    s->buffer_size = 0;

    if(close(s->fd))
        return -1;

    s->fd = -1;
    return 0;
}

static int open_memstream(struct cc_source *source, const char *filename) {
    int fd = open(filename, O_RDONLY);
    if(fd < 0)
        return -1;

    struct stat stat;
    if(fstat(fd, &stat) < 0)
        return -1;

    void *buffer = mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(buffer == MAP_FAILED)
        return -1;

    source->origin = filename;
    source->fd = fd;
    source->buffer = buffer;
    source->buffer_size = stat.st_size;

    return 0;
}

static struct cc_source *new_source(void) {
    struct cc_source *s = malloc(sizeof(struct cc_source));
    if(!s)
        return NULL;

    s->max_recursion = CC_DEFAULT_MAX_RECURSION;
    s->fd = -1;

    return s;
}

struct cc_source *cc_open(const char *filename) {
    struct cc_source *s = new_source();
    if(!s)
        return NULL;

    if(open_memstream(s, filename) < 0) {
        return NULL;
    }

    return s;
}

struct cc_source *cc_string_source(const char8_t *s) {
    if(!s) {
        errno = EINVAL;
        return NULL;
    }

    return cc_nstring_source(s, strlen((const char*) s));
}

struct cc_source *cc_nstring_source(const char8_t *str, size_t size) {
    if(!str)
        return NULL;

    struct cc_source *s = new_source();
    if(!s)
        return NULL;

    s->origin = "<string>";
    s->buffer = str;
    s->buffer_size = size;
    s->buffer_dtor = NULL;

    return s;
}

int cc_close(struct cc_source *s) {
    if(!s)
        return EINVAL;

    int err;
    if(s->fd >= 0 && (err = free_memstream(s)))
        return err;
    if(s->buffer_dtor && (err = s->buffer_dtor(s->buffer)))
        return err;

    free(s);
    return 0;
}

void *cc_fold_concat(size_t n, void **xs) {
    if(n == 1)
        return xs[0];

    size_t total = 0;

    for(size_t i = 0; i < n; i++)
        total += xs[i] ? strlen(xs[i]) : 0;

    size_t off = 0;
    char8_t *s = malloc((total + 1) * sizeof(char8_t));
    if(!s)
        return NULL;

    for(size_t i = 0; i < n; i++) {
        if(!xs[i])
            continue;

        size_t l = strlen(xs[i]);
        memcpy(s + off, xs[i], l);
        off += l;

        free(xs[i]);
    }

    s[off] = '\0';
    return s;
}

void *cc_fold_first(size_t n, void **r) {
    if(n == 0)
        return NULL;

    for(size_t i = 1; i < n; i++)
        free(r[i]);
    
    return r[0];
}

void *cc_fold_middle(size_t n, void **r) {
    if(n == 0)
        return NULL;

    size_t m = n / 2;

    for(size_t i = 0; i < n; i++) {
        if(i == m) continue;

        free(r[i]);
    }

    return r[m];
}

void *cc_fold_last(size_t n, void **r) {
    if(n == 0)
        return NULL;

    for(size_t i = 0; i < n - 1; i++)
        free(r[i]);

    return r[n - 1];
}

void *cc_fold_null(size_t n, void **r) {
    for(size_t i = 0; i < n; i++)
        free(r[i]);
    return NULL;
}

void *cc_apply_free(void *r) {
    free(r);
    return NULL;
}

