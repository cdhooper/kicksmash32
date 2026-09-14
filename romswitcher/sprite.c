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
     * Mouse pointer bitmap, interleaved for sprite DMA.
     * The last five rows are transparent.
     */
    static const uint32_t mouse_image[MOUSE_SPRITE_HEIGHT] = {
        0xc0004000, 0x7000b000, 0x3c004c00, 0x3f004300,
        0x1fc020c0, 0x1fc02000, 0x0f001100, 0x0d801280,
        0x04c00940, 0x046008a0, 0x00200040, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
    };

    sprite0_data = (uint32_t *) 0x1080;  // chip RAM address
    sprite0_data[0] = sprite_calcpos(0x80 + MOUSE_SPRITE_XOFFSET,
                                    0x2c, 0x2c + MOUSE_SPRITE_HEIGHT);
    memcpy(sprite0_data + 1, mouse_image, sizeof (mouse_image));
    sprite0_data[MOUSE_SPRITE_HEIGHT + 1] = 0x00000000;

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
    sprite1_data = sprite0_data + MOUSE_SPRITE_HEIGHT + 2;
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

    /* A disabled sprite only needs its zero position/control pair. */
    spritex_data = sprite1_data + 10;
    spritex_data[0] = 0x00000000;

    /* Mouse pointer colors. */
    *COLOR17 = 0xe44;  // Sprite 0 and 1 color 1   red
    *COLOR18 = 0x000;  // Sprite 0 and 1 color 2   black
    *COLOR19 = 0xeec;  // Sprite 0 and 1 color 3   cream

    *COLOR21 = 0x04f;  // Sprite 2 and 3 color 1
    *COLOR22 = 0x4f0;  // Sprite 2 and 3 color 2
    *COLOR23 = 0xa70;  // Sprite 2 and 3 color 3   orange (cursor)

    *DMACON = DMACON_SET | DMACON_SPREN;
}
