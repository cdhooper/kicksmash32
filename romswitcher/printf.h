/*
 * printf and debug output functions.
 *
 * This header file is part of the code base for a simple Amiga ROM
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
#ifndef _PRINTF_H
#define _PRINTF_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FILE      _stdio_FILE
#define fprintf   _stdio_fprintf
#define fputs     _stdio_fputs
#define fputc     _stdio_fputc
#define fread     _stdio_fread
#define getchar   _stdio_getchar
#define printf    _stdio_printf
#define putchar   _stdio_putchar
#define puts      _stdio_puts
#define snprintf  _stdio_snprintf
#define sprintf   _stdio_sprintf
#define ungetc    _stdio_ungetc
#define vprintf   _stdio_vprintf
#define vsnprintf _stdio_vsnprintf

#if 0
#include <stdio.h>
#endif
#undef FILE
#undef fprintf
#undef fputs
#undef fputc
#undef fread
#undef getchar
#undef printf
#undef putchar
#undef puts
#undef snprintf
#undef sprintf
#undef stdin
#undef stdout
#undef stderr
#undef ungetc
#undef vprintf
#undef vsnprintf
#undef __sputc_r
#undef __sputc

/* Redefine stdio functions and definitions */
#define __FILE_defined
#define stdin         ((FILE *) 0)
#define stdout        ((FILE *) 1)
#define stderr        ((FILE *) 2)
#define fileno(x)     (x)
#define fflush(x)
typedef void *FILE;

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
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

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
int snprintf(char *buf, size_t size, const char *fmt, ...);

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
int sprintf(char *buf, const char *fmt, ...);

/**
 * vprintf() is a stdio compatible function which operates on a format
 *           string and variable argument list.  Output is directed to
 *           the serial console.
 *
 * @param [in]  fmt   - A string describing the format of the output.  This
 *                      format string is compatible with that of printf().
 * @param [in]  ap    - A pointer to a variable list of arguments.
 *
 * @return      The number of characters written to the serial console.
 */
__attribute__((format(__printf__, 1, 0)))
int vprintf(const char *fmt, va_list ap);

/**
 * printf() is a stdio compatible function which operates on a format
 *          string and variable argument list.  Output is directed to
 *          the serial console.
 *
 * @param [in]  fmt - A string describing the format of the output.  This
 *                    format string is compatible with that of printf().
 * @param [in]  ... - A variable list of arguments.
 *
 * @return      The number of characters written to the serial console.
 */
__attribute__((format(__printf__, 1, 2)))
int printf(const char *fmt, ...);

/*
 * scanf() uses a format string to derive specified values from input buffer>
 *
 * @param [in]  str - A file pointer which is used to describe the input
 *                    buffer.
 * @param [in]  fmt - The scanf() format string to process.
 * @param [out] ... - A variable list of arguments.
 *
 * @return      The number of format items successfully matched and assigned.
 */
__attribute__((format(__scanf__, 1, 3)))
int sscanf(const char *str, char const *fmt, ...);


/**
 * errx() is a stdio compatible function which operates on a format
 *        string and variable argument list.  Output is directed to
 *        the serial console.
 *
 * @param [in]  rc  - Ignored.
 * @param [in]  fmt - A string describing the format of the output.  This
 *                    format string is compatible with that of printf().
 * @param [in]  ... - A variable list of arguments.
 *
 * @return      This function does not return.
 */
__attribute__((format(__printf__, 2, 3))) __attribute__((noreturn))
void errx(int rc, const char *fmt, ...);

/**
 * warnx() is a stdio compatible function which operates on a format
 *         string and variable argument list.  Output is directed to
 *         the serial console.
 *
 * @param [in]  fmt - A string describing the format of the output.  This
 *                    format string is compatible with that of printf().
 * @param [in]  ... - A variable list of arguments.
 *
 * @return      None.
 */
__attribute__((format(__printf__, 1, 2)))
void warnx(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#undef DO_PRINTF_TEST
#ifdef DO_PRINTF_TEST
/**
 * printf_test() is a quick function to test various printf() features.
 *               Output may be compared against the stdio printf() function
 *               by defining the CHECK_AGAINST_HOST_PRINTF macro in printf.h.
 *
 * This function requires no arguments.
 *
 * @return      RC_SUCCESS
 */
int printf_test(void);
#endif

#define EXIT_FAILURE 1

int putchar(int ch);
int puts(const char *str);
int getchar(void);

extern volatile uint8_t gui_wants_all_input;  // GUI wants all keyboard input

#endif /* _PRINTF_H */
