/*
 * Sprite functions.
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
#include "util.h"
#include "sprite.h"
#include "printf.h"
#include "amiga_chipset.h"

/*
 * Sprite data is actually a sequence of 16-bit values. The data
 * structure below is using 32-bit values because most values are
 * actually 32 bits.
 *
 * The first 32-bit word of the sprite data:
 *     Bit 31-24  Bits 0-7 of VSTART
 *     Bit 16-23  Bits 1-8 of HSTART
 *     Bit 15-8   Bits 0-7 of VSTOP
 *     Bit 7      Attach this odd number sprite to previous even number sprite
 *     Bit 6-3    Unused
 *     Bit 2      Bit 8 of VSTART
 *     Bit 1      Bit 8 of VSTOP
 *     Bit 0      Bit 0 of HSTART
 */
uint32_t *sprite0_data;
uint32_t *sprite1_data;
uint32_t *spritex_data;

uint
sprite_calcpos(uint x_start, uint y_start, uint y_end)
{
    return ((y_start << 24) |
            ((x_start >> 1) << 16) |
            ((y_end & 0xff) << 8) |
            ((y_start >> 6) & BIT(2)) |
            ((y_end >> 7) & BIT(1)) |
//          ((y_start & 0x100) >> 6) |
//          ((y_end & 0x100) >> 7) |
            (x_start & 1));
}

void
sprite_init(void)
{
    /*
     * 1. Create sprite data
     * 2. Set sprite pointers to sprite data
     * 3. Turn on sprite DMA
     * 4. Rewrite sprite pointers during vertical blanking
     *
     * Notes: y=2c-34 is top line of screen
     *        x=40 is top left corner
     */

    /*
     *      Mouse         White         Black
     * W W . . . . . .    11000000 c0   00000000 00
     * W B W . . . . .    10100000 a0   01000000 40
     * W B B W . . . .    10010000 90   01100000 60
     * W B B B W . . .    10001000 88   01110000 70
     * W B B W W W . .    10011100 9c   01100000 60
     * W W B B W . . .    11001000 c8   00110000 30
     * W . W B B W . .    10100100 a4   00011000 18
     * . . W B B W . .    00100100 24   00011000 18
     * . . . W W . . .    00011000 18   00000000 00
     */
    sprite0_data = (uint32_t *) 0x1080;  // chip RAM address
    sprite0_data[0] = 0x2c494000;  // HSTART, VSTART, VSTOP, control bits
    sprite0_data[1] = 0xc0000000;
    sprite0_data[2] = 0xa0004000;
    sprite0_data[3] = 0x90006000;
    sprite0_data[4] = 0x88007000;
    sprite0_data[5] = 0x9c006000;
    sprite0_data[6] = 0xc8003000;
    sprite0_data[7] = 0xa4001800;
    sprite0_data[8] = 0x24001800;
    sprite0_data[9] = 0x18000000;
    sprite0_data[10] = 0x00000000;

    /*
     *     Cursor
     * O O O O O O O O
     * O O O O O O O O
     * O O O O O O O O
     * O O O O O O O O
     * O O O O O O O O
     * O O O O O O O O
     * O O O O O O O O
     * O O O O O O O O
     * O O O O O O O O
     */
    sprite1_data = sprite0_data + 11;
    sprite1_data[0] = 0x2c403400;  // HSTART, VSTART, VSTOP, control bits
    sprite1_data[1] = 0xf000f000;
    sprite1_data[2] = 0xf000f000;
    sprite1_data[3] = 0xf000f000;
    sprite1_data[4] = 0xf000f000;
    sprite1_data[5] = 0xf000f000;
    sprite1_data[6] = 0xf000f000;
    sprite1_data[7] = 0xf000f000;
    sprite1_data[8] = 0xf000f000;  // next sprite usage (0x0000000 = last usage)
    sprite1_data[9] = 0x00000000;

    spritex_data = sprite1_data + 10;
    spritex_data[0] = 0x00000000;  // HSTART, VSTART, VSTOP, control bits
    spritex_data[1] = 0x00000000;
    spritex_data[2] = 0x00000000;
    spritex_data[3] = 0x00000000;
    spritex_data[4] = 0x00000000;
    spritex_data[5] = 0x00000000;
    spritex_data[6] = 0x00000000;
    spritex_data[7] = 0x00000000;
    spritex_data[8] = 0x00000000;
    spritex_data[9] = 0x00000000;  // next sprite usage (0x0000000 = last usage)

    *SPR0PTH = (uintptr_t) sprite0_data;
    *SPR1PTH = (uintptr_t) spritex_data;
    *SPR2PTH = (uintptr_t) sprite1_data;
    *SPR3PTH = (uintptr_t) spritex_data;
    *SPR4PTH = (uintptr_t) spritex_data;
    *SPR5PTH = (uintptr_t) spritex_data;
    *SPR6PTH = (uintptr_t) spritex_data;
    *SPR7PTH = (uintptr_t) spritex_data;
//  *SPR0POS = 0x2c20;
//  *SPR0CTL = 0x0800;
//  *SPR0DATA = 0x0800;
//  *SPR0DATB = 0x0800;

    // 0xdc0 is yellow, 0x840 is orange-brown
    // Sprite color 0 is always transparent mode
    *COLOR17 = 0xfff;  // Sprite 0 and 1 color 1   white
    *COLOR18 = 0x000;  // Sprite 0 and 1 color 2   black
    *COLOR19 = 0x44f;  // Sprite 0 and 1 color 3

    *COLOR21 = 0x04f;  // Sprite 2 and 3 color 1
    *COLOR22 = 0x4f0;  // Sprite 2 and 3 color 2
    *COLOR23 = 0xa70;  // Sprite 2 and 3 color 3   orange (cursor)

    *DMACON = DMACON_SET | DMACON_SPREN;
}
