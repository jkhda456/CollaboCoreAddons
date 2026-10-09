/*
 * The wasm signature of a function the program calls through a pointer: WebAssembly checks it on
 * every indirect call (a mismatch traps), so code that calls a function through a pointer of
 * another type — common in C with GLib (a GDestroyNotify where a GClosureNotify is called, a
 * signal handler that leaves out its last parameters) — can ask what the function really takes
 * and call it so (ffi_wasm_call, ffi.c).
 *
 * The program reads its own type, function and element sections from /proc/self/exe the first
 * time it asks: a function pointer is an index into the table, which the element section fills.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ffi.h"

static char **type_sigs;       /* per type: "r_pp" (i, j, f, d; v for no result) */
static uint32_t n_types;
static uint32_t *func_types;   /* per function (imports first): its type */
static uint32_t n_funcs;
static uint32_t *table;        /* per table slot: a function index, or UINT32_MAX */
static uint32_t n_table;
static pthread_once_t once = PTHREAD_ONCE_INIT;

struct reader { const uint8_t *p, *end; int bad; };

static uint32_t uleb(struct reader *r)
{
    uint32_t result = 0;
    int shift = 0;
    while (r->p < r->end) {
        uint8_t b = *r->p++;
        result |= (uint32_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
            return result;
        shift += 7;
        if (shift > 35)
            break;
    }
    r->bad = 1;
    return 0;
}

static char value_type(uint8_t t)
{
    switch (t) {
    case 0x7f: return 'i';
    case 0x7e: return 'j';
    case 0x7d: return 'f';
    case 0x7c: return 'd';
    default: return '?';
    }
}

static void skip_init_expr(struct reader *r)
{
    while (r->p < r->end && *r->p != 0x0b)
        r->p++;
    r->p++;
}

static int read_section(int fd, uint8_t **out, uint32_t size)
{
    *out = malloc(size ? size : 1);
    if (!*out)
        return -1;
    uint32_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, *out + done, size - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += n;
    }
    return 0;
}

