/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Kicksmash command interface
 */

#ifndef _SMASH_CMD_H
#define _SMASH_CMD_H

/* Command codes sent to Kicksmash */
#define KS_CMD_NULL          0x00  // Do nothing (no reply)
#define KS_CMD_NOP           0x01  // Do nothing but reply
#define KS_CMD_ID            0x02  // Send KS ID and configuration
#define KS_CMD_UPTIME        0x03  // Send KS uptime in microseconds (64-bit)
#define KS_CMD_CLOCK         0x04  // Get or set Amiga format time (sec + usec)
#define KS_CMD_TESTPATT      0x05  // Send test pattern
#define KS_CMD_LOOPBACK      0x06  // Reply with (exact) sent message
#define KS_CMD_SET           0x07  // Set Kicksmash value (options in high bits)
#define KS_CMD_FLASH_READ    0x10  // Generate flash read mode sequence
#define KS_CMD_FLASH_CMD     0x11  // Generate low level command to EEPROM
#define KS_CMD_FLASH_ID      0x12  // Generate flash ID sequence
#define KS_CMD_FLASH_ERASE   0x13  // Generate flash erase sequence
#define KS_CMD_FLASH_WRITE   0x14  // Generate flash write sequence
#define KS_CMD_FLASH_MWRITE  0x15  // Flash write multiple (not implemented)
#define KS_CMD_BANK_INFO     0x20  // Get ROM bank information structure
#define KS_CMD_BANK_SET      0x21  // Set bank (options in high bits)
#define KS_CMD_BANK_MERGE    0x22  // Merge or unmerge banks
#define KS_CMD_BANK_NAME     0x23  // Set a bank name
#define KS_CMD_BANK_LRESET   0x24  // Set bank longreset sequence
#define KS_CMD_MSG_STATE     0x30  // Application state (for remote message)
#define KS_CMD_MSG_INFO      0x31  // Query message queue sizes
#define KS_CMD_MSG_SEND      0x32  // Send a remote message
#define KS_CMD_MSG_RECEIVE   0x33  // Receive a remote message
#define KS_CMD_MSG_LOCK      0x34  // Lock or unlock message buffers
#define KS_CMD_MSG_FLUSH     0x35  // Flush and discard message buffer(s)

/* Status codes returned by Kicksmash */
#define KS_STATUS_OK       0x0000  // Success
#define KS_STATUS_FAIL     0x0100  // Generic failure
#define KS_STATUS_CRC      0x0200  // CRC failure
#define KS_STATUS_UNKCMD   0x0300  // Unknown command
#define KS_STATUS_BADARG   0x0400  // Bad command argument
#define KS_STATUS_BADLEN   0x0500  // Bad message length
#define KS_STATUS_NODATA   0x0600  // No data available
#define KS_STATUS_LOCKED   0x0700  // Resource locked
#define KS_STATUS_LAST_ENT 0x0800  // Fake status: must always be last + 1


/* Command-specific options (upper byte of command) */
#define KS_SET_NAME        0x0100  // Set board name

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

#define KS_MSG_STATE_SET   0x0100  // Update Amiga-side app state

#define KS_HDR_AND_CRC_LEN (8 + 2 + 2 + 4)  // Magic+Len+Cmd+CRC = 16 bytes

