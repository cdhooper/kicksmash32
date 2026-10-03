/*
 * smashtool
 * ---------
 * Workbench tool to manage a KickSmash installed in an Amiga. This
 * program combines some of the features of the command line smash utility
 * with the ROM bank presentation of the KickSmash ROM Switcher.
 *
 *   - View and edit the board name, bank names, LongReset sequence,
 *     PowerOn bank and ROM Switcher Auto Switch settings
 *   - Merge and unmerge ROM banks
 *   - Switch to another ROM bank and reboot
 *   - Program a ROM image file into a bank, save a bank to a ROM image
 *     file, or verify a bank against a ROM image file, with optional
 *     byte swapping of the image
 *   - Set the Amiga clock from the USB host or from KickSmash, or save
 *     the Amiga clock to KickSmash
 *
 * Copyright 2026 Chris Hooper. This program and source may be used
 * and distributed freely, for any purpose which benefits the Amiga
 * community. Commercial use of the binary, source, or algorithms requires
 * prior written approval from Chris Hooper <amiga@cdh.eebugs.com>.
 * All redistributions must retain this Copyright notice.
 *
 * Large portions of this program were implemented by Claude AI based
 * on reference sources in other code written by Chris Hooper. Some
 * portions of the code were implemented completely by Claude in response
 * to specific prompts.
 *
 * DISCLAIMER: THE SOFTWARE IS PROVIDED "AS-IS", WITHOUT ANY WARRANTY.
 * THE AUTHOR ASSUMES NO LIABILITY FOR ANY DAMAGE ARISING OUT OF THE USE
 * OR MISUSE OF THIS UTILITY OR INFORMATION REPORTED BY THIS UTILITY.
 */
#ifndef VERSION
#define VERSION "0.0"
#endif
#ifndef BUILD_DATE
#define BUILD_DATE __DATE__
#endif
#ifndef BUILD_TIME
#define BUILD_TIME __TIME__
#endif
const char *version = "\0$VER: smashtool "VERSION" ("BUILD_DATE") "
                      "\xA9 Chris Hooper";

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include <clib/dos_protos.h>
#include <fcntl.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/datetime.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <intuition/screens.h>
#include <intuition/sghooks.h>
#include <graphics/gfxbase.h>
#include <libraries/gadtools.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <libraries/asl.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include "smash_cmd.h"
#include "host_cmd.h"
#include "sm_msg.h"
#include "cpu_control.h"

#ifndef BIT
#define BIT(x) (1U << (x))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))
#endif

#define ROM_BASE         0x00f80000   // Base address of Kickstart ROM
#define ROM_WINDOW_SIZE  (512 << 10)  // 512 KB
#define MAX_CHUNK        (16 << 10)   // 16 KB flash read / verify chunk
#define WRITE_CHUNK      (4 << 10)    // 4 KB flash write chunk

/*
 * Window layout
 * -------------
 * The window is laid out in units of the font cell, so it scales with the
 * font. The system default font (GfxBase->DefaultFont, which is always
 * fixed width) is used when the resulting window fits on the screen.
 * Otherwise the ROM topaz 8 font is used. Coordinates are relative to
 * the inside of the window borders.
 *
 * A compact layout is used when the screen is not tall enough for the
 * normal layout (the compact topaz 8 layout fits a 640x200 Workbench).
 */
#define WIN_CHARS            76  // Window interior width in characters
#define ID_CHARS             26  // Maximum characters of KickSmash ID text
#define STATUS_CHARS         44  // Characters in the status line

/* Width of a button which has the specified number of text characters */
#define BUTTON_W(chars)  (((chars) + 2) * lay.fw + 4)

/* Bank table columns */
#define COL_BANK             0
#define COL_NAME             1
#define COL_MERGE            2
#define COL_LONGRESET        3
#define COL_POWERON          4
#define COL_CURRENT          5
#define COL_SELECT           6
#define COL_COUNT            7

/* Gadget IDs */
#define ID_BOARD_NAME        1
#define ID_BANK_DEFAULT      2
#define ID_BANK_TIMEOUT      3
#define ID_POWERON_RADIO     7
#define ID_CURRENT_RADIO     8
#define ID_SELECT_RADIO      9
#define ID_FILE_NAME         10
#define ID_FILE_SELECT       11
#define ID_PROGRAM           12
#define ID_READ              13
#define ID_VERIFY            14
#define ID_STOP              15
#define ID_VERIFY_AFTER      16
#define ID_SAVE              17
#define ID_RELOAD            18
#define ID_SWITCH            19
#define ID_LONGRESET_MINUS_0 32  // through 39
#define ID_LONGRESET_PLUS_0  40  // through 47
#define ID_BANK_NAME_0       48  // through 55

/* Menu item identifiers (stored as GadTools menu item user data) */
#define MENU_PROGRAM         1
#define MENU_READ            2
#define MENU_VERIFY          3
#define MENU_SAVE            4
#define MENU_RELOAD          5
#define MENU_SWITCH          6
#define MENU_ABOUT           7
#define MENU_QUIT            8
#define MENU_CLOCK_FROM_HOST 9
#define MENU_CLOCK_FROM_KS   10
#define MENU_CLOCK_TO_KS     11
#define MENU_UNMERGE_ALL     12
#define MENU_SWAP            0x100  // Low bits are SWAP_* mode
#define MENU_MERGE           0x200  // Low bits are MERGE_RANGE()
#define MENU_UNMERGE         0x300  // Low bits are MERGE_RANGE()
#define MENU_TYPE_MASK       0xf00

#define MERGE_RANGE(start, end) (((start) << 4) | (end))

/* Position of menu items which are enabled and disabled */
#define MENUNUM_SAVE         FULLMENUNUM(0, 4, NOSUB)
#define MENUNUM_SWITCH       FULLMENUNUM(0, 6, NOSUB)

/* Byte swap modes for ROM image files */
#define SWAP_AUTO            0  // Detect from Kickstart ROM header
#define SWAP_0123            1  // No swap
#define SWAP_1032            2  // Swap adjacent bytes in 16-bit words
#define SWAP_2301            3  // Swap adjacent 16-bit words
#define SWAP_3210            4  // Swap bytes in 32-bit longs

#define SI_BUF(gad) \
        ((char *) ((struct StringInfo *) (gad)->SpecialInfo)->Buffer)

/*
 * The following variables are part of the clib2 runtime configuration.
 * ^C break handling is disabled, and a larger stack than the Workbench
 * default (4096 bytes) is requested for the ASL and DOS calls.
 */
BOOL         __check_abort_enabled = 0;
unsigned int __stack_size          = 16384;

uint flag_debug = 0;  // Referenced by sm_msg.c

struct DosLibrary    *DOSBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase       *GfxBase;
struct Library       *GadToolsBase;
struct Library       *AslBase;

static struct timerequest TimeRequest;

typedef struct {
    WORD ox;            // Left edge of window interior
    WORD oy;            // Top edge of window interior
    WORD fw;            // Font character cell width
    WORD fh;            // Font character cell height
    WORD gh;            // Height of button, string and integer gadgets
    WORD width;         // Window interior width
    WORD inner_h;       // Window interior height
    WORD margin;        // Left and right margin
    WORD colb_x;        // Left edge of gadgets to the right of ID text
    WORD ty[2];         // Top block gadget rows
    WORD table_x;       // Bank table left edge
    WORD table_y;       // Bank table top edge
    WORD table_w;       // Bank table width
    WORD table_h;       // Bank table height
    WORD rows_y;        // Top of first bank row in table
    WORD row_h;         // Bank table row pitch
    WORD el_y;          // Offset of font-high element within table row
    WORD mx_w;          // Width of radio button in table
    WORD pm_w;          // Width of LongReset - and + buttons
    WORD pm_x;          // Offset of LongReset - button in its column
    WORD file_y;        // ROM file name row
    WORD ops_y;         // Program / Read / Verify row
    WORD stat_y;        // Status text and progress bar row
    WORD prog_x;        // Progress bar left edge
    WORD prog_w;        // Progress bar width (including border)
    WORD prog_h;        // Progress bar height (including border)
    WORD btn_y;         // Save / Reload / Switch row
} layout_t;

static layout_t lay;

/* These are widths (in characters) for the display table */
static const uint8_t banktable_widths[COL_COUNT] = {
    5,  // Bank
    17, // Name
    6,  // Merge
    10, // LongReset
    8,  // PowerOn
    8,  // Current
    8,  // Select
};
static const char *const banktable_titles[COL_COUNT] = {
    "Bank", "Name", "Merge", "LongReset", "PowerOn", "Current", "Select",
};
static WORD banktable_pos[COL_COUNT];

static const struct TextAttr topaz8_attr = {
    (STRPTR) "topaz.font", 8, FS_NORMAL, FPF_ROMFONT
};
static struct TextAttr font_attr;  // Font in use by the window

static const char *const mx_labels_all[] = {
    "", "", "", "", "", "", "", "", NULL
};
static const char *const mx_labels_one[] = { "", NULL };

static struct Screen   *screen;
static struct Window   *window;
static struct DrawInfo *drawinfo;
static struct TextFont *font;
static APTR             visualinfo;
static struct NewGadget ng;
static struct Menu     *menus;
static struct Gadget   *gadgets;
static struct Gadget   *gadget_board_name;
static struct Gadget   *gadget_timeout_bank;
static struct Gadget   *gadget_timeout_seconds;
static struct Gadget   *gadget_poweron;
static struct Gadget   *gadget_select;
static struct Gadget   *gadget_file;
static struct Gadget   *gadget_file_select;
static struct Gadget   *gadget_program;
static struct Gadget   *gadget_read;
static struct Gadget   *gadget_verify;
static struct Gadget   *gadget_stop;
static struct Gadget   *gadget_verify_after;
static struct Gadget   *gadget_save;
static struct Gadget   *gadget_reload;
static struct Gadget   *gadget_switch;

/* Bank name string gadgets (plain Intuition gadgets inside the table) */
static struct Gadget       name_gadget[ROM_BANKS];
static struct StringInfo   name_sinfo[ROM_BANKS];
static struct StringExtend name_sext;
static UBYTE               name_buf[ROM_BANKS][16];
static UBYTE               name_undo[16];
static UBYTE               name_work[16];
static uint8_t             name_gadgets_added;

static UWORD pen_text = 1;
static UWORD pen_back = 0;
static UWORD pen_fill = 3;

static char  window_title[40];
static char  status_text[STATUS_CHARS + 1];
static char  rom_filename[256];   // ROM file name kept across gadget rebuild
static char  op_filename[256];    // ROM file name of operation in progress
static int   prog_percent = -1;  // -1 when no operation has been started
static uint  prog_fill;
static uint  verify_after = 1;
static uint  busy;
static uint  op_abort;
static uint  swap_mode = SWAP_AUTO;  // Selected in the Swap menu

static bank_info_t info;         // Bank information, as edited
static bank_info_t info_saved;   // Bank information stored in Kicksmash
static smash_id_t  id;           // Kicksmash ID, as edited
static smash_id_t  id_saved;     // Kicksmash ID stored in Kicksmash
static uint        bank_select;  // Bank for ROM file operations and Switch
static uint        timeout_seconds;
static uint        timeout_seconds_saved;
static uint        timeout_bank;
static uint        timeout_bank_saved;

static void draw_status(void);
static void draw_progress(void);
static void draw_bank_column(uint col);
static void draw_window(void);
static int  flash_read_mode(uint expect_rom_signature);


/* ------------------------------------------------------------------------
 * Text, status, and requesters
 * ------------------------------------------------------------------------ */

/*
 * text_at
 * -------
 * Display text at the specified position in the window. Exactly width
 * characters are drawn, padded with spaces, so that old text is erased.
 */
static void
text_at(WORD x, WORD y, const char *str, uint width)
{
    struct RastPort *rp = window->RPort;
    char buf[80];
    uint len = strlen(str);

    if (width > sizeof (buf))
        width = sizeof (buf);
    if (len > width)
        len = width;
    memset(buf, ' ', width);
    memcpy(buf, str, len);

    SetFont(rp, font);
    SetAPen(rp, pen_text);
    SetBPen(rp, pen_back);
    SetDrMd(rp, JAM2);
    Move(rp, lay.ox + x, lay.oy + y + font->tf_Baseline);
    Text(rp, (STRPTR) buf, width);
}

/*
 * bevel
 * -----
 * Draw a beveled box in the window.
 */
static void
bevel(WORD x, WORD y, WORD w, WORD h, uint recessed)
{
    DrawBevelBox(window->RPort, lay.ox + x, lay.oy + y, w, h,
                 GT_VisualInfo, (ULONG) visualinfo,
                 recessed ? GTBB_Recessed : TAG_IGNORE, TRUE,
                 TAG_DONE);
}

/*
 * set_status
 * ----------
 * Update the status line text.
 */
static void
set_status(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(status_text, sizeof (status_text), fmt, ap);
    va_end(ap);
    if (window != NULL)
        draw_status();
}

/*
 * request
 * -------
 * Display a requester and wait for the user to select one of the
 * buttons. The buttons string is in EasyRequest() format, such as
 * "Yes|No". The right-most button returns 0.
 */
static LONG
request(const char *buttons, const char *fmt, va_list ap)
{
    static char       msgbuf[512];
    struct EasyStruct es;
    ULONG             args[1];

    vsnprintf(msgbuf, sizeof (msgbuf), fmt, ap);

    es.es_StructSize   = sizeof (es);
    es.es_Flags        = 0;
    es.es_Title        = (STRPTR) "SmashtTool";
    es.es_TextFormat   = (STRPTR) "%s";
    es.es_GadgetFormat = (STRPTR) buttons;
    args[0] = (ULONG) msgbuf;

    return (EasyRequestArgs(window, &es, NULL, (APTR) args));
}

static void
show_error(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (void) request("OK", fmt, ap);
    va_end(ap);
}

static LONG
ask(const char *buttons, const char *fmt, ...)
{
    va_list ap;
    LONG    rc;

    va_start(ap, fmt);
    rc = request(buttons, fmt, ap);
    va_end(ap);
    return (rc);
}

/*
 * str_to_uint
 * -----------
 * Convert a string of decimal digits to an unsigned integer.
 */
static uint
str_to_uint(const char *str)
{
    uint value = 0;

    while (*str == ' ')
        str++;
    while ((*str >= '0') && (*str <= '9'))
        value = value * 10 + *(str++) - '0';
    return (value);
}


/* ------------------------------------------------------------------------
 * Kicksmash ID, bank information, and settings
 * ------------------------------------------------------------------------ */

/*
 * get_id
 * ------
 * Acquire Kicksmash hardware and config information
 */
static uint
get_id(smash_id_t *sid)
{
    uint rc;
    uint rlen;

    memset(sid, 0, sizeof (*sid));
    rc = send_cmd_retry(KS_CMD_ID, NULL, 0, sid, sizeof (*sid), &rlen);
    if (rc != 0) {
        memset(sid, 0, sizeof (*sid));
        strcpy(sid->si_serial, "Comm. Failure");
    }
    sid->si_serial[sizeof (sid->si_serial) - 1] = '\0';
    sid->si_name[sizeof (sid->si_name) - 1] = '\0';
    return (rc);
}

/*
 * get_banks
 * ---------
 * Acquire ROM bank information from Kicksmash
 */
static uint
get_banks(bank_info_t *bi)
{
    uint rc;
    uint rlen;
    uint bank;

    memset(bi, 0, sizeof (*bi));
    rc = send_cmd_retry(KS_CMD_BANK_INFO, NULL, 0, bi, sizeof (*bi), &rlen);
    if (rc != 0) {
        memset(bi, 0, sizeof (*bi));
        memset(bi->bi_longreset_seq, 0xff, sizeof (bi->bi_longreset_seq));
        bi->bi_bank_current   = 0xff;
        bi->bi_bank_nextreset = 0xff;
    }
    for (bank = 0; bank < ROM_BANKS; bank++)
        bi->bi_name[bank][sizeof (bi->bi_name[0]) - 1] = '\0';
    return (rc);
}

/*
 * timeout_to_nv
 * -------------
 * Convert a ROM Switcher timeout in seconds to the value stored in the
 * Kicksmash NV0 byte. Timeouts longer than 127 seconds are stored in
 * minutes, with the high bit set.
 */
static uint8_t
timeout_to_nv(uint seconds)
{
    if (seconds <= 127)
        return (seconds);
    seconds /= 60;
    if (seconds > 127)
        seconds = 127;
    return (seconds | BIT(7));
}

static uint
timeout_from_nv(uint8_t value)
{
    if (value & BIT(7))
        return (60 * (value & 0x7f));  // Minutes
    return (value);                    // Seconds
}

/*
 * get_bank_timeout
 * ----------------
 * Acquire settings from Kicksmash that indicate whether a ROM Switcher
 * timeout is active and what bank the ROM Switcher should then choose.
 */
