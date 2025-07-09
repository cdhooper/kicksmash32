/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * KickSmash ROM switcher
 *
 * This program is a module which must be merged with a Kickstart ROM
 * in order to be useful. When started, it takes over the Amiga and
 * allows selection of a ROM image to execute.
 */

#include <string.h>
#ifdef STANDALONE
#include <stdint.h>
#include "util.h"
#include "draw.h"
#include "screen.h"
#include "intuition.h"
#include "gadget.h"
#include "keyboard.h"
#else
#include <inttypes.h>
#include <exec/types.h>
#include <proto/exec.h>

#include <graphics/videocontrol.h>
#include <clib/intuition_protos.h>
#include <clib/graphics_protos.h>
#include <clib/gadtools_protos.h>
#include <libraries/keymap.h>
#include <utility/hooks.h>
#include <intuition/intuition.h>
#include <intuition/sghooks.h>
#include <graphics/gfxmacros.h>

#include <inline/exec.h>
#include <inline/intuition.h>
#include <inline/gadtools.h>
#include <inline/graphics.h>

#include <utility/utility.h>  // UTILITYNAME
#endif

#include "printf.h"
#include "cpu_control.h"
#include "sm_msg.h"
#include "smash_cmd.h"

#define ROM_VERSION 1

#undef DEBUG_LONGRESET_BUTTONS
#ifdef DEBUG_LONGRESET_BUTTONS
#define LPRINTF(...) printf(__VA_ARGS__)
#else
#define LPRINTF(...)
#endif


#ifndef ADDR32
#define ADDR32(x)     ((volatile uint32_t *) ((uintptr_t)(x)))
#endif
#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))
#define BIT(x)        (1U << (x))
#define MIN(X,Y)      ((X) < (Y) ? (X) : (Y))

#ifdef STANDALONE
#define RawPutChar(ch) putchar(ch)
#else
#define RawPutChar(___c) \
        LP1NR(0x204, RawPutChar, UBYTE, ___c, d0, , SysBase)
#endif

#define SCREEN_WIDTH         640
#define SCREEN_HEIGHT        200
#define BANK_TABLE_YPOS      92
#define BUTTONS_YPOS         186

#define ID_BOARD_NAME        1
#define ID_POWERON_RADIO     2
#define ID_CURRENT_RADIO     3
#define ID_SWITCHTO_RADIO    4
#define ID_CANCEL            5
#define ID_SAVE              6
#define ID_SWITCH            7
#define ID_BANK_TIMEOUT      8
#define ID_BANK_DEFAULT      9
#define ID_LONGRESET_MINUS_0 10
#define ID_LONGRESET_MINUS_1 11
#define ID_LONGRESET_MINUS_2 12
#define ID_LONGRESET_MINUS_3 13
#define ID_LONGRESET_MINUS_4 14
#define ID_LONGRESET_MINUS_5 15
#define ID_LONGRESET_MINUS_6 16
#define ID_LONGRESET_MINUS_7 17
#define ID_LONGRESET_PLUS_0  18
#define ID_LONGRESET_PLUS_1  19
#define ID_LONGRESET_PLUS_2  20
#define ID_LONGRESET_PLUS_3  21
#define ID_LONGRESET_PLUS_4  22
#define ID_LONGRESET_PLUS_5  23
#define ID_LONGRESET_PLUS_6  24
#define ID_LONGRESET_PLUS_7  25
#define ID_BANK_NAME_0       26
#define ID_BANK_NAME_1       27
#define ID_BANK_NAME_2       28
#define ID_BANK_NAME_3       29
#define ID_BANK_NAME_4       30
#define ID_BANK_NAME_5       31
#define ID_BANK_NAME_6       32
#define ID_BANK_NAME_7       33

typedef unsigned int uint;

#ifndef STANDALONE
extern struct DosLibrary *DOSBase;
extern struct ExecBase *SysBase;

void rom_end(void);  // XXX: Not the real end; _end doesn't seem to work
void rom_main(struct ExecBase *SysBase asm("a6"));
extern const char RomName[];
extern const char RomID[];

const struct Resident resident = {
    RTC_MATCHWORD,         // rt_MatchWord - word to match on (ILLEGAL)
    (void *) &resident,    // rt_MatchTag - pointer to the above
    rom_end,               // rt_EndSkip - address to continue scan
    RTF_COLDSTART,         // rt_Flags - various tag flags
    ROM_VERSION,           // rt_Version - release version number
    NT_UNKNOWN,            // rt_Type - type of module
    5,                     // rt_Pri - initialization priority (before bootmenu)
    (char *) "romswitch",  // rt_Name - pointer to node name
    (char *) RomID,        // rt_IdString - pointer to identification string
    rom_main               // rt_Init - pointer to init code
};

const char RomID[]   = "romswitch 1.5 (2025-02-23)\r\n";
#endif /* STANDALONE */

struct GfxBase *GfxBase;
struct Library *GadToolsBase;
struct Library *IntuitionBase;
static APTR visualInfo;
static struct Screen *screen;
static struct Window *window;
static struct NewGadget *NewGadget;
static struct Gadget *gadgets;
static struct Gadget *LastAdded;
static struct Gadget *gadget_banktable_name[ROM_BANKS];
static struct Gadget *gadget_board_name;
static struct Gadget *gadget_save;
static struct Gadget *gadget_save_pre;
static struct Gadget *gadget_switch;
static struct Gadget *gadget_switch_pre;
static struct Gadget *gadget_switchto;
static struct Gadget *gadget_switchto_pre;
static struct Gadget *gadget_timeout_seconds;
static struct Gadget *gadget_timeout_bank;
static uint gadget_cancel_x;
static uint gadget_cancel_y;
static short gadget_cancel_w;
static short gadget_cancel_h;
static uint gadget_save_x;
static uint gadget_save_y;
static short gadget_save_w;
static short gadget_save_h;
static uint gadget_switch_x;
static uint gadget_switch_y;
static short gadget_switch_w;
static short gadget_switch_h;
static uint updated_names;
static uint updated_longreset;
static uint updated_poweron;
static uint updated_bank_timeout;
static uint disabled_save;
static uint disabled_switch;
static uint bank_switchto;
#ifndef STANDALONE
static void *globals_base;
#endif

static uint bank_box_top    = 0;
static uint bank_box_bottom = 0;
static uint bank_box_left   = 0;
static uint bank_box_right  = 0;
static uint current_bank    = 0xff;

#undef BANK_MOUSEBAR
#ifdef BANK_MOUSEBAR
static uint current_column  = 0;
#endif

static bank_info_t info;
static bank_info_t info_saved;
static smash_id_t id;
static smash_id_t id_saved;

uint8_t flag_output;  // Global
uint    flag_debug;  // Global

/*
 * dputs
 * -----
 * Emit debug text on the serial port.
 */
void
dputs(const char *str)
{
#ifndef STANDALONE
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    struct ExecBase *SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop
#endif

    while (*str != '\0')
        RawPutChar(*str++);
}

/*
 * dputx
 * -----
 * Emit a debug hex number on the serial port.
 */
void
dputx(uint x)
{
    uint digit;
    char buf[32];
    char *ptr = buf + sizeof (buf) - 1;
    *ptr = '\0';
    for (digit = 0; digit < 8; digit++) {
        *(--ptr) = "0123456789abcdef"[x & 0xf];
        x >>= 4;
    }
    dputs(ptr);
}

static const struct TextAttr font_attr =
{
    "topaz.font",
     8,
     FS_NORMAL,
     FPF_ROMFONT
};

/*
 * init_screeen
 * ------------
 * Set up Amiga screen and window.
 */
static void
init_screen(void)
{
    UWORD pens = 0xffff;
    ULONG monitor_id;
    struct TagItem taglist[2];

//  SetDefaultMonitor(ntsc ? (NTSC_MONITOR_ID >> 16) : (PAL_MONITOR_ID >> 16));

    if (GfxBase->DisplayFlags & NTSC)
        monitor_id = NTSC_MONITOR_ID | HIRES_KEY;
    else
        monitor_id = PAL_MONITOR_ID | HIRES_KEY;

    taglist[0].ti_Tag  = VTAG_BORDERSPRITE_SET;
    taglist[0].ti_Data = TRUE;
    taglist[1].ti_Tag  = TAG_DONE;

    screen = OpenScreenTags(NULL,
                            SA_Depth,        4,
                            SA_Font,         (ULONG) &font_attr,
                            SA_Type,         CUSTOMSCREEN,
                            SA_DisplayID,    monitor_id,
                            SA_Interleaved,  TRUE,
                            SA_Draggable,    FALSE,
                            SA_Quiet,        TRUE,
                            SA_Pens,         (ULONG) &pens,
                            SA_VideoControl, (ULONG) taglist,
                            TAG_DONE);

    window = OpenWindowTags(NULL,
                            WA_IDCMP,        (IDCMP_RAWKEY | BUTTONIDCMP |
                                              LISTVIEWIDCMP | STRINGIDCMP |
                                              MXIDCMP),
                            WA_CustomScreen, (ULONG) screen,
                            WA_Flags,        (WFLG_NOCAREREFRESH |
                                              WFLG_BORDERLESS |
                                              WFLG_ACTIVATE |
                                              WFLG_RMBTRAP),
//                          WA_ReportMouse,  TRUE,  // Generate IDCMP_MOUSEMOVE
                            TAG_DONE);

    visualInfo = GetVisualInfoA(screen, NULL);
}

/*
 * Drawing types
 *   1: trapezoid empty
 *   2: trapezoid filled
 *   3: dots (width and height specify the number of x and y)
 *   4: vertical pins
 *   5: horizontal pins
 *
 * pen:
 *   0: Background Gray
 *   1: Black
 *   2: White
 *   3: Lt. Blue
 *   4: Gold
 *   5: Gold Dim
 *   6: Dark Gray
 *   7: unassigned
 */