/* Application state bits */
#define MSG_STATE_SERVICE_UP    0x0001  // Message service running
#define MSG_STATE_HAVE_LOOPBACK 0x0002  // Loopback service available
#define MSG_STATE_HAVE_FILE     0x0004  // File service available

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
 *        firmware version and compile options. See smash_id_t.
 *   KS_CMD_UPTIME
 *        Report Kicksmash uptime in microseconds. This is a 64-bit value
 *        which will be in big endian format, suitable for direct use by
 *        an AmigaOS program.
 *   KS_CMD_CLOCK
 *        Return Amiga-format clock as two 32-bit values (seconds and
 *        microseconds since 1970). This command will only return 0 values
 *        unless a set has been performed since the last powercycle or
 *        Kicksmash reset (KS_CLOCK_SET option). Use KS_CLOCK_SET_IFNOT to
 *        optionally set if it's not already set.
 *   KS_CMD_TESTPATT
 *        Kicksmash will send a test pattern which includes 28x 32-bit
 *        values which test all bit values and many combinations. The
 *        following values are sent:
 *            0x54455354, 0x50415454, 0x202d2053, 0x54415254,
 *            0x5555aaaa, 0x3333cccc, 0x1111eeee, 0x99996666,
 *            0x01000200, 0x04000800, 0x10002000, 0x40008000,
 *            0x00010002, 0x00040008, 0x00100020, 0x00400080,
 *            0xfefffdff, 0xfbfff7ff, 0xefffdfff, 0xbfff7fff,
 *            0xfffefffd, 0xfffbfff7, 0xffefffdf, 0xffbfff7f,
 *            0x54455354, 0x50415454, 0x20454e44, 0x20636468,
 *   KS_CMD_LOOPBACK
 *        The sent pattern is returned intact, without recalculating CRC.
 *        This means that the status will remain KS_CMD_LOOPBACK instead
 *        of a typical 0 value for success.
 *   KS_CMD_FLASH_READ
 *        The flash will be put in read mode. This is the normal mode
 *        which is used to read stored flash data. The reply data from this
 *        command are the sequence of address offsets that the Amiga code
 *        must generate in the ROM address space. The Amiga must manipulate
 *        the address relative to the ROM and the address mode of the Amiga's
 *        ROM. Example, if the following single address value is returned:
 *            0x00555
 *        then a the program running on an Amiga 3000 must shift this address
 *        left by two bits and then add the ROM base (0x00f80000). Thus, the
 *        address to read will be 0x00f81554.
 *       *This command requires participation by code running under AmigaOS
 *        to generate the correct bus addresses to sequence the flash command.
 *   KS_CMD_FLASH_CMD
 *        A custom flash command sequence will be issued to the flash. The
 *        flash command addresses and data must be specified as an argument
 *        to this command. The first half of the values are the addresses
 *        to generate, which are then provided as a response to this command.
 *        The second half of the values are the data values which will be
 *        generated while the Amiga generates the address. It is up to the
 *        caller to correctly specify either 16-bit or 32-bit values as the
 *        data values depending on the mode in which KS is running against
 *        the flash. It is not necessary to provide a count of the number of
 *        addresses and data. That count is computed based on the total
 *        data count provided. An example of the flash ID command sequence
 *        would be as follows:
 *            0x00000555, 0x000002aa, 0x00000555,
 *            0x00aa00aa, 0x00550055, 0x00900090
 *        The first three values are the addresses to generate. These values
 *        are provided in the reply message. The following three values are
 *        the data values to generate. The main reason the values are
 *        provided in the reply message is that the calling code may be
 *        implemented in common with the other flash operation commands.
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
 *        The final address should be the address within the flash sector
 *        which is to be erased. It is necessary for calling code to first
 *        select the appropriate flash bank (KS_CMD_BANK_SET) on which to
 *        operate.
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
 *   KS_CMD_SET
 *        Set Kicksmash value. The following option must be specified with
 *        this command:
 *            KS_SET_NAME - Set the board name. A string of up to 16
 *                          characters (including ending NIL) follows.
 *   KS_CMD_BANK_INFO
 *        ROM bank information, having structure bank_info_t, will be
 *        returned to the requester. This information includes the current
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
 *        last bank in the group must be specified within a 16-bit argument.
 *        The high byte is the start bank number. The low byte is the end
 *        bank number. Adding the following command option will un-merge
 *        banks:
 *            KS_BANK_UNMERGE
 *   KS_CMD_BANK_NAME
 *        This command is used to set a string name for a bank. The first
 *        16-bit value is the bank number, and following string (up to 15
 *        characters) is the new bank name.
 *   KS_CMD_BANK_LRESET
 *        This command is used to specify the long reset sequence. Up to
 *        8 banks may be specified in the sequence, and the command length
 *        is always 8 bytes. Unused bank numbers must be set to 0xff values.
 *   KS_CMD_MSG_STATE
 *        Get application state information which is shared between Amiga
 *        and USB. Each is a 16-bit value:
 *              uint16_t app_state_amiga;
 *              uint16_t app_state_usb;
 *        A given side's values will be automatically reset to zero if not
 *        updated by that side within 10 seconds (no bits need to change on
 *        the update).
 *        Add KS_MSG_STATE_SET flag to instead update Amiga application state
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
 *        See below for payload format.
 *   KS_CMD_MSG_RECEIVE
 *        If there is data pending from the USB host, it will be returned to
 *        the Amiga in the buffer, given there is sufficient space available.
 *        See below for payload format.
 *   KS_CMD_MSG_LOCK
 *        A single value is specified, which are the lock bits:
 *              bit 0 = lock buffer 1 from Amiga access
 *              bit 1 = lock buffer 2 from Amiga access
 *              bit 2 = lock buffer 1 from USB access
 *              bit 3 = lock buffer 2 from USB access
 *        If the command code includes KS_MSG_UNLOCK, then the specified
 *        lock bits will be unlocked.
 *   KS_CMD_MSG_FLUSH
 *        The receive message buffer will be flushed. For the Amiga, this
 *        is the USB-to-Amiga buffer. If KS_MSG_ALTBUF is specified, then the
 *        opposite-direction buffer will be flushed.
 *
 * The payload of KS_CMD_MSG_SEND is normal byte order on the Amiga side,
 * but is byte-swapped when the USB host is dealing with the data. This is
 * an artifact of how the STM32 DMA works with GPIO ports. The USB host is
 * responsible for byte swapping messages, both for receive and transmit.
 * A message payload includes the following header structure:
 *      uint8_t  km_op;        // Operation to perform (KM_OP_*)
 *      uint8_t  km_status;    // Status reply
 *      uint16_t km_tag;       // Sequence number
 * All header fields except the status reply are preserved from a request
 * message to the reply message.
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
    uint8_t  si_unused1;                 // Unused space
    uint16_t si_usbdev;                  // USB device slot
    uint8_t  si_unused[24];              // Unused space
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

typedef struct {
    uint8_t  km_op;        // Operation to perform (KM_OP_*)
    uint8_t  km_status;    // Status reply
    uint16_t km_tag;       // Message tag or sequence number
} km_msg_hdr_t;

#endif /* _SMASH_CMD_H */
