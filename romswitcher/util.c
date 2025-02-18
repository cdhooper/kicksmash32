/*
 * Random utility functions.
 *
 * This source file is part of the code base for a simple Amiga ROM
 * replacement sufficient to allow programs using some parts of GadTools
 * to function.
 *
 * Copyright 2025 Chris Hooper. This program and source may be used
 * and distributed freely, for any purpose which benefits the Amiga
 * community. All redistributions must retain this Copyright notice.
 *
 * DISCLAIMER: THE SOFTWARE IS PROVIDED "AS-IS", WITHOUT ANY WARRANTY.
 * THE AUTHOR ASSUMES NO LIABILITY FOR ANY DAMAGE ARISING OUT OF THE USE
 * OR MISUSE OF THIS UTILITY OR INFORMATION REPORTED BY THIS UTILITY.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "printf.h"
#include "util.h"

static uint32_t *freelist = NULL;
static uint32_t *freelist_chipmem = NULL;

#define MALLOC_BASE              (RAM_BASE + (512 << 10))
#define MALLOC_BASE_CHIPMEM      (512 << 10)  // base at 512 KB
#define MALLOC_AREA_SIZE         (512 << 10)  // 512 KB
#define MALLOC_CHIPMEM_AREA_SIZE (512 << 10)  // 512 KB

#undef DEBUG_MALLOC
#ifdef DEBUG_MALLOC
#define DPRINTF(fmt, ...) printf(fmt, ...)

static void
show_freelist(void)
{
    uint32_t *cur;
    printf("FL:");
    for (cur = freelist; cur != NULL; cur = ADDR32(cur[1]))
        printf(" %x(%x)", cur, cur[0]);
    printf("\n");
}
#else
#define DPRINTF(fmt, ...)
#define show_freelist()
#endif

/* Dumb alloc by cdh */
void *
malloc(size_t size)
{
    uint32_t *cur;
    uint32_t *prev = NULL;
    if (freelist == NULL) {
        freelist = ADDR32(MALLOC_BASE);      // Base + 512 KB
        freelist[0] = MALLOC_AREA_SIZE - 4;  // Area size is 512KB - 4 bytes
        freelist[1] = (uintptr_t) NULL;      // Next pointer
    }

    size = (size + 3) & ~3;  // round up

    show_freelist();

    /* Search free list for first block of sufficient size */
    for (cur = freelist; cur != NULL; prev = cur, cur = ADDR32(cur[1])) {
        if (cur[0] >= size) {
            /* Size of freelist entry doesn't include 4 bytes for size */
            uint32_t *next;
            if (cur[0] >= size + 8) {
                /* Enough space to split block */
                next = &cur[size / 4 + 1];
                next[0] = cur[0] - size - 4;
                next[1] = cur[1];
                cur[0]  = size;
            } else {
                next = ADDR32(cur[1]);
            }

            /* Link to next */
            if (prev == NULL)
                freelist = next;
            else
                prev[1] = (uintptr_t) next;
            DPRINTF("alloc %x(%x)\n", cur, size);
            return (&cur[1]);
        }
    }
    DPRINTF("No memory: %x\n", size);
    return (NULL);
}

void *
calloc(size_t nmemb, size_t size)
{
    void *ptr;
    size *= nmemb;
    ptr = malloc(size);
    if (ptr == NULL)
        return (ptr);
    memset(ptr, 0, size);
    return (ptr);
}

void *
realloc(void *ptr, size_t size)
{
    /* This should be made a little less dumb */
    void *nptr = malloc(size);
    memcpy(nptr, ptr, size);
    free(ptr);
    return (nptr);
}

void
free(void *addr)
{
    uint size;
    uint32_t *cur;
    uint32_t *node;
    uint32_t *prev = NULL;
    if (addr == NULL) {
        DPRINTF("free NULL\n");
        return;
    }
    node = ADDR32(((uintptr_t)addr) - 4);
    size = node[0];
    DPRINTF("free %x(%x)\n", node, size);

    /* Look for a merge candidate */
    for (cur = freelist; cur != NULL; prev = cur, cur = ADDR32(cur[1])) {
        if (node == &cur[cur[0] / 4 + 1]) {
            DPRINTF("merge to tail of %p\n", cur);
            cur[0] += size + 4;
            show_freelist();

            /* Merge to tail could cause a subsequent tail merge (gap filled) */
            node = &cur[cur[0] / 4 + 1];
            if (cur[1] == (uintptr_t) node) {
                DPRINTF("merge tail of %p to next %p\n", cur, node);
                cur[0] += node[0] + 4;
                cur[1] = node[1];
                show_freelist();
            }
            return;
        }
        if (&node[size / 4 + 1] == cur) {
            DPRINTF("merge to head of %p\n", cur);
            node[0] += cur[0] + 4;
            node[1] = cur[1];
            if (prev == NULL)
                freelist = node;
            else
                prev[1] = (uintptr_t) node;
            show_freelist();
            return;
        }
        if (cur > node) {
            /* Need to merge in to free list here */
            node[1] = (uintptr_t)cur;
            if (prev == NULL)
                freelist = node;
            else
                prev[1] = (uintptr_t) node;
            show_freelist();
            return;
        }
    }

    /* No merge candidates found */
    node[1] = (uintptr_t)freelist;
    freelist = node;
    show_freelist();
}