struct drawing {
    uint8_t type;
    uint8_t pen;
    uint8_t x;
    uint8_t y;
    uint8_t w;
    uint8_t h;
};

static const struct drawing kicksmash_drawing[] = {
    { 4, 4,  20, 91, 140, 24 }, // Outer Pins
    { 4, 4,  18, 48,  10, 24 }, // Inner Pins
    { 1, 4,   0, 16,  10,  1 }, // KBRST Pin
    { 2, 1,  12, 12,   4,  4 }, // KBRST plastic
    { 2, 1,  24,  0, 140, 80 }, // PCB
    { 1, 1,  20,  4,   1, 80 }, // PCB face
    { 2, 1,  22, 84, 137,  5 }, // Pin guide
    { 2, 2,  24, 51,  19, 16 }, // USB-C outer
    { 1, 2,  20, 52,  22, 16 }, // USB-C bottom
    { 1, 3,  47,  7,  42, 36 }, // STM32 pins
    { 2, 6,  50,  8,  38, 32 }, // STM32
    { 1, 3,  99,  8,   1, 24 }, // Flash Hi pins
    { 1, 3, 146,  8,   2, 24 }, // Flash Hi pins far
    { 2, 6, 100,  8,  46, 24 }, // Flash Hi
    { 1, 3,  99, 44,   1, 24 }, // Flash Lo pins
    { 1, 3, 146, 44,   2, 24 }, // Flash Lo pins far
    { 2, 6, 100, 44,  46, 24 }, // Flash Lo
    { 1, 2,  56, 54,   6,  8 }, // Power LED
    { 1, 2,  80, 52,   8,  4 }, // Write LED
    { 1, 2,  80, 62,   8,  4 }, // Read LED
    { 3, 2,  30, 19,   1,  3 }, // Console holes
    { 3, 2,  37, 11,   1,  6 }, // ST-Link holes
    { 3, 2,  42,  5,   1,  1 }, // DFU hole
    { 3, 2,  28,  4,   1,  1 }, // KBRST hole
};

#define AREA_SIZE 40
static WORD areabuffer[AREA_SIZE];
static struct AreaInfo areaInfo = {0};
static struct TmpRas tmpras;

static void
init_tmpras(void)
{
    struct RastPort *wrp = window->RPort;
    InitArea(&areaInfo, areabuffer, AREA_SIZE * 2 / 5);
    wrp->AreaInfo = &areaInfo;

    tmpras.RasPtr = (BYTE *) AllocRaster(SCREEN_WIDTH, SCREEN_HEIGHT);
    tmpras.Size = (long) RASSIZE(SCREEN_WIDTH, SCREEN_HEIGHT);
    wrp->TmpRas = &tmpras;
}

/*
 * draw_array
 * ----------
 * Follow a list of box and line draws to create an image.
 * This is done in a pseudo-3D perspective.
 */
static void
draw_array(const struct drawing c[], int length)
{
    struct RastPort *rp = &screen->RastPort;
    struct RastPort *wrp = window->RPort;
    int x = 215, y = 60;  // drawing base
    int i;
    int j;

    init_tmpras();
    for (i = 0; i < length; i++) {
        struct drawing d = c[i];
        uint8_t type = d.type;
        uint8_t pen  = d.pen;

        uint16_t da[10];
        da[0] = x + d.x + d.y;
        da[1] = y + d.y / 4 - d.x / 4;
        da[2] = x + d.x + d.y + d.w;
        da[3] = y + d.y / 4 - (d.x + d.w) / 4;
        da[4] = x + d.x + d.w + d.y + d.h;
        da[5] = y + d.y / 4 - (d.x + d.w) / 4 + d.h / 4;
        da[6] = x + d.x + d.y + d.h;
        da[7] = y + d.y / 4 - d.x / 4 + d.h / 4;
        da[8] = da[0];
        da[9] = da[1];
        switch (type) {
            case 1:  // Outline
                SetAPen(rp, pen);
                Move(rp, da[0], da[1]);
                PolyDraw(rp, 5, da);
                break;
            case 2:  // Filled Area
                SetAPen(wrp, pen);
//              SetDrPt(wrp, ~0);  // solid draw pattern

                AreaMove(wrp, da[0], da[1]);
                AreaDraw(wrp, da[2], da[3]);
                AreaDraw(wrp, da[4], da[5]);
                AreaDraw(wrp, da[6], da[7]);
                AreaDraw(wrp, da[0], da[1]);
                AreaEnd(wrp);
                break;
            case 3:  // Board through-holes
                SetAPen(rp, pen);
                for (j = 0; j < d.w; j++) {      // x offset
                    int k;
                    int dx = j * 10;
                    for (k = 0; k < d.h; k++) {  // y offset
                        int dy = k * 6;
                        int nx = da[0] + dx + dy;
                        int ny = da[1] + dy / 4 - dx / 4;
                        Move(rp, nx, ny);
                        Draw(rp, nx + 1, ny);
                    }
                }
                break;
            case 4:  // Vertical pins
                for (j = 0; j < d.w; j += 7) {
                    int ny = y + d.y / 4 - (d.x + j) / 4;

                    SetAPen(rp, pen + 1);
                    Move(rp, j + da[0] + 3, ny);
                    Draw(rp, j + da[0] + 3, ny + d.h / 4);
                    SetAPen(rp, pen);
                    RectFill(rp, j + da[0], ny, j + da[0] + 2, ny + d.h / 4);
                }
                break;
            case 5:  // Horizontal pins
                for (j = 0; j < d.h; j += 2) {
                    int nx = x + d.x + d.y + j - 20;
                    SetAPen(rp, pen + 1);
                    Move(rp, nx + 1, j + da[1]);
                    Draw(rp, nx + d.w + 1, j + da[1]);

                    SetAPen(rp, pen);
                    Move(rp, nx, j + da[1]);
                    Draw(rp, nx + d.w, j + da[1]);
                }
                break;
        }
    }
}

/*
 * create_gadget
 * -------------
 * Simplified gadget create function.
 */
static struct Gadget *
create_gadget(UWORD kind)
{
    return (CreateGadget(kind, LastAdded, NewGadget,
                         GT_Underscore, '_', TAG_DONE));
}

/*
 * Print
 * -----
 * Display text at specified screen position
 */
static void
Print(STRPTR text, UWORD x, UWORD y, int center)
{
    struct RastPort *rp = &screen->RastPort;
    int len = strlen(text);

    /* Eliminate CRLF */
    while ((len > 0) && ((text[len - 1] == '\n') || (text[len - 1] == '\r')))
        len--;
    if (center == 1)       // center
        x += (SCREEN_WIDTH - TextLength(rp, text, len)) / 2;
    else if (center == 2)  // right align
        x -= TextLength(rp, text, len);
    Move(rp, x, y);
    Text(rp, text, len);
}

/*
 * update_status
 * -------------
 * Display status (error) text on screen.
 */
static void
update_status(const char *fmt, ...)
{
    va_list ap;
    char    buf[28];
    int     len;
    struct RastPort *rp = &screen->RastPort;

    memset(buf, ' ', sizeof (buf));

    va_start(ap, fmt);
    vsnprintf(buf, sizeof (buf), fmt, ap);
    va_end(ap);

    len = strlen(buf);
    if (len < sizeof (buf))
        buf[len] = ' ';
    SetAPen(&screen->RastPort, 2);  // White
    SetBPen(&screen->RastPort, 0);  // White
    Move(rp, 390, 84);
    Text(rp, buf, sizeof (buf));
}

/*
 * send_cmd_retry
 * --------------
 * Send a request to Kicksmash, retrying up to 5 times on error.
 */
static uint
send_cmd_retry(uint16_t cmd, void *arg, uint16_t arglen,
               void *reply, uint replymax, uint *replyalen)
{
    uint tries = 5;
    uint rc;

    do {
        rc = send_cmd(cmd, arg, arglen, reply, replymax, replyalen);
        if (rc == MSG_STATUS_SUCCESS)
            break;
    } while (--tries > 0);
    return (rc);
}

/*
 * get_banks
 * ---------
 * Acquire ROM bank information from KickSmash
 */
static int
get_banks(bank_info_t *bi)
{
    memset(bi, 0, sizeof (*bi));
    memset(bi->bi_longreset_seq, 0xff, sizeof (bi->bi_longreset_seq));
#undef UAE_SIM
#ifdef UAE_SIM
    strcpy(bi->bi_name[0], "KS322");
    strcpy(bi->bi_name[1], "KS322 backup");
    strcpy(bi->bi_name[2], "");
    strcpy(bi->bi_name[3], "DiagROM");
    strcpy(bi->bi_name[4], "KS322 romswitch");
    strcpy(bi->bi_name[5], "");
    strcpy(bi->bi_name[6], "");
    strcpy(bi->bi_name[7], "");
    bi->bi_bank_current = 4;
    bi->bi_bank_nextreset = 1;
    bi->bi_bank_poweron = 3;
    memset(bi->bi_longreset_seq, 0xff, sizeof (bi->bi_longreset_seq));
    bi->bi_longreset_seq[0] = 4;
    bi->bi_longreset_seq[1] = 3;
#else
    int rc;
    uint rlen;
    rc = send_cmd_retry(KS_CMD_BANK_INFO, NULL, 0, bi, sizeof (*bi), &rlen);
    if (rc != 0)
        update_status("FAIL info %d", rc);
#endif
    return (0);
}

