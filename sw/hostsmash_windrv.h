/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2026.
 *
 * ---------------------------------------------------------------------
 *
 * Hostsmash Windows driver setup for Kicksmash USB devices.
 */

#ifndef _HOSTSMASH_WINDRV_H
#define _HOSTSMASH_WINDRV_H

#ifdef _WIN32
#include <stdbool.h>

int  windrv_internal_main(int argc, char * const *argv);
bool windrv_fix_dfu(void);
bool windrv_fix_kicksmash(void);
#endif

#endif /* _HOSTSMASH_WINDRV_H */