static uint
get_bank_timeout(uint *seconds, uint *bank)
{
    uint    rc;
    uint    rlen;
    uint8_t buf[2];
    uint8_t rbuf[8];

    /* Initialize to 0 in case request fails */
    *seconds = 0;
    *bank = 0;

    buf[0] = 0;  // Start at NV0
    buf[1] = 4;  // Also read NV1
    rc = send_cmd_retry(KS_CMD_GET | KS_GET_NV,
                        buf, sizeof (buf), rbuf, sizeof (rbuf), &rlen);
    if (rc == 0) {
        *seconds = timeout_from_nv(rbuf[0]);
        *bank    = rbuf[1];
    }
    return (rc);
}

/*
 * set_bank_timeout
 * ----------------
 * Store ROM Switcher timeout and bank settings with Kicksmash.
 */
static uint
set_bank_timeout(uint seconds, uint bank)
{
    uint    rlen;
    uint8_t buf[4];

    buf[0] = 0;  // Start at NV0
    buf[1] = 2;  // Also write NV1
    buf[2] = timeout_to_nv(seconds);
    buf[3] = bank;
    return (send_cmd_retry(KS_CMD_SET | KS_SET_NV,
                           buf, sizeof (buf), NULL, 0, &rlen));
}

/*
 * bank_size_bytes
 * ---------------
 * Return the size in bytes of the specified bank, including all banks
 * which are merged with it.
 */
static uint
bank_size_bytes(const bank_info_t *bi, uint bank)
{
    return (((bi->bi_merge[bank] + 0x10) & 0xf0) << 15);
}

/*
 * set_initial_bank_select
 * -----------------------
 * Decide which bank should be initially selected.
 */
static void
set_initial_bank_select(void)
{
    bank_select = info_saved.bi_bank_nextreset;
    if (bank_select >= ROM_BANKS)
        bank_select = info_saved.bi_bank_current;
    if (bank_select >= ROM_BANKS)
        bank_select = 0;
}

/*
 * state_load
 * ----------
 * Acquire all information from Kicksmash which is displayed in the window.
 * Returns non-zero if Kicksmash did not answer.
 */
static uint
state_load(void)
{
    uint rc;

    rc = get_id(&id);
    if (rc == 0)
        rc = get_banks(&info);
    else
        (void) get_banks(&info);
    (void) get_bank_timeout(&timeout_seconds, &timeout_bank);

    memcpy(&id_saved, &id, sizeof (id_saved));
    memcpy(&info_saved, &info, sizeof (info_saved));
    timeout_seconds_saved = timeout_seconds;
    timeout_bank_saved    = timeout_bank;
    return (rc);
}

/*
 * state_dirty
 * -----------
 * Returns a mask of settings which differ from those stored in Kicksmash.
 * Bits 0-7 are bank names. Remaining bits are defined below.
 */
#define DIRTY_BOARD_NAME BIT(8)
#define DIRTY_LONGRESET  BIT(9)
#define DIRTY_POWERON    BIT(10)
#define DIRTY_TIMEOUT    BIT(11)
#define DIRTY_MERGE      BIT(12)

static uint
state_dirty(void)
{
    uint dirty = 0;
    uint bank;

    for (bank = 0; bank < ROM_BANKS; bank++)
        if (strcmp(info.bi_name[bank], info_saved.bi_name[bank]) != 0)
            dirty |= BIT(bank);
    if (strcmp(id.si_name, id_saved.si_name) != 0)
        dirty |= DIRTY_BOARD_NAME;
    if (memcmp(info.bi_longreset_seq, info_saved.bi_longreset_seq,
               sizeof (info.bi_longreset_seq)) != 0)
        dirty |= DIRTY_LONGRESET;
    if (info.bi_bank_poweron != info_saved.bi_bank_poweron)
        dirty |= DIRTY_POWERON;
    if ((timeout_bank != timeout_bank_saved) ||
        (timeout_to_nv(timeout_seconds) !=
         timeout_to_nv(timeout_seconds_saved)))
        dirty |= DIRTY_TIMEOUT;
    if (memcmp(info.bi_merge, info_saved.bi_merge,
               sizeof (info.bi_merge)) != 0)
        dirty |= DIRTY_MERGE;
    return (dirty);
}

/*
 * pull_edits
 * ----------
 * Copy the current content of the string and integer gadgets into the
 * edited state.
 */
static void
pull_edits(void)
{
    uint bank;

    if (gadgets == NULL)
        return;

    for (bank = 0; bank < ROM_BANKS; bank++) {
        strncpy(info.bi_name[bank], (char *) name_buf[bank],
                sizeof (info.bi_name[bank]) - 1);
        info.bi_name[bank][sizeof (info.bi_name[bank]) - 1] = '\0';
    }
    strncpy(id.si_name, SI_BUF(gadget_board_name), sizeof (id.si_name) - 1);
    id.si_name[sizeof (id.si_name) - 1] = '\0';

    timeout_bank    = str_to_uint(SI_BUF(gadget_timeout_bank));
    timeout_seconds = str_to_uint(SI_BUF(gadget_timeout_seconds));
}

/*
 * update_buttons
 * --------------
 * Enable or disable the Save and Switch buttons, and their menu items,
 * based on current state.
 */
static void
update_buttons(uint force)
{
    static uint8_t last_save   = 0xff;
    static uint8_t last_switch = 0xff;
    uint8_t        disable_save;
    uint8_t        disable_switch;

    if (busy || (gadgets == NULL))
        return;

    disable_save   = (state_dirty() == 0);
    disable_switch = (bank_select == info_saved.bi_bank_current);

    if (force || (disable_save != last_save)) {
        last_save = disable_save;
        GT_SetGadgetAttrs(gadget_save, window, NULL,
                          GA_Disabled, disable_save, TAG_DONE);
        if (disable_save)
            OffMenu(window, MENUNUM_SAVE);
        else
            OnMenu(window, MENUNUM_SAVE);
    }
    if (force || (disable_switch != last_switch)) {
        last_switch = disable_switch;
        GT_SetGadgetAttrs(gadget_switch, window, NULL,
                          GA_Disabled, disable_switch, TAG_DONE);
        if (disable_switch)
            OffMenu(window, MENUNUM_SWITCH);
        else
            OnMenu(window, MENUNUM_SWITCH);
    }
}

/*
 * merge_range_add
 * ---------------
 * Returns the number of additional banks which are merged with the
 * specified bank, if that bank is the first bank of a complete merged
 * range. Otherwise 0 is returned.
 *
 * Each bank of a merged range has the number of additional banks of the
 * range in the high nibble of its merge value, and its position within
 * the range in the low nibble.
 */
static uint
merge_range_add(const uint8_t *merge, uint bank)
{
    uint add = merge[bank] >> 4;
    uint pos;

    if (((merge[bank] & 0x0f) != 0) || (add == 0) ||
        (bank + add >= ROM_BANKS)) {
        return (0);
    }
    for (pos = 1; pos <= add; pos++)
        if (merge[bank + pos] != ((add << 4) | pos))
            return (0);
    return (add);
}

/*
 * bank_merge_cmd
 * --------------
 * Request KickSmash to merge or unmerge a range of banks.
 */
static uint
bank_merge_cmd(uint bank_start, uint bank_end, uint unmerge)
{
    uint16_t argval = bank_start | (bank_end << 8);

    return (send_cmd_retry(KS_CMD_BANK_MERGE |
                           (unmerge ? KS_BANK_UNMERGE : 0),
                           &argval, sizeof (argval), NULL, 0, NULL));
}

/*
 * merge_save
 * ----------
 * Send the merge and unmerge requests to KickSmash which change the
 * merge state stored there into the merge state as edited. KickSmash
 * will only unmerge banks which are merged, and will only merge banks
 * which are not merged, so every stored range which is to change is
 * first unmerged, and then each new range is merged.
 *
 * info_saved is updated as each request succeeds, so that it continues
 * to match what KickSmash has stored if a later request fails.
 */
static uint
merge_save(void)
{
    uint8_t       *cur  = info_saved.bi_merge;
    const uint8_t *want = info.bi_merge;
    uint           bank;
    uint           rc;

    /* Unmerge stored ranges which differ from what is wanted */
    for (bank = 0; bank < ROM_BANKS; bank++) {
        uint bank_start = bank;
        uint bank_end   = bank;
        uint sub        = cur[bank] & 0x0f;

        if ((cur[bank] == 0) || (cur[bank] == want[bank]))
            continue;

        /* Unmerge the entire range if this bank is part of one */
        if (sub <= bank) {
            uint add = merge_range_add(cur, bank - sub);

            if ((add != 0) && (add >= sub)) {
                bank_start = bank - sub;
                bank_end   = bank_start + add;
            }
        }
        rc = bank_merge_cmd(bank_start, bank_end, 1);
        if (rc != 0)
            return (rc);
        memset(cur + bank_start, 0, bank_end - bank_start + 1);
    }

    /* Merge ranges which are wanted, but are not what is stored */
    for (bank = 0; bank < ROM_BANKS; bank++) {
        uint add = merge_range_add(want, bank);

        if ((add == 0) || (memcmp(cur + bank, want + bank, add + 1) == 0))
            continue;

        rc = bank_merge_cmd(bank, bank + add, 0);
        if (rc != 0)
            return (rc);
        memcpy(cur + bank, want + bank, add + 1);
        bank += add;
    }

    if (memcmp(cur, want, ROM_BANKS) != 0)
        return (MSG_STATUS_BAD_DATA);  // Not a set of complete ranges
    return (0);
}

/*
 * state_save
 * ----------
 * Send updated (user-modified) settings to Kicksmash
 */
static void
state_save(void)
{
    char        errbuf[80];
    const char *what = NULL;
    uint        dirty;
    uint        bank;
    uint        rlen;
    uint        rc;

    pull_edits();
    dirty = state_dirty();

    if ((dirty & DIRTY_TIMEOUT) && (timeout_bank >= ROM_BANKS)) {
        show_error("Auto Switch bank %u is invalid.\n"
                   "The bank number must be 0 to %u.",
                   timeout_bank, ROM_BANKS - 1);
        return;
    }

    /*
     * Only the first bank of a merged range may be a LongReset bank or
     * the PowerOn bank. Check this before sending anything to KickSmash.
     */
    for (bank = 0; bank < ARRAY_SIZE(info.bi_longreset_seq); bank++) {
        uint lbank = info.bi_longreset_seq[bank];

        if ((lbank < ROM_BANKS) && ((info.bi_merge[lbank] & 0x0f) != 0)) {
            show_error("LongReset bank %u is merged, but is not the first\n"
                       "bank of its merged range. Remove bank %u from the\n"
                       "LongReset list, or use bank %u instead.",
                       lbank, lbank, lbank - (info.bi_merge[lbank] & 0x0f));
            return;
        }
    }
    bank = info.bi_bank_poweron;
    if ((bank < ROM_BANKS) && ((info.bi_merge[bank] & 0x0f) != 0)) {
        show_error("PowerOn bank %u is merged, but is not the first\n"
                   "bank of its merged range. Use bank %u instead.",
                   bank, bank - (info.bi_merge[bank] & 0x0f));
        return;
    }

    errbuf[0] = '\0';

    /* Merge state is saved first: it decides which banks may be used */
    if (dirty & DIRTY_MERGE) {
        rc = merge_save();
        if (rc != 0) {
            what = "bank merge";
            strcpy(errbuf, smash_err(rc));
        }
        draw_bank_column(COL_MERGE);
    }
    for (bank = 0; bank < ROM_BANKS; bank++) {
        char     argbuf[32];
        uint16_t argval = bank;

        if ((dirty & BIT(bank)) == 0)
            continue;  // This name was not updated

        memset(argbuf, 0, sizeof (argbuf));
        memcpy(argbuf, &argval, 2);
        strcpy(argbuf + 2, info.bi_name[bank]);
        rc = send_cmd_retry(KS_CMD_BANK_NAME, argbuf,
                            strlen(argbuf + 2) + 3, NULL, 0, &rlen);
        if (rc == 0) {
            strcpy(info_saved.bi_name[bank], info.bi_name[bank]);
        } else if (what == NULL) {
            what = "bank name";
            strcpy(errbuf, smash_err(rc));
        }
    }
    if (dirty & DIRTY_BOARD_NAME) {
        char namebuf[sizeof (id.si_name)];

        memset(namebuf, 0, sizeof (namebuf));
        strcpy(namebuf, id.si_name);
        rc = send_cmd_retry(KS_CMD_SET | KS_SET_NAME,
                            namebuf, sizeof (namebuf), NULL, 0, NULL);
        if (rc == 0) {
            strcpy(id_saved.si_name, id.si_name);
        } else if (what == NULL) {
            what = "board name";
            strcpy(errbuf, smash_err(rc));
        }
    }
    if (dirty & DIRTY_LONGRESET) {
        rc = send_cmd_retry(KS_CMD_BANK_LRESET, info.bi_longreset_seq,
                            sizeof (info.bi_longreset_seq), NULL, 0, NULL);
        if (rc == 0) {
            memcpy(info_saved.bi_longreset_seq, info.bi_longreset_seq,
                   sizeof (info_saved.bi_longreset_seq));
        } else if (what == NULL) {
            what = "LongReset sequence";
            strcpy(errbuf, smash_err(rc));
        }
    }
    if (dirty & DIRTY_POWERON) {
        uint16_t argval = info.bi_bank_poweron;

        rc = send_cmd_retry(KS_CMD_BANK_SET | KS_BANK_SETPOWERON,
                            &argval, sizeof (argval), NULL, 0, &rlen);
        if (rc == 0) {
            info_saved.bi_bank_poweron = info.bi_bank_poweron;
        } else if (what == NULL) {
            what = "PowerOn bank";
            strcpy(errbuf, smash_err(rc));
        }
    }
    if (dirty & DIRTY_TIMEOUT) {
        rc = set_bank_timeout(timeout_seconds, timeout_bank);
        if (rc == 0) {
            /* Show the timeout as Kicksmash is able to store it */
            timeout_seconds       = timeout_from_nv(
                                        timeout_to_nv(timeout_seconds));
            timeout_seconds_saved = timeout_seconds;
            timeout_bank_saved    = timeout_bank;
            GT_SetGadgetAttrs(gadget_timeout_seconds, window, NULL,
                              GTIN_Number, timeout_seconds, TAG_DONE);
        } else if (what == NULL) {
            what = "Auto Switch settings";
            strcpy(errbuf, smash_err(rc));
        }
    }

    if (what != NULL) {
        set_status("Save failed");
        show_error("Failed to save %s\n%s", what, errbuf);
    } else {
        set_status("Settings saved to KickSmash");
    }
    update_buttons(0);
}

/*
 * longreset_normalize
 * -------------------
 * Remove holes, invalid bank numbers, and duplicate banks from a LongReset
 * sequence, so that every entry of the sequence is one which is displayed.
 * Returns the number of banks in the sequence.
 */
static uint
longreset_normalize(uint8_t *seq)
{
    uint count = 0;
    uint pos;
    uint prev;

    for (pos = 0; pos < ROM_BANKS; pos++) {
        uint8_t bank = seq[pos];

        if (bank >= ROM_BANKS)
            continue;  // Unused or invalid
        for (prev = 0; prev < count; prev++)
            if (seq[prev] == bank)
                break;
        if (prev < count)
            continue;  // Duplicate
        seq[count++] = bank;
    }
    for (pos = count; pos < ROM_BANKS; pos++)
        seq[pos] = 0xff;
    return (count);
}

/*
 * bank_longreset_change
 * ---------------------
 * Handle updates to the LongReset column
 *
 *     Bank is not in the sequence
 *         "-" adds it at the end (highest position)
 *         "+" adds it at the beginning (position 0)
 *     Bank is in the sequence
 *         "-" moves it one position lower, or removes it from the
 *             sequence if it is already at position 0
 *         "+" moves it one position higher, or removes it from the
 *             sequence if it is already at the highest position
 */