static uint    timeout_seconds;
static uint    timeout_seconds_saved;  // Current value in NVM
static uint    timeout_seconds_remaining;
static uint    timeout_seconds_ticks;  // INTUITICKS count (rollover at 10)
static uint8_t timeout_active;
static uint8_t timeout_bank;
static uint8_t timeout_bank_saved;     // Current value in NVM

/*
 * get_bank_timeout
 * ----------------
 * Acquire settings from Kicksmash that indicate whether a timeout is
 * active and what default bank to use.
 */
static void
get_bank_timeout(uint *seconds, uint8_t *bank)
{
    int  rc;
    uint rlen;
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
        uint8_t data = rbuf[0];
        if (data & 0x80)
            *seconds = 60 * (data & 0x7f);  // Minutes
        else
            *seconds = data;                // Seconds
        *bank = rbuf[1];
    } else {
        update_status("FAIL Get NV %d", rc);
    }
}

/*
 * set_bank_timeout
 * ----------------
 * Store settings with Kicksmash that indicate whether a timeout is
 * active and what default bank to use.
 */
static int
set_bank_timeout(uint seconds, uint8_t bank)
{
    int  rc;
    uint rlen;
    uint8_t buf[4];
    buf[0] = 0;  // Start at NV0
    buf[1] = 2;  // Also write NV1
    if (seconds < 127) {
        buf[2] = seconds;           // store seconds
    } else {
        seconds /= 60;
        if (seconds > 127)
            seconds = 127;
        buf[2] = seconds | BIT(7);  // store minutes
    }
    buf[3] = bank;
    rc = send_cmd_retry(KS_CMD_SET | KS_SET_NV,
                        buf, sizeof (buf), NULL, 0, &rlen);
    if (rc != 0)
        update_status("FAIL set timeout %d", rc);

    return (rc);
}

/*
 * get_id
 * ------
 * Acquire KickSmash hardware and config information
 */
static void
get_id(smash_id_t *id)
{
    memset(id, 0, sizeof (*id));
    strcpy(id->si_serial, "Comm. Failure");
#ifdef UAE_SIM
    id->si_ks_version[0] = 1;
    id->si_ks_version[1] = 1;
    id->si_ks_date[0] = 20;  // Century
    id->si_ks_date[1] = 24;  // Year
    id->si_ks_date[2] = 11;  // Month
    id->si_ks_date[3] = 28;  // Day
    id->si_ks_time[0] = 12;  // H
    id->si_ks_time[1] = 34;  // M
    id->si_ks_time[3] = 56;  // S
    strcpy(id->si_serial, "_x__simulator__x_");
    id->si_features = 0x0001;   // Protocol version
    id->si_rev = 0x0001;        // Features
    id->si_usbid = 0x12091610;
    strcpy(id->si_name, "ksname");
    id->si_mode = 0;
#else
    int rc;
    uint rlen;
    rc = send_cmd_retry(KS_CMD_ID, NULL, 0, id, sizeof (*id), &rlen);
    if (rc != 0)
        update_status("FAIL id %d", rc);
#endif
}

/*
 * bank_state_save
 * ---------------
 * Send updated (user-modified) bank information to KickSmash
 */
static void
bank_state_save(void)
{
    int rc;
    uint rlen;
    uint had_error = 0;
    if (updated_names) {
        char argbuf[64];
        uint bank;
        for (bank = 0; bank < ROM_BANKS; bank++) {
            int slen = MIN(sizeof (info.bi_name[bank]), sizeof (argbuf) - 3);
            uint16_t argval = bank;

            if ((updated_names & BIT(bank)) == 0)
                continue;  // This name was not updated

            memcpy(argbuf, &argval, 2);
            strncpy(argbuf + 2, info.bi_name[bank], slen);
            argbuf[sizeof (argbuf) - 1] = '\0';

            rc = send_cmd_retry(KS_CMD_BANK_NAME, argbuf,
                                strlen(argbuf + 2) + 3, NULL, 0, &rlen);
            if ((rc != 0) && (had_error++ == 0))
                update_status("FAIL name %d: %d", bank, rc);
            if (rc == 0)
                updated_names &= ~BIT(bank);
        }
        if (updated_names & BIT(ROM_BANKS)) {
            /* Board name was updated */
            rc = send_cmd_retry(KS_CMD_SET | KS_SET_NAME,
                                id.si_name, sizeof (id.si_name), NULL, 0, NULL);
            if ((rc != 0) && (had_error++ == 0))
                update_status("FAIL name: %d", rc);
            if (rc == 0)
                updated_names &= ~BIT(ROM_BANKS);
        }
    }
    if (updated_longreset) {
        rc = send_cmd_retry(KS_CMD_BANK_LRESET, info.bi_longreset_seq,
                            sizeof (info.bi_longreset_seq), NULL, 0, NULL);
        if ((rc != 0) && (had_error++ == 0))
            update_status("FAIL set longreset: %d", rc);
        if (rc == 0)
            updated_longreset = 0;
    }
    if (updated_poweron) {
        uint16_t argval = info.bi_bank_poweron;
        rc = send_cmd_retry(KS_CMD_BANK_SET | KS_BANK_SETPOWERON,
                            &argval, sizeof (argval), NULL, 0, &rlen);
        if ((rc != 0) && (had_error++ == 0))
            update_status("FAIL set poweron: %d", rc);
        if (rc == 0)
            updated_poweron = 0;
    }
    if (updated_bank_timeout) {
        if (set_bank_timeout(timeout_seconds, timeout_bank) == 0) {
            updated_bank_timeout = 0;
            timeout_seconds_saved = timeout_seconds;
            timeout_bank_saved = timeout_bank;
        }
    }
    if (had_error == 0) {
        update_status("Success");
        memcpy(&info, &info_saved, sizeof (info));
    }
}

/*
 * bank_set_current_and_reboot
 * ---------------------------
 * Request KickSmash to set the current bank and force a reset.
 */
static void
bank_set_current_and_reboot(void)
{
    int rc;
    uint rlen;
    uint16_t argval = bank_switchto;
    rc = send_cmd_retry(KS_CMD_BANK_SET | KS_BANK_SETCURRENT | KS_BANK_REBOOT,
                        &argval, sizeof (argval), NULL, 0, &rlen);
    update_status("FAIL set reboot %d", rc);
}


/* These are widths (in characters) for the display table */
const uint8_t
banktable_widths[] = {
    5,  // Bank
    17, // Name (actually 16, but string gadget requires extra space)
    6,  // Merge
    10, // LongReset
    8,  // PowerOn
    8,  // Current
    10, // SwitchTo
};

uint banktable_pos[ARRAY_SIZE(banktable_widths)];

/*
 * box
 * ---
 * Draw a beveled box on screen.
 */
static void
box(uint x, uint y, uint w, uint h, ULONG tag)
{
    DrawBevelBox(&screen->RastPort,
                 x, y, w, h,
                 GT_VisualInfo, (ULONG)visualInfo,
                 tag,           TRUE,
                 TAG_DONE);
}

/*
 * sbox
 * ----
 * Draw a simple box on screen.
 */
static void
sbox(uint x, uint y, uint w, uint h)
{
    struct RastPort *rp = &screen->RastPort;
    WORD da[8];
    da[0] = x + w;
    da[1] = y;
    da[2] = x + w;
    da[3] = y + h;
    da[4] = x;
    da[5] = y + h;
    da[6] = x;
    da[7] = y;
    Move(rp, x, y);
    PolyDraw(rp, 4, da);

    /* Thicken the box sides */
    Move(rp, x - 1, y);
    Draw(rp, x - 1, y + h);
    Move(rp, x + w + 1, y);
    Draw(rp, x + w + 1, y + h);
}

/*
 * show_bank_cell
 * --------------
 * Display on screen a single cell of the ROM bank information table.
 */
static void
show_bank_cell(uint bank, uint col)
{
    char text[20];
    struct RastPort *rp = &screen->RastPort;
    uint xoff = banktable_pos[col];
    uint y = BANK_TABLE_YPOS + 2;
    uint chars = banktable_widths[col];

    memset(text, ' ', chars);
    text[chars] = '\0';
    switch (col) {
        case 0:  // Bank
            text[2] = bank + '0';
            break;
        case 1:  // Name
            memcpy(text, info.bi_name[bank], strlen(info.bi_name[bank]));
            xoff += 3;
            break;
        case 2: { // Merge
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
            break;
        }
        case 3: { // LongReset
            uint p;
            for (p = 0; p < ARRAY_SIZE(info.bi_longreset_seq); p++)
                if (info.bi_longreset_seq[p] == bank)
                    break;

            if (p < ARRAY_SIZE(info.bi_longreset_seq)) {
                text[4] = '0' + p;
            }
            /* Text between the + and - buttons */
            Move(rp, xoff + 7 + 4 * 8, y + 21 + bank * 9);
            Text(rp, text + 4, 1);

            /* Text to the right of the + button */
            Move(rp, xoff + 3 + 8 * 8, y + 21 + bank * 9);
            Text(rp, text, 2);

            /* Text to the left of the - button */
            chars = 2;
            break;
        }
        case 4: // PowerOn
            chars = 2;
skip_center_button:
            /* Text to the right of the button */
            Move(rp, xoff + 35 + chars * 8, y + 21 + bank * 9);
            Text(rp, text, chars);
            break;
        case 5: // Current
            if (bank == info.bi_bank_current) {
                chars = 2;
                goto skip_center_button;
            }
            break;
        case 6: // SwitchTo
            chars = 3;
            goto skip_center_button;
    }
    Move(rp, xoff + 3, y + 21 + bank * 9);
    Text(rp, text, chars);
}

/*
 * show_bank_table_column
 * ----------------------
 * Show a column of the ROM bank information table
 */
