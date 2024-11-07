/*
 * These printf() functions are based on NetBSD kernel printf code as
 * of version 1.20 2011/05/20. They have been extensively modified,
 *
 * http://fxr.watson.org/fxr/source/lib/libsa/subr_prf.c?v=NETBSD
 */

/*      $NetBSD: subr_prf.c,v 1.20 2011/05/20 16:33:07 tsutsui Exp $    */

/*
 * Copyright (c) 1993
 *      The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *      @(#)printf.c    8.1 (Berkeley) 6/11/93
 */


/*
 * Scaled down version of printf(3).
 */

#include "printf.h"

#define USE_SERIAL_OUTPUT

#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>

/* Compatibility types */
#define INTMAX_T      int64_t  // Signed largest integer
#define UINTMAX_T     uint64_t // Unsigned largest integer
#define PTRDIFF_T     int32_t  // Signed difference between two pointers

/* Output formatting flags */
#define FMT_LONG       0x0001   // Value is a 32-bit long integer
#define FMT_LLONG      0x0002   // Value is a 64-bit long long integer
#define FMT_ALT        0x0004   // Prefix octal by "0" and hex by "0x"
#define FMT_SPACE      0x0008   // If the value is positive, pad sign with ' '
#define FMT_LJUST      0x0010   // left justify (padding on right side)
#define FMT_SIGN       0x0020   // If the value is positive, print '+' for sign
#define FMT_ZEROPAD    0x0040   // Zero-pad (on the left) to the specified width
#define FMT_NEGATIVE   0x0080   // Value is negative
#define FMT_UPPERCASE  0x0100   // Uppercase hex (A-F) instead of (a-f)
#define FMT_DOT        0x0200   // Dot specifier was used

#ifdef NO_DEBUG
#undef USE_SERIAL_OUTPUT
#undef printf
#endif

#include <exec/types.h>
#include <inline/exec.h>
#include <inline/dos.h>
extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

#define RawPutChar(___c) \
        LP1NR(0x204, RawPutChar , UBYTE, ___c, d0, , SysBase)

/* Output buffer structure */
typedef struct {
    char *buf_cur;
    char *buf_end;
} buf_t;

extern uint8_t flag_output;

int
putchar(int ch)
{
#ifdef USE_SERIAL_OUTPUT
#ifdef ROMFS
    if (flag_output)
        RawPutChar(ch);
#else
    if (flag_output == 1) {
        FPutC(Output(), ch);
    } else if (flag_output) {
        RawPutChar(ch);
    }
#endif
#endif
    return (ch);
}

int
puts(const char *str)
{
#ifdef USE_SERIAL_OUTPUT
#ifdef ROMFS
    if (flag_output) {
        while (*str != '\0') {
            RawPutChar(*str);
            str++;
        }
        RawPutChar('\n');
    }
#else
    if (flag_output == 1) {
        PutStr(str);
        FPutC(Output(), '\n');
    } else if (flag_output) {
        while (*str != '\0') {
            RawPutChar(*str);
            str++;
        }
        RawPutChar('\n');
    }
#endif
#else
    (void) str;
#endif
    return (0);
}

/**
 * put() sends the specified character either to a buffer to the console.
 *
 * @param [in]  ch   - The character to output.
 * @param [out] desc - A pointer to the buffer structure describing the
 *                     storage buffer for the output character.  If this
 *                     pointer is NULL, then output should instead be
 *                     directed to the serial console.  The buffer
 *                     structure contains a pointer to the current position
 *                     in the buffer where the next character is to be
 *                     stored.  If this pointer reaches the end of buffer
 *                     pointer, no additional characters will be stored in
 *                     the buffer.  This allows the calling function to
 *                     insert a NIL ('\\0') at the tail of the output string.
 *
 * @return      None.
 *
 * @see         kprintn() and kdoprnt().
 */
static void
put(int ch, buf_t *desc)
{
    if (desc == NULL) {
        putchar(ch);
    } else if (desc->buf_cur < desc->buf_end) {
        *(desc->buf_cur)++ = (char) ch;
    }
}