static void
bank_longreset_change(uint bank, int addsub)
{
    uint8_t *seq   = info.bi_longreset_seq;
    uint     count = longreset_normalize(seq);
    uint     curpos;
    uint     pos;

    /* Find the current position of the bank in the sequence */
    for (curpos = 0; curpos < count; curpos++)
        if (seq[curpos] == bank)
            break;

    if (curpos == count) {
        /* Not currently in the sequence */
        uint sub = info.bi_merge[bank] & 0x0f;

        if (sub != 0) {
            set_status("Bank %u is merged: use bank %u", bank, bank - sub);
            return;
        }
        if (count >= ROM_BANKS)
            return;  // No space

        if (addsub < 0) {
            /* Subtract when not in list adds it to the end of the list */
            seq[count] = bank;
        } else {
            /* Add when not in list adds it to the beginning of the list */
            for (pos = count; pos > 0; pos--)
                seq[pos] = seq[pos - 1];
            seq[0] = bank;
        }
        return;
    }

    /* Currently in the sequence */

    if (((addsub < 0) && (curpos == 0)) ||
        ((addsub > 0) && (curpos == count - 1))) {
        /*
         * Subtract when at the start of the list, or add when at the
         * highest position of the list = Remove
         */
        for (pos = curpos; pos < count - 1; pos++)
            seq[pos] = seq[pos + 1];
        seq[count - 1] = 0xff;
        return;
    }

    /* There is another bank in the sequence to trade positions with */
    if (addsub < 0) {
        /* Swap with position immediately before */
        seq[curpos] = seq[curpos - 1];
        seq[curpos - 1] = bank;
    } else {
        /* Swap with position immediately after */
        seq[curpos] = seq[curpos + 1];
        seq[curpos + 1] = bank;
    }
}

/*
 * merge_pending
 * -------------
 * Returns non-zero, after telling the user, if the displayed bank merge
 * state has not been saved to KickSmash. Operations which depend on how
 * banks are merged are not allowed until it is saved (or reloaded).
 */
static uint
merge_pending(void)
{
    if ((state_dirty() & DIRTY_MERGE) == 0)
        return (0);

    show_error("Bank merge changes have not been saved to KickSmash.\n"
               "Select Save or Reload first.");
    return (1);
}

/*
 * bank_merge
 * ----------
 * Merge a range of banks into a single larger bank, or unmerge a range
 * of banks. This changes only what is displayed. The change is sent to
 * KickSmash when the user selects Save.
 *
 * The same rules are applied as those of KickSmash: banks to merge must
 * not already be merged, and banks to unmerge must be merged. In addition,
 * a range to unmerge must not include just part of a larger merged range.
 */
static void
bank_merge(uint bank_start, uint bank_end, uint unmerge)
{
    uint banks_add = bank_end - bank_start;
    uint bank;

    for (bank = bank_start; bank <= bank_end; bank++) {
        uint merge       = info.bi_merge[bank];
        uint range_start = bank - (merge & 0x0f);
        uint range_end   = range_start + (merge >> 4);

        if (!unmerge && (merge != 0)) {
            show_error("Bank %u is already part of merged bank range %u-%u.\n"
                       "Unmerge that range first.",
                       bank, range_start, range_end);
            return;
        }
        if (unmerge && (merge == 0)) {
            show_error("Bank %u is not part of a merged bank range.", bank);
            return;
        }
        if (unmerge && ((range_start < bank_start) ||
                        (range_end > bank_end))) {
            show_error("Bank %u is part of merged bank range %u-%u.\n"
                       "Unmerge that range instead.",
                       bank, range_start, range_end);
            return;
        }
    }

    for (bank = bank_start; bank <= bank_end; bank++) {
        if (unmerge)
            info.bi_merge[bank] = 0;
        else
            info.bi_merge[bank] = (banks_add << 4) | (bank - bank_start);
    }
    draw_bank_column(COL_MERGE);

    set_status("Banks %u-%u %smerged%s", bank_start, bank_end,
               unmerge ? "un" : "",
               (state_dirty() & DIRTY_MERGE) ? " (not yet saved)" : "");
}

/*
 * bank_unmerge_all
 * ----------------
 * Unmerge every merged bank range. As with bank_merge(), this changes
 * only what is displayed until the user selects Save.
 */
static void
bank_unmerge_all(void)
{
    uint bank;
    uint count = 0;

    for (bank = 0; bank < ROM_BANKS; bank++) {
        if (info.bi_merge[bank] != 0) {
            info.bi_merge[bank] = 0;
            count++;
        }
    }
    draw_bank_column(COL_MERGE);

    if (count == 0)
        set_status("No banks are merged");
    else
        set_status("All banks unmerged%s",
                   (state_dirty() & DIRTY_MERGE) ? " (not yet saved)" : "");
}

/*
 * bank_switch_and_reboot
 * ----------------------
 * Request Kicksmash to set the current bank and reset the Amiga.
 */
static void
bank_switch_and_reboot(void)
{
    uint     rc;
    uint     rlen;
    uint     sub;
    uint16_t argval = bank_select;

    if (bank_select == info_saved.bi_bank_current)
        return;

    pull_edits();
    if (merge_pending())
        return;

    sub = info_saved.bi_merge[bank_select] & 0x0f;
    if (sub != 0) {
        show_error("Bank %u is part of a merged bank, but is not the first.\n"
                   "Select bank %u instead.", bank_select, bank_select - sub);
        return;
    }
    if (ask("Switch and Reboot|Cancel",
            "Switch to bank %u \"%s\"\nand reboot the Amiga now?%s",
            bank_select, info_saved.bi_name[bank_select],
            state_dirty() ? "\n\nChanges which have not been saved "
                            "will be lost." : "") == 0) {
        return;
    }

    rc = send_cmd_retry(KS_CMD_BANK_SET | KS_BANK_SETCURRENT | KS_BANK_REBOOT,
                        &argval, sizeof (argval), NULL, 0, &rlen);
    if (rc != 0) {
        set_status("Bank switch failed");
        show_error("Failed to switch to bank %u\n%s",
                   bank_select, smash_err(rc));
    } else {
        set_status("Switching to bank %u", bank_select);
    }
}


/* ------------------------------------------------------------------------
 * Flash access (from the smash utility)
 * ------------------------------------------------------------------------ */

typedef struct {
    uint32_t ci_id;       // Vendor code
    char     ci_dev[16];  // ID string for display
} chip_ids_t;
static const chip_ids_t chip_ids[] = {
    { 0x000122D2, "M29F160FT" },   // AMD+others 2MB top boot
    { 0x000122D8, "M29F160FB" },   // AMD+others 2MB bottom boot
    { 0x000122D6, "M29F800FT" },   // AMD+others 1MB top boot
    { 0x00012258, "M29F800FB" },   // AMD+others 1MB bottom boot
    { 0x00012223, "M29F400FT" },   // AMD+others 512K top boot
    { 0x000122ab, "M29F400FB" },   // AMD+others 512K bottom boot
    { 0x000422d2, "M29F160TE" },   // Fujitsu 2MB top boot
    { 0x000422D8, "M29F160TB" },   // Fujitsu 2MB bottom boot
    { 0x00c222D6, "MX29F800CT" },  // Macronix 1MB top boot
    { 0x00c22258, "MX29F800CB" },  // Macronix 1MB bottom boot
    { 0x00c222c4, "MX29LV160CT" }, // Macronix 2MB top boot
    { 0x00c22249, "MX29LV160CB" }, // Macronix 2MB bottom boot
    { 0x002022cc, "M29F160BT" },   // ST-Micro 2MB top boot
    { 0x0020224b, "M29F160BB" },   // ST-Micro 2MB bottom boot
    { 0x002022c4, "M29W160ET" },   // ST-Micro 2MB top boot
    { 0x00202249, "M29W160EB" },   // ST-Micro 2MB bottom boot
    { 0x00000000, "Unknown" },     // Must remain last
};

typedef struct {
    uint16_t cb_chipid;   // Chip id code
    uint8_t  cb_bbnum;    // Boot block number (0=Bottom boot)
    uint8_t  cb_bsize;    // Common block size in Kwords (typical 32K)
    uint8_t  cb_ssize;    // Boot block sector size in Kwords (typical 4K)
    uint8_t  cb_map;      // Boot block sector erase map
} chip_blocks_t;

static const chip_blocks_t chip_blocks[] = {
    { 0x22D2, 31, 32, 4, 0x71 },  // 01110001 8K 4K 4K 16K (top)
    { 0x22D8,  0, 32, 4, 0x1d },  // 00011101 16K 4K 4K 8K (bottom)
    { 0x22D6, 15, 32, 4, 0x71 },  // 01110001 8K 4K 4K 16K (top)
    { 0x2258,  0, 32, 4, 0x1d },  // 00011101 16K 4K 4K 8K (bottom)
    { 0x22CC, 31, 32, 4, 0x71 },  // 01110001 8K 4K 4K 16K (top)
    { 0x224B,  0, 32, 4, 0x1d },  // 00011101 16K 4K 4K 8K (bottom)
    { 0x22C4, 15, 32, 4, 0x71 },  // 01110001 8K 4K 4K 16K (top)
    { 0x2249,  0, 32, 4, 0x1d },  // 00011101 16K 4K 4K 8K (bottom)
    { 0x0000,  0, 32, 4, 0x1d },  // Default to bottom boot
};

static const uint8_t flag_conservative_mode = 0;
static const uint8_t flag_flash_write_bug   = 1;

/* Details of the most recent flash write failure */
static struct {
    uint     ff_bank;
    uint     ff_addr;
    uint32_t ff_readback;
    uint32_t ff_expected;
} flash_fail;

static const chip_blocks_t *
get_chip_block_info(uint32_t chipid)
{
    uint16_t cid = (uint16_t) chipid;
    uint pos;

    /* Search for exact match */
    for (pos = 0; pos < ARRAY_SIZE(chip_blocks) - 1; pos++)
        if (chip_blocks[pos].cb_chipid == cid)
            break;

    return (&chip_blocks[pos]);
}

static const char *
ee_id_string(uint32_t chipid)
{
    uint pos;

    for (pos = 0; pos < ARRAY_SIZE(chip_ids) - 1; pos++)
        if (chip_ids[pos].ci_id == chipid)
            break;

    return (chip_ids[pos].ci_dev);
}

static uint
send_cmd_core_retry(uint16_t cmd, void *arg, uint16_t arglen,
                    void *reply, uint replymax, uint *replyalen)
{
    uint rc;
    uint tries = 5;

    do {
        rc = send_cmd_core(cmd, arg, arglen, reply, replymax, replyalen);
        if ((rc != 0) &&
            (cmd != KS_CMD_FLASH_READ) && (cmd != KS_CMD_FLASH_CMD) &&
            (cmd != KS_CMD_FLASH_ERASE) && (cmd != KS_CMD_FLASH_WRITE)) {
            flash_read_mode(1);
        }
        if ((rc != MSG_STATUS_BAD_CRC) &&
            (rc != MSG_STATUS_NO_REPLY) &&
            (rc != KS_STATUS_CRC)) {
            break;
        }
    } while (--tries > 0);
    return (rc);
}

/*
 * flash_cmd_core
 * --------------
 * Must be called with interrupts and cache disabled
 *
 * This function will send a command to the flash after setting the
 * transaction up with the Kicksmash CPU.
 */
static int
flash_cmd_core(uint32_t cmd, void *arg, uint argsize)
{
    uint32_t addr;
    uint32_t addrs[64];
    uint     retry = 0;
    uint     num_addr;
    uint     pos;
    int      rc;

    rc = send_cmd_core(cmd, arg, argsize, addrs, sizeof (addrs), &num_addr);
    if (rc != 0)
        goto flash_cmd_cleanup;

    /*
     * All Kicksmash flash commands have a similar sequence, where
     * the command is issued to Kicksmash, then Kicksmash will reply
     * with the sequence of addresses to blind read.
     * There is additional work for write and erase, where status must be
     * repeatedly read to poll flash state. It is left to the caller
     * to complete whatever additional polling is necessary and issue
     * the next command to Kicksmash.
     */
    num_addr /= 4;

    cia_spin(CIA_USEC(10));
    if (flag_conservative_mode)
        cia_spin(20);

    addr = ROM_BASE + ((addrs[0] << smash_cmd_shift) & 0x7ffff);
    (void) *VADDR32(addr);  // Generate OE strobe to kick off DMA
    cia_spin(1);
    if (flag_conservative_mode)
        cia_spin(2);

    for (pos = 0; pos < num_addr; pos++) {
        uint32_t val;

        addr = ROM_BASE + ((addrs[pos] << smash_cmd_shift) & 0x7ffff);
        val = *VADDR32(addr);  // Generate address on the bus
        if ((pos == 0) && (val == 0xffffffff)) {
            if (retry++ > 5) {
                rc = 7;  // RC_TIMEOUT
                goto flash_cmd_cleanup;
            }
            pos--;
            continue;
        }
    }

flash_cmd_cleanup:
    cia_spin(1);
    if (rc != 0) {
        /* Attempt to drain data and wait for Kicksmash to enable flash */
        for (pos = 0; pos < 500; pos++) {
            (void) *VADDR32(ROM_BASE + 4);
            cia_spin(10);
        }
        /* Wait 10 ms longer */
        for (pos = 0; pos < 10; pos++)
            cia_spin(CIA_USEC(1000));
    }
    return (rc);
}

static uint32_t rom_header_expected;
static void
rom_get_normal(void)
{
    rom_header_expected = *VADDR32(ROM_BASE);
}

/*
 * rom_wait_normal() waits until ROM is back in normal mode.
 */
static uint
rom_wait_normal(void)
{
    uint timeout = 0;
    while (*VADDR32(ROM_BASE) != rom_header_expected) {
        if (timeout++ > 100)
            return (1);  // Took too long.
        cia_spin(10);
    }
    return (0);
}

/*
 * flash_read_mode() returns the flash to normal read mode.
 *
 * expect_rom_signature says whether to expect the Kickstart ROM signature
 * at offset 0.
 */
static int
flash_read_mode(uint expect_rom_signature)
{
    uint count;
    int rc = flash_cmd_core(KS_CMD_FLASH_READ, NULL, 0);
    if ((rc != 0) || (expect_rom_signature && rom_wait_normal()))  {
        /* Try harder to restore flash to normal read mode */
        for (count = 0; count < 10; count++) {
            for (count = 0; count < 10; count++)
                (void) *VADDR16(ROM_BASE + 8);
            cia_spin(CIA_USEC(1000));
            if ((flash_cmd_core(KS_CMD_FLASH_READ, NULL, 0) == 0) &&
                ((expect_rom_signature == 0) || (rom_wait_normal() == 0))) {
                rc = 0;
                break;
            }
        }
    }
    return (rc);
}

/*
 * flash_id
 * --------
 * Acquire the ID of the flash parts, which also determines whether flash
 * is operating in 16-bit or 32-bit mode.
 */
static int
flash_id(uint32_t *dev1, uint32_t *dev2, uint *mode)
{
    uint32_t data[32];
    uint     pos;
    int      rc1;
    int      rc2;

    *dev1 = 0;
    *dev2 = 0;

    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

    rc1 = flash_cmd_core(KS_CMD_FLASH_ID, NULL, 0);

    /* Workaround for cold STM32 not releasing OEWE in time */
    (void) *VADDR16(ROM_BASE + 32);
    (void) *VADDR16(ROM_BASE + 36);
    (void) *VADDR16(ROM_BASE + 40);
    (void) *VADDR16(ROM_BASE + 44);
    cia_spin(CIA_USEC(5));

    /* Get flash ID data */
    for (pos = 0; pos < ARRAY_SIZE(data); pos++)
        data[pos] = *VADDR32(ROM_BASE + pos * 4);

    rc2 = flash_read_mode(1);

    CACHE_FLUSH();
    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();

    if (rc1 == 0) {
        /*
         * Validation for a recognized flash device is done by
         * get_chip_block_info(). The code just needs to determine
         * whether it's a 16-bit or 32-bit device.
         */
        uint flash_mode;
        uint16_t device;
        const chip_blocks_t *cb;

        if ((data[0x0] != 0x00000000) && (data[0x0] != 0xffffffff) &&
            (data[0x1] != 0x00000000) && (data[0x1] != 0xffffffff) &&
            (data[0x2] == 0x00000000) &&
            ((uint16_t) data[0x1] == (data[1] >> 16))) {
            /* flash is 32-bit */
            flash_mode = 32;
            device = data[1] & 0xffff;
            cb = get_chip_block_info(device);
            if ((cb->cb_chipid == 0) ||  // Unknown chip
                (data[0x4] != data[0x0]) || (data[0x5] != data[0x1]) ||
                (data[0x8] != data[0x0]) || (data[0x9] != data[0x1]) ||
                (data[0xc] != data[0x0]) || (data[0xd] != data[0x1])) {
                rc1 = MSG_STATUS_BAD_DATA;  // Unexpected ID values
            }
        } else {
            /* flash is 16-bit */
            flash_mode = 16;
            device = data[0] & 0xffff;
            cb = get_chip_block_info(device);
            if ((cb->cb_chipid == 0) ||  // Unknown chip
                (data[0x2] != data[0x0]) || (data[0x3] != data[0x1]) ||
                (data[0x4] != data[0x0]) || (data[0x5] != data[0x1]) ||
                (data[0x6] != data[0x0]) || (data[0x7] != data[0x1])) {
                rc1 = MSG_STATUS_BAD_DATA;  // Unexpected ID values
            }
        }
        if (mode != NULL)
            *mode = flash_mode;

        if (flash_mode == 16)
            smash_cmd_shift = 1;
        else
            smash_cmd_shift = 2;
    }
    *dev1 = (data[0] << 16) | ((uint16_t) data[1]);
    *dev2 = (data[0] & 0xffff0000) | (data[1] >> 16);

    return (rc1 ? rc1 : rc2);
}