static void
show_bank_table_column(uint col)
{
    uint bank;
    struct RastPort *rp = &screen->RastPort;

    if (current_bank != 0)
        SetBPen(rp, 0);

    for (bank = 0; bank < ROM_BANKS; bank++) {
        if (bank == current_bank)
            SetBPen(rp, 3);
        show_bank_cell(bank, col);
        if (bank == current_bank)
            SetBPen(rp, 0);
    }
}

/*
 * set_initial_bank_switchto
 * -------------------------
 * Decide what the default ROM bank should be for the Switch button.
 */
static void
set_initial_bank_switchto(void)
{
    uint bank;

    bank_switchto = info.bi_bank_nextreset;
    if (bank_switchto >= ROM_BANKS) {
        /*
         * If the current bank is one of the longreset banks, then
         * pick the next one in that list as the default for SwitchTo
         */
        for (bank = 0; bank < ARRAY_SIZE(info.bi_longreset_seq); bank++) {
            if (info.bi_longreset_seq[bank] == 0xff)
                break;
            if (info.bi_longreset_seq[bank] == info.bi_bank_current) {
                /* Found current bank in longreset list */
                uint nextbank = bank + 1;
                if ((nextbank >= ARRAY_SIZE(info.bi_longreset_seq)) ||
                    (info.bi_longreset_seq[nextbank] >= ROM_BANKS)) {
                    nextbank = 0;  // wrap
                }
                bank_switchto = info.bi_longreset_seq[nextbank];
                break;
            }
        }
    }
    if (bank_switchto >= ROM_BANKS) {
        /* No other choice; just set to the current bank (disable Switch) */
        bank_switchto = info.bi_bank_current;
    }
}

/*
 * show_banks
 * ----------
 * Display the ROM bank information table.
 */
static void
show_banks(void)
{
    uint col;
    uint x = 32;
    uint y = BANK_TABLE_YPOS;
    uint xoff;
    uint width;
    struct RastPort *rp = &screen->RastPort;

    for (width = 0, col = 0; col < ARRAY_SIZE(banktable_widths); col++)
        width += banktable_widths[col] * 8 + 8;

    SetAPen(&screen->RastPort, 1);  // Black
    Print("Bank       Name         Merge  LongReset  PowerOn  Current  ",
          x + 10, y + 10, FALSE);
    Print("SwitchTo",
          x + width - banktable_widths[col - 1] * 8 + 6, y + 10, FALSE);

    SetAPen(rp, 1);
    box(x, y, width + 6, 18 + ROM_BANKS * 9, GTBB_Recessed);
    y += 2;
    xoff = x + 3;
    bank_box_top = y + 14;
    bank_box_bottom = y + 12 + 4 + ROM_BANKS * 9;
    bank_box_left = xoff;
    bank_box_right = xoff + width;
    for (col = 0; col < ARRAY_SIZE(banktable_widths); col++) {
        uint pwidth = banktable_widths[col] * 8 + 8;
        banktable_pos[col] = xoff;

        /* Title box */
        box(xoff, y, pwidth, 12, TAG_IGNORE);

        /* Column box */
        box(xoff, y + 12, pwidth, 3 + ROM_BANKS * 9, TAG_IGNORE);

        show_bank_table_column(col);

        xoff += pwidth;
    }
}

#ifdef BANK_MOUSEBAR
/*
 * bank_mouseover
 * --------------
 * Handle mouse movement over the ROM bank table (draw blue current line)
 */
static void
bank_mouseover(uint pos)
{
    uint bank = pos / 9;
    uint col;
    struct RastPort *rp;

    if (bank == current_bank)
        return;

    rp = &screen->RastPort;
    SetAPen(rp, 1);

    if ((bank >= ROM_BANKS) || (current_bank != 0xff)) {
        SetBPen(rp, 0);
        if (current_column != 0) {
            show_bank_cell(current_bank, current_column);
        } else {
            for (col = 0; col < ARRAY_SIZE(banktable_widths); col++)
                show_bank_cell(current_bank, col);
        }
        if (bank >= ROM_BANKS) {
            current_bank = 0xff;
            return;
        }
    }

    SetBPen(rp, 3);
    if (current_column != 0) {
        show_bank_cell(current_bank, current_column);
    } else {
        for (col = 0; col < ARRAY_SIZE(banktable_widths); col++)
            show_bank_cell(bank, col);
    }
    current_bank = bank;
}
#endif

/*
 * update_save_box
 * ---------------
 * Update the gadget handling the Save box to be disabled or enabled.
 */
static void
update_save_box(void)
{
    ULONG state = (updated_names | updated_longreset | updated_poweron |
                   updated_bank_timeout) ? FALSE : TRUE;
    static ULONG lstate = 0xff;

    if (lstate != state) {
        lstate = state;
        disabled_save = state;
        GT_SetGadgetAttrs(gadget_save, NULL, NULL,
                          GA_Disabled, state, TAG_DONE);
        RefreshGList(gadget_save_pre, window, NULL, -1);
    }
}

/*
 * update_switch_box
 * -----------------
 * Update the gadget controlling the Switch box to be disabled or enabled.
 */
static uint
update_switch_box(void)
{
    ULONG state = (bank_switchto != info.bi_bank_current) ? FALSE : TRUE;
    static ULONG lstate = 0xff;

    if (lstate != state) {
        lstate = state;
        disabled_switch = state;
        GT_SetGadgetAttrs(gadget_switch, NULL, NULL,
                          GA_Disabled, state, TAG_DONE);
        RefreshGList(gadget_switch_pre, window, NULL, -1);
        return (0);
    }
    return (1);
}

static void
update_switchto(int incdec)
{
    if (incdec > 0) {
        if (++bank_switchto >= ROM_BANKS)
            bank_switchto = 0;
    } else {
        if (--bank_switchto >= ROM_BANKS)
            bank_switchto = ROM_BANKS - 1;
    }
    GT_SetGadgetAttrs(gadget_switchto, NULL, NULL,
                      GTMX_Active, (LONG) bank_switchto,
                      TAG_DONE);

    if (update_switch_box())
        RefreshGList(gadget_switchto_pre, window, NULL, -1);
}

/*
 * bank_update_names
 * -----------------
 * If one of the bank name gadgets is currently active, update the backing
 * store of the name string for that gadget.
 */
static void
bank_update_names(void)
{
    uint bank;
    uint updated_names_prev = updated_names;
    for (bank = 0; bank < ROM_BANKS; bank++) {
        struct Gadget *gad = gadget_banktable_name[bank];
        if (gad == NULL)
            continue;
        if (gad->Activation & GACT_ACTIVEGADGET) {
            STRPTR string;
            GT_GetGadgetAttrs(gad, NULL, NULL,
                              GTST_String, (LONG) &string, TAG_DONE);
            strcpy(info.bi_name[bank], string);
            if (strcmp(info_saved.bi_name[bank], string) != 0) {
                updated_names |= BIT(bank);
            } else {
                updated_names &= ~BIT(bank);
            }
            break;
        }
    }
    if (gadget_board_name->Activation & GACT_ACTIVEGADGET) {
        STRPTR string;
        GT_GetGadgetAttrs(gadget_board_name, NULL, NULL,
                          GTST_String, (LONG) &string, TAG_DONE);
        strcpy(id.si_name, string);
        if (strcmp(id_saved.si_name, string) != 0) {
            updated_names |= BIT(ROM_BANKS);
        } else {
            updated_names &= ~BIT(ROM_BANKS);
        }
    }
    if (updated_names != updated_names_prev)
        update_save_box();
}

/*
 * show_id
 * -------
 * Display KickSmash identification information (version, build, serial, etc)
 */
static void
show_id(void)
{
    uint x = 40;
    uint y = 17;
    char buf[40];
    get_id(&id);
    memcpy(&id_saved, &id, sizeof (id_saved));

    SetAPen(&screen->RastPort, 1);  // Black
    sprintf(buf, "KickSmash%u %u.%u",
            ((id.si_mode != 1) && (id.si_mode != 2)) ? 32 : 16,
            id.si_ks_version[0], id.si_ks_version[1]);
    Print(buf, x, y += 9, 0);

    sprintf(buf, "Built %02u%02u-%02u-%02u %02u:%02u:%02u",
            id.si_ks_date[0], id.si_ks_date[1],
            id.si_ks_date[2], id.si_ks_date[3],
            id.si_ks_time[0], id.si_ks_time[1], id.si_ks_time[2]);
    Print(buf, x, y += 9, 0);

    sprintf(buf, "Serial \"%s\"", id.si_serial);
    Print(buf, x, y += 9, 0);

    sprintf(buf, "USB %04x.%04x", id.si_usbid >> 16, (uint16_t) id.si_usbid);
    Print(buf, x, y += 9, 0);

    Print("Board name", x, y + 16, 0);
    SetAPen(&screen->RastPort, 1);
}

static void
switch_to_timeout_bank(void)
{
    update_status("Switching to bank %u", timeout_bank & 0x7);
    bank_switchto = timeout_bank;
    bank_set_current_and_reboot();
}

