/*
 * Start-up code and board initialization.
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
#include "cache.h"
#include "keyboard.h"
#include "serial.h"
#include "med_cmdline.h"
#include "med_readline.h"
#include "cpu_control.h"
#include "mouse.h"
#include "autoconfig.h"
#include "testdraw.h"
#include "testgadget.h"
#include "printf.h"
#include "screen.h"
#include "audio.h"
#include "sprite.h"
#include "timer.h"
#include "util.h"
#include "vectors.h"
#include "main.h"

/*
 * Memory map
 *    0x00000100     [0x4] pointer to globals
 *    0x00000180    [0x26] register save area
 *    0x00000200   [0x100] vectors
 *    0x00001000    [0x80] runtime counters
 *    0x00001080    [0x80] sprite data
 *    0x00001100  [0xff00] stack
 *    0x00010000 [0x10000] bsschip
 *    0x00020000  [0x5000] bitplane 0
 *    0x00025000  [0x5000] bitplane 1
 *    0x0002a000  [0x5000] bitplane 2
 *    0x00030000 [0x10000] globals
 */

const char RomID[] = "ROM Switcher "VERSION" (" BUILD_DATE" "BUILD_TIME ")\n";

#define VECTORS_BASE (RAM_BASE + 0x200)
#define COUNTER0     (RAM_BASE + 0x1000)
#define COUNTER1     (RAM_BASE + 0x1004)
#define COUNTER2     (RAM_BASE + 0x1008)
#define COUNTER3     (RAM_BASE + 0x100c)
#define STACK_BASE   (RAM_BASE + 0x10000 - 4)
#define GLOBALS_BASE (RAM_BASE + 0x30000)

__attribute__ ((section (".reset")))
void
reset(void)
{
    __asm("dc.w 0x1114");
    __asm("jmp _reset_hi");    // dc.w 0x4ef9  dc.l 0x00f80010
    __asm("nop");
}

void
chipset_init_early(void)
{
    /* Shut down interrupts and DMA */
    *CIAA_ICR = 0x7f;    // Disable interrupt forwarding to chipset
    *INTENA   = 0x7fff;  // Disable interrupt forwarding to m68k
    *INTREQ   = 0x7fff;  // Reply to all interrupt requests
    *INTREQ   = 0x7fff;  // Reply to all interrupt requests (A4000 bug)
    *DMACON   = 0x7fff;  // Disable all chipset DMA

    /* Stop timers */
    *CIAA_CRA  = 0x00;
    *CIAA_CRB  = 0x00;
    *CIAA_CRA  = 0;

    /* Silence audio */
    *AUD0VOL  = 0;
    *AUD1VOL  = 0;
    *AUD2VOL  = 0;
    *AUD3VOL  = 0;
}

void
chipset_init(void)
{
    /* Ramsey config */
//  *RAMSEY_CONTROL = RAMSEY_CONTROL_REFRESH0;  // Clobber burst/page/wrap

    /* Re-enable CIA interrupts (keyboard) */
    *INTENA   = INTENA_SETCLR |  // Set
                INTENA_INTEN |   // Enable interrupts
                INTENA_PORTS;    // CIA-A

    /* Enable interrupts for keyboard input */
    *CIAA_ICR = CIA_ICR_SET | CIA_ICR_SP;  // Serial Port input from keyboard
//  *CIAA_ICR = CIA_ICR_SET | CIA_ICR_TA;
}

void
globals_init(void)
{
    void *data_start;
    uint  data_size;
    uint  bss_size;
    __asm("lea __sdata_rom,%0"  : "=a" (data_start) ::);
    __asm("lea ___data_size,%0" : "=a" (data_size) ::);
    __asm("lea ___bss_size,%0"  : "=a" (bss_size) ::);

    /* Set up globals (compile with -fbaserel for a4 to be globals pointer) */
    uint8_t *globals = (uint8_t *) (GLOBALS_BASE);  // Globals begin at 192K

    memcpy(globals, data_start, data_size);
    memset(globals + data_size, 0, bss_size);

    globals += 0x7ffe;  // offset that gcc applies to a4-relative globals
    __asm("move.l %0,a4" :: "r" (globals));  // set up globals pointer
    __asm("move.l a4,0x100");  // save globals pointer at fixed location
}

__attribute__ ((section (".reset_hi")))
void
reset_hi(void)
{
    const uint stack_base = STACK_BASE;

    /* Delay for hardware init to complete */
    __asm("move.l #0x20000, d0 \n"
          "reset_loop: \n"
          "dbra d0, reset_loop");

    /* Set up stack in low 64K of chipmem */
    __asm("move.l %0, sp" :: "r" (stack_base));

    /* Turn off ROM overlay (OVL) and make LED go bright */
    __asm("move.b #3, 0xbfe201 \n"  // Set CIA A DRA bit 0 and 1 as output
          "move.b #2, 0xbfe001");   // Set CIA A PRA bit 0=OVL, bit 1=LED

    __asm("jmp _setup");  // setup()
}

void
main_poll()
{
    cmdline();
    mouse_poll();     // handle mouse buttons
    keyboard_poll();  // handle key repeats
}

void __attribute__ ((noinline))
setup(void)
{
    globals_init();
    vectors_init((void *)VECTORS_BASE);
    memset(ADDR8(0), 0xa5, 0x100);  // Help catch NULL pointer usage
    chipset_init_early();
    cpu_control_init();  // Get CPU type
    serial_init();
    serial_puts("\n\033[31m");
    serial_puts(RomID);
    serial_puts("\033[0m\n");

    cache_init();        // Enable cache
    serial_putc('A');
    chipset_init();
    serial_putc('B');
    screen_init();
    serial_putc('C');
//  dbg_show_string(RomID);

    timer_init();
    serial_putc('D');
    audio_init();
//  serial_putc('E');
//  serial_init();  // Now that ECLK is known
    serial_putc('F');
    keyboard_init();
    serial_putc('G');
    mouse_init();
    serial_putc('H');
    sprite_init();
    serial_putc('I');
    autoconfig_init();
    serial_putc('J');

    gui_wants_all_input = 1;
    rl_initialize();
    using_history();
    serial_putc('K');
    test_draw();
    test_gadget();
    serial_putc('\n');
#ifdef STANDALONE
    extern void main_func(void);
    main_func();
#endif
    while (1) {
        main_poll();
    }
#if 0
    *COLOR00 = 0x0f0c;  // Purple
    __asm("halt");
#endif
}

void
debug_cmdline(void)
{
    /* Set up globals (compile with -fbaserel for a4 to be globals pointer) */
    uint8_t *globals = (uint8_t *) (GLOBALS_BASE);  // Globals begin at 192K
    globals += 0x7ffe;  // offset that gcc applies to a4-relative globals
    __asm("move.l %0,a4" :: "r" (globals));  // set up globals pointer
    __asm("move.l a4,0x100");  // save globals pointer at fixed location

//  globals_init();

    /* Activate MED cmdline */
    gui_wants_all_input = 0;  // Turns off GUI stealing input
    cursor_visible |= 2;
    dbg_all_scroll = 25;
    dbg_cursor_y = 25;

    rl_initialize();
    using_history();
    while (1) {
        main_poll();
    }
}