/*
 * rom_copy
 * --------
 * Copy data from the ROM address space. The length is rounded up to a
 * multiple of four bytes.
 */
static void
rom_copy(void *dst, uint32_t addr, uint len)
{
    uint32_t *dst32 = dst;
    uint      count = (len + 3) / 4;

    while (count-- > 0) {
        *(dst32++) = *VADDR32(addr);
        addr += 4;
    }
}

static uint
read_from_flash(uint bank, uint addr, void *buf, uint len)
{
    uint rc;
    uint16_t bankarg = bank;
    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

    rc = send_cmd_core_retry(KS_CMD_BANK_SET | KS_BANK_SETTEMP,
                             &bankarg, sizeof (bankarg), NULL, 0, NULL);
    cia_spin(6);
    rom_copy(buf, ROM_BASE + addr, len);
    rc |= send_cmd_core_retry(KS_CMD_BANK_SET | KS_BANK_UNSETTEMP,
                              &bankarg, sizeof (bankarg), NULL, 0, NULL);
    cia_spin(CIA_USEC(1000));

    CACHE_FLUSH();
    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
    return (rc);
}

static int
wait_for_flash_done(uint addr, uint erase_mode, uint32_t expected)
{
    uint32_t status;
    uint32_t cstatus = 0;
    uint32_t lstatus;
    uint     spins = erase_mode ? 1000000 : 50000; // 1 sec or 50ms
    uint     spin_count = 0;
    int      same_count = 0;
    int      see_fail_count = 0;

    cia_spin(2);
    lstatus = *VADDR32(addr);
    while (spin_count < spins) {
        status = *VADDR32(addr);

        cstatus = status;
        /* Filter out checking of status which is already done */
        if (((cstatus ^ lstatus) & 0x0000ffff) == 0)
            cstatus &= ~0x0000ffff;
        if (((cstatus ^ lstatus) & 0xffff0000) == 0)
            cstatus &= ~0xffff0000;

        if (status == lstatus) {
            if (same_count++ >= 1) {
                /* Same for 2 tries */
                if (status == expected) {
                    return (0);  // Done
                }
                /* Something went wrong -- block protected? */
                return (MSG_STATUS_PRG_FAIL);
            }
        } else {
            same_count = 0;
            lstatus = status;
        }

        if (cstatus & (BIT(5) | BIT(5 + 16)))  // Program / erase failure
            if (see_fail_count++ > 5)
                break;
        __asm__ __volatile__("nop");
        cia_spin(1);
    }

    if (cstatus & (BIT(5) | BIT(5 + 16))) {
        /* Program / erase failure */
        return (MSG_STATUS_PRG_FAIL);
    }

    /* Program / erase timeout */
    return (MSG_STATUS_PRG_TMOUT);
}

/*
 * write_to_flash
 * --------------
 * Write data to flash. The length must be a multiple of four bytes, and
 * the area being written must have been previously erased.
 */
static uint
write_to_flash(uint bank, uint addr, void *buf, uint len)
{
    uint rc;
    uint rc2;
    uint xlen;
    uint cmd_tries = 0;
    uint write_tries = 0;
    uint8_t *xbuf = buf;
    uint16_t bankarg = bank;
    uint fail_data = 0;

    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

    /* Switch to flash bank to be programmed */
    rc = send_cmd_core_retry(KS_CMD_BANK_SET | KS_BANK_SETTEMP,
                             &bankarg, sizeof (bankarg), NULL, 0, NULL);
    cia_spin(CIA_USEC(100));
    if (rc != 0) {
        goto write_give_up;
    }

    /* Write flash data */
    while (len > 0) {
        xlen = len;
        if (xlen > 4)
            xlen = 4;

        /*
         * A typical Kickstart ROM has 0xffffffff values. This can
         * improve flash write performance by about 4%.
         */
        if (*ADDR32(xbuf) == *VADDR32(ROM_BASE + addr))
            goto skip_write;  // Destination already has value

        rc = flash_cmd_core(KS_CMD_FLASH_WRITE, xbuf, xlen);
        if (rc != 0) {
            uint count;
            for (count = 0; count < 10; count++) {
                (void) *VADDR32(ROM_BASE + addr);  // Address for write
                cia_spin(10);
            }
            flash_read_mode(0);
            if (cmd_tries++ < 5)
                continue;  // Try again
            break;         // Give up
        }
        cmd_tries = 0;

        (void) *VADDR32(ROM_BASE + addr);  // Generate address for write
        (void) *VADDR32(ROM_BASE + addr);  // Generate address for write
        rc = wait_for_flash_done(ROM_BASE + addr, 0, *ADDR32(xbuf));
        if (rc != 0) {
            if (write_tries++ < 5) {
                /*
                 * Try again after forcing read mode and verifying that
                 * the same or more bits are set in the destination.
                 */
                flash_read_mode(1);  // Force "try hard"
                cia_spin(10);
                if ((*ADDR32(xbuf) & *VADDR32(ROM_BASE + addr)) ==
                    *ADDR32(xbuf)) {
                    continue;  // Retry
                }
            }
            break;
        }
        if (flag_flash_write_bug &&
            ((addr & 0xff) == 0x54)) {  // 0x1554 and variants
            /*
             * Some flash parts have a bug where writes to the 0xAAA
             * offset and variants could get interpreted as a command,
             * potentially corrupting flash. Work around this by
             * stomping on the false "command" with a READ command.
             */
            flash_read_mode(0);
        }
        if (flag_conservative_mode &&
            ((*VADDR32(ROM_BASE + addr) != *ADDR32(xbuf)) ||
             (*VADDR32(ROM_BASE + addr) != *ADDR32(xbuf)))) {
            rc = MSG_STATUS_MISMATCH;
            break;
        }
        write_tries = 0;

skip_write:
        len  -= xlen;
        xbuf += xlen;
        addr += xlen;
    }
    cia_spin(CIA_USEC(10));
    if (rc != 0)
        fail_data = *VADDR32(ROM_BASE + addr);

    /* Restore flash to read mode */
    rc2 = flash_read_mode(1);  // Force "try harder"
    if (rc == 0)
        rc = rc2;
    cia_spin(CIA_USEC(10));

write_give_up:
    /* Return to "current" flash bank */
    rc2 = send_cmd_core_retry(KS_CMD_BANK_SET | KS_BANK_UNSETTEMP,
                              &bankarg, sizeof (bankarg), NULL, 0, NULL);
    if (rc == 0)
        rc = rc2;
    cia_spin(CIA_USEC(1000));

    CACHE_FLUSH();
    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();

    if (rc != 0) {
        flash_fail.ff_bank     = bank;
        flash_fail.ff_addr     = addr;
        flash_fail.ff_readback = fail_data;
        flash_fail.ff_expected = (len > 0) ? *ADDR32(xbuf) : 0;
    }
    return (rc);
}

static uint
erase_flash_block(uint bank, uint addr)
{
    uint rc;
    uint rc1;
    uint count;
    uint cmd_tries = 0;
    uint erase_tries = 0;
    uint16_t bankarg = bank;

    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

    /* Switch to flash bank to be erased */
    rc = send_cmd_core_retry(KS_CMD_BANK_SET | KS_BANK_SETTEMP,
                             &bankarg, sizeof (bankarg), NULL, 0, NULL);
    cia_spin(CIA_USEC(100));

    /* Send erase command */
try_erase_again:
    while (cmd_tries++ < 5) {
        rc = flash_cmd_core(KS_CMD_FLASH_ERASE, NULL, 0);
        if (rc == 0)
            break;
        for (count = 0; count < 10; count++) {
            (void) *VADDR32(ROM_BASE + addr);  // Generate address for erase
            cia_spin(5);
        }
        flash_read_mode(0);
    }

    if (rc == 0) {
        (void) *VADDR32(ROM_BASE + addr);  // Generate address for erase
        rc = wait_for_flash_done(ROM_BASE + addr, 1, 0xffffffff);
        if (rc != 0) {
            if (erase_tries++ < 5) {
                /*
                 * Try again after forcing read mode.
                 */
                (void) flash_read_mode(0);
                cia_spin(10);
                goto try_erase_again;
            }
        }
    }
    /* Restore flash to read mode */
    rc1 = flash_read_mode(1);  // Force "try harder"
    if (rc == 0)
        rc = rc1;

    cia_spin(CIA_USEC(10));

    /* Return to "current" flash bank */
    rc1 = send_cmd_core_retry(KS_CMD_BANK_SET | KS_BANK_UNSETTEMP,
                              &bankarg, sizeof (bankarg), NULL, 0, NULL);
    if (rc == 0)
        rc = rc1;
    cia_spin(CIA_USEC(1000));

    CACHE_FLUSH();
    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
    return (rc);
}

static uint
get_flash_bsize(const chip_blocks_t *cb, uint flash_addr)
{
    uint flash_bsize = cb->cb_bsize << (10 + smash_cmd_shift);
    uint flash_bnum  = flash_addr / flash_bsize;

    if (flash_bnum == cb->cb_bbnum) {
        /*
         * Boot block area has variable sub-block size.
         *
         * The map is 8 bits which are arranged in order such that Bit 0
         * represents the first sub-block and Bit 7 represents the last
         * sub-block.
         *
         * If a given bit is 1, this is the start of an erase block.
         * If a given bit is 0, then it is a continuation of the previous
         * bit's erase block.
         */
        uint bboff = flash_addr & (flash_bsize - 1);
        uint bsnum = (bboff / cb->cb_ssize) >> (10 + smash_cmd_shift);
        uint first_snum = bsnum;
        uint last_snum = bsnum;
        uint smap = cb->cb_map;

        /* Find first bit of this map */
        while (first_snum > 0) {
            if (smap & BIT(first_snum))  // Found base
                break;
            first_snum--;
        }
        while (++last_snum < 8) {
            if (smap & BIT(last_snum))  // Found next base
                break;
        }
        flash_bsize = (cb->cb_ssize * (last_snum - first_snum)) <<
                      (10 + smash_cmd_shift);
    }
    return (flash_bsize);
}


/* ------------------------------------------------------------------------
 * ROM file operations
 * ------------------------------------------------------------------------ */

/*
 * progress_start
 * --------------
 * Begin a new phase of an operation: update the status text and reset
 * the progress bar.
 */
static void
progress_start(const char *what, uint bank)
{
    prog_percent = 0;
    prog_fill    = 0;
    set_status("%s bank %u", what, bank);
    draw_progress();
}

/*
 * progress_update
 * ---------------
 * Update the progress bar and percent complete.
 */
static void
progress_update(uint done, uint total)
{
    uint inner_w = lay.prog_w - 4;
    uint fill;
    int  percent;

    /* Scale values so that multiplication can not overflow */
    while (total > 0xffff) {
        total >>= 1;
        done  >>= 1;
    }
    if (total == 0)
        total = 1;
    if (done > total)
        done = total;

    percent = done * 100 / total;
    fill    = done * inner_w / total;
    if ((percent != prog_percent) || (fill != prog_fill)) {
        prog_percent = percent;
        prog_fill    = fill;
        draw_progress();
    }
}

/*
 * progress_clear
 * --------------
 * Remove the progress display of the previous operation.
 */
static void
progress_clear(void)
{
    if (prog_percent >= 0) {
        prog_percent = -1;
        prog_fill    = 0;
        draw_progress();
    }
}

/*
 * poll_abort
 * ----------
 * Handle window events while an operation is in progress. Only the Stop
 * gadget and window refresh are honored. Returns non-zero if the user
 * has requested that the operation stop.
 */
static uint
poll_abort(void)
{
    struct IntuiMessage *msg;

    while ((msg = GT_GetIMsg(window->UserPort)) != NULL) {
        ULONG class = msg->Class;
        UWORD gid   = 0;

        if (class == IDCMP_GADGETUP)
            gid = ((struct Gadget *) msg->IAddress)->GadgetID;
        GT_ReplyIMsg(msg);

        switch (class) {
            case IDCMP_REFRESHWINDOW:
                GT_BeginRefresh(window);
                draw_window();
                GT_EndRefresh(window, TRUE);
                break;
            case IDCMP_GADGETUP:
                if (gid == ID_STOP)
                    op_abort = 1;
                break;
        }
    }
    return (op_abort);
}

/*
 * set_busy
 * --------
 * Disable gadgets and menus which may not be used while an operation is
 * in progress, and enable the Stop gadget. This is reversed when the
 * operation completes.
 */
static void
set_busy(uint on)
{
    struct Gadget *list[7];
    uint           cur;

    list[0] = gadget_program;
    list[1] = gadget_read;
    list[2] = gadget_verify;
    list[3] = gadget_file_select;
    list[4] = gadget_reload;
    list[5] = gadget_save;
    list[6] = gadget_switch;

    /* Save and Switch are restored by update_buttons() */
    for (cur = 0; cur < (on ? 7 : 5); cur++)
        GT_SetGadgetAttrs(list[cur], window, NULL,
                          GA_Disabled, on ? TRUE : FALSE, TAG_DONE);
    GT_SetGadgetAttrs(gadget_stop, window, NULL,
                      GA_Disabled, on ? FALSE : TRUE, TAG_DONE);
    busy = on;
    op_abort = 0;

    if (on) {
        ClearMenuStrip(window);
    } else {
        ResetMenuStrip(window, menus);

        /* Events were discarded while busy; put gadgets back in sync */
        GT_SetGadgetAttrs(gadget_poweron, window, NULL,
                          GTMX_Active, info.bi_bank_poweron & 7, TAG_DONE);
        GT_SetGadgetAttrs(gadget_select, window, NULL,
                          GTMX_Active, bank_select, TAG_DONE);
        update_buttons(1);
    }
}

/*
 * dos_error
 * ---------
 * Returns a string describing the most recent AmigaDOS error.
 */
static const char *
dos_error(void)
{
    static char buf[82];
    LONG        err = IoErr();

    buf[0] = '\0';
    if ((err == 0) || (Fault(err, NULL, (STRPTR) buf, sizeof (buf)) == 0))
        sprintf(buf, "Error %d", (int) err);
    return (buf);
}

/*
 * file_size
 * ---------
 * Returns the size of an open file, or -1 on failure. The file position
 * is left at the start of the file.
 */
static LONG
file_size(BPTR fh)
{
    if (Seek(fh, 0, OFFSET_END) < 0)
        return (-1);
    return (Seek(fh, 0, OFFSET_BEGINNING));
}

static const char *const swap_names[] = {
    "Auto", "0123", "1032", "2301", "3210"
};

/*
 * execute_swapmode
 * ----------------
 * Swap bytes in the specified buffer according to the swap mode. Each of
 * the swap modes is its own inverse, so the same swap is used whether the
 * data is on its way to flash or came from flash.
 */
static void
execute_swapmode(uint8_t *buf, uint len, uint mode)
{
    uint    pos;
    uint8_t temp;

    switch (mode) {
        case SWAP_1032:
            /* Swap adjacent bytes in 16-bit words */
            for (pos = 0; pos + 1 < len; pos += 2) {
                temp         = buf[pos + 0];
                buf[pos + 0] = buf[pos + 1];
                buf[pos + 1] = temp;
            }
            break;
        case SWAP_2301:
            /* Swap adjacent (16-bit) words */
            for (pos = 0; pos + 3 < len; pos += 4) {
                temp         = buf[pos + 0];
                buf[pos + 0] = buf[pos + 2];
                buf[pos + 2] = temp;
                temp         = buf[pos + 1];
                buf[pos + 1] = buf[pos + 3];
                buf[pos + 3] = temp;
            }
            break;
        case SWAP_3210:
            /* Swap bytes in 32-bit longs */
            for (pos = 0; pos + 3 < len; pos += 4) {
                temp         = buf[pos + 0];
                buf[pos + 0] = buf[pos + 3];
                buf[pos + 3] = temp;
                temp         = buf[pos + 1];
                buf[pos + 1] = buf[pos + 2];
                buf[pos + 2] = temp;
            }
            break;
        default:
            break;  // SWAP_0123: Normal (no swap)
    }
}

/*
 * swap_detect
 * -----------
 * Determine the swap mode to use from the first four bytes of a ROM
 * image. A Kickstart ROM starts with 11 xx 4e f9 when its bytes are in
 * the order in which the Amiga sees them (xx is 11 for a 256 KB ROM and
 * 14 for a 512 KB ROM). The swap mode returned is the one which will put
 * a recognized image in that order. An image which is not recognized is
 * not swapped.
 *
 * This probe must only be done once for an image, with the start of its
 * first chunk. All following chunks of the image then use the swap mode
 * which was detected, as they do not start with a ROM header.
 */