static void
bank_update_timeout(void)
{
    uint value;
    uint did_update = FALSE;
    static uint8_t timeout_was_active;

    if (timeout_active) {
        timeout_was_active = TRUE;
        if (++timeout_seconds_ticks == 10) {
            timeout_seconds_ticks = 0;
            timeout_seconds_remaining--;
            if (timeout_seconds_remaining == 0) {
                switch_to_timeout_bank();
                timeout_active = FALSE;
            }
        }
        if (timeout_seconds_ticks == 0) {
            update_status("Switching to bank %u in %u",
                          timeout_bank & 0x7, timeout_seconds_remaining);
        }
    } else if (timeout_was_active) {
        timeout_was_active = FALSE;
        update_status("");
    }

    if (gadget_timeout_bank->Activation & GACT_ACTIVEGADGET) {
        GT_GetGadgetAttrs(gadget_timeout_bank, NULL, NULL,
                          GTIN_Number, (uintptr_t) &value, TAG_DONE);
        timeout_bank = value;
        did_update = TRUE;
    }
    if (gadget_timeout_seconds->Activation & GACT_ACTIVEGADGET) {
        GT_GetGadgetAttrs(gadget_timeout_seconds, NULL, NULL,
                          GTIN_Number, (uintptr_t) &value, TAG_DONE);
        timeout_seconds = value;
        did_update = TRUE;
    }
    if (did_update) {
#undef DEBUG_TIMEOUT
#ifdef DEBUG_TIMEOUT
        if (timeout_active)
            update_status("Timeout disabled by gadget");
#endif
        timeout_active = FALSE;
// printf("[%x,%x %x,%x]", timeout_bank, timeout_bank_saved, timeout_seconds, timeout_seconds_saved);
        updated_bank_timeout = (timeout_bank != timeout_bank_saved) ||
                               (timeout_seconds != timeout_seconds_saved);
        update_save_box();
    }
}

void
show_bank_timeout(void)
{
    struct RastPort *rp = &screen->RastPort;
    get_bank_timeout(&timeout_seconds, &timeout_bank);
    timeout_seconds_saved = timeout_seconds;
    timeout_bank_saved = timeout_bank;

    if (timeout_seconds != 0) {
        timeout_seconds_remaining = timeout_seconds;
        timeout_active = TRUE;
    }
    SetAPen(rp, 2);
    Print("Auto Switch", 508, 38, 0);

    struct NewGadget ng;
    ng.ng_TextAttr   = NULL;
    ng.ng_Flags      = 0;
    ng.ng_VisualInfo = visualInfo;

    ng.ng_Width      = 30;
    ng.ng_Height     = 10;
    ng.ng_TopEdge    = 43;
    ng.ng_LeftEdge   = 541;
    ng.ng_GadgetText = "Bank";
    ng.ng_GadgetID   = ID_BANK_DEFAULT;
    LastAdded = CreateGadget(INTEGER_KIND, LastAdded, &ng,
                             GTIN_MaxChars, 1,
                             GTIN_Number, timeout_bank,
                             GA_TabCycle, TRUE,
                             TAG_DONE);
    gadget_timeout_bank = LastAdded;

    ng.ng_Width      = 52;
    ng.ng_Height     = 10;
    ng.ng_TopEdge    = 56;
    ng.ng_LeftEdge   = 541;
    ng.ng_GadgetText = "Timeout";
    ng.ng_GadgetID   = ID_BANK_TIMEOUT;
    LastAdded = CreateGadget(INTEGER_KIND, LastAdded, &ng,
                             GTIN_MaxChars, 4,
                             GTIN_Number, timeout_seconds,
                             GA_TabCycle, TRUE,
                             TAG_DONE);
    gadget_timeout_seconds = LastAdded;
}


#if 0
static ULONG
string_hook(struct Hook *hook asm("a0"), struct SGWork *sgw asm("a2"),
            ULONG *msg asm("a1"))
{
    UBYTE *work_ptr;
    ULONG rc;

    (void) hook;

// XXX: all I want to do is mark that an edit occurred in the current string
//      so that it can later be compared to update the Save box
#define toupper(x) (((x >= 'a') && (x <= 'z')) ? (x + 'A' - 'a') : x)
#define isxdigit(x) ((((x) >= 'a') && ((x) <= 'f')) || \
                     (((x) >= 'A') && ((x) <= 'F')) || \
                     (((x) >= '0') && ((x) <= '9')))
printf("hook=%x eo=%x code=%x modes=%x actions=%x editop=%x\n", *msg, sgw->EditOp, sgw->Code, sgw->Modes, sgw->Actions, sgw->EditOp);
    /*
     * Hook must return non-zero if command is supported.
     * This will be changed to zero if the command is unsupported.
    */
    rc = ~0L;
#if 0
    /* This does not work until after first message */
               sgw->Actions |= SGA_END;  // End input for this string
               sgw->Actions &= ~SGA_USE;  // Do not use input
return (rc);
#endif


    if (*msg == SGH_KEY) {
        /*
         * key hit -- could be any key (Shift, repeat, character, etc.)
         *
         * allow only upper case characters to be entered.
         * act only on modes that add or update characters in the buffer.
         */
        if (sgw->EditOp == EO_ENTER) {
            printf("enter pressed (terminate)\n");
            return (rc);
        }
        if ((sgw->EditOp == EO_REPLACECHAR) ||
            (sgw->EditOp == EO_INSERTCHAR)) {
            /*
             * Code contains the ASCII representation of the character
             * entered, if it maps to a single byte.  We could also look
             * into the work buffer to find the new character.
             *
             *     sgw->Code == sgw->WorkBuffer[sgw->BufferPos - 1]
             *
             * If the character is not a legal hex digit, don't use
             * the work buffer and beep the screen.
             */
            if (!isxdigit(sgw->Code)) {
                sgw->Actions |= SGA_BEEP;
//              sgw->Actions |= SGA_END;  // End input for this string
                sgw->Actions &= ~SGA_USE;  // Do not use input
            } else {
                /* make it upper-case, for nicety */
                sgw->WorkBuffer[sgw->BufferPos - 1] = toupper(sgw->Code);
            }
        }
    } else if (*msg == SGH_CLICK) {
        /*
         * mouse click
         * zero the digit clicked on
         */
        if (sgw->BufferPos < sgw->NumChars) {
            work_ptr = sgw->WorkBuffer + sgw->BufferPos;
            *work_ptr = '0';
        }
    } else {
        rc = 0;  // Unknown command -- return 0
    }

    return (rc);
}

static ULONG
string_hook_entry(struct Hook *hook asm("a0"), struct SGWork *sgw asm("a2"),
                  ULONG *msg asm("a1"))
{
    ULONG rc;
    __asm("move.l a4,-(sp)");                     // Preserve caller's a4
    __asm("move.l %0,a4" :: "r" (hook->h_Data));  // Get globals
    rc = (*hook->h_SubEntry)(hook, sgw, msg);
    __asm("move.l (sp)+,a4");                     // Restore caller's a4
    return (rc);
}
#endif

#ifndef STANDALONE
static void
cleanup_bank_name_gadgets(void)
{
    struct RastPort *rp = &screen->RastPort;
    uint bank;
    uint y = bank_box_top + 1;
    uint x1 = banktable_pos[1] + 2;
    uint x2 = x1 + banktable_widths[1] * 8;

    SetAPen(rp, 0);
    for (bank = 0; bank < ROM_BANKS; bank++) {
        sbox(x1, y, 1, 7);
        sbox(x1 + 2, y, 0, 7);
        sbox(x2, y, 1, 7);
        sbox(x2 + 2, y, 0, 7);
        y += 9;
    }
}
#endif

/*
 * draw_page
 * ---------
 * Draw the screen, creating all gadgets.
 */
