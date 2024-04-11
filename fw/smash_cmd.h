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
#define KS_CMD_NOP           0x01  // Do nothing but reply
#define KS_CMD_ID            0x02  // Reply with KS ID and configuration
#define KS_CMD_UPTIME        0x03  // Report KS uptime in microseconds (64-bit)
#define KS_CMD_TESTPATT      0x04  // Reply with bit test pattern
#define KS_CMD_LOOPBACK      0x05  // Reply with (exact) sent message
#define KS_CMD_FLASH_READ    0x06  // Generate flash read mode sequence
#define KS_CMD_FLASH_CMD     0x07  // Issue low level command to EEPROM
#define KS_CMD_FLASH_ID      0x08  // Generate flash ID sequence
#define KS_CMD_FLASH_ERASE   0x09  // Generate flash erase sequence
#define KS_CMD_FLASH_WRITE   0x0a  // Generate flash write sequence
#define KS_CMD_FLASH_MWRITE  0x0b  // Flash write multiple (not implemented)
#define KS_CMD_BANK_INFO     0x10  // Get ROM bank information structure
#define KS_CMD_BANK_SET      0x11  // Set bank (options in high bits)
#define KS_CMD_BANK_MERGE    0x12  // Merge or unmerge banks
#define KS_CMD_BANK_NAME     0x13  // Set a bank name
#define KS_CMD_BANK_LRESET   0x14  // Set bank longreset sequence
#define KS_CMD_CLOCK         0x18  // Get or set Amiga format time (sec + usec)
#define KS_CMD_APP_STATE     0x19  // Application state
#define KS_CMD_MSG_INFO      0x20  // Query message queue sizes
#define KS_CMD_MSG_SEND      0x21  // Send a remote message
#define KS_CMD_MSG_RECEIVE   0x22  // Receive a remote message
#define KS_CMD_MSG_LOCK      0x23  // Lock or unlock message buffers

/* Status codes returned by Kicksmash */
#define KS_STATUS_OK       0x0000  // Success
#define KS_STATUS_FAIL     0x0100  // Generic failure
#define KS_STATUS_CRC      0x0200  // CRC failure
#define KS_STATUS_UNKCMD   0x0300  // Unknown command
#define KS_STATUS_BADARG   0x0400  // Bad command argument
#define KS_STATUS_BADLEN   0x0500  // Bad message length
#define KS_STATUS_NODATA   0x0600  // No data available
#define KS_STATUS_LOCKED   0x0700  // Resource locked

/* Command-specific options (upper byte of command) */
#define KS_BANK_SETCURRENT 0x0100  // Set current ROM bank (immediate change)
#define KS_BANK_SETRESET   0x0200  // Set ROM bank in effect at next reset
#define KS_BANK_SETPOWERON 0x0400  // Set ROM bank in effect at cold poweron
#define KS_BANK_SETTEMP    0x1000  // Temporarily set ROM bank (unmerged)
#define KS_BANK_UNSETTEMP  0x2000  // Remove temporary ROM bank setting
#define KS_BANK_REBOOT     0x8000  // Option to reboot Amiga when complete

#define KS_BANK_UNMERGE    0x0100  // Unmerge bank range (KS_BANK_MERGE)

#define KS_MSG_ALTBUF      0x0100  // Perform operations on alternate buffer

#define KS_MSG_UNLOCK      0x0100  // Unlock instead of lock

#define KS_CLOCK_SET       0x0100  // Set Amiga-relative clock
#define KS_CLOCK_SET_IFNOT 0x0200  // Set Amiga-relative clock only if not set

#define KS_APP_STATE_SET   0x0100  // Update Amiga-side app state