/**
 * kprintn() converts a binary number to ASCII in the specified base.
 *           Common bases are 2, 8, 10, and 16.  Bases up to 36 are supported.
 *
 * This function is an enhanced port of the NetBSD kernel function of the
 * same name.
 *
 * @param [out] desc  - Output buffer descriptor.  If NULL, output will be
 *                      directed to the serial console.
 * @param [in]  value - Binary value to be converted to ASCII.
 * @param [in]  base  - Base in which the converted value is to be expressed.
 *                      Common bases are 2, 8, 10, and 16.  The minimum base
 *                      value supported is 2, and the maximum is 36.
 * @param [in]  flags - Output formatting flags.  The following are available:
 *                          FMT_ALT       - alternate representation (octal
 *                                          prefixed by 0 or hex prefixed by 0x)
 *                          FMT_LJUST     - output should be left justified in
 *                                          the specified field.  The default
 *                                          is to right justify output.
 *                          FMT_NEGATIVE  - output value is negative.
 *                          FMT_SIGN      - output should always include the
 *                                          sign of the value ("+" or "-").
 *                          FMT_SPACE     - output should have a space in the
 *                                          position where a negative sign
 *                                          would otherwise be placed.
 *                          FMT_UPPERCASE - uppercase hexadecimal (A-F)
 *                          FMT_ZEROPAD   - output should be zero padded (on
 *                                          the left) to the specified width.
 * @param [in]  width - The width of the output field in digits.  If zero or
 *                      less, the output field will not be padded.
 * @param [in]  dot   - The number of value's least significant digits that
 *                      are to the right of the decimal point (zero if none).
 *
 * @return      The number of characters that would be written to the buffer if
 *              enough space had been available.
 *
 * @see         kdoprnt().
 */
int
kprintn(buf_t *desc, UINTMAX_T value, uint base, int flags, int width, int dot)
{
    int   ret = 0;
    /* hold a INTMAX_T in base 8 */
    char *p, buf[(sizeof (INTMAX_T) * 8 / 3) + 1 + 2 /* FMT_ALT + FMT_SIGN */];
    char *q;
    char  hex_a = 'a' - 10;
    char  lpad;

    if (dot != 0)
        width += dot;

    if (flags & FMT_UPPERCASE)
        hex_a = 'A' - 10;  // Use uppercase hex
    if (base < 2)
        base = 16;

    p = buf;
    do {
        uint digit = value % base;
        *p++ = (char) (digit + ((digit <= 9) ? '0' : hex_a));
    } while ((value /= base) != 0);

    q = p;
    if (flags & FMT_ALT && *(p - 1) != '0') {
        if (base == 8) {
            *p++ = '0';
        } else if (base == 16) {
            *p++ = 'x';
            *p++ = '0';
        }
    }
    if (flags & FMT_NEGATIVE)
        *p++ = '-';
    else if (flags & FMT_SIGN)
        *p++ = '+';
    else if (flags & FMT_SPACE)
        *p++ = ' ';

    if (flags & FMT_ZEROPAD)
        lpad = '0';
    else
        lpad = ' ';
    width -= p - buf;
    if ((flags & FMT_LJUST) == 0) {
        while (width-- > 0) {
            put(lpad, desc);
            ret++;
        }
    }
    if ((flags & FMT_LJUST) == 0) {
        while (p > q) {
            put(*--p, desc);
            ret++;
        }
    }
    do {
        if (dot != 0 && dot == p - buf) {
            put('.', desc);
            ret++;
        }
        put(*--p, desc);
        ret++;
    } while (p > buf);

    if ((flags & (FMT_ZEROPAD | FMT_LJUST)) == FMT_LJUST) {
        while (width-- > 0) {
            put(' ', desc);
            ret++;
        }
    }

    return (ret);
}

