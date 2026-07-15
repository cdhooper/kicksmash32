/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * CRC-32 calculator.
 */

#ifndef _CRC32_H
#define _CRC32_H

uint32_t crc32(uint32_t oldcrc, const void *data, size_t len);
uint32_t crc32r(uint32_t oldcrc, const void *data, size_t len);
uint32_t crc32s(uint32_t oldcrc, const void *data, size_t len);

#endif /* _CRC32_H */
