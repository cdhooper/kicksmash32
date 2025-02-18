/*
 * CPU interrupt vectors and handlers.
 *
 * This source file is part of the code base for a simple Amiga ROM
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
#include <stdint.h>
#include <string.h>
#include "amiga_chipset.h"
#include "util.h"
#include "audio.h"
#include "serial.h"
#include "vectors.h"
#include "med_cmdline.h"
#include "mouse.h"
#include "reset.h"
#include "screen.h"
#include "sprite.h"
#include "timer.h"
#include "keyboard.h"
#include "printf.h"

/*
 * Memory map
 *    0x00000000   [0x100] vectors
 *    0x00000100     [0x4] pointer to globals
 *    0x00001000    [0x80] runtime counters
 *    0x00001080    [0x80] sprite data
 *    0x00001100  [0xff00] stack
 *    0x00010000 [0x10000] globals
 *    0x00020000  [0x5000] bitplane 0
 *    0x00025000  [0x5000] bitplane 1
 *    0x0002a000  [0x5000] bitplane 2
 */

#define COUNTER0     (RAM_BASE + 0x1000)
#define COUNTER1     (RAM_BASE + 0x1004)
#define COUNTER2     (RAM_BASE + 0x1008)
#define COUNTER3     (RAM_BASE + 0x100c)
#define STACK_BASE   (RAM_BASE + 0x10000 - 4)
#define GLOBALS_BASE (RAM_BASE + 0x10000)

uint vblank_ints;

void Default(void);
void reset_hi(void);

#define ChkInst Default  // CHK instruction resulted in "out of bounds"
#define Trace   Default  // Instruction Trace step

__attribute__ ((interrupt)) void
Audio(void)
{
    /* Clear audio interrupts */
    *INTREQ = INTREQ_AUD0 | INTREQ_AUD1 | INTREQ_AUD2 | INTREQ_AUD3;

    SAVE_A4();
    GET_GLOBALS_PTR();
    audio_handler();
    RESTORE_A4();
    (*ADDR32(COUNTER0))++;  // counter
}

__attribute__ ((interrupt)) void
Blitter(void)
{
    *INTREQ = INTREQ_BLIT;
    (*ADDR32(COUNTER1))++;  // counter
}

void
Ports_poll(void)
{
}

__attribute__ ((interrupt)) void
Ports(void)
{
    uint8_t st;
    *INTREQ  = INTREQ_PORTS;  // clear interrupt
//  *INTENA  = INTENA_PORTS;  // disable interrupt
    st = *CIAA_ICR;

    /*
     * If additional interrupts are handled by this routine in the
     * future, keyboard_irq() will need to change because it's
     * greedy with spin loops.
     */
    if (st & CIA_ICR_TA) {
        *CIAA_ICR = CIA_ICR_TA;  // Disable Timer interrupt
        *COLOR00 = 0x880;  // Yellow background !YAY DEBUG!
    }
    if (st & CIA_ICR_SP) {
        /* Keyboard serial input */
        keyboard_irq();
    }
    (*ADDR32(COUNTER2))++;  // counter
}

/* Address Error (misaligned) */
__attribute__ ((interrupt)) void
AddrErr(void)
{
    serial_puts("Address Error\n");
    reset_cpu();
}

/* Bus Cycle timeout or failure */
__attribute__ ((interrupt)) void
BusErr(void)
{
    serial_puts("Bus Error\n");
    reset_cpu();
}

/* Illegal Instruction */
__attribute__ ((interrupt)) void
IllInst(void)
{
    serial_puts("Illegal Instruction\n");
    reset_cpu();
}

/* Division by Zero */
__attribute__ ((interrupt)) void
DivZero(void)
{
    serial_puts("Division by Zero\n");
    reset_cpu();
}

/* TRAPV with overflow flag set */
__attribute__ ((interrupt)) void
TrapV(void)
{
    serial_puts("TrapV\n");
    reset_cpu();
}

/* Privilege Violation */
__attribute__ ((interrupt)) void
PrivVio(void)
{
    serial_puts("Privilege Violation\n");
    reset_cpu();
}

/* Unimplemented Instruction (line A) */
__attribute__ ((interrupt)) void
ExLineA(void)
{
    serial_puts("Unimplemented Instruction (line A)\n");
    reset_cpu();
}

/* Unimplemented Instruction (line F) */
__attribute__ ((interrupt)) void
ExLineF(void)
{
    serial_puts("Unimplemented Instruction (line F)\n");
    reset_cpu();
}

/* Spurious IRQ */
__attribute__ ((interrupt)) void
SpurIRQ(void)
{
    serial_puts("Spurious IRQ\n");
    reset_cpu();
}

