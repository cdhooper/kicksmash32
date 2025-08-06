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
#include "main.h"

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

#define FULL_STACK_REGS 0x180
#define SAVE_FULL_FRAME() __asm("movem.l d0-d7/a0-a7,0x180\n\t" \
                                "move.w 0(sp),0x1c0\n\t" \
                                "move.l 2(sp),0x1c2")

typedef struct
__attribute__((packed)) {
    uint32_t d[6];
    uint32_t a[5];
    uint16_t sr;
    uint32_t pc;
} vblank_stack_regs_t;

typedef struct
__attribute__((packed)) {
    uint32_t d[8];
    uint32_t a[8];
    uint16_t sr;
    uint32_t pc;
} full_stack_regs_t;

uint vblank_ints;

static void Default(void);
void reset_hi(void);
__attribute__((noinline)) static void irq_debugger(uint32_t sp_reg, uint mode);

__attribute__((noinline))
static void
irq_debugger_msg(const char *msg)
{
    full_stack_regs_t *regs = (void *)(uintptr_t) FULL_STACK_REGS;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
    memcpy(regs + 1, regs, sizeof (*regs));
    printf(msg);
    irq_debugger(0, 1);
}

__attribute__ ((noinline)) static void
Unknown_common(uint intnum)
{
    char buf[40];
    sprintf(buf, "\nUnknown interrupt %u", intnum);
    irq_debugger_msg(buf);
    reset_cpu();
}

__attribute__ ((interrupt)) static void
Default(void)
{
    Unknown_common(0);
}