static uint
swap_detect(const uint8_t *buf)
{
    if ((buf[1] == 0x11) && (buf[2] == 0xf9) && (buf[3] == 0x4e))
        return (SWAP_1032);  // Odd/even bytes are swapped
    if ((buf[0] == 0x4e) && (buf[1] == 0xf9) && (buf[2] == 0x11))
        return (SWAP_2301);  // 16-bit words are swapped
    if ((buf[0] == 0xf9) && (buf[1] == 0x4e) && (buf[3] == 0x11))
        return (SWAP_3210);  // Bytes in 32-bit longs are reversed
    return (SWAP_0123);
}

/*
 * swap_for_file
 * -------------
 * Returns the swap mode to use for all chunks of the specified open ROM
 * image file. This is the mode selected in the Swap menu, unless that is
 * Auto, where the swap mode is then probed once from the start of the
 * first chunk of the file. The file position is left at the start of the
 * file.
 */
static uint
swap_for_file(BPTR fh)
{
    uint8_t hdr[4];
    uint    mode = swap_mode;

    if (mode == SWAP_AUTO) {
        mode = SWAP_0123;
        if (Read(fh, hdr, sizeof (hdr)) == sizeof (hdr))
            mode = swap_detect(hdr);
        (void) Seek(fh, 0, OFFSET_BEGINNING);
    }
    return (mode);
}

/*
 * swap_suffix
 * -----------
 * Returns text to add to a status message when an image was swapped.
 */
static const char *
swap_suffix(uint mode)
{
    static char buf[16];

    buf[0] = '\0';
    if ((mode != SWAP_0123) && (mode != SWAP_AUTO))
        sprintf(buf, ", swap %s", swap_names[mode]);
    return (buf);
}

/*
 * rom_op_setup
 * ------------
 * Verify that the selected bank and the specified file name may be used
 * for a ROM file operation. Returns 0 on success, with the size of the
 * bank (including merged banks) stored in bank_size and the file name
 * stored in op_filename. The file name is captured because the user is
 * still able to edit the file name gadget while an operation is running.
 *
 * If writing is non-zero, the bank is to be erased and programmed. This is
 * refused for the bank which the Amiga is currently running from.
 */
static uint
rom_op_setup(uint bank, uint *bank_size, uint writing)
{
    bank_info_t bi;
    uint        rlen;
    uint        rc;
    uint        bank_sub;
    uint        bank_last;

    pull_edits();

    if (bank >= ROM_BANKS) {
        show_error("No bank is selected.");
        return (1);
    }
    if (merge_pending())
        return (1);
    strncpy(op_filename, SI_BUF(gadget_file), sizeof (op_filename) - 1);
    op_filename[sizeof (op_filename) - 1] = '\0';
    if (op_filename[0] == '\0') {
        show_error("Enter a ROM file name, or choose one with Select...");
        return (1);
    }

    rc = send_cmd_retry(KS_CMD_BANK_INFO, NULL, 0, &bi, sizeof (bi), &rlen);
    if (rc != 0) {
        show_error("Failed to get bank information\n%s", smash_err(rc));
        return (rc);
    }
    bank_sub = bi.bi_merge[bank] & 0x0f;
    if (bank_sub != 0) {
        show_error("Bank %u is part of a merged bank, but is not the first.\n"
                   "Select bank %u instead.", bank, bank - bank_sub);
        return (1);
    }
    *bank_size = bank_size_bytes(&bi, bank);

    bank_last = bank + *bank_size / ROM_WINDOW_SIZE - 1;
    if (writing &&
        (bi.bi_bank_current >= bank) && (bi.bi_bank_current <= bank_last)) {
        if (bank_last == bank) {
            show_error("Bank %u is the bank which the Amiga is currently\n"
                       "running from. It will not be erased or programmed.\n"
                       "Switch to another bank first.", bank);
        } else {
            show_error("Banks %u-%u are merged, and include bank %u, which\n"
                       "the Amiga is currently running from. They will not\n"
                       "be erased or programmed. Switch to another bank "
                       "first.", bank, bank_last, bi.bi_bank_current);
        }
        return (1);
    }
    return (0);
}

/*
 * flash_identify
 * --------------
 * Acquire flash ID and from it determine the erase block layout.
 * Returns NULL on failure, after having reported the problem.
 */
static const chip_blocks_t *
flash_identify(void)
{
    const chip_blocks_t *cb;
    const chip_blocks_t *cb2;
    uint32_t             flash_dev1;
    uint32_t             flash_dev2;
    uint                 mode = 0;
    int                  rc;

    rc = flash_id(&flash_dev1, &flash_dev2, &mode);
    if (rc != 0) {
        show_error("Flash ID failure\n%s\nID %08x %08x",
                   smash_err(rc), (uint) flash_dev1, (uint) flash_dev2);
        return (NULL);
    }
    if (strcmp(ee_id_string(flash_dev1), "Unknown") == 0) {
        show_error("Failed to identify flash device 1 (%08x)",
                   (uint) flash_dev1);
        return (NULL);
    }
    if ((mode == 32) && (strcmp(ee_id_string(flash_dev2), "Unknown") == 0)) {
        show_error("Failed to identify flash device 2 (%08x)",
                   (uint) flash_dev2);
        return (NULL);
    }

    cb = get_chip_block_info(flash_dev1);
    if (mode == 32) {
        cb2 = get_chip_block_info(flash_dev2);
        if ((cb->cb_bbnum != cb2->cb_bbnum) ||
            (cb->cb_bsize != cb2->cb_bsize) ||
            (cb->cb_ssize != cb2->cb_ssize) ||
            (cb->cb_map != cb2->cb_map)) {
            show_error("Flash devices are not compatible (%08x %08x)",
                       (uint) flash_dev1, (uint) flash_dev2);
            return (NULL);
        }
    }
    return (cb);
}

/*
 * rom_erase
 * ---------
 * Erase the flash blocks which include the specified area of a bank.
 */
static uint
rom_erase(const chip_blocks_t *cb, uint bank, uint addr, uint len)
{
    uint rc = 0;
    uint total;
    uint flash_start_addr;
    uint flash_end_addr;
    uint flash_start_bsize;
    uint flash_end_bsize;

    flash_start_addr  = bank * ROM_WINDOW_SIZE + addr;
    flash_end_addr    = bank * ROM_WINDOW_SIZE + addr + len - 1;
    flash_start_bsize = get_flash_bsize(cb, flash_start_addr);
    flash_end_bsize   = get_flash_bsize(cb, flash_end_addr);

    /* Round start address down and end address up, then compute length */
    flash_start_addr = flash_start_addr & ~(flash_start_bsize - 1);
    flash_end_addr   = (flash_end_addr | (flash_end_bsize - 1)) + 1;
    len  = flash_end_addr - flash_start_addr;
    addr = addr & ~(flash_start_bsize - 1);

    bank += addr / ROM_WINDOW_SIZE;
    addr &= (ROM_WINDOW_SIZE - 1);

    total = len;
    progress_start("Erasing", bank);
    while (len > 0) {
        uint xlen = get_flash_bsize(cb, bank * ROM_WINDOW_SIZE + addr);

        rc = erase_flash_block(bank, addr);
        if (rc != 0)
            break;

        if (xlen > len)
            xlen = len;
        len  -= xlen;
        addr += xlen;
        if (addr >= ROM_WINDOW_SIZE) {
            addr -= ROM_WINDOW_SIZE;
            bank++;
        }
        progress_update(total - len, total);
        if (poll_abort() && (len > 0)) {
            rc = MSG_STATUS_FAIL;
            break;
        }
    }
    return (rc);
}

/*
 * rom_verify
 * ----------
 * Compare flash content with the content of a file. The file must be
 * positioned at the offset to compare. The swap mode is that which would
 * be used to program the file. Returns 0 if the content matches.
 * If a miscompare is found, 1 is returned and details are reported to
 * the user.
 */
static uint
rom_verify(BPTR fh, uint bank, uint len, uint8_t *buf, uint8_t *vbuf,
           uint swap)
{
    uint rc = 0;
    uint addr = 0;
    uint total = len;

    progress_start("Verifying", bank);
    while (len > 0) {
        uint xlen = len;

        if (xlen > MAX_CHUNK)
            xlen = MAX_CHUNK;
        if (xlen > ROM_WINDOW_SIZE - addr)
            xlen = ROM_WINDOW_SIZE - addr;

        /* Read from file */
        if (Read(fh, vbuf, xlen) != (LONG) xlen) {
            show_error("Failed to read %u bytes from file\n%s",
                       xlen, dos_error());
            return (1);
        }

        /* Read from flash */
        rc = read_from_flash(bank, addr, buf, xlen);
        if (rc != 0) {
            show_error("KickSmash failure reading bank %u\n%s",
                       bank, smash_err(rc));
            return (rc);
        }

        /* Put flash data in the byte order of the file */
        execute_swapmode(buf, (xlen + 3) & ~3, swap);

        if (memcmp(buf, vbuf, xlen) != 0) {
            uint pos;
            uint first = xlen;
            uint count = 0;

            for (pos = 0; pos < xlen; pos++) {
                if (buf[pos] != vbuf[pos]) {
                    if (count++ == 0)
                        first = pos;
                }
            }
            show_error("Verify failure at bank %u address 0x%05x\n"
                       "Flash %02x != file %02x\n"
                       "%u bytes differ in this %u KB block",
                       bank, addr + first, buf[first], vbuf[first],
                       count, MAX_CHUNK >> 10);
            return (1);
        }

        len  -= xlen;
        addr += xlen;
        if (addr >= ROM_WINDOW_SIZE) {
            addr -= ROM_WINDOW_SIZE;
            bank++;
        }
        progress_update(total - len, total);
        if (poll_abort() && (len > 0))
            return (MSG_STATUS_FAIL);
    }
    return (0);
}

/*
 * op_program
 * ----------
 * Erase the selected bank and then program it with the content of the
 * ROM file. Optionally verify the bank once programming is complete.
 */
static void
op_program(void)
{
    const chip_blocks_t *cb;
    const char *filename = op_filename;
    uint8_t    *buf;
    uint8_t    *vbuf;
    BPTR        fh;
    LONG        fsize;
    uint        start_bank = bank_select;
    uint        bank = bank_select;
    uint        bank_size;
    uint        addr = 0;
    uint        len;
    uint        total;
    uint        do_verify;
    uint        swap;
    uint        rc;
    char        swaptext[48];

    if (rom_op_setup(bank, &bank_size, 1))
        return;

    fh = Open((STRPTR) filename, MODE_OLDFILE);
    if (fh == 0) {
        show_error("Failed to open \"%s\"\n%s", FilePart((STRPTR) filename),
                   dos_error());
        return;
    }
    fsize = file_size(fh);
    if (fsize <= 0) {
        show_error("\"%s\" is empty or its size could not be determined.",
                   FilePart((STRPTR) filename));
        goto fail_close;
    }
    len = fsize;
    if (len > bank_size) {
        show_error("\"%s\" is %u bytes, which is larger than\n"
                   "the %u KB size of bank %u.",
                   FilePart((STRPTR) filename), len, bank_size >> 10, bank);
        goto fail_close;
    }

    swap = swap_for_file(fh);
    swaptext[0] = '\0';
    if (swap != SWAP_0123) {
        sprintf(swaptext, "\nThe file will be byte swapped (%s%s).",
                (swap_mode == SWAP_AUTO) ? "Auto: " : "", swap_names[swap]);
    }

    if (ask("Program|Cancel",
            "Erase bank %u \"%s\" and program it with\n"
            "\"%s\" (%u bytes)?%s",
            bank, info_saved.bi_name[bank], FilePart((STRPTR) filename), len,
            swaptext) == 0) {
        goto fail_close;
    }

    /* Acquire flash id to get block erase zones */
    cb = flash_identify();
    if (cb == NULL)
        goto fail_close;

    buf  = AllocMem(MAX_CHUNK, MEMF_PUBLIC);
    vbuf = AllocMem(MAX_CHUNK, MEMF_PUBLIC);
    if ((buf == NULL) || (vbuf == NULL)) {
        show_error("Failed to allocate memory");
        goto fail_free;
    }

    do_verify = (gadget_verify_after->Flags & GFLG_SELECTED) ? 1 : 0;
    set_busy(1);

    rc = rom_erase(cb, bank, 0, len);
    if (rc != 0) {
        if (op_abort) {
            set_status("Erase stopped: bank %u is incomplete", start_bank);
        } else {
            set_status("Erase of bank %u failed", start_bank);
            show_error("Erase failure in bank %u\n%s",
                       start_bank, smash_err(rc));
        }
        goto fail_busy;
    }

    total = len;
    progress_start("Programming", bank);
    while (len > 0) {
        uint xlen = len;
        uint wlen;

        if (xlen > WRITE_CHUNK)
            xlen = WRITE_CHUNK;
        if (xlen > ROM_WINDOW_SIZE - addr)
            xlen = ROM_WINDOW_SIZE - addr;

        /* Read from file */
        wlen = (xlen + 3) & ~3;
        memset(buf + (xlen & ~3), 0xff, 4);
        if (Read(fh, buf, xlen) != (LONG) xlen) {
            set_status("Programming bank %u failed", start_bank);
            show_error("Failed to read %u bytes from \"%s\"\n%s",
                       xlen, FilePart((STRPTR) filename), dos_error());
            goto fail_busy;
        }
        execute_swapmode(buf, wlen, swap);

        /* Write to flash */
        rc = write_to_flash(bank, addr, buf, wlen);
        if (rc != 0) {
            set_status("Programming bank %u failed", start_bank);
            show_error("Write failure at bank %u address 0x%05x\n"
                       "Readback %08x, file %08x\n%s",
                       flash_fail.ff_bank, flash_fail.ff_addr,
                       (uint) flash_fail.ff_readback,
                       (uint) flash_fail.ff_expected, smash_err(rc));
            goto fail_busy;
        }

        len  -= xlen;
        addr += xlen;
        if (addr >= ROM_WINDOW_SIZE) {
            addr -= ROM_WINDOW_SIZE;
            bank++;
        }
        progress_update(total - len, total);
        if (poll_abort() && (len > 0)) {
            set_status("Programming stopped: bank %u is incomplete",
                       start_bank);
            goto fail_busy;
        }
    }

    if (do_verify) {
        (void) Seek(fh, 0, OFFSET_BEGINNING);
        rc = rom_verify(fh, start_bank, total, buf, vbuf, swap);
        if (rc != 0) {
            if (op_abort)
                set_status("Bank %u programmed; verify stopped", start_bank);
            else
                set_status("Verify of bank %u failed", start_bank);
            goto fail_busy;
        }
        set_status("Bank %u programmed and verified%s", start_bank,
                   swap_suffix(swap));
    } else {
        set_status("Bank %u programmed%s", start_bank, swap_suffix(swap));
    }

fail_busy:
    set_busy(0);
fail_free:
    if (buf != NULL)
        FreeMem(buf, MAX_CHUNK);
    if (vbuf != NULL)
        FreeMem(vbuf, MAX_CHUNK);
fail_close:
    Close(fh);
}

/*
 * op_read
 * -------
 * Save the content of the selected bank to the ROM file.
 */
static void
op_read(void)
{
    const char *filename = op_filename;
    uint8_t    *buf;
    BPTR        lock;
    BPTR        fh;
    uint        start_bank = bank_select;
    uint        bank = bank_select;
    uint        bank_size;
    uint        addr = 0;
    uint        len;
    uint        total;
    uint        failed = 1;
    uint        swap = swap_mode;
    uint        rc;

    if (rom_op_setup(bank, &bank_size, 0))
        return;

    lock = Lock((STRPTR) filename, ACCESS_READ);
    if (lock != 0) {
        UnLock(lock);
        if (ask("Replace|Cancel", "\"%s\" already exists.\nReplace it with "
                "the content of bank %u?",
                FilePart((STRPTR) filename), bank) == 0) {
            return;
        }
    }

    buf = AllocMem(MAX_CHUNK, MEMF_PUBLIC);
    if (buf == NULL) {
        show_error("Failed to allocate memory");
        return;
    }
    fh = Open((STRPTR) filename, MODE_NEWFILE);
    if (fh == 0) {
        show_error("Failed to open \"%s\" for write\n%s",
                   FilePart((STRPTR) filename), dos_error());
        FreeMem(buf, MAX_CHUNK);
        return;
    }

    set_busy(1);
    len   = bank_size;
    total = len;
    progress_start("Reading", bank);
    while (len > 0) {
        uint xlen = len;

        if (xlen > MAX_CHUNK)
            xlen = MAX_CHUNK;
        if (xlen > ROM_WINDOW_SIZE - addr)
            xlen = ROM_WINDOW_SIZE - addr;

        /* Read from flash */
        rc = read_from_flash(bank, addr, buf, xlen);
        if (rc != 0) {
            set_status("Read of bank %u failed", start_bank);
            show_error("KickSmash failure reading bank %u\n%s",
                       bank, smash_err(rc));
            goto fail;
        }

        /*
         * Auto swap mode is probed once, from the first chunk of the
         * bank. All following chunks use the mode which was detected.
         */
        if ((swap == SWAP_AUTO) && (len == total))
            swap = swap_detect(buf);
        execute_swapmode(buf, xlen, swap);

        /* Write to file */
        if (Write(fh, buf, xlen) != (LONG) xlen) {
            set_status("Read of bank %u failed", start_bank);
            show_error("Failed to write to \"%s\"\n%s",
                       FilePart((STRPTR) filename), dos_error());
            goto fail;
        }

        len  -= xlen;
        addr += xlen;
        if (addr >= ROM_WINDOW_SIZE) {
            addr -= ROM_WINDOW_SIZE;
            bank++;
        }
        progress_update(total - len, total);
        if (poll_abort() && (len > 0)) {
            set_status("Read of bank %u stopped", start_bank);
            goto fail;
        }
    }
    failed = 0;
    set_status("Bank %u saved to file (%u KB)%s", start_bank, total >> 10,
               swap_suffix(swap));

fail:
    Close(fh);
    if (failed)
        DeleteFile((STRPTR) filename);  // Do not leave a partial image
    FreeMem(buf, MAX_CHUNK);
    set_busy(0);
}