/**
 * kdoprnt() processes a format string and a variable argument list to
 *           generate formatted output in provided buffer.
 *
 * This function is an enhanced port of the NetBSD kernel function of the
 * same name.
 *
 * @param [out] desc  - Output buffer descriptor.  If NULL, output will be
 *                      directed to the serial console.
 * @param [in]  fmt   - A string describing the format of the output.
 *                      This format string is implemented to be as close
 *                      as possible to that of the UNIX stdio printf()
 *                      function.
 * @param [in]  ap    - A pointer to a variable list of arguments.
 *
 * @return      The number of characters that would have been written to the
 *              provided buffer if enough space had been available.
 *
 * @see         kprintn().
 */
static int
kdoprnt(buf_t *desc, const char *fmt, va_list ap)
{
    int       ret = 0;
    char      *p;
    int       ch;
    UINTMAX_T ul;
    int       flags;
    int       width;
    int       mwidth;
    char     *q;

    for (;;) {
        while ((ch = *fmt++) != '%') {
            if (ch == '\0')
                return (ret);
            put(ch, desc);
            ret++;
        }
        flags  = 0;
        width  = 0;
        mwidth = 2048;  // Limit max string output
reswitch:
        switch (ch = *fmt++) {
            case '#':
                flags |= FMT_ALT;
                goto reswitch;
            case ' ':
                flags |= FMT_SPACE;
                goto reswitch;
            case '-':
                flags |= FMT_LJUST;
                goto reswitch;
            case '+':
                flags |= FMT_SIGN;
                goto reswitch;
            case '*':
                width = va_arg(ap, int);
                if (width < 0) {
                    flags |= FMT_LJUST;
                    width = -width;
                }
                goto reswitch;
            case '.':
                flags |= FMT_DOT;
                ch = *fmt++;
                if (ch == '*') {
                    mwidth = va_arg(ap, int);
                } else {
                    mwidth = 0;
                    for (mwidth = 0; ch >= '0' && ch <= '9'; ch = *fmt++)
                        mwidth = (mwidth * 10) + (ch - '0');
                    fmt--;
                }
                goto reswitch;
            case '0':
                flags |= FMT_ZEROPAD;
                goto reswitch;
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                for (;;) {
                    width *= 10;
                    width += ch - '0';
                    ch = *fmt;
                    if ((unsigned)ch - '0' > 9)
                        break;
                    ++fmt;
                }
                goto reswitch;
            case 'l':
                if (*fmt == 'l') {
                    ++fmt;
                    flags |= FMT_LLONG;
                } else {
                    flags |= FMT_LONG;
                }
                goto reswitch;
            case 't':
                if (sizeof (PTRDIFF_T) == sizeof (long))
                    flags |= FMT_LONG;
                goto reswitch;
            case 'z':
                if (sizeof (ssize_t) == sizeof (long))
                    flags |= FMT_LONG;
                goto reswitch;
            case 'c':
                ch = va_arg(ap, int);
                --width;
                if ((flags & FMT_LJUST) == 0)
                    while (width-- > 0) {
                        put(' ', desc);
                        ret++;
                    }
                put(ch & 0xFF, desc);
                ret++;
                if ((flags & (FMT_ZEROPAD | FMT_LJUST)) == FMT_LJUST)
                    while (width-- > 0) {
                        put(' ', desc);
                        ret++;
                    }
                break;
            case 's':
                p = va_arg(ap, char *);
                if (p == NULL)
                    p = (char *)"(null)";
                for (q = p; *q != '\0'; ++q)
                    continue;
                width -= q - p;
                if ((flags & FMT_LJUST) == 0)
                    while (width-- > 0) {
                        put(' ', desc);
                        ret++;
                    }
                while ((ch = (unsigned char)*p++) != 0) {
                    if (mwidth-- == 0)
                        break;
                    put(ch, desc);
                    ret++;
                }
                if ((flags & (FMT_ZEROPAD | FMT_LJUST)) == FMT_LJUST)
                    while (width-- > 0) {
                        put(' ', desc);
                        ret++;
                    }
                break;
            case 'd':
                ul = (flags & FMT_LLONG) ? va_arg(ap, int64_t) :
                     (flags & FMT_LONG)  ? va_arg(ap, long)    :
                                           va_arg(ap, int);
                if ((INTMAX_T)ul < 0) {
                    ul = (UINTMAX_T) -(INTMAX_T)ul;
                    flags |= FMT_NEGATIVE;
                }
                ret += kprintn(desc, ul, 10, flags, width,
                               flags & FMT_DOT ? mwidth : 0);
                break;
            case 'o':
                ul = (flags & FMT_LLONG) ? va_arg(ap, uint64_t)      :
                     (flags & FMT_LONG)  ? va_arg(ap, unsigned long) :
                                           va_arg(ap, unsigned int);
                ret += kprintn(desc, ul, 8, flags, width, 0);
                break;
            case 'u':
                ul = (flags & FMT_LLONG) ? va_arg(ap, uint64_t)      :
                     (flags & FMT_LONG)  ? va_arg(ap, unsigned long) :
                                           va_arg(ap, unsigned int);
                ret += kprintn(desc, ul, 10, flags, width,
                               flags & FMT_DOT ? mwidth : 0);
                break;
            case 'p':
                flags |= (FMT_LONG | FMT_ALT);
                goto do_hex;
            case 'X':
                flags |= FMT_UPPERCASE;
                goto do_hex;
            case 'x':
do_hex:
                ul = (flags & FMT_LLONG) ? va_arg(ap, uint64_t)      :
                     (flags & FMT_LONG)  ? va_arg(ap, unsigned long) :
                                           va_arg(ap, unsigned int);
                ret += kprintn(desc, ul, 16, flags, width, 0);
                break;
            default:
                if (ch == '\0')
                    return (ret);
                put(ch, desc);
                ret++;
                break;
        }
    }
}