static void
draw_page(void)
{
    uint bank;
    struct RastPort *rp = &screen->RastPort;
    struct ViewPort *vp = &screen->ViewPort;
    volatile char *sptr;
    struct NewGadget ng;

    ng.ng_TextAttr   = NULL;
    ng.ng_Flags      = 0;
    ng.ng_VisualInfo = visualInfo;
    ng.ng_Width      = 87;
    ng.ng_Height     = 14;
    ng.ng_TopEdge    = 183;

    NewGadget = &ng;

    // XXX: Could also use LoadRGB4 to load all pens more efficiently
    // 0 = Background (Gray)
    // 1 = Black
    // 2 = White
    SetRGB4(vp, 3, 6, 8, 11);   // Pen 3: Lt.Blue
    SetRGB4(vp, 4, 13, 13, 5);  // Pen 4: Gold
    SetRGB4(vp, 5, 10, 9, 2);   // Pen 5: Gold dim
    SetRGB4(vp, 6, 3, 3, 3);    // Pen 6: Dark Gray

#if 0
    if (gadgets) {
        RemoveGList(window, gadgets, -1);
        FreeGadgets(gadgets);
        LastAdded = NULL;
        SetRast(rp, 0);
    }
#endif
    SetAPen(rp, 1);
    char buf[32];
    sprintf(buf, "KickSmash ROM switcher %3s", VERSION);
    Print(buf, 0, 10, TRUE);
    box(40, 0, 560, 14, GTBB_Recessed);

    gadgets = CreateContext(&LastAdded);

    draw_array(kicksmash_drawing, ARRAY_SIZE(kicksmash_drawing));

    get_banks(&info);
    memcpy(&info_saved, &info, sizeof (info));
    set_initial_bank_switchto();
    show_banks();
    show_id();
    show_bank_timeout();

    /* LongReset + and - buttons */
    ng.ng_Width = 14;
    ng.ng_Height     = 8;
    for (bank = 0; bank < ROM_BANKS; bank++) {
        ng.ng_TopEdge = bank_box_top + 9 * bank;
        ng.ng_LeftEdge = banktable_pos[3] + 6 + 2 * 8;
        ng.ng_GadgetID = ID_LONGRESET_MINUS_0 + bank;
        ng.ng_GadgetText = "-";
        LastAdded = create_gadget(BUTTON_KIND);

        ng.ng_LeftEdge = banktable_pos[3] + 10 + 5 * 8;
        ng.ng_GadgetID = ID_LONGRESET_PLUS_0 + bank;
        ng.ng_GadgetText = "+";
        LastAdded = create_gadget(BUTTON_KIND);
    }

    /* Current ROM bank */
    char *current_sel_labels[] = { "", NULL };
    ng.ng_Width      = 26;
    ng.ng_Height     = 8;
    ng.ng_TopEdge  = bank_box_top + 9 * info.bi_bank_current;
    ng.ng_LeftEdge = (banktable_pos[6] + banktable_pos[5] -
                      ng.ng_Width) / 2 - 1;
    ng.ng_GadgetID = ID_CURRENT_RADIO;
    ng.ng_GadgetText = NULL;
    LastAdded = CreateGadget(MX_KIND, LastAdded, NewGadget,
                             GTMX_Labels, (ULONG) current_sel_labels,
                             GTMX_Active, (UWORD) 0,
                             GTMX_Spacing, (UWORD) 1,
                             GTMX_Scaled, TRUE,
                             TAG_DONE);

    /* ROM bank names */
    ng.ng_Height     = 9;
    ng.ng_GadgetText = NULL;
    ng.ng_LeftEdge   = banktable_pos[1];
    ng.ng_Width      = banktable_pos[2] - banktable_pos[1];
    for (bank = 0; bank < ROM_BANKS; bank++) {
        sptr = info.bi_name[bank];
        ng.ng_GadgetID   = ID_BANK_NAME_0 + bank;
        ng.ng_TopEdge    = bank_box_top + bank * 9;

        LastAdded = CreateGadget(STRING_KIND, LastAdded, NewGadget,
                                 GTST_MaxChars, sizeof (info.bi_name[0]) - 1,
                                 GTST_String, (ULONG) sptr,
                                 GA_Border, 6,
                                 GA_TabCycle, TRUE,
                                 TAG_DONE);
        gadget_banktable_name[bank] = LastAdded;
    }

#if 0
    static struct Hook string_edit_hook;
    string_edit_hook.h_Entry = string_hook_entry;
    string_edit_hook.h_SubEntry = string_hook;
    string_edit_hook.h_Data = globals_base;
#endif

    /* KickSmash board name */
    ng.ng_LeftEdge   = 40;
    ng.ng_TopEdge    = 72;
    ng.ng_Width      = 8 * 18;
    ng.ng_Height     = 12;
    ng.ng_GadgetText = NULL;
    ng.ng_GadgetID   = ID_BOARD_NAME;
    sptr = id.si_name;  // work around odd compiler bug
    LastAdded = CreateGadget(STRING_KIND, LastAdded, NewGadget,
                             GTST_MaxChars, (UWORD) sizeof (id.si_name) - 1,
                             GTST_String, (ULONG) sptr,
//                           GTST_EditHook, (ULONG) &string_edit_hook,
                             GA_TabCycle, TRUE,
                             TAG_DONE);
    gadget_board_name = LastAdded;

    ng.ng_LeftEdge   = 0;

    /* Save button */
    gadget_save_pre = LastAdded;
    ng.ng_Height     = 12;
    ng.ng_TopEdge    = BUTTONS_YPOS;
    ng.ng_LeftEdge  += 120;
    ng.ng_Width      = 88;
    ng.ng_GadgetText = "_Save";
    ng.ng_GadgetID   = ID_SAVE;
    disabled_save = TRUE;
    LastAdded = CreateGadget(BUTTON_KIND, LastAdded, NewGadget,
                             GA_DISABLED, disabled_save,
                             GT_Underscore, '_',
                             TAG_DONE);
    gadget_save = LastAdded;
    gadget_save_x = ng.ng_LeftEdge - 3;
    gadget_save_y = ng.ng_TopEdge - 2;
    gadget_save_w = ng.ng_Width + 5;
    gadget_save_h = ng.ng_Height + 3;

    /* Cancel button */
    ng.ng_LeftEdge  += 120;
    ng.ng_Width      = 88;
    ng.ng_GadgetText = "_Cancel";
    ng.ng_GadgetID   = ID_CANCEL;
    LastAdded = create_gadget(BUTTON_KIND);
    gadget_cancel_x = ng.ng_LeftEdge - 3;
    gadget_cancel_y = ng.ng_TopEdge - 2;
    gadget_cancel_w = ng.ng_Width + 5;
    gadget_cancel_h = ng.ng_Height + 3;

    /* Switch and Reboot button */
    gadget_switch_pre = LastAdded;
    disabled_switch = (bank_switchto != info.bi_bank_current) ? FALSE : TRUE;
    ng.ng_LeftEdge  += 120;
    ng.ng_Width      = 176;
    ng.ng_GadgetText = "Switch and _Reboot";
    ng.ng_GadgetID   = ID_SWITCH;
    LastAdded = CreateGadget(BUTTON_KIND, LastAdded, NewGadget,
                             GA_DISABLED, disabled_switch,
                             GT_Underscore, '_',
                             TAG_DONE);
    gadget_switch_x = ng.ng_LeftEdge - 3;
    gadget_switch_y = ng.ng_TopEdge - 2;
    gadget_switch_w = ng.ng_Width + 5;
    gadget_switch_h = ng.ng_Height + 3;
    gadget_switch = LastAdded;

#if 0
    ng.ng_LeftEdge   = 540;
    ng.ng_TopEdge    = 76;
    ng.ng_Width      = 0;
    ng.ng_Height     = 10;
    ng.ng_GadgetText = NULL;
    ng.ng_GadgetID   = ID_SWITCHTO_STRING;
    LastAdded = CreateGadget(STRING_KIND, LastAdded, NewGadget,
                             GTST_MaxChars, 0,
                             GA_TabCycle, TRUE,
                             TAG_DONE);
    gadget_switchto_string = LastAdded;
#endif
#if 0
    /*
     * GA_Immediate -- Hear IDCMP_GADGETDOWN events from string gadget
     * GTST_EditHook -- custom string gadget edit hook
     */

    struct StringInfo *si = LastAdded->SpecialInfo;
    printf("si=%x\n", si);
    if (si != NULL) {
        printf("si buffer=%p\n", si->Buffer);
//      strcpy(si->Buffer, info.bi_name[0]);
        printf("si undo buffer=%p\n", si->UndoBuffer);
        printf("si maxchars=%x\n", si->MaxChars);
        printf("si Extension=%p\n", si->Extension);
        struct StringExtend *ext = si->Extension;
        if (ext != NULL) {
            printf("pens=%x %x\n", ext->Pens[0], ext->Pens[1]);
            printf("active_pens=%x %x\n", ext->ActivePens[0],
                   ext->ActivePens[1]);
            printf("modes=%x\n", ext->InitialModes);
            ext->ActivePens[1] = 3;  // Background to blue
        }
    }

    static UBYTE undo_buffer[128];
    static struct StringExtend StringExt = {
            NULL,
            { 2, 3, },
            { 3, 2, },
            0,
            NULL,
            NULL,
            { 0 }
    };
    static struct StringInfo sinfo = {
            info.bi_name[1],
            info.bi_name[1],
            0,
            15,
            0,
            0, 0, 0, 0, 0,
            (APTR)&StringExt,
            0,
            NULL
    };

    LastAdded->SpecialInfo = &sinfo;
#endif

    /*
     * To read a string gadget's buffer, look at the Gadget's StringInfo Buffer:
     * ((struct StringInfo *)gad->SpecialInfo)->Buffer
     *
     * ActivateGadget() to activate a specific gadget
     */

    /* PowerOn select radio */
    char *poweron_sel_labels[] = { "", "", "", "", "", "", "", "", NULL };
    ng.ng_TopEdge    = bank_box_top;
    ng.ng_Width      = 26;
    ng.ng_LeftEdge   = (banktable_pos[5] + banktable_pos[4] -
                        ng.ng_Width) / 2 - 1;
    ng.ng_Height     = 8;
    ng.ng_GadgetText = NULL;
    ng.ng_GadgetID   = ID_POWERON_RADIO;
    LastAdded = CreateGadget(MX_KIND, LastAdded, NewGadget,
                             GTMX_Labels, (ULONG) poweron_sel_labels,
                             GTMX_Active, (UWORD) info.bi_bank_poweron,
                             GTMX_Spacing, (UWORD) 1,
                             GTMX_Scaled, TRUE,
                             GA_Immediate, TRUE,
//                           GA_DISABLED, TRUE,
                             TAG_DONE);

    /* SwitchTo select radio */
    gadget_switchto_pre = LastAdded;
    ng.ng_Width      = 26;
    ng.ng_TopEdge = bank_box_top;
    ng.ng_LeftEdge = banktable_pos[6] +
                     (banktable_widths[6] * 8 - ng.ng_Width) / 2 + 3;
    ng.ng_GadgetID = ID_SWITCHTO_RADIO;
    LastAdded = CreateGadget(MX_KIND, LastAdded, NewGadget,
                             GTMX_Labels, (ULONG) poweron_sel_labels,
                             GTMX_Active, (UWORD) bank_switchto,
                             GTMX_Spacing, (UWORD) 1,
                             GTMX_Scaled, TRUE,
                             TAG_DONE);
    gadget_switchto = LastAdded;

    AddGList(window, gadgets, -1, -1, NULL);
    RefreshGList(gadgets, window, NULL, -1);
    GT_RefreshWindow(window, NULL);
#ifndef STANDALONE
    cleanup_bank_name_gadgets();
#endif
}

/*
 * bank_longreset_change
 * ---------------------
 * Handle updates to the LongReset column
 */