__attribute__ ((interrupt)) void
VBlank(void)
{
    static uint16_t mouse_quad_last;
    uint16_t mouse_quad_cur;

    SAVE_A4();
    GET_GLOBALS_PTR();

    /*
     * Reset bitplane DMA pointers. This could also be done by the copper.
     *
     *   AddrPlanexH = address of bit plane x, bits 16-18
     *   AddrPlanexL = address of bit plane x, bits 0-15
     *   MOVE #AddrPlanelH,BPLlPTH initialize pointer to bit plane 1
     *   MOVE #AddrPlanelL,BPLlPTL
     *   MOVE #AddrPlane2H,BPLlPTH initialize pointer to bit plane 2
     *   MOVE #AddrPlane2L,BPLlPTL
     *   MOVE #AddrPlane3H,BPLlPTH initialize pointer to bit plane 3
     *   MOVE #AddrPlane3L,BPLlPTL
     *   MOVE #AddrPlane4H,BPLlPTH initialize pointer to bit plane 4
     *   MOVE #AddrPlane4L,BPLlPTL
     *   WAIT ($FF,$FE)
     *   ;end of the Copper list (wait for an impossible screen position)
     */
    *BPL1PT  = BITPLANE_0_BASE;  // Bitplane 0 base address
    *BPL2PT  = BITPLANE_1_BASE;  // Bitplane 1 base address
    *BPL3PT  = BITPLANE_2_BASE;  // Bitplane 2 base address

    *INTREQ = INTREQ_VERTB;
    (*ADDR32(COUNTER3))++;  // counter

    uint32_t sr = irq_disable();
    uint16_t cur = eclk_ticks();
    uint16_t diff = eclk_last_update - cur;
    timer_tick_base += diff;
    eclk_last_update = cur;
    irq_restore(sr);

    int8_t move_x;
    int8_t move_y;
    mouse_quad_cur = *VADDR16(JOY0DAT);  // mouse X and Y counters
    move_x = (mouse_quad_cur & 0xff) - (mouse_quad_last & 0xff);
    move_y = (mouse_quad_cur >> 8) - (mouse_quad_last >> 8);
    mouse_x += move_x * 2;
    mouse_y += move_y;
    if (mouse_x < 0)
        mouse_x = 0;
    if (mouse_x > SCREEN_WIDTH - 1)
        mouse_x = SCREEN_WIDTH - 1;
    if (mouse_y < 0)
        mouse_y = 0;
    if (mouse_y > SCREEN_HEIGHT + 8)
        mouse_y = SCREEN_HEIGHT + 8;
    mouse_quad_last = mouse_quad_cur;

    /*
     * The first 32-bit word of the sprite data:
     *     Bit 31-24  Bits 0-7 of VSTART
     *     Bit 16-23  Bits 1-8 of HSTART
     *     Bit 15-8   Bits 0-7 of VSTOP
     *     Bit 7      Attach this odd # sprite to previous even # sprite
     *     Bit 6-3    Unused
     *     Bit 2      Bit 8 of VSTART
     *     Bit 1      Bit 8 of VSTOP
     *     Bit 0      Bit 0 of HSTART
     */

    /* Position mouse pointer */
    uint x_start = mouse_x / 2 + 0x80;  // Sprite X position is lowres
    uint y_start = mouse_y + 0x2c;
    uint y_end   = y_start + 9;

    /* Mouse pointer */
    if (sprite0_data != NULL) {
        sprite0_data[0] = sprite_calcpos(x_start, y_start, y_end);

        /* Position cursor */
        if (cursor_visible) {
            if (cursor_visible == 1) {
                x_start = cursor_x_start / 2 + cursor_x * 4 + 0x80;
                y_start = cursor_y_start + cursor_y * 8 + 0x2c;
            } else {
                x_start = dbg_cursor_x * 4 + 0x80;
                y_start = dbg_cursor_y * 8 + 0x2c;
            }
            y_end = y_start + 8;
            sprite1_data[0] = sprite_calcpos(x_start, y_start, y_end);
        } else {
            sprite1_data[0] = 0x00000000;
        }

        *SPR0PTH = (uintptr_t) sprite0_data;
        *SPR1PTH = (uintptr_t) spritex_data;
        *SPR2PTH = (uintptr_t) sprite1_data;
        *SPR3PTH = (uintptr_t) spritex_data;
        *SPR4PTH = (uintptr_t) spritex_data;
        *SPR5PTH = (uintptr_t) spritex_data;
        *SPR6PTH = (uintptr_t) spritex_data;
        *SPR7PTH = (uintptr_t) spritex_data;
    }

    if (vblank_ints++ > 240) {
        extern uint8_t serial_active;
        uint32_t x;
        uint32_t *sp = &x;
        vblank_ints = 0;
        serial_active = 1;
        printf("\nStuck?\n");
        for (x = 0; x < 30; x++) {
            printf(" %08x", *(sp++));
            if ((x & 7) == 7)
                printf("\n");
        }
        printf("\n");
    }

    RESTORE_A4();
}

/*
 * Null copper list
 *
 * cp = null_mode_copper_list = alloc_chipmem(sizeof(cop_t) * 4);
 * CMOVE(cp, R_COLOR00, 0x0000);   // background is black
 * CMOVE(cp, R_BPLCON0, 0x0000);   // no planes to fetch from
 * CWAIT(cp, 255, 255);    // COPEND
 * CWAIT(cp, 255, 255);    // COPEND really
 *
 * // install this list and turn DMA on
 * custom.cop1lc = PREP_DMA_MEM(null_mode_copper_list);
 * custom.copjmp1 = 0;
 * custom.dmacon = DMAF_SETCLR | DMAF_MASTER | DMAF_RASTER | DMAF_COPPER;
 */


__asm("_Default: RTE");

__attribute__ ((section (".text")))
const void *vectors[] =
{
    (void *)0x80000,
            reset_hi, BusErr,  AddrErr, IllInst, DivZero, ChkInst, TrapV,
    PrivVio, Trace,   ExLineA, ExLineF, Default, Default, Default, Default,
    Default, Default, Default, Default, Default, Default, Default, Default,
    SpurIRQ, Default, Ports,   VBlank,  Audio,   Default, Default, Default,
    Default, Default, Default, Default, Default, Default, Default, Default,
    Default, Default, Default, Default, Default, Default, Default, Default,
    Default, Default, Default, Default, Default, Default, Default, Default,
    Default, Default, Default, Default, Default, Default, Default, Default,
};

void
vectors_init(void *base)
{
    memcpy(base, vectors, sizeof (vectors));

    __asm("movec %0,VBR" :: "r" (base));  // Set up vector base

    /* Enable interrupts and stay in supervisor mode */
    irq_enable();
//  __asm("move.w #0x2000, SR");
}