/**
 * vsnprintf() is a stdio compatible function which operates on a buffer, size
 *             format string, and variable argument list.  Output is directed
 *             to the specified buffer.
 *
 * @param [out] buf   - A pointer to the buffer where output is to be stored.
 * @param [in]  size  - The length of the buffer where output is to be stored.
 * @param [in]  fmt   - A string describing the format of the output.  This
 *                      format string is compatible with that of printf().
 * @param [in]  ap    - A pointer to a variable list of arguments.
 *
 * @return      The number of characters (not including the trailing '\\0')
 *              which would have been printed to the output buffer if enough
 *              space had been available. Thus, a return value greater than or
 *              equal to the given size indicates that the output was truncated.
 */
__attribute__((format(__printf__, 3, 0)))
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    int ret;
    buf_t desc;
    desc.buf_cur = buf;
    desc.buf_end = (buf != NULL) ? buf + size - 1 : buf;

    ret = kdoprnt(&desc, fmt, ap);
    if (buf != NULL)
        *desc.buf_cur = '\0';   // terminate the string
    return (ret);
}

/**
 * snprintf() is a stdio compatible function which operates on a buffer, size,
 *            format string, and variable argument list.  Output is directed
 *            to the specified buffer.
 *
 * @param [out] buf   - A pointer to the buffer where output is to be stored.
 * @param [in]  size  - The length of the buffer where output is to be stored.
 * @param [in]  fmt   - A string describing the format of the output.  This
 *                      format string is compatible with that of printf().
 * @param [in]  ...   - A variable list of arguments.
 *
 * @return      The number of characters (not including the trailing '\\0')
 *              which would have been printed to the output buffer if enough
 *              space had been available. Thus, a return value greater than or
 *              equal to the given size indicates that the output was truncated.
 */
__attribute__((format(__printf__, 3, 4)))
int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int     rc;

    va_start(ap, fmt);
    rc = vsnprintf(buf, size, fmt, ap);
    va_end(ap);

    return (rc);
}

/**
 * sprintf() is a stdio compatible function which operates on a buffer,
 *           format string, and variable argument list.  Output is directed
 *           to the specified buffer.  A maximum of 80 characters (including the
 *           terminating '\\0') may be written to the buffer.  Use snprintf() to
 *           write larger buffers.
 *
 * @param [out] buf   - A pointer to the buffer where output is to be stored.
 * @param [in]  fmt   - A string describing the format of the output.  This
 *                      format string is compatible with that of printf().
 * @param [in]  ...   - A variable list of arguments.
 *
 * @return      The number of characters (not including the trailing '\\0') that
 *              would have been written to the buffer if the output was not
 *              limited to 80 bytes.
 *
 * @see         snprintf();
 */