#define KS_HDR_AND_CRC_LEN (8 + 2 + 2 + 4)  // Magic+Len+Cmd+CRC = 16 bytes

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
 *        firwmare version and compile options. See smash_id_t.
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
 *   KS_CMD_FLASH_CMD
 *        A custom flash command sequence will be issued to the flash. The
 *        flash command data sequence must be specified as an argument to
 *        this command. This command is not yet implemented.
 *   KS_CMD_FLASH_READ
 *        The flash will be put in read mode (this is the normal mode
 *        which is used to read stored flash data). The reply data from this
 *        command are the sequence of addresses that the Amiga code must
 *        generate.
 *       *This command requires participation by code running under AmigaOS
 *        to generate the correct bus addresses to sequence the flash command.
 *   KS_CMD_FLASH_ID
 *        The flash will be put in ID mode, which reports standard CFI
 *        information instead of the stored flash data. Use KS_CMD_FLASH_READ
 *        to return the flash array to read mode.
 *       *This command requires participation by code running under AmigaOS
 *        to generate the correct bus addresses to sequence the flash command.
 *   KS_CMD_FLASH_ERASE
 *        A single flash sector will be erased. Data returned from this command
 *        include the sequences of addresses that the Amiga code must generate.
 *       *This command requires participation by code running under AmigaOS
 *        to generate the correct bus addresses to sequence the flash command.
 *   KS_CMD_FLASH_WRITE
 *        A single 32-bit or 16-bit value may be written at a time. This
 *        value is presented as data to KS_CMD_FLASH_CMD. The reply data
 *        are the unlock addresses which must be generated. The Amiga program
 *        must generate reads of those specified addresses followed by a
 *        read of the data address to write.
 *       *This command requires participation by code running under AmigaOS
 *        to generate the correct bus addresses to sequence the flash command.
 *   KS_CMD_FLASH_MWRITE
 *        This command will set up a multiple data write sequence for the
 *        flash. It is not currently implemented.
 *   KS_CMD_BANK_INFO
 *        ROM bank information, having structure bank_info_t,  will be
 *        returnsed to the requester. This information includes the current
 *        bank, bank at next reset, bank at cold power-on, the sequence of
 *        banks to step through when a long reset is detected, an encoding
 *        of which banks are merged into larger bank (1M, 2M, 4M, 8M), and
 *        finally a short description of each bank (name). This information
 *        is stored independent of the actual flash contents, and will
 *        persist across reboots and power cycles.
 *   KS_CMD_BANK_SET
 *        This command may set the non-volatile bank for current, reset,
 *        power on, or may be used to temporarily change the bank for
 *        inspection of another bank. One of the following command options
 *        must be specified to select which operation should be performed:
 *            KS_BANK_SETCURRENT
 *            KS_BANK_SETRESET
 *            KS_BANK_SETPOWERON
 *            KS_BANK_SETTEMP
 *            KS_BANK_UNSETTEMP
 *        In addition, the Amiga may be optionally rebooted immediately
 *        after setting is complete by adding this option:
 *            KS_BANK_REBOOT
 *   KS_CMD_BANK_MERGE
 *        This command is used to merge a sequence of banks. The first and
 *        last bank in the group must be specified. Adding the following
 *        command option will un-merge banks:
 *            KS_BANK_UNMERGE
 *   KS_CMD_BANK_NAME
 *        This command is used to set a string name for a bank. The first
 *        16-bit value is the bank number, and following string (up to 15
 *        characters) is the new bank name.
 *   KS_CMD_BANK_LRESET
 *        This command is used to specify the long reset sequence. Up to
 *        8 banks may be specified in the sequence, and the command length
 *        is always 8 bytes. Unused bank numbers must be set to 0xff values.
 *   KS_CMD_CLOCK
 *        Return Amiga-format clock as two 32-bit values (seconds and
 *        microseconds since 1970). This command will only return 0 values
 *        unless a set has been performed since the last powercycle or
 *        Kicksmash reset (KS_CLOCK_SET option). Use KS_CLOCK_SET_IFNOT to
 *        optionall set if it's not already set.
 *   KS_CMD_APP_STATE
 *        Get application state information which is shared between Amiga
 *        and USB. Each is a 16-bit value:
 *              uint16_t app_state_amiga;
 *              uint16_t app_state_usb;
 *        A given side's values will be automatically reset to zero if not
 *        updated by that side within 10 seconds (no bits need to change on
 *        the update).
 *        Add KS_APP_STATE_SET flag to instead update Amiga application state
 *        (provide a value of bits to affect, the new value, and optional
 *        expiration):
 *              uint16_t amiga_bits_to_affect;
 *              uint16_t amiga_new_bits;
 *              uint16_t expire_msec;  // Time until expiration <= 65 seconds
 *        If expiration is not provided, it will default to 10 seconds.
 *   KS_CMD_MSG_INFO
 *        A structure is returned with message buffer space in use and
 *        space available for both the Amiga -> USB Host (atou)
 *        and USB Host -> Amiga (utoa) buffers.
 *              uint16_t smi_atou_inuse;
 *              uint16_t smi_atou_avail;
 *              uint16_t smi_utoa_inuse;
 *              uint16_t smi_utoa_avail;
 *              uint16_t smi_app_state_amiga;
 *              uint16_t smi_app_state_usb;
 *   KS_CMD_MSG_SEND
 *        Any data provided, including Header and CRC, is sent to the USB host.
 *   KS_CMD_MSG_RECV
 *        If there is data pending from the USB host, it will be returned to
 *        the Amiga in the buffer, given there is sufficient space available.
 *   KS_CMD_MSG_LOCK
 *        A single value is specified, which are the lock bits:
 *              bit 0 = lock buffer 1 from Amiga access
 *              bit 1 = lock buffer 2 from Amiga access
 *              bit 2 = lock buffer 1 from USB access
 *              bit 3 = lock buffer 2 from USB access
 *        If the command code includes KS_MSG_UNLOCK, then the specified
 *        lock bits will be unlocked.
 */