static void
bank_longreset_change(uint bank, int addsub)
{
    uint curpos = 0xff;
    uint lastpos;
    uint pos;

    /* Find the current size of the reset sequence */
    for (lastpos = 0; lastpos < ARRAY_SIZE(info.bi_longreset_seq); lastpos++)
        if (info.bi_longreset_seq[lastpos] == 0xff)
            break;

    LPRINTF("bank=%u addsub=%d lastpos=%u curpos=%u\n",
            bank, addsub, lastpos, curpos);

    /* Find the current position of the bank in the sequence */
    for (pos = 0; pos < lastpos; pos++)
        if (info.bi_longreset_seq[pos] == bank) {
            curpos = pos;
            break;
        }

    LPRINTF("START:");
    for (pos = 0; pos < ARRAY_SIZE(info.bi_longreset_seq); pos++) {
        if (info.bi_longreset_seq[pos] == 0xff)
            break;
        LPRINTF(" %u", info.bi_longreset_seq[pos]);
    }
    LPRINTF("\n");

    if ((addsub < 0) && (curpos == 0xff)) {
        /* Subtract when not in list adds it to the end of the list */
        LPRINTF("subtract when not in list\n");
        if (lastpos <= ARRAY_SIZE(info.bi_longreset_seq))
            info.bi_longreset_seq[lastpos] = bank;
        goto end;
        return;
    }

    if ((addsub > 0) && (curpos == 0xff)) {
        /* Add when not in list adds it to the beginning of the list */
        LPRINTF("add when not in list\n");
        if (lastpos < ARRAY_SIZE(info.bi_longreset_seq)) {
            for (pos = lastpos; pos > 0; pos--)
                info.bi_longreset_seq[pos] = info.bi_longreset_seq[pos - 1];
            info.bi_longreset_seq[0] = bank;
        }
        goto end;
    }

    /* Currently in the list */

    if ((addsub < 0) && (curpos == 0)) {
        /* Subtract when at start of list = Remove */
        LPRINTF("subtract when at start of list = Remove\n");
        for (pos = 0; pos < lastpos; pos++)
            info.bi_longreset_seq[pos] = info.bi_longreset_seq[pos + 1];
        info.bi_longreset_seq[lastpos - 1] = 0xff;
        goto end;
        return;
    }
    if ((addsub > 0) && (curpos == lastpos - 1)) {
        /* Add when at end of list = Remove */
        LPRINTF("add when at end of list = Remove\n");
        info.bi_longreset_seq[curpos] = 0xff;
        goto end;
        return;
    }

    /* Somewhere in the middle of the list */
    if (addsub < 0) {
        /* Swap with position immediately before */
        LPRINTF("swap with position before\n");
        info.bi_longreset_seq[curpos] = info.bi_longreset_seq[curpos - 1];
        info.bi_longreset_seq[curpos - 1] = bank;
    } else {
        /* Swap with position immediately after */
        LPRINTF("swap with position after\n");
        info.bi_longreset_seq[curpos] = info.bi_longreset_seq[curpos + 1];
        info.bi_longreset_seq[curpos + 1] = bank;
    }

end:
    LPRINTF("  END:");
    for (pos = 0; pos < ARRAY_SIZE(info.bi_longreset_seq); pos++) {
        if (info.bi_longreset_seq[pos] == 0xff)
            break;
        LPRINTF(" %u", info.bi_longreset_seq[pos]);
    }
    LPRINTF("\n");
}

/*
 * event_loop
 * ----------
 * Handle event messages from AmigaOS
 */
static void
event_loop(void)
{
    ULONG class;
    UWORD icode;
    LONG temp;
    struct IntuiMessage *msg;
    struct RastPort *rp = &screen->RastPort;
    struct Gadget *gad;
    int esc_trigger = FALSE;

    while (1) {
        WaitPort(window->UserPort);
        if ((msg = GT_GetIMsg(window->UserPort))) {
            class = msg->Class;
            icode = msg->Code;
            gad = msg->IAddress;
#undef DEBUG_IDCMP
#ifdef DEBUG_IDCMP
            if (class != IDCMP_INTUITICKS) {
                printf("class=%x code=%x gad=%x q=%x\n",
                       class, icode, gad ? gad->GadgetID : 0xff,
                       msg->Qualifier);
            }
#endif
            GT_ReplyIMsg(msg);
            switch (class) {
                case IDCMP_VANILLAKEY:
                    update_status("vanilla %x %x\n", icode, msg->Qualifier);
                    break;
                case IDCMP_RAWKEY:
                    if ((icode & 0x80) == 0) {
                        /* Key down */
#ifdef DEBUG_TIMEOUT
                        if (timeout_active)
                            update_status("Timeout disabled Key %x", icode);
#endif
                        timeout_active = FALSE;
                    }
                    if ((esc_trigger) && (icode != RAWKEY_ESC) &&
                                         (icode != RAWKEY_ESC + 0x80)) {
                        update_status("");
                        esc_trigger = FALSE;
                        SetAPen(rp, 0);
                        sbox(gadget_cancel_x, gadget_cancel_y,
                             gadget_cancel_w, gadget_cancel_h);
                    }
                    switch (icode) {
                        case RAWKEY_ESC + 0x80: // key up ESC
                            if (esc_trigger) {
                                ColdReboot();
                            } else {
                                update_status("Press ESC again to reset");
                                esc_trigger = TRUE;
                                SetAPen(rp, 3);
                                sbox(gadget_cancel_x, gadget_cancel_y,
                                     gadget_cancel_w, gadget_cancel_h);
                            }
                            break;
                        case 0x33:            // key down C
                            if ((msg->Qualifier & (IEQUALIFIER_LCOMMAND |
                                                   IEQUALIFIER_RCOMMAND |
                                                   IEQUALIFIER_CONTROL)) == 0) {
                                break;  // Left Amiga or Right Amiga not held
                            }
                            SetAPen(rp, 3);
                            sbox(gadget_cancel_x, gadget_cancel_y,
                                 gadget_cancel_w, gadget_cancel_h);
                            break;
                        case 0x33 + 0x80:     // key up C
                            if ((msg->Qualifier & (IEQUALIFIER_LCOMMAND |
                                                   IEQUALIFIER_RCOMMAND |
                                                   IEQUALIFIER_CONTROL)) == 0) {
                                break;  // Left Amiga or Right Amiga not held
                            }
                            ColdReboot();
                            break;
                        case 0x21:            // key down S
                            if ((msg->Qualifier & (IEQUALIFIER_LCOMMAND |
                                                   IEQUALIFIER_RCOMMAND |
                                                   IEQUALIFIER_CONTROL)) == 0) {
                                break;  // Left Amiga or Right Amiga not held
                            }
                            if (disabled_save)
                                break;
                            SetAPen(rp, 3);
                            sbox(gadget_save_x, gadget_save_y,
                                 gadget_save_w, gadget_save_h);
                            break;
                        case 0x21 + 0x80:     // key up S
                            if ((msg->Qualifier & (IEQUALIFIER_LCOMMAND |
                                                   IEQUALIFIER_RCOMMAND |
                                                   IEQUALIFIER_CONTROL)) == 0) {
                                break;  // Left Amiga or Right Amiga not held
                            }
                            if (disabled_save)
                                break;
                            SetAPen(rp, 0);
                            sbox(gadget_save_x, gadget_save_y,
                                 gadget_save_w, gadget_save_h);
                            goto id_save;
                        case RAWKEY_CRSRDOWN: // key down Cursor-Down
                        case 0x26:            // key down J
                        case 0x1e:            // key down numeric keypad 2
                            update_switchto(1);
                            break;
                        case RAWKEY_CRSRUP:   // key down Cursor-Up
                        case 0x27:            // key down K
                        case 0x3e:            // key down numeric keypad 8
                            update_switchto(-1);
                            break;
                        case 0x13:            // key down R
                            if ((msg->Qualifier & (IEQUALIFIER_LCOMMAND |
                                                   IEQUALIFIER_RCOMMAND |
                                                   IEQUALIFIER_CONTROL)) == 0) {
                                break;  // Left Amiga or Right Amiga not held
                            }
                            if (disabled_switch)
                                break;
                            /* FALLTHROUGH */
#if 0
                        case RAWKEY_ENTER:    // key down Enter (num keypd)
                        case RAWKEY_RETURN:   // key down Return
#endif
                            if (disabled_switch)
                                break;
                            SetAPen(rp, 3);
                            sbox(gadget_switch_x, gadget_switch_y,
                                 gadget_switch_w, gadget_switch_h);
                            break;
                        case 0x13 + 0x80:          // key up R
                            if ((msg->Qualifier & (IEQUALIFIER_LCOMMAND |
                                                   IEQUALIFIER_RCOMMAND |
                                                   IEQUALIFIER_CONTROL)) == 0) {
                                break;  // Left Amiga or Right Amiga not held
                            }
                            /* FALLTHROUGH */
#if 0
                        case RAWKEY_ENTER + 0x80:  // key up Enter (num keypd)
                        case RAWKEY_RETURN + 0x80: // key up Return
#endif
                            if (disabled_switch)
                                break;
                            SetAPen(rp, 0);
                            sbox(gadget_switch_x, gadget_switch_y,
                                 gadget_switch_w, gadget_switch_h);
                            bank_set_current_and_reboot();
                            break;
                        case RAWKEY_TAB: // key down Tab
                            if (msg->Qualifier == 0x8001) {
                                ActivateGadget(gadget_banktable_name[7],
                                               window, NULL);
                            } else {
                                ActivateGadget(gadget_board_name, window, NULL);
                            }
                            break;
                    }
                    break;
#if 0
                case IDCMP_MOUSEMOVE:
                    /*
                     * GadTools puts the gadget address into IAddress
                     * of IDCMP_MOUSEMOVE messages.  This is NOT true
                     * for standard Intuition messages, but is an added
                     * feature of GadTools.
                     */
                    if ((window->MouseY > bank_box_top) &&
                        (window->MouseY < bank_box_bottom) &&
                        (window->MouseX > bank_box_left) &&
                        (window->MouseX < bank_box_right)) {
                        bank_mouseover(window->MouseY - bank_box_top);
                    } else if (current_bank != 0xff) {
                        bank_mouseover(bank_box_bottom);
                    }
                    break;
#endif
                case IDCMP_INTUITICKS:
#ifdef BANK_MOUSEBAR
                    /* The color bar gets in the way of the string cursor */
                    if ((window->MouseY > bank_box_top) &&
                        (window->MouseY < bank_box_bottom) &&
                        (window->MouseX > bank_box_left) &&
                        (window->MouseX < bank_box_right)) {
                        bank_mouseover(window->MouseY - bank_box_top);
                    } else if (current_bank != 0xff) {
                        bank_mouseover(bank_box_bottom);
                    }
#endif
                    if ((msg->MouseX > SCREEN_WIDTH / 3) |
                        (msg->MouseY > SCREEN_HEIGHT / 3)) {
                        /* Mouse movement disables automatic bank select */
#ifdef DEBUG_TIMEOUT
                        if (timeout_active)
                            update_status("Timeout disabled by mouse");
#endif
                        timeout_active = FALSE;
                    }
                    bank_update_names();
                    bank_update_timeout();
                    break;
                case IDCMP_GADGETDOWN:
                    switch (gad->GadgetID) {
                        case ID_POWERON_RADIO: {
                            uint prev = updated_poweron;
                            GT_GetGadgetAttrs(gad, NULL, NULL,
                                              GTMX_Active, (LONG) &temp,
                                              TAG_DONE);
                            info.bi_bank_poweron = temp;
                            updated_poweron = info.bi_bank_poweron !=
                                              info_saved.bi_bank_poweron;
                            if (updated_poweron != prev)
                                update_save_box();
                            break;
                        }
                        case ID_SWITCHTO_RADIO:
                            GT_GetGadgetAttrs(gad, NULL, NULL,
                                              GTMX_Active, (LONG) &temp,
                                              TAG_DONE);
                            bank_switchto = temp;
                            update_switch_box();
                            break;
                    }
                    break;
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                        case ID_LONGRESET_MINUS_0:
                        case ID_LONGRESET_MINUS_1:
                        case ID_LONGRESET_MINUS_2:
                        case ID_LONGRESET_MINUS_3:
                        case ID_LONGRESET_MINUS_4:
                        case ID_LONGRESET_MINUS_5:
                        case ID_LONGRESET_MINUS_6:
                        case ID_LONGRESET_MINUS_7: {
                            uint prev;
                            bank_longreset_change(gad->GadgetID -
                                                  ID_LONGRESET_MINUS_0, -1);
handle_longreset_change:
                            prev = updated_longreset;
                            updated_longreset =
                                    memcmp(info.bi_longreset_seq,
                                           info_saved.bi_longreset_seq,
                                           sizeof (info.bi_longreset_seq));
                            if (updated_longreset != prev)
                                update_save_box();
                            show_bank_table_column(3);
                            break;
                        }
                        case ID_LONGRESET_PLUS_0:
                        case ID_LONGRESET_PLUS_1:
                        case ID_LONGRESET_PLUS_2:
                        case ID_LONGRESET_PLUS_3:
                        case ID_LONGRESET_PLUS_4:
                        case ID_LONGRESET_PLUS_5:
                        case ID_LONGRESET_PLUS_6:
                        case ID_LONGRESET_PLUS_7:
                            bank_longreset_change(gad->GadgetID -
                                                  ID_LONGRESET_PLUS_0, 1);
                            goto handle_longreset_change;
                            break;
                        case ID_CANCEL:
                            ColdReboot();
                            break;
                        case ID_SAVE:
id_save:
                            bank_state_save();
                            update_save_box();
                            break;
                        case ID_SWITCH:
                            /* Request update Current & Reset */
                            bank_set_current_and_reboot();
                            break;
                        case ID_BANK_TIMEOUT:
                            break;
                        case ID_BANK_DEFAULT:
                            break;
                        case ID_BOARD_NAME:
                            if (msg->Qualifier == 0x8001) {
                                /*
                                 * Shift-tab from board name
                                 *
                                 * It would be desirable to deactivate the
                                 * current gadget (gadget_banktable_name[7]),
                                 * but I can't figure out a way to do this.
                                 * I've tried various methods and all have
                                 * failed.
                                 */
//                              printf("switchto 1\n");
                            }
                            break;
                        case ID_BANK_NAME_7:
                            if (msg->Qualifier == 0x8000) {
                                /*
                                 * Tab from last bank name
                                 */
//                              printf("switchto 2\n");
                            }
                            break;
                    }
                    break;
            }
        }
    }
}