/*
 * op_verify
 * ---------
 * Verify that the content of the selected bank matches the ROM file.
 */
static void
op_verify(void)
{
    const char *filename = op_filename;
    uint8_t    *buf;
    uint8_t    *vbuf;
    BPTR        fh;
    LONG        fsize;
    uint        bank = bank_select;
    uint        bank_size;
    uint        swap;
    uint        rc;

    if (rom_op_setup(bank, &bank_size, 0))
        return;

    fh = Open((STRPTR) filename, MODE_OLDFILE);
    if (fh == 0) {
        show_error("Failed to open \"%s\"\n%s", FilePart((STRPTR) filename),
                   dos_error());
        return;
    }
    fsize = file_size(fh);
    if (fsize <= 0) {
        show_error("\"%s\" is empty or its size could not be determined.",
                   FilePart((STRPTR) filename));
        Close(fh);
        return;
    }
    swap = swap_for_file(fh);
    if ((uint) fsize > bank_size) {
        show_error("\"%s\" is %u bytes, which is larger than\n"
                   "the %u KB size of bank %u.",
                   FilePart((STRPTR) filename), (uint) fsize,
                   bank_size >> 10, bank);
        Close(fh);
        return;
    }

    buf  = AllocMem(MAX_CHUNK, MEMF_PUBLIC);
    vbuf = AllocMem(MAX_CHUNK, MEMF_PUBLIC);
    if ((buf == NULL) || (vbuf == NULL)) {
        show_error("Failed to allocate memory");
    } else {
        set_busy(1);
        rc = rom_verify(fh, bank, fsize, buf, vbuf, swap);
        if (rc == 0)
            set_status("Bank %u matches file (%u KB)%s", bank,
                       (uint) fsize >> 10, swap_suffix(swap));
        else if (op_abort)
            set_status("Verify of bank %u stopped", bank);
        else
            set_status("Verify of bank %u failed", bank);
        set_busy(0);
    }
    if (buf != NULL)
        FreeMem(buf, MAX_CHUNK);
    if (vbuf != NULL)
        FreeMem(vbuf, MAX_CHUNK);
    Close(fh);
}

/*
 * select_file
 * -----------
 * Open a file requester for the user to choose the ROM image file.
 */
static void
select_file(void)
{
    struct FileRequester *fr;
    char                  drawer[256];
    char                 *file = SI_BUF(gadget_file);

    if (AslBase == NULL) {
        show_error("asl.library could not be opened.\n"
                   "Type the ROM file name instead.");
        return;
    }

    /* Start the requester at the location of the current file name */
    strncpy(drawer, file, sizeof (drawer) - 1);
    drawer[sizeof (drawer) - 1] = '\0';
    *PathPart((STRPTR) drawer) = '\0';

    fr = AllocAslRequestTags(ASL_FileRequest,
                             ASLFR_Window, (ULONG) window,
                             ASLFR_SleepWindow, TRUE,
                             ASLFR_TitleText, (ULONG) "Select ROM image file",
                             ASLFR_InitialDrawer, (ULONG) drawer,
                             ASLFR_InitialFile, (ULONG) FilePart((STRPTR) file),
                             ASLFR_RejectIcons, TRUE,
                             TAG_DONE);
    if (fr == NULL) {
        show_error("Failed to open file requester");
        return;
    }
    if (AslRequest(fr, NULL)) {
        strncpy(rom_filename, (char *) fr->fr_Drawer,
                sizeof (rom_filename) - 1);
        rom_filename[sizeof (rom_filename) - 1] = '\0';
        if (AddPart((STRPTR) rom_filename, fr->fr_File,
                    sizeof (rom_filename))) {
            GT_SetGadgetAttrs(gadget_file, window, NULL,
                              GTST_String, (ULONG) rom_filename, TAG_DONE);
        } else {
            show_error("File name is too long");
        }
    }
    FreeAslRequest(fr);
}


/* ------------------------------------------------------------------------
 * Clock
 * ------------------------------------------------------------------------ */

static int
OpenTimer(void)
{
    return (OpenDevice((STRPTR) TIMERNAME, UNIT_MICROHZ,
                       (struct IORequest *) &TimeRequest, 0L));
}

static void
CloseTimer(void)
{
    CloseDevice((struct IORequest *) &TimeRequest);
}

static void
setsystime(uint sec, uint usec)
{
    TimeRequest.tr_node.io_Command = TR_SETSYSTIME;
    TimeRequest.tr_time.tv_secs = sec;
    TimeRequest.tr_time.tv_micro = usec;
    DoIO((struct IORequest *) &TimeRequest);
}

static uint
getsystime(uint *usec)
{
    TimeRequest.tr_node.io_Command = TR_GETSYSTIME;
    DoIO((struct IORequest *) &TimeRequest);
    *usec = TimeRequest.tr_time.tv_micro;
    return (TimeRequest.tr_time.tv_secs);
}

/*
 * time_string
 * -----------
 * Convert Amiga time (seconds since 1978) to a date and time string.
 */
static const char *
time_string(uint sec)
{
    static char     buf[2 * LEN_DATSTRING + 2];
    struct DateTime dtime;
    char            datebuf[LEN_DATSTRING];
    char            timebuf[LEN_DATSTRING];
    uint            min = sec / 60;

    dtime.dat_Stamp.ds_Days   = min / (24 * 60);
    dtime.dat_Stamp.ds_Minute = min % (24 * 60);
    dtime.dat_Stamp.ds_Tick   = (sec % 60) * TICKS_PER_SECOND;
    dtime.dat_Format          = FORMAT_DOS;
    dtime.dat_Flags           = 0x0;
    dtime.dat_StrDay          = NULL;
    dtime.dat_StrDate         = (STRPTR) datebuf;
    dtime.dat_StrTime         = (STRPTR) timebuf;
    if (DateToStr(&dtime))
        sprintf(buf, "%s %s", datebuf, timebuf);
    else
        sprintf(buf, "%u seconds", sec);
    return (buf);
}

static uint
get_ks_clock(uint *sec, uint *usec)
{
    uint ks_clock[2];
    uint rc;
    uint rlen;

    *sec  = 0;
    *usec = 0;
    rc = send_cmd(KS_CMD_CLOCK, NULL, 0, &ks_clock, sizeof (ks_clock), &rlen);
    if (rc == 0) {
        *sec  = ks_clock[0];
        *usec = ks_clock[1];
    }
    return (rc);
}

static uint
set_ks_clock(uint sec, uint usec)
{
    uint ks_clock[2];

    ks_clock[0] = sec;
    ks_clock[1] = usec;

    return (send_cmd(KS_CMD_CLOCK | KS_CLOCK_SET, ks_clock, sizeof (ks_clock),
                     NULL, 0, NULL));
}

static uint
get_host_clock(uint *sec, uint *usec)
{
    hm_clock_t  hm;
    hm_clock_t *hmr;
    uint        rc;
    uint        rlen;

    *sec  = 0;
    *usec = 0;
    memset(&hm, 0, sizeof (hm));  // Clock get
    hm.hm_hdr.km_op = KM_OP_CLOCK;

    rc = host_msg(&hm, sizeof (hm), (void **) &hmr, &rlen);
    if (rc == 0) {
        *sec  = hmr->hm_sec;
        *usec = hmr->hm_usec;
    }
    return (rc);
}

/*
 * clock_load
 * ----------
 * Set the Amiga clock from either the USB host or from Kicksmash.
 */
static void
clock_load(uint from_host)
{
    const char *source = from_host ? "USB host" : "KickSmash";
    uint        sec;
    uint        usec;
    uint        rc;

    if (from_host) {
        uint16_t states[2];

        /* Check that an application on the USB host is there to answer */
        rc = send_cmd_retry(KS_CMD_MSG_STATE, NULL, 0, states,
                            sizeof (states), NULL);
        if ((rc == 0) && ((states[1] & MSG_STATE_SERVICE_UP) == 0)) {
            set_status("USB host is not available");
            show_error("The USB host is not answering.\n"
                       "Is hostsmash running on the host with the\n"
                       "message service (-m) enabled?");
            return;
        }
        if (rc == 0)
            rc = get_host_clock(&sec, &usec);
    } else {
        rc = get_ks_clock(&sec, &usec);
    }
    if (rc != 0) {
        set_status("Failed to get %s clock", source);
        show_error("Failed to get clock from %s\n%s", source, smash_err(rc));
        return;
    }
    if ((sec == 0) && (usec == 0)) {
        set_status("%s clock is not set", source);
        show_error("%s does not know the current time.", source);
        return;
    }
    if (OpenTimer()) {
        show_error("Failed to open timer.device");
        return;
    }
    setsystime(sec, usec);
    CloseTimer();
    set_status("Clock from %s: %s", from_host ? "host" : "KickSmash",
               time_string(sec));
}

/*
 * clock_save
 * ----------
 * Save the Amiga clock to Kicksmash.
 */
static void
clock_save(void)
{
    uint sec;
    uint usec;
    uint rc;

    if (OpenTimer()) {
        show_error("Failed to open timer.device");
        return;
    }
    sec = getsystime(&usec);
    CloseTimer();

    rc = set_ks_clock(sec, usec);
    if (rc != 0) {
        set_status("Failed to set KickSmash clock");
        show_error("Failed to set KickSmash clock\n%s", smash_err(rc));
        return;
    }
    set_status("Clock to KickSmash: %s", time_string(sec));
}


/* ------------------------------------------------------------------------
 * Window display
 * ------------------------------------------------------------------------ */

/* Positions of the Auto Switch gadgets, which follow the "Auto Switch" text */
#define AUTO_BANK_X  (lay.colb_x + 17 * lay.fw)
#define AUTO_BANK_W  (2 * lay.fw + 12)
#define AUTO_TIME_X  (AUTO_BANK_X + AUTO_BANK_W + 9 * lay.fw)
#define AUTO_TIME_W  (5 * lay.fw + 12)

/*
 * layout_compute
 * --------------
 * Compute positions of everything in the window from the size of the
 * font which is in use.
 */
static void
layout_compute(uint compact)
{
    WORD fw  = font->tf_XSize;
    WORD fh  = font->tf_YSize;
    WORD gap = compact ? 1 : 3;
    WORD top_end;
    WORD xoff;
    uint col;

    lay.fw      = fw;
    lay.fh      = fh;
    lay.gh      = fh + 4;
    lay.width   = WIN_CHARS * fw;
    lay.margin  = fw;
    lay.colb_x  = lay.margin + (ID_CHARS + 1) * fw;

    /* The top block is the taller of three text lines and two gadgets */
    lay.ty[0]   = 2;
    lay.ty[1]   = lay.ty[0] + lay.gh + (compact ? 0 : 1);
    top_end     = lay.ty[1] + lay.gh;
    if (top_end < lay.ty[0] + 1 + 3 * (fh + 1))
        top_end = lay.ty[0] + 1 + 3 * (fh + 1);

    lay.row_h   = fh + (compact ? 1 : 2);
    lay.el_y    = (lay.row_h - fh) / 2;
    lay.table_y = top_end + gap;
    lay.rows_y  = lay.table_y + fh + 6;
    lay.table_h = fh + 6 + ROM_BANKS * lay.row_h + 3;
    lay.mx_w    = (fw * 13) / 4;
    lay.pm_w    = fw + 6;
    lay.pm_x    = ((banktable_widths[COL_LONGRESET] + 1) * fw -
                   (2 * lay.pm_w + fw + 6)) / 2;

    lay.file_y  = lay.table_y + lay.table_h + gap + 1;
    lay.ops_y   = lay.file_y + lay.gh + gap;
    lay.stat_y  = lay.ops_y + lay.gh + gap + 1;
    lay.prog_h  = fh + 2;
    lay.prog_x  = lay.margin + (STATUS_CHARS + 1) * fw;
    lay.prog_w  = lay.width - lay.margin - 4 * fw - 4 - lay.prog_x;
    lay.btn_y   = lay.stat_y + lay.prog_h + gap + 1;
    lay.inner_h = lay.btn_y + lay.gh + 2;

    /* Center the bank table */
    lay.table_w = 6;
    for (col = 0; col < COL_COUNT; col++)
        lay.table_w += (banktable_widths[col] + 1) * fw;
    lay.table_x = (lay.width - lay.table_w) / 2;

    xoff = lay.table_x + 3;
    for (col = 0; col < COL_COUNT; col++) {
        banktable_pos[col] = xoff;
        xoff += (banktable_widths[col] + 1) * fw;
    }
}

/*
 * draw_status
 * -----------
 * Display the status line.
 */
static void
draw_status(void)
{
    text_at(lay.margin, lay.stat_y + 1, status_text, STATUS_CHARS);
}

/*
 * draw_progress
 * -------------
 * Display the interior of the progress bar and the percent complete.
 */
static void
draw_progress(void)
{
    struct RastPort *rp = window->RPort;
    char buf[8];
    WORD x  = lay.ox + lay.prog_x + 2;
    WORD y  = lay.oy + lay.stat_y + 1;
    WORD w  = lay.prog_w - 4;
    WORD y2 = y + lay.prog_h - 3;

    if (prog_fill > 0) {
        SetAPen(rp, pen_fill);
        RectFill(rp, x, y, x + prog_fill - 1, y2);
    }
    if (prog_fill < (uint) w) {
        SetAPen(rp, pen_back);
        RectFill(rp, x + prog_fill, y, x + w - 1, y2);
    }

    buf[0] = '\0';
    if (prog_percent >= 0)
        sprintf(buf, "%3d%%", prog_percent);
    text_at(lay.prog_x + lay.prog_w + 4, lay.stat_y + 1, buf, 4);
}

/*
 * draw_id
 * -------
 * Display KickSmash identification information (version, build, serial)
 */
static void
draw_id(void)
{
    char buf[64];
    WORD y = lay.ty[0] + 1;

    sprintf(buf, "KickSmash%u %u.%u",
            ((id.si_mode != 1) && (id.si_mode != 2)) ? 32 : 16,
            id.si_ks_version[0], id.si_ks_version[1]);
    text_at(lay.margin, y, buf, ID_CHARS);

    sprintf(buf, "Built %02u%02u-%02u-%02u %02u:%02u:%02u",
            id.si_ks_date[0], id.si_ks_date[1],
            id.si_ks_date[2], id.si_ks_date[3],
            id.si_ks_time[0], id.si_ks_time[1], id.si_ks_time[2]);
    text_at(lay.margin, y + lay.fh + 1, buf, ID_CHARS);

    sprintf(buf, "Serial \"%s\"", id.si_serial);
    text_at(lay.margin, y + 2 * (lay.fh + 1), buf, ID_CHARS);
}

/*
 * draw_bank_cell
 * --------------
 * Display a single text cell of the ROM bank information table. Other
 * cells of the table are gadgets.
 */