#define ROM_BANKS 8
typedef struct {
    uint8_t  bi_valid;                    // 0x01 = valid
    uint8_t  bi_bank_current;             // currently active bank
    uint8_t  bi_bank_nextreset;           // bank at next reset
    uint8_t  bi_bank_poweron;             // bank at cold poweron
    uint8_t  bi_longreset_seq[ROM_BANKS]; // 0xff = end of list
    uint8_t  bi_merge[ROM_BANKS];         // bank is merged with next
    char     bi_name[ROM_BANKS][16];      // bank name (description) string
    uint8_t  bi_unused[12];               // Unused space
} bank_info_t;

typedef struct {
    uint16_t si_ks_version[2];           // Kicksmash version    (major-minor)
    uint8_t  si_ks_date[4];              // Kicksmash build date (cc-yy-mm-dd)
    uint8_t  si_ks_time[4];              // Kicksmash build time (hh-mm-ss-00)
    char     si_serial[24];              // Kicksmash serial number
    uint16_t si_features;                // Available features
    uint16_t si_rev;                     // Protocol revision (00.01)
    uint32_t si_usbid;                   // USB id (0x12091610)
    char     si_name[16];                // Unique name for this board
    uint8_t  si_mode;                    // ROM mode (0=32-bit, 1=16-bit)
    uint8_t  si_unused[27];              // Unused space
} smash_id_t;

typedef struct {
    uint16_t smi_atou_inuse;             // Amiga -> USB buffer bytes in use
    uint16_t smi_atou_avail;             // Amiga -> USB buffer bytes free
    uint16_t smi_utoa_inuse;             // USB -> Amiga buffer bytes in use
    uint16_t smi_utoa_avail;             // USB -> Amiga buffer bytes free
    uint16_t smi_state_amiga;            // Amiga connection state
    uint16_t smi_state_usb;              // USB host connection state
    uint8_t  smi_unused[16];             // Unused space
} smash_msg_info_t;

#endif /* _SMASH_H */