void *
malloc_chipmem(size_t size)
{
    void *addr;
    static uint32_t *tmplist;
    if ((uintptr_t) freelist <= (2 << 20))  // < 2 MB
        return (malloc(size));      // Only allocating from chipmem

    if (freelist_chipmem == NULL) {
        /* Need to set up chipmem memory area */
        freelist_chipmem = ADDR32(MALLOC_BASE_CHIPMEM);
        freelist_chipmem[0] = MALLOC_CHIPMEM_AREA_SIZE - 4;
        freelist_chipmem[1] = (uintptr_t) NULL;
    }
    tmplist = freelist;
    freelist = freelist_chipmem;
    addr = malloc(size);
    freelist_chipmem = freelist;
    freelist = tmplist;
    return (addr);
}

void
free_chipmem(void *addr)
{
    static uint32_t *tmplist;
    if ((uintptr_t) freelist <= (2 << 20))  // < 2 MB
        return (free(addr));      // Only allocating from chipmem

    tmplist = freelist;
    freelist = freelist_chipmem;
    free(addr);
    freelist_chipmem = freelist;
    freelist = tmplist;
}

void *
AllocVec(uint byteSize, uint requirements)
{
    if (requirements & MEMF_CHIP)
        return (malloc_chipmem(byteSize));
    else
        return (malloc(byteSize));
}

void *
AllocMem(uint byteSize, uint requirements)
{
    if (requirements & MEMF_CHIP)
        return (malloc_chipmem(byteSize));
    else
        return (malloc(byteSize));
}

char *
strdup(const char *s)
{
    char *ptr;
    int len = strlen(s) + 1;
    ptr = malloc(len);
    if (ptr == NULL)
        return (NULL);
    memcpy(ptr, s, len);
    return (ptr);
}


/*
 * C library functions we don't want to use
 *
 *     __locale_ctype_ptr
 *     wctob
 *     mbrtowc
 *     iswspace
 *
 * C library functions that are okay to use
 *     memset
 *     memcpy
 *     memmove
 *     strlen
 *     strcmp
 *     strncmp
 *     strstr
 *
 *     __udivsi3
 *     __umoddi3
 *     __umodsi3
 *     __mulsi3
 */

#if 0
const char *
__locale_ctype_ptr(locale_t l)
{
    (void) (l);
    return (NULL);
}
#endif

#if 0
/*
 * These functions should never be called -- if they are, an undesirable
 * C library function is being used.
 */
void
_exit(int ec)
{
    printf("_exit %u\n", ec);
    __builtin_unreachable();
}

void
exit(int ec)
{
    printf("exit %u\n", ec);
    __builtin_unreachable();
}

#endif

#if 0
// XXX: stub for now
uint
send_cmd_core(uint16_t cmd, void *arg, uint16_t arglen,
              void *reply, uint replymax, uint *replyalen)
{
    (void) cmd;
    (void) arg;
    (void) arglen;
    (void) reply;
    (void) replymax;
    (void) replyalen;
    return (1);
}
uint (*esend_cmd_core)(uint16_t cmd, void *arg, uint16_t arglen,
                       void *reply, uint replymax, uint *replyalen) =
                      &send_cmd_core;

uint
send_cmd(uint16_t cmd, void *arg, uint16_t arglen,
         void *reply, uint replymax, uint *replyalen)
{
    (void) cmd;
    (void) arg;
    (void) arglen;
    (void) reply;
    (void) replymax;
    (void) replyalen;
    return (-1);
}
#endif

#if 0
void
Disable(void)
{
//      move.w  sr,d0
//      or.w    #$0700,sr
}

void
Enable(void)
{
//      move.w  sr,d0
//      and.w   #$f8ff,sr
}
#endif

void
checknull(uintptr_t addr, const char *text)
{
    if (*VADDR32(addr) != 0xa5a5a5a5)
        printf("%s\n", text);
}