static void
draw_bank_cell(uint bank, uint col)
{
    char text[8];
    WORD x = banktable_pos[col];
    WORD y = lay.rows_y + bank * lay.row_h + lay.el_y;

    memset(text, ' ', sizeof (text));
    switch (col) {
        case COL_BANK:
            text[0] = bank + '0';
            text_at(x + (banktable_widths[col] * lay.fw) / 2, y, text, 1);
            break;
        case COL_MERGE: {
            uint banks_add = info.bi_merge[bank] >> 4;
            uint bank_sub  = info.bi_merge[bank] & 0xf;

            if (banks_add != 0) {
                if (bank_sub == 0) {
                    text[1] = '-';
                    text[2] = '\\';
                } else if (bank_sub == banks_add) {
                    text[1] = '-';
                    text[2] = '/';
                } else {
                    text[3] = '|';
                }
            }
            text_at(x + lay.fw / 2, y, text, 6);
            break;
        }
        case COL_LONGRESET: {
            uint pos;

            for (pos = 0; pos < ARRAY_SIZE(info.bi_longreset_seq); pos++)
                if (info.bi_longreset_seq[pos] == bank)
                    break;

            if (pos < ARRAY_SIZE(info.bi_longreset_seq))
                text[0] = '0' + pos;

            /* Text between the - and + buttons */
            text_at(x + lay.pm_x + lay.pm_w + 3, y, text, 1);
            break;
        }
    }
}

static void
draw_bank_column(uint col)
{
    uint bank;

    for (bank = 0; bank < ROM_BANKS; bank++)
        draw_bank_cell(bank, col);
}

/*
 * draw_banks
 * ----------
 * Display the frame, column titles, and text of the ROM bank table.
 */
static void
draw_banks(void)
{
    uint col;

    bevel(lay.table_x, lay.table_y, lay.table_w, lay.table_h, 1);

    for (col = 0; col < COL_COUNT; col++) {
        const char *title  = banktable_titles[col];
        WORD        pwidth = (banktable_widths[col] + 1) * lay.fw;
        WORD        tlen   = strlen(title);

        /* Column title */
        bevel(banktable_pos[col], lay.table_y + 2, pwidth, lay.fh + 3, 0);
        text_at(banktable_pos[col] + (pwidth - tlen * lay.fw) / 2,
                lay.table_y + 4, title, tlen);

        /* Column of bank rows */
        bevel(banktable_pos[col], lay.rows_y - 1, pwidth,
              ROM_BANKS * lay.row_h + 2, 0);

        draw_bank_column(col);
    }
}

/*
 * draw_window
 * -----------
 * Display everything in the window which is not a gadget.
 */
static void
draw_window(void)
{
    draw_id();

    text_at(lay.colb_x, lay.ty[1] + 2, "Auto Switch", 11);
    text_at(AUTO_TIME_X + AUTO_TIME_W + lay.fw, lay.ty[1] + 2, "seconds", 7);

    draw_banks();

    bevel(lay.prog_x, lay.stat_y, lay.prog_w, lay.prog_h, 1);
    draw_progress();
    draw_status();
}

/*
 * ng_set
 * ------
 * Fill in the NewGadget structure for the next gadget to create.
 */
static void
ng_set(WORD x, WORD y, WORD w, WORD h, const char *text, UWORD gid,
       ULONG flags)
{
    ng.ng_LeftEdge   = lay.ox + x;
    ng.ng_TopEdge    = lay.oy + y;
    ng.ng_Width      = w;
    ng.ng_Height     = h;
    ng.ng_GadgetText = (UBYTE *) text;
    ng.ng_TextAttr   = &font_attr;
    ng.ng_GadgetID   = gid;
    ng.ng_Flags      = flags;
    ng.ng_VisualInfo = visualinfo;
    ng.ng_UserData   = NULL;
}

/*
 * name_gadgets_setup
 * ------------------
 * Initialize the bank name string gadgets. These are plain Intuition
 * string gadgets (without a border) so that they fit within the rows of
 * the bank table.
 */
static void
name_gadgets_setup(void)
{
    uint bank;

    memset(name_gadget, 0, sizeof (name_gadget));
    memset(name_sinfo, 0, sizeof (name_sinfo));
    memset(&name_sext, 0, sizeof (name_sext));

    name_sext.Font          = font;
    name_sext.Pens[0]       = pen_text;
    name_sext.Pens[1]       = pen_back;
    name_sext.ActivePens[0] = pen_text;
    name_sext.ActivePens[1] = pen_back;
    name_sext.WorkBuffer    = name_work;

    for (bank = 0; bank < ROM_BANKS; bank++) {
        struct Gadget     *gad = &name_gadget[bank];
        struct StringInfo *si  = &name_sinfo[bank];

        strcpy((char *) name_buf[bank], info.bi_name[bank]);

        si->Buffer     = name_buf[bank];
        si->UndoBuffer = name_undo;
        si->MaxChars   = sizeof (name_buf[bank]);
        si->Extension  = &name_sext;

        gad->NextGadget  = (bank < ROM_BANKS - 1) ? &name_gadget[bank + 1] :
                                                    NULL;
        gad->LeftEdge    = lay.ox + banktable_pos[COL_NAME] + lay.fw;
        gad->TopEdge     = lay.oy + lay.rows_y + bank * lay.row_h + lay.el_y;
        gad->Width       = sizeof (name_buf[bank]) * lay.fw;
        gad->Height      = lay.fh;
        gad->Flags       = GFLG_GADGHCOMP | GFLG_TABCYCLE | GFLG_STRINGEXTEND;
        gad->Activation  = GACT_RELVERIFY;
        gad->GadgetType  = GTYP_STRGADGET;
        gad->SpecialInfo = si;
        gad->GadgetID    = ID_BANK_NAME_0 + bank;
    }
}

/*
 * gadgets_create
 * --------------
 * Create all gadgets and add them to the window. Returns non-zero on
 * failure.
 */
static uint
gadgets_create(void)
{
    struct Gadget *gad;
    STRPTR         sptr;
    uint           bank;
    uint           mx_spacing;
    WORD           fw = lay.fw;
    WORD           fh = lay.fh;
    WORD           gh = lay.gh;
    WORD           x;
    WORD           y;
    WORD           w;

    /*
     * Radio buttons scale to the font height with V39 and higher GadTools.
     * They are at least 9 pixels high with older versions.
     */
    if (GadToolsBase->lib_Version >= 39)
        mx_spacing = lay.row_h - fh;
    else
        mx_spacing = (lay.row_h > 9) ? (lay.row_h - ((fh > 9) ? fh : 9)) : 0;

    gad = CreateContext(&gadgets);

    /* KickSmash board name */
    sptr = (STRPTR) id.si_name;
    ng_set(lay.colb_x + 11 * fw, lay.ty[0], 16 * fw + 16, gh, "Board name",
           ID_BOARD_NAME, PLACETEXT_LEFT);
    gad = CreateGadget(STRING_KIND, gad, &ng,
                       GTST_MaxChars, sizeof (id.si_name) - 1,
                       GTST_String, (ULONG) sptr,
                       GA_TabCycle, TRUE,
                       TAG_DONE);
    gadget_board_name = gad;

    /* ROM Switcher Auto Switch bank and timeout */
    ng_set(AUTO_BANK_X, lay.ty[1], AUTO_BANK_W, gh, "Bank", ID_BANK_DEFAULT,
           PLACETEXT_LEFT);
    gad = CreateGadget(INTEGER_KIND, gad, &ng,
                       GTIN_MaxChars, 1,
                       GTIN_Number, timeout_bank,
                       GA_TabCycle, TRUE,
                       TAG_DONE);
    gadget_timeout_bank = gad;

    ng_set(AUTO_TIME_X, lay.ty[1], AUTO_TIME_W, gh, "Timeout",
           ID_BANK_TIMEOUT, PLACETEXT_LEFT);
    gad = CreateGadget(INTEGER_KIND, gad, &ng,
                       GTIN_MaxChars, 4,
                       GTIN_Number, timeout_seconds,
                       GA_TabCycle, TRUE,
                       TAG_DONE);
    gadget_timeout_seconds = gad;

    /* LongReset - and + buttons */
    x = banktable_pos[COL_LONGRESET] + lay.pm_x;
    for (bank = 0; bank < ROM_BANKS; bank++) {
        y = lay.rows_y + bank * lay.row_h + lay.el_y;
        ng_set(x, y, lay.pm_w, fh, "-",
               ID_LONGRESET_MINUS_0 + bank, PLACETEXT_IN);
        gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);

        ng_set(x + lay.pm_w + fw + 6, y, lay.pm_w, fh, "+",
               ID_LONGRESET_PLUS_0 + bank, PLACETEXT_IN);
        gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);
    }

    /* PowerOn bank select radio */
    x = ((banktable_widths[COL_POWERON] + 1) * fw - lay.mx_w) / 2;
    y = lay.rows_y + lay.el_y;
    ng_set(banktable_pos[COL_POWERON] + x, y, lay.mx_w, fh, NULL,
           ID_POWERON_RADIO, 0);
    gad = CreateGadget(MX_KIND, gad, &ng,
                       GTMX_Labels, (ULONG) mx_labels_all,
                       GTMX_Active, info.bi_bank_poweron & 7,
                       GTMX_Spacing, mx_spacing,
                       GTMX_Scaled, TRUE,
                       TAG_DONE);
    gadget_poweron = gad;

    /* Current ROM bank (display only) */
    if (info_saved.bi_bank_current < ROM_BANKS) {
        ng_set(banktable_pos[COL_CURRENT] + x,
               y + info_saved.bi_bank_current * lay.row_h, lay.mx_w, fh, NULL,
               ID_CURRENT_RADIO, 0);
        gad = CreateGadget(MX_KIND, gad, &ng,
                           GTMX_Labels, (ULONG) mx_labels_one,
                           GTMX_Active, 0,
                           GTMX_Spacing, mx_spacing,
                           GTMX_Scaled, TRUE,
                           TAG_DONE);
    }

    /* Bank selection radio */
    ng_set(banktable_pos[COL_SELECT] + x, y, lay.mx_w, fh, NULL,
           ID_SELECT_RADIO, 0);
    gad = CreateGadget(MX_KIND, gad, &ng,
                       GTMX_Labels, (ULONG) mx_labels_all,
                       GTMX_Active, bank_select,
                       GTMX_Spacing, mx_spacing,
                       GTMX_Scaled, TRUE,
                       TAG_DONE);
    gadget_select = gad;

    /* ROM file name, with the Select button at the right edge */
    w = BUTTON_W(9);
    x = lay.margin + 9 * fw;
    sptr = (STRPTR) rom_filename;
    ng_set(x, lay.file_y, lay.width - lay.margin - w - 4 - x, gh, "ROM file",
           ID_FILE_NAME, PLACETEXT_LEFT);
    gad = CreateGadget(STRING_KIND, gad, &ng,
                       GTST_MaxChars, sizeof (rom_filename) - 1,
                       GTST_String, (ULONG) sptr,
                       GA_TabCycle, TRUE,
                       TAG_DONE);
    gadget_file = gad;

    ng_set(lay.width - lay.margin - w, lay.file_y, w, gh, "Select...",
           ID_FILE_SELECT, PLACETEXT_IN);
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);
    gadget_file_select = gad;

    /* ROM file operations */
    x = lay.margin;
    w = BUTTON_W(7);
    ng_set(x, lay.ops_y, w, gh, "Program", ID_PROGRAM, PLACETEXT_IN);
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);
    gadget_program = gad;

    x += w + 4;
    w = BUTTON_W(4);
    ng_set(x, lay.ops_y, w, gh, "Read", ID_READ, PLACETEXT_IN);
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);
    gadget_read = gad;

    x += w + 4;
    w = BUTTON_W(6);
    ng_set(x, lay.ops_y, w, gh, "Verify", ID_VERIFY, PLACETEXT_IN);
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);
    gadget_verify = gad;

    x += w + 4;
    w = BUTTON_W(4);
    ng_set(x, lay.ops_y, w, gh, "Stop", ID_STOP, PLACETEXT_IN);
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
                       GA_Disabled, TRUE,
                       TAG_DONE);
    gadget_stop = gad;

    x += w + 3 * fw;
    ng_set(x, lay.ops_y, (26 * fw) / 8, fh + 3, "Verify after programming",
           ID_VERIFY_AFTER, PLACETEXT_RIGHT);
    gad = CreateGadget(CHECKBOX_KIND, gad, &ng,
                       GTCB_Checked, verify_after,
                       GTCB_Scaled, TRUE,
                       TAG_DONE);
    gadget_verify_after = gad;

    /* Settings buttons */
    x = lay.margin;
    w = BUTTON_W(4);
    ng_set(x, lay.btn_y, w, gh, "Save", ID_SAVE, PLACETEXT_IN);
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
                       GA_Disabled, TRUE,
                       TAG_DONE);
    gadget_save = gad;

    x += w + 4;
    w = BUTTON_W(6);
    ng_set(x, lay.btn_y, w, gh, "Reload", ID_RELOAD, PLACETEXT_IN);
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);
    gadget_reload = gad;

    w = BUTTON_W(17);
    ng_set(lay.width - lay.margin - w, lay.btn_y, w, gh,
           "Switch and Reboot", ID_SWITCH, PLACETEXT_IN);
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
                       GA_Disabled, TRUE,
                       TAG_DONE);
    gadget_switch = gad;

    if (gad == NULL) {
        FreeGadgets(gadgets);
        gadgets = NULL;
        return (1);
    }

    AddGList(window, gadgets, (UWORD) -1, -1, NULL);
    RefreshGList(gadgets, window, NULL, -1);
    GT_RefreshWindow(window, NULL);

    name_gadgets_setup();
    AddGList(window, &name_gadget[0], (UWORD) -1, ROM_BANKS, NULL);
    RefreshGList(&name_gadget[0], window, NULL, ROM_BANKS);
    name_gadgets_added = 1;

    update_buttons(1);
    return (0);
}

/*
 * gadgets_free
 * ------------
 * Remove all gadgets from the window and free them. Content of the ROM
 * file name and state of the verify checkbox are retained so that they
 * are preserved if the gadgets are created again.
 */
static void
gadgets_free(void)
{
    if (name_gadgets_added) {
        RemoveGList(window, &name_gadget[0], ROM_BANKS);
        name_gadgets_added = 0;
    }
    if (gadgets != NULL) {
        strncpy(rom_filename, SI_BUF(gadget_file), sizeof (rom_filename) - 1);
        rom_filename[sizeof (rom_filename) - 1] = '\0';
        verify_after = (gadget_verify_after->Flags & GFLG_SELECTED) ? 1 : 0;

        RemoveGList(window, gadgets, -1);
        FreeGadgets(gadgets);
        gadgets = NULL;
    }
}

/*
 * state_reload
 * ------------
 * Discard any edits, acquire current information from KickSmash, and
 * then display it.
 */
static void
state_reload(void)
{
    struct RastPort *rp = window->RPort;
    uint             rc;

    pull_edits();
    if (state_dirty() &&
        (ask("Reload|Cancel", "Discard changes which have not been saved?")
         == 0)) {
        return;
    }

    gadgets_free();
    rc = state_load();
    if (bank_select >= ROM_BANKS)
        set_initial_bank_select();

    SetAPen(rp, pen_back);
    RectFill(rp, window->BorderLeft, window->BorderTop,
             window->Width - window->BorderRight - 1,
             window->Height - window->BorderBottom - 1);
    if (rc == 0)
        set_status("Settings loaded from KickSmash");
    else
        set_status("KickSmash is not answering");
    draw_window();
    if (gadgets_create())
        show_error("Failed to create gadgets");
    if (rc != 0)
        show_error("KickSmash is not answering\n%s", smash_err(rc));
}

/*
 * show_bank_selected
 * ------------------
 * Report the bank which has been selected in the status line.
 */
static void
show_bank_selected(void)
{
    uint sub = info.bi_merge[bank_select] & 0x0f;

    if (sub != 0) {
        set_status("Bank %u is merged with bank %u",
                   bank_select, bank_select - sub);
    } else {
        set_status("Bank %u selected (%u KB)", bank_select,
                   bank_size_bytes(&info, bank_select) >> 10);
    }
}

/*
 * show_about
 * ----------
 * Display information about this program.
 */
static void
show_about(void)
{
    (void) ask("OK",
               "SmashTool %s\n"
               "Built %s %s\n"
               "\n"
               "Workbench utility for KickSmash ROM bank settings,\n"
               "ROM image programming, and Clock setting.\n"
               "\n"
               "KickSmash%u firmware %u.%u\n"
               "Built %02u%02u-%02u-%02u %02u:%02u:%02u\n"
               "\n"
               "\xA9 Chris Hooper\n"
               "https://github.com/cdhooper/kicksmash32",
               VERSION, BUILD_DATE, BUILD_TIME,
               ((id.si_mode != 1) && (id.si_mode != 2)) ? 32 : 16,
               id.si_ks_version[0], id.si_ks_version[1],
               id.si_ks_date[0], id.si_ks_date[1],
               id.si_ks_date[2], id.si_ks_date[3],
               id.si_ks_time[0], id.si_ks_time[1], id.si_ks_time[2]);
}

/*
 * quit_confirm
 * ------------
 * Returns non-zero if the program may exit. If there are changes which
 * have not been saved, the user is asked whether to discard them.
 */
