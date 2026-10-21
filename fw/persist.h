/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2026.
 *
 * ---------------------------------------------------------------------
 *
 * Generic POSIX function emulation.
 */

#ifndef _PERSIST_H
#define _PERSIST_H

/* Force a variable into the custom "persist" section */
#define SRAM_PERSIST __attribute__((section(".persist,\"aw\",%nobits@")))

#define SP_MAGIC 0x734422ce  // Magic number which starts persist values

#define RESET_TO_DFU_ROM_MAGIC 0xd0df00ba  // STM32 ROM DFU programmer
#define RESET_TO_DFU_MAGIC     0xd0df0bee  // Stand-alone DFU (in firmware)

typedef struct {
    uint32_t sp_magic;         // Magic number (SP_MAGIC)
    uint32_t sp_addr;          // Address of start of this structure
    uint32_t sp_valid;         // Which fields are valid in this structure
    uint32_t sp_reset_magic;   // Reset mode magic
    uint8_t  sp_bank_current;  // Currently active bank
    uint8_t  sp_bank_merge;    // Current banks merged
    uint8_t  sp_unused[110];   // Unused values (0x00)
} sram_persist_t;

SRAM_PERSIST extern sram_persist_t persist;

void persist_init(void);

#endif /* _PERSIST_H */
