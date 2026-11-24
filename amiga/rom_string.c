/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2026.
 *
 * ---------------------------------------------------------------------
 *
 * Small memory functions for the ROM builds, which replace the larger
 * speed-optimized clib2 versions (about 640 bytes smaller). memcpy() uses
 * exec CopyMem(), which is already optimized and is in Kickstart.
 *
 * These must not use globals: the ROM startup code calls memcpy() and
 * memset() before the a4-relative globals exist (see globals_alloc()).
 * SysBase is therefore read from address 4, not from the global.
 */

#include <string.h>
#include <proto/exec.h>

#pragma GCC diagnostic ignored "-Warray-bounds="  // Read of address 4

void *
memcpy(void *dst, const void *src, size_t len)
{
    struct ExecBase *SysBase = *(struct ExecBase **) 4UL;

    CopyMem((APTR) src, dst, len);
    return (dst);
}

void *
memset(void *dst, int ch, size_t len)
{
    char *ptr = dst;

    while (len-- > 0)
        *(ptr++) = ch;
    return (dst);
}

void *
memchr(const void *src, int ch, size_t len)
{
    const unsigned char *ptr = src;

    for (; len > 0; len--, ptr++)
        if (*ptr == (unsigned char) ch)
            return ((void *) ptr);
    return (NULL);
}