static uint
quit_confirm(void)
{
    pull_edits();
    if (state_dirty() == 0)
        return (1);
    return (ask("Quit|Cancel", "Changes have not been saved to KickSmash.\n"
                "Quit anyway?") != 0);
}

/*
 * handle_gadget
 * -------------
 * Handle a gadget event from the window.
 */
static void
handle_gadget(ULONG class, UWORD gid, UWORD code)
{
    if (gadgets == NULL)
        return;

    if (class == IDCMP_GADGETDOWN) {
        switch (gid) {
            case ID_POWERON_RADIO:
                info.bi_bank_poweron = code;
                break;
            case ID_SELECT_RADIO:
                bank_select = code;
                progress_clear();
                show_bank_selected();
                break;
        }
        update_buttons(0);
        return;
    }

    /* IDCMP_GADGETUP */
    if ((gid < ID_BANK_NAME_0) && (gid != ID_BOARD_NAME) &&
        (gid != ID_BANK_DEFAULT) && (gid != ID_BANK_TIMEOUT) &&
        (gid != ID_FILE_NAME) && (gid != ID_VERIFY_AFTER)) {
        progress_clear();  // Buttons remove progress of the last operation
    }

    if ((gid >= ID_LONGRESET_MINUS_0) &&
        (gid < ID_LONGRESET_MINUS_0 + ROM_BANKS)) {
        bank_longreset_change(gid - ID_LONGRESET_MINUS_0, -1);
        draw_bank_column(COL_LONGRESET);
    } else if ((gid >= ID_LONGRESET_PLUS_0) &&
               (gid < ID_LONGRESET_PLUS_0 + ROM_BANKS)) {
        bank_longreset_change(gid - ID_LONGRESET_PLUS_0, 1);
        draw_bank_column(COL_LONGRESET);
    } else {
        switch (gid) {
            case ID_FILE_SELECT:
                select_file();
                break;
            case ID_PROGRAM:
                op_program();
                break;
            case ID_READ:
                op_read();
                break;
            case ID_VERIFY:
                op_verify();
                break;
            case ID_SAVE:
                state_save();
                break;
            case ID_RELOAD:
                state_reload();
                return;
            case ID_SWITCH:
                bank_switch_and_reboot();
                break;
        }
    }
    pull_edits();
    update_buttons(0);
}

/*
 * The menu strip. Positions of the Save and Switch and Reboot items must
 * match MENUNUM_SAVE and MENUNUM_SWITCH.
 */
#define MENU_TITLE(name) \
        { NM_TITLE, (STRPTR) (name), NULL, 0, 0, NULL }
#define MENU_ITEM(name, key, mid) \
        { NM_ITEM, (STRPTR) (name), (STRPTR) (key), 0, 0, (APTR) (mid) }
#define MENU_CHECK(name, flags, num, mid) \
        { NM_ITEM, (STRPTR) (name), NULL, CHECKIT | (flags), ~BIT(num), \
          (APTR) (mid) }
#define MENU_BAR \
        { NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL }

static struct NewMenu newmenus[] = {
    MENU_TITLE("KickSmash"),
    MENU_ITEM("Program",           "P",  MENU_PROGRAM),
    MENU_ITEM("Read",              "R",  MENU_READ),
    MENU_ITEM("Verify",            "V",  MENU_VERIFY),
    MENU_BAR,
    MENU_ITEM("Save",              "S",  MENU_SAVE),
    MENU_ITEM("Reload",            "L",  MENU_RELOAD),
    MENU_ITEM("Switch and Reboot", NULL, MENU_SWITCH),
    MENU_BAR,
    MENU_ITEM("About...",          "?",  MENU_ABOUT),
    MENU_ITEM("Quit",              "Q",  MENU_QUIT),

    MENU_TITLE("Clock"),
    MENU_ITEM("Set Amiga clock from Host",      NULL, MENU_CLOCK_FROM_HOST),
    MENU_ITEM("Set Amiga clock from KickSmash", NULL, MENU_CLOCK_FROM_KS),
    MENU_ITEM("Save Amiga clock to KickSmash",  NULL, MENU_CLOCK_TO_KS),

    MENU_TITLE("Swap"),
    MENU_CHECK("Auto", CHECKED, 0, MENU_SWAP | SWAP_AUTO),
    MENU_CHECK("0123", 0,       1, MENU_SWAP | SWAP_0123),
    MENU_CHECK("1032", 0,       2, MENU_SWAP | SWAP_1032),
    MENU_CHECK("2301", 0,       3, MENU_SWAP | SWAP_2301),
    MENU_CHECK("3210", 0,       4, MENU_SWAP | SWAP_3210),

    MENU_TITLE("Merge"),
    MENU_ITEM("Merge 0-1", NULL, MENU_MERGE | MERGE_RANGE(0, 1)),
    MENU_ITEM("Merge 2-3", NULL, MENU_MERGE | MERGE_RANGE(2, 3)),
    MENU_ITEM("Merge 4-5", NULL, MENU_MERGE | MERGE_RANGE(4, 5)),
    MENU_ITEM("Merge 6-7", NULL, MENU_MERGE | MERGE_RANGE(6, 7)),
    MENU_ITEM("Merge 0-3", NULL, MENU_MERGE | MERGE_RANGE(0, 3)),
    MENU_ITEM("Merge 4-7", NULL, MENU_MERGE | MERGE_RANGE(4, 7)),
    MENU_ITEM("Merge 0-7", NULL, MENU_MERGE | MERGE_RANGE(0, 7)),

    MENU_TITLE("Unmerge"),
    MENU_ITEM("Unmerge 0-1", NULL, MENU_UNMERGE | MERGE_RANGE(0, 1)),
    MENU_ITEM("Unmerge 2-3", NULL, MENU_UNMERGE | MERGE_RANGE(2, 3)),
    MENU_ITEM("Unmerge 4-5", NULL, MENU_UNMERGE | MERGE_RANGE(4, 5)),
    MENU_ITEM("Unmerge 6-7", NULL, MENU_UNMERGE | MERGE_RANGE(6, 7)),
    MENU_ITEM("Unmerge 0-3", NULL, MENU_UNMERGE | MERGE_RANGE(0, 3)),
    MENU_ITEM("Unmerge 4-7", NULL, MENU_UNMERGE | MERGE_RANGE(4, 7)),
    MENU_ITEM("Unmerge 0-7", NULL, MENU_UNMERGE | MERGE_RANGE(0, 7)),
    MENU_ITEM("Unmerge All", NULL, MENU_UNMERGE_ALL),

    { NM_END, NULL, NULL, 0, 0, NULL },
};

/*
 * menus_create
 * ------------
 * Create the menu strip and attach it to the window. Returns non-zero on
 * failure.
 */
static uint
menus_create(void)
{
    menus = CreateMenusA(newmenus, NULL);
    if (menus == NULL)
        return (1);

    if (LayoutMenus(menus, visualinfo,
                    GTMN_NewLookMenus, TRUE,
                    TAG_DONE) == FALSE) {
        FreeMenus(menus);
        menus = NULL;
        return (1);
    }
    SetMenuStrip(window, menus);
    return (0);
}

/*
 * handle_menu
 * -----------
 * Handle a menu selection, which might be several menu items. Returns
 * non-zero if the program should exit.
 */
static uint
handle_menu(UWORD code)
{
    uint quit = 0;

    while ((code != MENUNULL) && !quit) {
        struct MenuItem *item = ItemAddress(menus, code);
        uint             mid;

        if (item == NULL)
            break;
        mid  = (uint) GTMENUITEM_USERDATA(item);
        code = item->NextSelect;

        if (mid == MENU_QUIT) {
            quit = quit_confirm();
            continue;
        }
        if (mid == MENU_ABOUT) {
            show_about();
            continue;
        }
        if ((mid & MENU_TYPE_MASK) == MENU_SWAP) {
            swap_mode = mid & 0xff;
            set_status("Byte swap mode %s", swap_names[swap_mode]);
            continue;
        }
        if (gadgets == NULL)
            continue;  // Nothing else may be done without the gadgets

        progress_clear();  // Remove progress of the last operation
        switch (mid & MENU_TYPE_MASK) {
            case MENU_MERGE:
                bank_merge((mid >> 4) & 0xf, mid & 0xf, 0);
                continue;
            case MENU_UNMERGE:
                bank_merge((mid >> 4) & 0xf, mid & 0xf, 1);
                continue;
        }
        switch (mid) {
            case MENU_PROGRAM:
                op_program();
                break;
            case MENU_READ:
                op_read();
                break;
            case MENU_VERIFY:
                op_verify();
                break;
            case MENU_SAVE:
                state_save();
                break;
            case MENU_RELOAD:
                state_reload();
                break;
            case MENU_SWITCH:
                bank_switch_and_reboot();
                break;
            case MENU_CLOCK_FROM_HOST:
                clock_load(1);
                break;
            case MENU_CLOCK_FROM_KS:
                clock_load(0);
                break;
            case MENU_CLOCK_TO_KS:
                clock_save();
                break;
            case MENU_UNMERGE_ALL:
                bank_unmerge_all();
                break;
        }
    }
    pull_edits();
    update_buttons(0);
    return (quit);
}

/*
 * event_loop
 * ----------
 * Handle event messages from Intuition until the window is closed.
 */
static void
event_loop(void)
{
    struct IntuiMessage *msg;
    uint                 done = 0;

    while (!done) {
        WaitPort(window->UserPort);
        while ((msg = GT_GetIMsg(window->UserPort)) != NULL) {
            ULONG class = msg->Class;
            UWORD code  = msg->Code;
            UWORD gid   = 0;

            if ((class == IDCMP_GADGETUP) || (class == IDCMP_GADGETDOWN))
                gid = ((struct Gadget *) msg->IAddress)->GadgetID;
            GT_ReplyIMsg(msg);

            switch (class) {
                case IDCMP_CLOSEWINDOW:
                    done = quit_confirm();
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(window);
                    draw_window();
                    GT_EndRefresh(window, TRUE);
                    break;
                case IDCMP_INTUITICKS:
                    /* Notice edits to string gadgets as they are made */
                    pull_edits();
                    update_buttons(0);
                    break;
                case IDCMP_GADGETDOWN:
                case IDCMP_GADGETUP:
                    handle_gadget(class, gid, code);
                    break;
                case IDCMP_MENUPICK:
                    done = handle_menu(code);
                    break;
            }
            if (done)
                break;
        }
    }
}

/*
 * font_try
 * --------
 * Open the specified font and compute the window layout for it. Returns
 * 0 if the font is usable and the window will fit within the specified
 * size. Otherwise the font is closed and non-zero is returned.
 */
static uint
font_try(const struct TextAttr *attr, WORD max_w, WORD max_h)
{
    uint compact;

    font_attr = *attr;
    font = OpenFont(&font_attr);
    if (font == NULL)
        return (1);

    /* The layout requires a fixed width font of the size requested */
    if (((font->tf_Flags & FPF_PROPORTIONAL) == 0) &&
        (font->tf_YSize == attr->ta_YSize) && (font->tf_XSize > 0)) {
        for (compact = 0; compact < 2; compact++) {
            layout_compute(compact);
            if ((lay.width <= max_w) && (lay.inner_h <= max_h))
                return (0);
        }
    }
    CloseFont(font);
    font = NULL;
    return (1);
}

/*
 * window_open
 * -----------
 * Open the window on the default public screen (Workbench). Returns
 * non-zero on failure.
 */
static uint
window_open(void)
{
    struct TextFont *sysfont = GfxBase->DefaultFont;
    struct TextAttr  sysattr;
    WORD             border_w;
    WORD             border_h;
    WORD             max_w;
    WORD             max_h;

    screen = LockPubScreen(NULL);
    if (screen == NULL)
        return (1);

    visualinfo = GetVisualInfoA(screen, NULL);
    drawinfo   = GetScreenDrawInfo(screen);
    if (visualinfo == NULL)
        return (1);

    if (drawinfo != NULL) {
        pen_text = drawinfo->dri_Pens[TEXTPEN];
        pen_back = drawinfo->dri_Pens[BACKGROUNDPEN];
        pen_fill = drawinfo->dri_Pens[FILLPEN];
    }

    border_w = screen->WBorLeft + screen->WBorRight;
    border_h = screen->WBorTop + screen->Font->ta_YSize + 1 +
               screen->WBorBottom;
    max_w    = screen->Width - border_w;
    max_h    = screen->Height - border_h;

    /*
     * Use the system default font if the window will then fit on the
     * screen. Otherwise use topaz 8, even if the window doesn't fit.
     */
    if (sysfont != NULL) {
        sysattr.ta_Name  = (STRPTR) sysfont->tf_Message.mn_Node.ln_Name;
        sysattr.ta_YSize = sysfont->tf_YSize;
        sysattr.ta_Style = sysfont->tf_Style & ~FSF_TAGGED;
        sysattr.ta_Flags = sysfont->tf_Flags;
    }
    if ((sysfont == NULL) || (sysattr.ta_Name == NULL) ||
        font_try(&sysattr, max_w, max_h)) {
        if (font_try(&topaz8_attr, max_w, max_h)) {
            font_attr = topaz8_attr;
            font = OpenFont(&font_attr);
            if (font == NULL)
                return (1);
            layout_compute(1);
        }
    }

    sprintf(window_title, "SmashTool %s", VERSION);
    window = OpenWindowTags(NULL,
                            WA_Left, (max_w > lay.width) ?
                                     (max_w - lay.width) / 2 : 0,
                            WA_Top, (max_h > lay.inner_h) ?
                                    (max_h - lay.inner_h) / 2 : 0,
                            WA_InnerWidth, lay.width,
                            WA_InnerHeight, lay.inner_h,
                            WA_Title, (ULONG) window_title,
                            WA_PubScreen, (ULONG) screen,
                            WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
                                      IDCMP_INTUITICKS | IDCMP_MENUPICK |
                                      BUTTONIDCMP | STRINGIDCMP |
                                      INTEGERIDCMP | MXIDCMP | CHECKBOXIDCMP,
                            WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                                      WFLG_CLOSEGADGET | WFLG_ACTIVATE |
                                      WFLG_SMART_REFRESH,
                            WA_NewLookMenus, TRUE,
                            TAG_DONE);
    if (window == NULL)
        return (1);

    lay.ox = window->BorderLeft;
    lay.oy = window->BorderTop;
    return (menus_create());
}

/*
 * window_close
 * ------------
 * Close the window and release all resources acquired by window_open().
 */
static void
window_close(void)
{
    if (window != NULL) {
        gadgets_free();
        if (menus != NULL) {
            ClearMenuStrip(window);
            FreeMenus(menus);
            menus = NULL;
        }
        CloseWindow(window);
        window = NULL;
    }
    if (font != NULL)
        CloseFont(font);
    if (screen != NULL) {
        if (drawinfo != NULL)
            FreeScreenDrawInfo(screen, drawinfo);
        if (visualinfo != NULL)
            FreeVisualInfo(visualinfo);
        UnlockPubScreen(NULL, screen);
    }
}

int
main(int argc, char *argv[])
{
    uint rc;
    int  ret = 20;

    (void) argc;  // argc is 0 when started from Workbench
    (void) argv;

    cpu_control_init();  // cpu_type, SysBase
    rom_get_normal();    // Get Kickstart ROM signature

    IntuitionBase = (struct IntuitionBase *)
                    OpenLibrary((STRPTR) "intuition.library", 37);
    GfxBase       = (struct GfxBase *)
                    OpenLibrary((STRPTR) "graphics.library", 37);
    GadToolsBase  = OpenLibrary((STRPTR) "gadtools.library", 37);
    AslBase       = OpenLibrary((STRPTR) "asl.library", 37);  // Optional
    if ((IntuitionBase == NULL) || (GfxBase == NULL) || (GadToolsBase == NULL))
        goto cleanup;

    /* Get all information from KickSmash before opening the window */
    while ((rc = state_load()) != 0) {
        if (ask("Retry|Quit", "KickSmash is not answering\n%s",
                smash_err(rc)) == 0) {
            goto cleanup;
        }
    }
    set_initial_bank_select();

    if (window_open()) {
        show_error("Failed to open window");
        goto cleanup_window;
    }
    set_status("Bank %u selected for ROM file and Switch", bank_select);
    draw_window();
    if (gadgets_create()) {
        show_error("Failed to create gadgets");
        goto cleanup_window;
    }

    event_loop();
    ret = 0;

cleanup_window:
    window_close();
cleanup:
    if (AslBase != NULL)
        CloseLibrary(AslBase);
    if (GadToolsBase != NULL)
        CloseLibrary(GadToolsBase);
    if (GfxBase != NULL)
        CloseLibrary((struct Library *) GfxBase);
    if (IntuitionBase != NULL)
        CloseLibrary((struct Library *) IntuitionBase);
    return (ret);
}
