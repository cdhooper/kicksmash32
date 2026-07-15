/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Erase and write STM32 internal flash memory
 */

#ifndef _STM32FLASH_H
#define _STM32FLASH_H

int stm32flash_erase(uint32_t addr, uint len);
int stm32flash_write(uint32_t addr, uint len, void *buf, uint flags);
int stm32flash_read(uint32_t addr, uint len, void *buf);

#define STM32FLASH_FLAG_AUTOERASE 1

#endif /* _STM32FLASH_H */