/*
 * cleanup_screen
 * --------------
 * Deallocate screen structures and gadgets
 */
static void
cleanup_screen(void)
{
    CloseWindow(window);
    FreeVisualInfo(visualInfo);
    CloseScreen(screen);
    FreeGadgets(gadgets);
    FreeRaster(tmpras.RasPtr, SCREEN_WIDTH, SCREEN_HEIGHT);
}

/*
 * sm_msg_copy_to_ram
 * ------------------
 * Move Kicksmash messaging code to RAM, since the code which talks with
 * KickSmash can't be executing from ROM.
 */
uint8_t *copy_to_ram_ptr;
static void
sm_msg_copy_to_ram(void)
{
    uint copy_to_ram_start;
    uint copy_to_ram_end;
    __asm("lea _copy_to_ram_start,%0" : "=a" (copy_to_ram_start) ::);
    __asm("lea _copy_to_ram_end,%0" : "=a" (copy_to_ram_end) ::);

    uint len = copy_to_ram_end - copy_to_ram_start;
//  printf("len=%x s=%x e=%x\n", len, copy_to_ram_start, copy_to_ram_end);
    copy_to_ram_ptr = AllocMem(len, MEMF_PUBLIC);
    if (copy_to_ram_ptr == NULL) {
        dputs("AllocMem fail 1\n");
        return;
    }
    memcpy(copy_to_ram_ptr, (void *) copy_to_ram_start, len);
    esend_cmd_core = (void *) (copy_to_ram_ptr +
                               (uintptr_t) send_cmd_core - copy_to_ram_start);
}

/*
 * main_func
 * ---------
 * Handle normal program initialization, screen draw, main loop, and cleanup
 */
#ifdef STANDALONE
void
#else
static void
#endif
main_func(void)
{
#ifndef STANDALONE
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop
#endif

#undef SERIAL_DEBUG
#if defined(SERIAL_DEBUG) || defined(DEBUG_IDCMP)
    flag_output = 2;
    flag_debug = 1;
#endif

#ifndef STANDALONE
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 0);
    IntuitionBase = OpenLibrary("intuition.library", 0);

    InitResident(FindResident("gadtools.library"), 0);
    GadToolsBase = OpenLibrary("gadtools.library", 36);

    extern struct Library * __UtilityBase;
    __UtilityBase = OpenLibrary(UTILITYNAME, 0);

    /* malloc/free() library Constructors / Destructors */
    void __ctor_stdlib_memory_init(void *);
    void __dtor_stdlib_memory_exit(void *);
    __ctor_stdlib_memory_init(SysBase);
#endif

    cpu_control_init();
    sm_msg_copy_to_ram();

    init_screen();
    draw_page();
    event_loop();
    cleanup_screen();

#ifndef STANDALONE
    CloseLibrary(IntuitionBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary(GadToolsBase);
#endif
}

#ifndef STANDALONE
/*
 * rom_main
 * --------
 * Entry for ROM module, called by exec startup code.
 * The a6 register is SysBase
 */
void
rom_main(struct ExecBase *SysBase asm("a6"))
{
    uint8_t *globals;

#undef DEBUG_ENTRY
#ifdef DEBUG_ENTRY
    *ADDR32(0x07770040) = (uintptr_t) rom_main;
    *ADDR32(0x07770044) = (uintptr_t) SysBase;
#endif

    void *data_start;
    uint  data_size;
    uint  bss_size;
    __asm("lea __sdata,%0"      : "=a" (data_start) ::);
    __asm("lea ___data_size,%0" : "=a" (data_size) ::);
    __asm("lea ___bss_size,%0"  : "=a" (bss_size) ::);

    if ((globals = AllocVec(data_size + bss_size, MEMF_PUBLIC)) == NULL) {
        dputs("AllocMem fail 2\n");
        goto rom_main_end;
    }

    /* Must compile with -fbaserel so a4 will be used for globals */
    memcpy(globals, data_start, data_size);
    memset(globals + data_size, 0, bss_size);

#undef DEBUG_ENTRY
#ifdef DEBUG_ENTRY
    dputs("globals=");
    dputx((uintptr_t) globals);
    dputs(" data=");
    dputx((uintptr_t) data_size);
    dputs(" bss=");
    dputx((uintptr_t) bss_size);
    dputs("\n");
    void *bss_start;
    __asm("lea __bss_start,%0" : "=a" (bss_start) ::);
    dputs("romdata=");
    dputx((uintptr_t) data_start);
    dputs(" rombss=");
    dputx((uintptr_t) bss_start);
    dputs("\n");
    dputs("sprintf=");
    dputx((uintptr_t) sprintf);
    dputs("\n");
#endif

    globals += 0x7ffe;  // offset that gcc applies to a4-relative globals
    __asm("move.l %0,a4" :: "r" (globals));
#ifndef STANDALONE
    globals_base = globals;
#endif

    /* Globals are now available */
    main_func();

rom_main_end: ;  // semicolon needed for gcc 6.5
}
#endif