__attribute__ ((interrupt)) void
Audio(void)
{
    /* Clear audio interrupts */
    *INTREQ = INTREQ_AUD0 | INTREQ_AUD1 | INTREQ_AUD2 | INTREQ_AUD3;
//  *INTENA = INTENA_AUD0 | INTENA_AUD1 | INTENA_AUD2 | INTENA_AUD3; // Disable

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
__attribute__ ((interrupt)) static void
AddrErr(void)
{
    irq_debugger_msg("Address Error\n");
    reset_cpu();
}

/* Bus Cycle timeout or failure */
__attribute__ ((interrupt)) static void
BusErr(void)
{
    irq_debugger_msg("Bus Error\n");
    reset_cpu();
}

/* Illegal Instruction */
__attribute__ ((interrupt)) static void
IllInst(void)
{
    irq_debugger_msg("Illegal Instruction\n");
    reset_cpu();
}

/* Division by Zero */
__attribute__ ((interrupt)) static void
DivZero(void)
{
    irq_debugger_msg("Division by Zero\n");
    reset_cpu();
}

/* TRAPV with overflow flag set */
__attribute__ ((interrupt)) static void
TrapV(void)
{
    irq_debugger_msg("TrapV\n");
    reset_cpu();
}

/* Privilege Violation */
__attribute__ ((interrupt)) static void
PrivVio(void)
{
    irq_debugger_msg("Privilege Violation\n");
    reset_cpu();
}

/* Unimplemented Instruction (line A) */
__attribute__ ((interrupt)) static void
ExLineA(void)
{
    irq_debugger_msg("Unimplemented Instruction (line A)\n");
    reset_cpu();
}

/* Unimplemented Instruction (line F) */
__attribute__ ((interrupt)) static void
ExLineF(void)
{
    irq_debugger_msg("Unimplemented Instruction (line F)\n");
    reset_cpu();
}

/* Check Instruction */
__attribute__ ((interrupt)) static void
ChkInst(void)
{
    irq_debugger_msg("Check Instruction\n");
    reset_cpu();
}

/* Instruction Trace */
__attribute__ ((interrupt)) static void
Trace(void)
{
    irq_debugger_msg("Instruction Trace\n");
    reset_cpu();
}

/* Spurious IRQ */
__attribute__ ((interrupt)) static void
SpurIRQ(void)
{
    irq_debugger_msg("Spurious IRQ\n");
    reset_cpu();
}

/* Coprocessor Error */
__attribute__ ((interrupt)) static void
CopErr(void)
{
    irq_debugger_msg("Coprocessor Error\n");
    reset_cpu();
}

/* Format Error */
__attribute__ ((interrupt)) static void
FmtErr(void)
{
    irq_debugger_msg("Format Error\n");
    reset_cpu();
}

/* Uninitialized Interrupt */
__attribute__ ((interrupt)) static void
UninitI(void)
{
    irq_debugger_msg("Uninitialized Interrupt\n");
    reset_cpu();
}

__attribute__ ((interrupt)) static void
VBlank(void)
{
#define VBLANK_FULL_DEBUG
    SAVE_A4();
    GET_GLOBALS_PTR();

    static uint16_t mouse_quad_last;
    uint16_t mouse_quad_cur;
#ifndef VBLANK_FULL_DEBUG
    uint32_t local_sp = get_sp();
#endif

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
    *BPL1PT = BITPLANE_0_BASE;  // Bitplane 0 base address
    *BPL2PT = BITPLANE_1_BASE;  // Bitplane 1 base address
    *BPL3PT = BITPLANE_2_BASE;  // Bitplane 2 base address

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

    if (vblank_ints++ > 120) {  // 2 seconds
#ifdef VBLANK_FULL_DEBUG
        irq_debugger_msg("\nStuck?");
#else
        printf("\nStuck?");
        irq_debugger(local_sp, 2);
#endif
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

/* Int29() is Amiga interrupt L5 (Serial RBF, DSKSYNC) */
__attribute__ ((interrupt)) void
Int29(void)
{
    serial_poll();
}

#if 0
/*
 * Int6() handles interrupts from CIAB, among other CPU INT6 sources
 */
__attribute__ ((interrupt)) void
Int6(void)
{
    irq_debugger_msg("Int6\n");
    reset_cpu();
}

__attribute__ ((interrupt)) void
Int7(void)
{
    irq_debugger_msg("Int7\n");
    reset_cpu();
}
#endif

__attribute__ ((interrupt)) static void Int12(void) { Unknown_common(12); }
__attribute__ ((interrupt)) static void Int16(void) { Unknown_common(16); }
__attribute__ ((interrupt)) static void Int17(void) { Unknown_common(17); }
__attribute__ ((interrupt)) static void Int18(void) { Unknown_common(18); }
__attribute__ ((interrupt)) static void Int19(void) { Unknown_common(19); }
__attribute__ ((interrupt)) static void Int20(void) { Unknown_common(20); }
__attribute__ ((interrupt)) static void Int21(void) { Unknown_common(21); }
__attribute__ ((interrupt)) static void Int22(void) { Unknown_common(22); }
__attribute__ ((interrupt)) static void Int23(void) { Unknown_common(23); }
__attribute__ ((interrupt)) static void Int25(void) { Unknown_common(25); }
__attribute__ ((interrupt)) static void Int30(void) { Unknown_common(30); }
__attribute__ ((interrupt)) static void Int31(void) { Unknown_common(31); }

#define VECTOR_WRAP(func) void _##func(void) { SAVE_FULL_FRAME(); func(); }
#define VECTOR(func) _##func

VECTOR_WRAP(Audio);
VECTOR_WRAP(VBlank);
VECTOR_WRAP(BusErr);
VECTOR_WRAP(AddrErr);
VECTOR_WRAP(IllInst);
VECTOR_WRAP(DivZero);
VECTOR_WRAP(ChkInst);
VECTOR_WRAP(TrapV);
VECTOR_WRAP(PrivVio);
VECTOR_WRAP(Trace);
VECTOR_WRAP(ExLineA);
VECTOR_WRAP(ExLineF);
VECTOR_WRAP(Int12);
VECTOR_WRAP(CopErr);
VECTOR_WRAP(FmtErr);
VECTOR_WRAP(UninitI);
VECTOR_WRAP(Int16);
VECTOR_WRAP(Int17);
VECTOR_WRAP(Int18);
VECTOR_WRAP(Int19);
VECTOR_WRAP(Int20);
VECTOR_WRAP(Int21);
VECTOR_WRAP(Int22);
VECTOR_WRAP(Int23);
VECTOR_WRAP(SpurIRQ);
VECTOR_WRAP(Int25);
VECTOR_WRAP(Ports);
VECTOR_WRAP(Int29);
VECTOR_WRAP(Int30);
VECTOR_WRAP(Int31);
VECTOR_WRAP(Default);

/*
 *  Vector Address Function  Description
 *  0      0                 Reset initial SP
 *  1      4       reset_hi  Reset initial PC
 *  2      8       BusErr    Bus Error
 *  3      c       AddrErr   Address Error
 *  4      10      IllInst   Illegal Instruction
 *  5      14      DivZero   Divide by Zero
 *  6      18      ChkInst   Check Instruction (CHK, CHK2)
 *  7      1c      TrapV     Trap Vector (cpTRAPcc, TRAPcc, TRAPV)
 *  8      20      PrivVio   Privilege Violation
 *  9      24      Trace     Instruction Trace
 *  10     28      ExLineA   Unimplemented Instruction (FPU line A)
 *  11     2c      ExLineF   Unimplemented Instruction (FPU line F)
 *  12     30      ?         Unassigned
 *  13     34      CopErr    Coprocessor Protocol Violation
 *  14     38      FmtErr    Format Error
 *  15     3c      UninitI   Uninitialized Interrupt
 *  ...                      Unassigned / reserved
 *  24     60      SpurIRQ   Spurious Interrupt (TBE)
 *  25     64                L1 (DSKBLK, SOFTINT)
 *  26     68      Ports     L2 (CIA-A, Zorro, onboard SCSI)
 *  27     6c      VBlank    L3 (VERTB, COPER, BLIT)
 *  28     70      Audio     L4 (AUD0, AUD1, AUD2, AUD3)
 *  29     74                L5 (Serial RBF, DSKSYNC)
 *  30     78      Int6      L6 (EXTER / INTEN, CIA-B)
 *  31     7c                L7 NMI
 *  32     80                Trap #0
 *  ...                      Traps #1..#14
 *  47     bc                Trap #15
 *  48     c0                FPCP Branch or Set on Unordered Condition
 *  49     c4                FPCP Inexact Result
 *  50     c8                FPCP Divide by Zero
 *  51     cc                FPCP Underflow
 *  52     d0                FPCP Operand Error
 *  53     d4                FPCP Overflow
 *  54     d8                FPCP Signaling NAN
 *  55     dc                Unassigned / reserved
 *  56     e0                MMU Configuration Error
 *  57     e4                MC688851-specific
 *  58     e8                MC688851-specific
 *  ...                      Unassigned / reserved
 *  64     100               User Defined Vector #0
 *  ...
 *  255    3fc               User Defined Vector #191
 */
#define INITSP (void *)0x80000

__attribute__ ((section (".text")))
const void *vectors[] =
{
    INITSP,          reset_hi,        VECTOR(BusErr),  VECTOR(AddrErr),
    VECTOR(IllInst), VECTOR(DivZero), VECTOR(ChkInst), VECTOR(TrapV),
    VECTOR(PrivVio), VECTOR(Trace),   VECTOR(ExLineA), VECTOR(ExLineF),
    VECTOR(Int12),   VECTOR(CopErr),  VECTOR(FmtErr),  VECTOR(UninitI),
    VECTOR(Int16),   VECTOR(Int17),   VECTOR(Int18),   VECTOR(Int19),
    VECTOR(Int20),   VECTOR(Int21),   VECTOR(Int22),   VECTOR(Int23),
    VECTOR(SpurIRQ), VECTOR(Int25),   VECTOR(Ports),   VECTOR(VBlank),
    VECTOR(Audio),   VECTOR(Int29),   VECTOR(Int30),   VECTOR(Int31),
    VECTOR(Default), VECTOR(Default), VECTOR(Default), VECTOR(Default),
    VECTOR(Default), VECTOR(Default), VECTOR(Default), VECTOR(Default),
    VECTOR(Default), VECTOR(Default), VECTOR(Default), VECTOR(Default),
    VECTOR(Default), VECTOR(Default), VECTOR(Default), VECTOR(Default),
    VECTOR(Default), VECTOR(Default), VECTOR(Default), VECTOR(Default),
    VECTOR(Default), VECTOR(Default), VECTOR(Default), VECTOR(Default),
    VECTOR(Default), VECTOR(Default), VECTOR(Default), VECTOR(Default),
    VECTOR(Default), VECTOR(Default), VECTOR(Default), VECTOR(Default),
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

__attribute__ ((interrupt))
__attribute__((noinline))
static void
irq_debugger(uint32_t sp_reg, uint mode)
{
    uint x;
    uint32_t *sp;
    extern uint8_t serial_active;
    uint reg;

    SAVE_A4();
    GET_GLOBALS_PTR();

    vblank_ints = 0;
    serial_active = 1;

    if ((mode == 0) || (mode == 1)) {
        full_stack_regs_t *regs = (void *)(uintptr_t) FULL_STACK_REGS;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
        if (mode == 0)
            memcpy(regs + 1, regs, sizeof (*regs));
        regs++;
#if 0
        if ((regs->a[7] > 0x01000) && (regs->a[7] < 0x10000)) {
            /* Fixup A7 past local variables */
            sp_reg = regs->a[7] + 10 * sizeof (uint16_t);
            regs->sr = *ADDR16(sp_reg);
            regs->pc = *ADDR32(sp_reg + sizeof (uint16_t));
//          sp_reg += 3 * sizeof (uint16_t);
//          }
#else
        if ((regs->a[7] > 0x01000) && (regs->a[7] < 0x10000)) {
            /* Fixup A7 past SR and PC */
            sp_reg = regs->a[7] + 6;
#endif
        } else {
            sp_reg = get_sp();
        }
        sp = (void *)(uintptr_t) sp_reg;
        printf("  SP %08x  PC %08x  SR %02x\n",
               sp_reg, regs->pc, regs->sr);
        printf("  Ax ");
        for (reg = 0; reg < ARRAY_SIZE(regs->a); reg++) {
            if (reg != 0)
                printf(" ");
            printf("%08x", regs->a[reg]);
        }
        printf("\n  Dx ");
        for (reg = 0; reg < ARRAY_SIZE(regs->d); reg++) {
            if (reg != 0)
                printf(" ");
            printf("%08x", regs->d[reg]);
        }
#pragma GCC diagnostic pop
        printf("\n");
    } else if (sp_reg == 0) {
        sp_reg = get_sp();
        printf("  SP %08x", sp_reg);
        sp = (void *)(uintptr_t) (sp_reg);
    } else {
        vblank_stack_regs_t *regs = (void *)(uintptr_t) (sp_reg);
        sp = (void *) (regs + 1);
        printf("  SP %08x  PC %08x  SR %02x\n",
               sp_reg, regs->pc, regs->sr);
        printf("  Ax ");
        for (reg = 0; reg < ARRAY_SIZE(regs->a); reg++) {
            if (reg != 0)
                printf(" ");
            printf("%08x", regs->a[reg]);
        }
        printf("\n  Dx ");
        for (reg = 0; reg < ARRAY_SIZE(regs->d); reg++) {
            if (reg != 0)
                printf(" ");
            printf("%08x", regs->d[reg]);
        }
        printf("\n");
    }
    for (x = 0; x < 32; x++) {
        if ((x & 7) == 0)
            printf("\n ");
        printf(" %08x", *(sp++));
    }
    printf("\nForcing cmdline...\n");
    debug_cmdline();
    reset_cpu();

    RESTORE_A4();
}