static void load(void)
{
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    uint8_t head[8];
    if (read(fd, head, 8) != 8 || memcmp(head, "\0asm", 4)) {
        close(fd);
        return;
    }
    uint32_t imported_funcs = 0, defined_funcs = 0;
    uint32_t *imported_types = NULL, *defined_types = NULL;
    for (;;) {
        uint8_t id;
        if (read(fd, &id, 1) != 1)
            break;
        /* the section size, a LEB128 read a byte at a time */
        uint32_t size = 0;
        int shift = 0;
        uint8_t b;
        do {
            if (read(fd, &b, 1) != 1)
                goto done;
            size |= (uint32_t)(b & 0x7f) << shift;
            shift += 7;
        } while (b & 0x80);
        if (id != 1 && id != 2 && id != 3 && id != 9) {
            if (lseek(fd, size, SEEK_CUR) < 0)
                break;
            continue;
        }
        uint8_t *data;
        if (read_section(fd, &data, size) < 0)
            break;
        struct reader r = { data, data + size, 0 };
        uint32_t count = uleb(&r);
        if (id == 1) {
            type_sigs = calloc(count, sizeof *type_sigs);
            n_types = count;
            for (uint32_t i = 0; i < count && !r.bad; i++) {
                if (r.p >= r.end || *r.p++ != 0x60) {
                    r.bad = 1;
                    break;
                }
                uint32_t np = uleb(&r);
                char *sig = malloc(np + 4);
                uint32_t k = 2;
                const uint8_t *params = r.p;
                r.p += np;
                uint32_t nr = uleb(&r);
                sig[0] = nr ? value_type(*r.p) : 'v';
                r.p += nr;
                sig[1] = '_';
                for (uint32_t q = 0; q < np; q++)
                    sig[k++] = value_type(params[q]);
                sig[k] = 0;
                type_sigs[i] = sig;
            }
        } else if (id == 2) {
            imported_types = calloc(count, sizeof *imported_types);
            for (uint32_t i = 0; i < count && !r.bad; i++) {
                for (int s = 0; s < 2; s++) {
                    uint32_t len = uleb(&r);
                    r.p += len;
                }
                uint8_t kind = *r.p++;
                if (kind == 0)
                    imported_types[imported_funcs++] = uleb(&r);
                else if (kind == 1) {
                    r.p++;
                    uint32_t flags = uleb(&r);
                    uleb(&r);
                    if (flags & 1) uleb(&r);
                } else if (kind == 2) {
                    uint32_t flags = uleb(&r);
                    uleb(&r);
                    if (flags & 1) uleb(&r);
                } else if (kind == 3)
                    r.p += 2;
                else if (kind == 4) {
                    r.p++;
                    uleb(&r);
                }
            }
        } else if (id == 3) {
            defined_types = calloc(count, sizeof *defined_types);
            defined_funcs = count;
            for (uint32_t i = 0; i < count && !r.bad; i++)
                defined_types[i] = uleb(&r);
        } else if (id == 9) {
            for (uint32_t s = 0; s < count && !r.bad; s++) {
                uint32_t flags = uleb(&r);
                uint32_t offset = 0;
                if (flags == 0 || flags == 4 || flags == 2 || flags == 6) {
                    if (flags & 2)
                        uleb(&r); /* table index */
                    /* i32.const N */
                    if (r.p < r.end && *r.p == 0x41) {
                        r.p++;
                        int32_t v = 0;
                        int sh = 0;
                        uint8_t c;
                        do {
                            c = *r.p++;
                            v |= (int32_t)(c & 0x7f) << sh;
                            sh += 7;
                        } while (c & 0x80);
                        if (sh < 32 && (c & 0x40))
                            v |= -(1 << sh);
                        offset = (uint32_t)v;
                    } else
                        skip_init_expr(&r);
                    if (r.p[-1] != 0x0b)
                        skip_init_expr(&r);
                }
                if (flags == 1 || flags == 2 || flags == 3)
                    r.p++; /* element kind */
                uint32_t n = uleb(&r);
                if ((flags & 1) || (flags & 4)) {
                    /* passive/declarative, or expressions: not ours to index */
                    for (uint32_t k = 0; k < n && !r.bad; k++) {
                        if (flags & 4)
                            skip_init_expr(&r);
                        else
                            uleb(&r);
                    }
                    continue;
                }
                if (offset + n > n_table) {
                    uint32_t *t = realloc(table, (offset + n) * sizeof *table);
                    if (!t) {
                        r.bad = 1;
                        break;
                    }
                    for (uint32_t k = n_table; k < offset + n; k++)
                        t[k] = UINT32_MAX;
                    table = t;
                    n_table = offset + n;
                }
                for (uint32_t k = 0; k < n && !r.bad; k++)
                    table[offset + k] = uleb(&r);
            }
        }
        free(data);
        if (r.bad)
            break;
    }
done:
    close(fd);
    n_funcs = imported_funcs + defined_funcs;
    func_types = calloc(n_funcs ? n_funcs : 1, sizeof *func_types);
    if (func_types) {
        if (imported_types)
            memcpy(func_types, imported_types, imported_funcs * sizeof *func_types);
        if (defined_types)
            memcpy(func_types + imported_funcs, defined_types, defined_funcs * sizeof *func_types);
    }
    free(imported_types);
    free(defined_types);
}

const char *ffi_wasm_signature(void (*fn)(void))
{
    pthread_once(&once, load);
    uintptr_t slot = (uintptr_t)fn;
    if (!table || slot >= n_table || table[slot] == UINT32_MAX || table[slot] >= n_funcs)
        return NULL;
    uint32_t type = func_types[table[slot]];
    return type < n_types ? type_sigs[type] : NULL;
}
