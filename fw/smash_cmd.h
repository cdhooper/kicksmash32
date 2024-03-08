/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * main prototypes.
 */

#ifndef _SMASH_H
#define _SMASH_H

/* Command codes sent to Kicksmash */
#define KS_CMD_NULL          0x00  // Do nothing
#define KS_CMD_NOP           0x01  // Do nothing
#define KS_CMD_ID            0x02  // Reply with software ID
#define KS_CMD_UPTIME        0x03  // Report KS uptime in microseconds
#define KS_CMD_TESTPATT      0x04  // Reply with bit test pattern
#define KS_CMD_LOOPBACK      0x05  // Reply with sent message
#define KS_CMD_FLASH_READ    0x06  // Generate flash read mode sequence
#define KS_CMD_FLASH_CMD     0x07  // Issue low level command to EEPROM
#define KS_CMD_FLASH_ID      0x08  // Generate flash ID sequence
#define KS_CMD_FLASH_WRITE   0x09  // Generate flash write sequence
#define KS_CMD_FLASH_ERASE   0x0a  // Generate flash erase sequence
#define KS_CMD_BANK_INFO     0x10  // Get ROM bank information structure
#define KS_CMD_BANK_SET      0x11  // Set bank (options in high bits)
#define KS_CMD_BANK_MERGE    0x12  // Merge or unmerge banks
#define KS_CMD_BANK_COMMENT  0x13  // Set a bank comment
#define KS_CMD_BANK_LRESET   0x14  // Set bank longreset sequence
#define KS_CMD_REMOTE_MSG    0x20  // Send or receive a remote message

/* Status codes returned by Kicksmash */
#define KS_STATUS_OK       0x0000  // Success
#define KS_STATUS_FAIL     0x0081  // Generic failure
#define KS_STATUS_CRC      0x0082  // CRC failure
#define KS_STATUS_UNKCMD   0x0083  // Unknown command
#define KS_STATUS_BADARG   0x0084  // Bad command argument
#define KS_STATUS_BADLEN   0x0085  // Bad message length

/* Command-specific options (upper byte of command) */
#define KS_BANK_SETCURRENT 0x0100  // Set current ROM bank (immediate change)
#define KS_BANK_SETRESET   0x0200  // Set ROM bank in effect at next reset
#define KS_BANK_SETPOWERON 0x0400  // Set ROM bank in effect at cold poweron
#define KS_BANK_SETTEMP    0x1000  // Temporarily set ROM bank (unmerged)
#define KS_BANK_UNSETTEMP  0x2000  // Remove temporary ROM bank setting
#define KS_BANK_REBOOT     0x8000  // Option to reboot Amiga when complete

#define KS_BANK_UNMERGE    0x0100  // Unmerge bank range (KS_BANK_MERGE)


/*
 * All Kicksmash commands are encapsulated within a standard message body
 * which includes a 64-bit Magic sequence, Length, Command code, additional
 * data (optional), and final CRC.
 *     Magic (64 bits)
 *        0x0117, 0x0119, 0x1017, 0x0204
 *     Length (16 bits)
 *        The length specifies the number of payload bytes (not including
 *        magic, length, command, or CRC bytes at end). This number may be
 *        zero (0) if only a command is present.
 *     Command or status code (16 bits)
 *        KS_CMD_*
 *     Additional data (if any)
 *     CRC (32 bits)
 *        CRC is over all content except magic (includes length and command).
 *        The CRC algorithm is a big endian version of the CRC hardware unit
 *        present in some STM32 processors.
 * -----------------------------------------------------------------------
 * All commands will generate a response message which is in a similar
 * format: Magic sequence, Length, Status code, additional data (optional),
 * and final CRC.
 * -----------------------------------------------------------------------
 * Kicksmash commands
 *   KS_CMD_NULL
 *        No operation and no response
 *   KS_CMD_NOP
 *        No operation but response indicating success is given
 *   KS_CMD_ID
 *        Provides identification information of Kicksmash, including
 *        firwmare version and compile options.
 *   KS_CMD_TESTPATT
 *        Kicksmash will send a test pattern which includes 28x 32-bit
 *        values which test all bit values and many combinations.
 *   KS_CMD_ROMSEL
 *        Force or release specific ROM address lines A17, A18, and A19.
 *        This is how ROM banks may be selected. Command options include
 *        which address bits are to be forced and whether it should be
 *        a saved state or just a temporary change.
 *        Command options TO BE DOCUMENTED.
 *   KS_CMD_LOOPBACK
 *        The sent pattern is returned intact.
 *   KS_CMD_FLASH_READ
 *        The flash will be put in read mode (this is the normal mode
 *        which is used to read stored flash data).
 *       *This command requires participation by code running under AmigaOS
 *        to generate the correct bus addresses to sequence the flash command.
 *   KS_CMD_FLASH_ID
 *        The flash will be put in ID mode, which reports standard CFI
 *        information instead of the stored flash data. Use KS_CMD_FLASH_READ
 *        to return the flash array to read mode.
 *       *This command requires participation by code running under AmigaOS
 *        to generate the correct bus addresses to sequence the flash command.
 *   KS_CMD_FLASH_WRITE
 *        A sequence of data (up to one page) will be written to the flash
 *        array. The data to be written must be provided as an argument to
 *        this command.
 *       *This command requires participation by code running under AmigaOS
 *        to generate the correct bus addresses to sequence the flash command.
 *   KS_CMD_FLASH_ERASE
 *        A single flash sector will be erased.
 *       *This command requires participation by code running under AmigaOS
 *        to generate the correct bus addresses to sequence the flash command.
 *   KS_CMD_FLASH_CMD
 *        A custom flash command sequence will be issued to the flash. The
 *        flash command sequence must be specified as the command option.
 *       *This command requires participation by code running under AmigaOS
 *        to generate the correct bus addresses to sequence the flash command.
 */

#define ROM_BANKS 8
typedef struct {
    uint8_t  bi_valid;                    // 0x01 = valid
    uint8_t  bi_bank_current;             // currently active bank
    uint8_t  bi_bank_nextreset;           // bank at next reset
    uint8_t  bi_bank_poweron;             // bank at cold poweron
    uint8_t  bi_longreset_seq[ROM_BANKS]; // 0xff = end of list
    uint8_t  bi_merge[ROM_BANKS];         // bank is merged with next
    char     bi_desc[ROM_BANKS][16];      // bank description string
    uint8_t  bi_unused[12];                // Unused space
} bank_info_t;

typedef struct {
    uint32_t si_rev;                     // Protocol revision (xxxx 00.01)
    uint32_t si_usbid;                   // USB id (0x12091610)
    uint32_t si_ks_version;              // Kicksmash version
    uint8_t  si_ks_date[4];              // Kicksmash build date (cc-yy-mm-dd)
    uint8_t  si_ks_time[4];              // Kicksmash build time (hh-mm-ss-00)
    uint32_t si_features;                // Available features
    uint8_t  si_unused[16];              // Unused space
} smash_id_t;

#endif /* _SMASH_H */
