/*
 * Amiga reset handling.
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
#include "util.h"
#include "serial.h"
#include "amiga_chipset.h"

/*
 * reset instruction needs to be longword-aligned
 */
__attribute__ ((noinline))
__attribute__((optimize("align-functions=16")))
static void
do_reset(void)
{
    __asm("reset \n"
          "jmp _reset_hi \n"
          "nop");
}

void
reset_cpu(void)
{
    /*
     * Resetting Amiga hardware involves several steps to undo
     * configuration:
     * 1. Shut down chipset DMA and interrupts, set coldreboot
     * 2. Flush and disable caches
     * 3. Turn off MMU
     * 4. Enter supervisor state
     * 5. Reset and branch to reset vector (must be in same longword)
     */
    serial_flush();
    *CIAA_ICR = 0x7f;    // Disable interrupt forwarding to chipset
    *INTENA   = 0x7fff;  // Disable interrupt forwarding to m68k
    *INTREQ   = 0x7fff;  // Reply to all interrupt requests
    *INTREQ   = 0x7fff;  // Reply to all interrupt requests (A4000 bug)
    *DMACON   = 0x7fff;  // Disable all chipset DMA
    *BPLCON0  = 0x0000;  // Shut off bitplanes

    *COLDSTART |= BIT(7);  // Make it a coldstart

    do_reset();
}