__attribute__((format(__printf__, 2, 3)))
int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int     rc;
    size_t  size = 80;

    va_start(ap, fmt);
    rc = vsnprintf(buf, size, fmt, ap);
    va_end(ap);

    return (rc);
}

/**
 * vprintf() is a stdio compatible function which operates on a format
 *           string and variable argument list.  Output is directed to
 *           the serial console.
 *
 * @param [in]  fmt   - A string describing the format of the output.  This
 *                      format string is compatible with that of printf().
 * @param [in]  ap    - A pointer to a variable list of arguments.
 *
 * @return      The number of bytes written to the serial console.
 */
__attribute__((format(__printf__, 1, 0)))
int vprintf(const char *fmt, va_list ap)
{
    return (kdoprnt(NULL, fmt, ap));
}

/**
 * printf() is a stdio compatible function which operates on a format
 *          string and variable argument list.  Output is directed to
 *          the serial console.
 *
 * @param [in]  fmt - A string describing the format of the output.  This
 *                    format string is compatible with that of printf().
 * @param [in]  ... - A variable list of arguments.
 *
 * @return      The number of bytes written to the serial console.
 */
__attribute__((format(__printf__, 1, 2)))
int printf(const char *fmt, ...)
{
    int rc;
    va_list args;

    va_start(args, fmt);
    rc = vprintf(fmt, args);
    va_end(args);

    return (rc);
}

#undef DO_PRINTF_TEST
#ifdef DO_PRINTF_TEST

/**
 * printf_test() is a quick function to test various printf() features.
 *               Output may be compared against the stdio printf() function
 *               by defining the CHECK_AGAINST_HOST_PRINTF macro in printf.h.
 *
 * This function requires no arguments.
 *
 * @return      None.
 */
void
printf_test(void)
{
    printf("printf test");
    printf("\n|                              | 00 Blank line\n");
    printf("|%%%c", 'w');
    printf("o");
    printf("%c", 'r');
    printf("%-2c%c%2s%1s%%", 'd', 'W', "OR", "D");
    printf("%19s| 01 \"%cword WORD%%\", left justified\n", "", '%');
    printf("|%-30s| 02 \"word\", left justified\n", "word");
    printf("|%-30.4s| 03 \"WORD\", left justified\n", "WORD");
    printf("|%*s| 04 \"word\", left justified\n", -30, "word");
    printf("|%*.*s| 05 \"WORD\", left justified\n", -30, 4, "WORD");
    printf("|%-30x| 06 \"c001\", left justified\n", 0xc001);
    printf("|%-30d| 07 \"-1\", left justified\n", -1);
    printf("|%-+30d| 08 \"+1\", left justified\n", +1);
    printf("|%+30d| 09 \"-1\" right justified\n", -1);
    printf("|%+30d| 10 \"+1\" right justified\n", 1);
    printf("|%30s| 11 \"word\", right justified\n", "word");
    printf("|%28.4s%.2s| 12 \"WORD\", right justified\n", "WO", "RD");
    printf("|%*s| 13 \"word\", right justified\n", 30, "word");
    printf("|%*.*s| 14 \"WORD\", right justified\n", 30, 4, "WORD");
    printf("|%30X| 15 \"E1D2C3B4\", right justified\n", 0xe1d2c3b4);
    printf("|%30lu| 16 \"4294967295\", right justified\n", 4294967295UL);
    printf("|%030x| 17 \"000...0baddad\", fully zero padded\n", 0x0baddad);
    printf("|%030x| 18 \"000...000\" all zeros\n", 0);
    printf("All separators should line up, and data must be as described "
           "on the right.\n\n");
}
#endif /* DO_PRINTF_TEST */
