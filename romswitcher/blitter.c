/*
 * Amiga Blitter control.
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
#include <stdlib.h>
#include <string.h>
#include "amiga_chipset.h"
#include "util.h"
#include "printf.h"
#include "draw.h"
#include "screen.h"
#include "intuition.h"
#include "timer.h"
#include "vectors.h"
#include <hardware/blit.h>

void
fill_rect_cpu(uint fgpen, uint x1, uint y1, uint x2, uint y2)
{
    uint     plane;
    uint     left  = x1 & ~0xf;             // round down
    uint     right = (x2 + 0xf) & ~0xf;     // round up
    uint     blit_width_pixels = right - left;
    uint     blit_height = (y2 - y1) + 1;
    uint     num_words = blit_width_pixels / 16;
    uint     word;
    uint16_t left_mask  = 0xffff >> (x1 & 0xf);
    uint16_t right_mask = 0xffff << (16 - (x2 & 0xf));
    if (right_mask == 0)
        right_mask = 0xffff;

//  printf(" %x:%x:%x", x1, left, left_mask);
//  printf(" %x:%x:%x", x2, right, right_mask);
    for (plane = 0; plane < SCREEN_BITPLANES; plane++) {
        uintptr_t src = BITPLANE_0_BASE + plane * BITPLANE_OFFSET +
                        (y1 * SCREEN_WIDTH / 8) + left / 8;
        uint16_t *rptr = (uint16_t *) src;
        for (uint line = 0; line < blit_height; line++) {
            uint16_t *ptr = rptr;
            if ((fgpen & BIT(plane)) == 0) {
                /* Erase in this bitplane */
                if (num_words > 1) {
                    *ptr &= ~left_mask;
                    ptr++;
                    for (word = 1; word < num_words - 1; word++) {
                        *(ptr++) = 0x0000;
                    }
                    *ptr &= ~right_mask;
                } else {
                    *ptr &= ~(left_mask & right_mask);
                }
            } else {
                /* Draw in this bitplane */
                if (num_words > 1) {
                    *ptr |= left_mask;
                    ptr++;
                    for (word = 1; word < num_words - 1; word++) {
                        *(ptr++) = 0xffff;
                    }
                    *ptr |= right_mask;
                } else {
                    *ptr |= (left_mask & right_mask);
                }
            }
            rptr += SCREEN_WIDTH / 8 / 2;
        }
    }
}

/*
 * Use area fill in the specified rectangular region. It does this
 * by copying the area to itself using D = A in descending mode,
 * where src A is the screen image itself. Set the fill bits
 * to specify the fill operation
 *
 * Note: fill comes after shift, mask and logical operations, so
 * we can't mask out the fill.
 *
 * xor
 *      When not set, inclusive fill is enabled
 *      When set, exclusive fill is enabled
 * fill_carry_input
 *      BIT(0) is FCI
 *      BIT(1) is EFE
 */
void
fill_rect_blit(uint fgpen, uint x1, uint y1, uint x2, uint y2, uint8_t xor,
               uint8_t fill_carry_input)
{
// XXX: When in DESCENDING MODE, the artifacts appear more consistent, and
//      seem to occur at odd x values.
#undef DO_DESCENDING
#ifdef DO_DESCENDING
#else
    /* When in ascending mode, even x starts are broken. Use only odd */
    x1 |= 1;
#endif
    /*
     * Determine the left and right borders, which are at the
     * word boundaries to the left and right sides
     */
    uint     plane;
    uint     left  = x1 & ~0xf;
    uint     right = (x2 + 0xf) & ~0xf;
    uint     blit_width_pixels = right - left;
    uint     blit_height = (y2 - y1) + 1;
    uint     num_words = blit_width_pixels / 16;
    uint8_t  fill_mode = xor ? FILL_XOR: FILL_OR;
    uint16_t bltmod = (SCREEN_WIDTH - blit_width_pixels) / 8;  // in bytes
    uint16_t left_mask  = 0xffff >> (x1 & 0xf);
    uint16_t right_mask = 0xffff << (16 - (x2 & 0xf));
    if (right_mask == 0)
        right_mask = 0xffff;

//  printf(" %x", blit_width_pixels);
//  printf(" %x:%x:%x", x1, left, left_mask);
//  printf(" %x:%x:%x", x2, right, right_mask);
    for (plane = 0; plane < SCREEN_BITPLANES; plane++) {
#ifdef DO_DESCENDING
        /*
         * the address of source A and D has to be the word that defines
         * the right bottom corner
         */
        uintptr_t src = BITPLANE_0_BASE + plane * BITPLANE_OFFSET +
                        (y2 * SCREEN_WIDTH / 8) + right / 8;
#else
        uintptr_t src = BITPLANE_0_BASE + plane * BITPLANE_OFFSET +
                        (y1 * SCREEN_WIDTH / 8) + left / 8;
#endif
        WaitBlit();

//      printf(" %x:%x", x1, BIT(x1 & 0xf) - 1);
        if ((fgpen & BIT(plane)) == 0) {
            /* Erase area */
            // XXX: Bug: over-erases area
            *BLTCON0 = 0x0100;
            *BLTCON1 = 0x0000;
        } else {
            *BLTADAT = 0xffff;   // Pre-load A value
#ifdef DO_DESCENDING
            /* descending mode + fill parameters */
            *BLTCON1 = fill_mode | (fill_carry_input << 2) | BIT(1);
            *BLTCON0 = 0x09f0;   // enable channels A and D, LF := D = A
#else
            /* ascending mode + fill parameters */
            *BLTCON1 = fill_mode | (fill_carry_input << 2);
            *BLTCON0 = 0x01f0;   // enable channel D, LF := D = A
#endif
        }
#ifdef DO_DESCENDING
        *BLTAFWM = right_mask;
        *BLTALWM = left_mask;
#else
        *BLTAFWM = left_mask;
        *BLTALWM = right_mask;
#endif
//      *BLTAFWM = 0xffff;
//      *BLTALWM = 0xffff;

        *BLTDPT  = src;
        *BLTAPT  = src;
        *BLTDMOD = bltmod;
        *BLTAMOD = bltmod;

        *BLTSIZE = (blit_height << 6) | (num_words & 0x3f);
// break;  // only update bitplane 0 for now
    }
}

void
fill_rect(uint fgpen, uint x1, uint y1, uint x2, uint y2)
{
    fill_rect_cpu(fgpen, x1, y1, x2, y2);
//  fill_rect_blit(fgpen, x1, y1, x2, y2, FALSE, 1);
}


static void
gray_rect_cpu(uint fgpen, uint x1, uint y1, uint x2, uint y2)
{
    uint     plane;
    uint     left  = x1 & ~0xf;             // round down
    uint     right = (x2 + 0xf) & ~0xf;     // round up
    uint     blit_width_pixels = right - left;
    uint     blit_height = (y2 - y1) + 1;
    uint     num_words = blit_width_pixels / 16;
    uint     word;
    uint16_t left_mask1  = 0xaaaa >> (x1 & 0xf);
    uint16_t left_mask2  = 0x5555 >> (x1 & 0xf);
    uint16_t right_mask1 = 0xaaaa << (16 - (x2 & 0xf));
    uint16_t right_mask2 = 0x5555 << (16 - (x2 & 0xf));
    if ((right_mask1 == 0) && (right_mask2 == 0)) {
        right_mask1 = 0xaaaa;
        right_mask2 = 0x5555;
    }
    if (x1 & 1) {
        uint16_t temp = left_mask1;
        left_mask1 = left_mask2;
        left_mask2 = temp;
    }
    if (x2 & 1) {
        uint16_t temp = right_mask1;
        right_mask1 = right_mask2;
        right_mask2 = temp;
    }

//  printf(" %x:%x:%x", x1, left, left_mask);
//  printf(" %x:%x:%x", x2, right, right_mask);
    for (plane = 0; plane < SCREEN_BITPLANES; plane++) {
        uintptr_t src = BITPLANE_0_BASE + plane * BITPLANE_OFFSET +
                        (y1 * SCREEN_WIDTH / 8) + left / 8;
        uint16_t *rptr = (uint16_t *) src;
        for (uint line = 0; line < blit_height; line++) {
            uint16_t *ptr = rptr;
            uint16_t fill_mask;
            uint16_t left_mask;
            uint16_t right_mask;
            if ((line & 1) == 0) {
                fill_mask = 0xaaaa;
                left_mask = left_mask1;
                right_mask = right_mask1;
            } else {
                fill_mask = 0x5555;
                left_mask = left_mask2;
                right_mask = right_mask2;
            }

            if ((fgpen & BIT(plane)) == 0) {
                /* Erase in this bitplane */
                *ptr &= ~left_mask;
                if (num_words > 1) {
                    ptr++;
                    for (word = 1; word < num_words - 1; word++) {
                        *(ptr++) &= ~fill_mask;
                    }
                }
                *ptr &= ~right_mask;
            } else {
                /* Draw in this bitplane */
                *ptr |= left_mask;
                if (num_words > 1) {
                    ptr++;
                    for (word = 1; word < num_words - 1; word++) {
                        *(ptr++) |= fill_mask;
                    }
                }
                *ptr |= right_mask;
            }
            rptr += SCREEN_WIDTH / 8 / 2;
        }
    }
}

void
gray_rect(uint fgpen, uint x1, uint y1, uint x2, uint y2)
{
    // XXX: gray_rect_cpu() can't handle the case where x2 - x1 < 16
    gray_rect_cpu(fgpen, x1, y1, x2, y2);
}

#undef TAKEN_FROM_THE_INTERNET_UNTESTED
#ifdef TAKEN_FROM_THE_INTERNET_UNTESTED
/*
 * The below code is from modsurfer:
 *
 * https://github.com/amigageek/modsurfer/blob/master/blit.c
 * I've only tried to get blit_fill() to work, but don't know how to
 * drive it properly to fill an irregular polygon: see test_blitfill()
 */
#ifndef BOOL
#define BOOL int
#endif

#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))
#define MAX(X, Y) ((X) > (Y) ? (X) : (Y))
#define ABS(X) ((X) < 0 ? (-(X)) : (X))

#define BLTCON0_ASH0_SHF 0xC
#define BLTCON0_USEA     0x0800
#define BLTCON0_USEB     0x0400
#define BLTCON0_USEC     0x0200
#define BLTCON0_USED     0x0100
#define BLTCON0_LF0_SHF  0x0
#define BLTCON1_BSH0_SHF 0xC
#define BLTCON1_TEX0_SHF 0xC
#define BLTCON1_SIGN_SHF 0x6
#define BLTCON1_AUL_SHF  0x2
#define BLTCON1_SING_SHF 0x1
#define BLTCON1_IFE      0x0008
#define BLTCON1_DESC     0x0002
#define BLTSIZE_H0_SHF   0x6
#define BLTSIZE_W0_SHF   0x0
#define BPLCON0_BPU_SHF  0xC
#define BPLCON0_COLOR    0x0200
#define BPLCON3_SPRES_Shf 0x6

#define kBytesPerWord 0x2
#define kDispRowPadW 8 // padded for horizontal scrolling
#define kDispColPad 1  // ^-
#define kDispStride ((SCREEN_WIDTH / 8) + (kDispRowPadW * kBytesPerWord))
#define kDispSlice (kDispStride * (SCREEN_HEIGHT + kDispColPad))
#define kFontWidth 8
#define kFontHeight 8
#define kFontNGlyphs 0x60

void
blit_copy(APTR src_base, UWORD src_stride_b, UWORD src_x, UWORD src_y,
          APTR dst_base, UWORD dst_stride_b, UWORD dst_x, UWORD dst_y,
          UWORD copy_w, UWORD copy_h, BOOL replace_bg, BOOL force_desc)
{
    UWORD start_x[2] = {src_x, dst_x};
    UWORD start_x_word[2], end_x_word[2], num_words[2], word_offset[2];

    for (UWORD i = 0; i < 2; ++ i) {
        start_x_word[i] = start_x[i] >> 4;
        end_x_word[i] = ((start_x[i] + copy_w) + 0xF) >> 4;
        num_words[i] = end_x_word[i] - start_x_word[i];
        word_offset[i] = start_x[i] & 0xF;
    }

    UWORD width_words = MAX(num_words[0], num_words[1]);
    WORD shift = (WORD)word_offset[1] - (WORD)word_offset[0];
    UWORD src_mod_b = src_stride_b - (width_words * kBytesPerWord);
    UWORD dst_mod_b = dst_stride_b - (width_words * kBytesPerWord);

    BOOL desc = force_desc || (shift < 0);

    UWORD start_x_off = desc ? (width_words - 1) : 0;
    UWORD start_y_off = desc ? (copy_h - 1) : 0;

    ULONG src_start_b = (ULONG)src_base +
        ((src_y + start_y_off) * src_stride_b) +
        ((start_x_word[0] + start_x_off) * kBytesPerWord);
    ULONG dst_start_b = (ULONG)dst_base +
        ((dst_y + start_y_off) * dst_stride_b) +
        ((start_x_word[1] + start_x_off) * kBytesPerWord);

    UWORD left_word_mask = (UWORD)(0xFFFFU << (word_offset[0] +
                MAX(0, 0x10 - (word_offset[0] + copy_w)))) >> word_offset[0];
    UWORD right_word_mask;

    if (width_words == 1) {
        right_word_mask = left_word_mask;
    } else {
        right_word_mask = 0xFFFFU << MIN(0x10,
                ((start_x_word[0] + width_words) << 4) - (src_x + copy_w));
    }

    WaitBlit();

    // A = Mask of bits inside copy region
    // B = Source data
    // C = Destination data (for region outside mask)
    // D = Destination data
    UWORD minterm = replace_bg ? 0xCA : 0xEA;

    *BLTCON0 = (ABS(shift) << BLTCON0_ASH0_SHF) |
               BLTCON0_USEB | BLTCON0_USEC | BLTCON0_USED | minterm;
    *BLTCON1 = (ABS(shift) << BLTCON1_BSH0_SHF) | (desc ? BLTCON1_DESC : 0);
    *BLTBMOD = src_mod_b;
    *BLTCMOD = dst_mod_b;
    *BLTDMOD = dst_mod_b;
    *BLTAFWM = (desc ? right_word_mask : left_word_mask);
    *BLTALWM = (desc ? left_word_mask : right_word_mask);
    *BLTADAT = 0xFFFF;
    *BLTBPT = (uintptr_t)src_start_b;
    *BLTCPT = (uintptr_t)dst_start_b;
    *BLTDPT = (uintptr_t)dst_start_b;
    *BLTSIZE = (copy_h << BLTSIZE_H0_SHF) | width_words;
}

void
blit_rect(APTR dst_base, UWORD dst_stride_b, UWORD dst_x, UWORD dst_y,
          APTR mask_base, UWORD mask_stride_b, UWORD mask_x, UWORD mask_y,
          UWORD width, UWORD height, BOOL set_bits)
{
    UWORD start_x_word = dst_x >> 4;
    UWORD end_x_word = ((dst_x + width) + 0xF) >> 4;
    UWORD width_words = end_x_word - start_x_word;
    UWORD word_offset = dst_x & 0xF;

    UWORD dst_mod_b = dst_stride_b - (width_words * kBytesPerWord);
    UWORD mask_mod_b = mask_stride_b - (width_words * kBytesPerWord);

    ULONG dst_start_b = (ULONG)dst_base + (dst_y * dst_stride_b) +
                        (start_x_word * kBytesPerWord);
    ULONG mask_start_b = (ULONG)mask_base + (mask_y * mask_stride_b) +
                         (start_x_word * kBytesPerWord);

    UWORD left_word_mask = (UWORD)(0xFFFFU << (word_offset +
                        MAX(0, 0x10 - (word_offset + width)))) >> word_offset;
    UWORD right_word_mask;

    (void) mask_x;
    if (width_words == 1) {
        right_word_mask = left_word_mask;
    } else {
        right_word_mask = 0xFFFFU <<
            MIN(0x10, ((start_x_word + width_words) << 4) - (dst_x + width));
    }

    UWORD minterm = 0xA;

    if (mask_base) {
        minterm |= set_bits ? 0xB0 : 0x80;
    } else {
        minterm |= set_bits ? 0xF0 : 0x00;
    }

    WaitBlit();

    // A = Mask of bits inside copy region
    // B = Optional bitplane mask
    // C = Destination data (for region outside mask)
    // D = Destination data
    *BLTCON0 = BLTCON0_USEC | BLTCON0_USED |
               (mask_base ? BLTCON0_USEB : 0) | minterm;
    *BLTCON1 = 0;
    *BLTBMOD = mask_mod_b;
    *BLTCMOD = dst_mod_b;
    *BLTDMOD = dst_mod_b;
    *BLTAFWM = left_word_mask;
    *BLTALWM = right_word_mask;
    *BLTADAT = 0xFFFF;
    *BLTBPT = (uintptr_t)mask_start_b;
    *BLTCPT = (uintptr_t)dst_start_b;
    *BLTDPT = (uintptr_t)dst_start_b;
    *BLTSIZE = (height << BLTSIZE_H0_SHF) | width_words;
}

void
blit_line(APTR dst_base, UWORD dst_stride_b, UWORD x0, UWORD y0,
          UWORD x1, UWORD y1)
{
    UWORD dx = ABS(x1 - x0);
    UWORD dy = ABS(y1 - y0);
    UWORD dmax = MAX(dx, dy);
    UWORD dmin = MIN(dx, dy);
    ULONG dst_start = (ULONG)dst_base + (y0 * dst_stride_b) +
                                        ((x0 / 0x8) & ~0x1);
    UBYTE octant =
        ((((dx >= dy) && (x0 >= x1)) | ((dx < dy) && (y0 >= y1))) << 0) |
        ((((dx >= dy) && (y0 >= y1)) | ((dx < dy) && (x0 >= x1))) << 1) |
        ((dx >= dy) << 2);

    WaitBlit();

    // A = Line parameters
    // C = Destination data (for region outside mask)
    // D = Destination data
    *BLTCON0 = ((x0 & 0xF) << BLTCON0_ASH0_SHF) |
               BLTCON0_USEA | BLTCON0_USEC | BLTCON0_USED |
               (0xCA << BLTCON0_LF0_SHF);
    *BLTCON1 =
        ((x0 & 0xF) << BLTCON1_TEX0_SHF) |
        ((((4 * dmin) - (2 * dmax)) < 0 ? 1 : 0) << BLTCON1_SIGN_SHF) |
        (octant << BLTCON1_AUL_SHF) |
        (0 << BLTCON1_SING_SHF) |
        BLTCON1_LINE;
    *BLTADAT = 0x8000;
    *BLTBDAT = 0xFFFF;
    *BLTAFWM = 0xFFFF;
    *BLTALWM = 0xFFFF;
    *BLTAMOD = 4 * (dmin - dmax);
    *BLTBMOD = 4 * dmin;
    *BLTCMOD = dst_stride_b;
    *BLTDMOD = dst_stride_b;
    *BLTAPT = (uintptr_t)(ULONG)((4 * dmin) - (2 * dmax));
    *BLTCPT = (uintptr_t)dst_start;
    *BLTDPT = (uintptr_t)dst_start;
    *BLTSIZE = ((dmax + 1) << BLTSIZE_H0_SHF) | (0x2 << BLTSIZE_W0_SHF);
}

void
blit_fill(APTR dst_base, UWORD dst_stride_b, UWORD x, UWORD y,
          UWORD width, UWORD height)
{
    UWORD start_x_word = x / 0x10;
    UWORD end_x_word = (x + width) / 0x10;
    UWORD width_words = end_x_word - start_x_word;
    UWORD word_offset = x & 0xF;
    UWORD mod_b = dst_stride_b - (width_words * kBytesPerWord);

    ULONG dst_start_b = (ULONG)dst_base +
        ((y + height - 1) * dst_stride_b) +
        ((start_x_word + width_words - 1) * kBytesPerWord);

    UWORD left_word_mask = (UWORD)(0xFFFFU << (word_offset +
                        MAX(0, 0x10 - (word_offset + width)))) >> word_offset;
    UWORD right_word_mask;

    if (width_words == 1) {
        right_word_mask = left_word_mask;
    } else {
        right_word_mask = 0xFFFFU <<
                MIN(0x10, ((start_x_word + width_words) << 4) - (x + width));
    }

    WaitBlit();

    // A = Mask of bits inside copy region
    // B = Source data
    // C = Destination data (for region outside mask)
    // D = Destination data
    *BLTCON0 = BLTCON0_USEA | BLTCON0_USED | 0xF0;
    *BLTCON1 = BLTCON1_IFE | BLTCON1_DESC;
    *BLTAMOD = mod_b;
    *BLTDMOD = mod_b;
    *BLTAFWM = right_word_mask;
    *BLTALWM = left_word_mask;
    *BLTAPT = (uintptr_t)dst_start_b;
    *BLTDPT = (uintptr_t)dst_start_b;
    *BLTSIZE = (height << BLTSIZE_H0_SHF) | width_words;
}

void
blit_char(APTR font_base, UWORD glyph_idx, APTR dst_row_base,
          UWORD dst_x, UWORD color, BOOL replace_bg)
{
    UWORD start_x_word = dst_x >> 4;
    UWORD end_x_word = ((dst_x + kFontWidth) + 0xF) >> 4;
    UWORD width_words = end_x_word - start_x_word;

    ULONG src_start = (ULONG)font_base + (glyph_idx << 1);
    ULONG dst_start = (ULONG)dst_row_base + (start_x_word << 1);
    UWORD shift = dst_x & 0xF;
    UWORD right_word_mask = (width_words == 1 ? 0xF800 : 0);

    UWORD minterm = replace_bg ? 0xCA : 0xEA;

    for (UWORD plane_idx = 0; plane_idx < SCREEN_BITPLANES; ++ plane_idx) {
        if (color & (1 << plane_idx)) {
            WaitBlit();

            *BLTCON0 = (shift << BLTCON0_ASH0_SHF) |
                       BLTCON0_USEB | BLTCON0_USEC | BLTCON0_USED | minterm;
            *BLTCON1 = shift << BLTCON1_BSH0_SHF;
            *BLTBMOD = (kFontNGlyphs * kBytesPerWord) - (width_words << 1);
            *BLTCMOD = kDispStride - (width_words << 1);
            *BLTDMOD = kDispStride - (width_words << 1);
            *BLTAFWM = 0xF800;
            *BLTALWM = right_word_mask;
            *BLTADAT = 0xFFFF;
            *BLTBPT = (uintptr_t)src_start;
            *BLTCPT = (uintptr_t)dst_start;
            *BLTDPT = (uintptr_t)dst_start;
            *BLTSIZE = (kFontHeight << BLTSIZE_H0_SHF) | width_words;
        }

        dst_start += kDispSlice;
    }
}
#endif

/*
 * Amiga Blitter minterm functions
 *
 *   Expression BLTCON0 LF   Expression BLTCON0 LF
 *   ---------- ----------   ---------- ----------
 *   D=A        0xf0         D=AB       0xc0
 *   D=!A       0x0f         D=A(!B)    0x30
 *   D=B        0xcc         D=(!A)B    0x0c
 *   D=!B       0x33         D=!(AB)    0x03
 *   D=C        0xaa         D=BC       0x88
 *   D=!C       0x55         D=B(!C)    0x44
 *   D=AC       0xa0         D=(!B)C    0x22
 *   D=A(!C)    0x50         D=!(BC)    0x11
 *   D=(!A)C    0x0a         D=A|(!B)   0xf3
 *   D=!(AC)    0x05         D=!(A|B)   0x3f
 *   D=A|B      0xfc         D=A|(!C)   0xf5
 *   D=(!A)|B   0xcf         D=!(A|C)   0x5f
 *   D=A|C      0xfa         D=B|(!C)   0xdd
 *   D=(!A)|C   0xaf         D=!(B|C)   0x77
 *   D=B|C      0xee         D=AB|(!A)C 0xca
 *   D=(!B)|C   0xbb         D=A(!B)|AC 0xac
 */

#define LF_COOKIE_CUT (0xca)
#define LF_XOR        (0x4a)

/*
 *  Draw a line assuming left top corner is at 0, 0 of the destination
 *  bit plane.
 */
void
draw_line(uint fgpen, int x1, int y1, int x2, int y2)
{
    UWORD dx = abs(x2 - x1), dy = abs(y2 - y1), dmax, dmin;
    UWORD bytes_per_line = SCREEN_WIDTH / 8;
    int   plane;                    // bitplane depends on the color choice
    UBYTE pattern_offset = 0;       // or 3 if line_pattern is 0xcccc
    UBYTE lf_byte = LF_COOKIE_CUT;  // or LF_XOR
    UBYTE single = FALSE;           // Not sure what this is
    UBYTE omit_first_pixel = FALSE; // Is this ever desirable?
    UWORD line_pattern = 0xffff;    // maybe also 0xcccc

    /*
     * Perform the same blitter set-bits operation on every plane which is
     * part of the current draw color. Planes which are not part of the
     * current draw color must have a clear-bits done instead.
     */
    for (plane = 0; plane < SCREEN_BITPLANES; plane++) {
        line_pattern = fgpen & BIT(plane) ? 0xffff : 0x0000;

        // Determine the octant code
        UBYTE code;
        if (y1 >= y2) {
            if (x1 <= x2) {
                code = dx >= dy ? 6 : 1;
            } else {
                code = dx <= dy ? 3 : 7;
            }
        } else {
            if (x1 >= x2) {
                code = dx >= dy ? 5 : 2;
            } else {
                code = dx <= dy ? 0 : 4;
            }
        }

        if (dx <= dy) {
            dmin = dx;
            dmax = dy;
        } else {
            dmin = dy;
            dmax = dx;
        }
        WORD aptlval = 4 * dmin - 2 * dmax;
        UWORD startx = (x1 & 0xf) << 12;  // x1 modulo 16
        /* texture is BSH in BLTCON1 */
        UWORD texture = ((x1 + pattern_offset) & 0xf) << 12;
        UWORD sign = (aptlval < 0 ? 1 : 0) << 6;
        UWORD bltcon1val = texture | sign | (code << 2) | (single << 1) | 0x01;

        APTR start_address = (APTR) BITPLANE_0_BASE +
                             plane * BITPLANE_OFFSET +
                             y1 * bytes_per_line + x1 / 8;

        WaitBlit();
        *BLTAPT  = ((UWORD) aptlval);
        *BLTCPT  = (uintptr_t) start_address;

        /*
         * If the first pixel is not to be plotted, then scratchmem will be
         * used in place of the start address.
         */
        static UWORD __chip scratchmem[12];

        *BLTDPT  = (uintptr_t) (omit_first_pixel ? scratchmem : start_address);

        *BLTAMOD = 4 * (dmin - dmax);
        *BLTBMOD = 4 * dmin;

        *BLTCMOD = SCREEN_WIDTH / 8;  // destination width in bytes
        *BLTDMOD = SCREEN_WIDTH / 8;
        *BLTCON0 = 0x0b00 | lf_byte | startx;
        *BLTCON1 = bltcon1val;

        *BLTADAT = 0x8000;  // draw "pen" pixel
        *BLTBDAT = line_pattern;
        *BLTAFWM = 0xffff;
        *BLTALWM = 0xffff;

        *BLTSIZE = ((dmax + 1) << 6) + 2;
    }
}

typedef struct poly_bounding_box {
    int16_t min_x;
    int16_t min_y;
    int16_t max_x;
    int16_t max_y;
} poly_bounding_box;

#if 0

/**
 * poly_is_convex
 * Returns 1 if the polygon described by points is convex, returns 0 otherwise
 * Convexness of a polygon - having only interior angles measuring less than
 * 180 degrees.
 * This function treats polygons with three colinear adjacent points as convex
 *
 * points - the list of points, ordered {x, y, x, y, x, y, ...}
 * npoints - the number of points
 */
static int
poly_is_convex(int16_t *points, int npoints)
{
    // triangles are always convex
    if (npoints <= 3) {
        return (1);
    }

    // we can do cross products from directed side to directed side to see
    // if all points curl in the same direction
    // we will first need to find a winding direction
    // we will do this with a loop, since we have no guarantees that 3 points
    // will never be colinear
    // each loop will take 2 adjacent sides, consisting of 3 points
    int i = 0;
    int winding = 0;
    for (; winding == 0 && i < npoints - 2; i++) {
        int side0x = points[i * 2 + 2] - points[i * 2]; // x1 - x0
        int side0y = points[i * 2 + 3] - points[i * 2 + 1]; // y1 - y0
        int side1x = points[i * 2 + 4] - points[i * 2 + 2]; // x2 - x1
        int side1y = points[i * 2 + 5] - points[i * 2 + 3]; // y2 - y1

        // winding = side0 cross side1
        winding = side0x * side1y - side0y * side1x;
    }

    // this is the part where we check to make sure
    for (; i < npoints - 2; i++) {
        int side0x = points[i * 2 + 2] - points[i * 2]; // x1 - x0
        int side0y = points[i * 2 + 3] - points[i * 2 + 1]; // y1 - y0
        int side1x = points[i * 2 + 4] - points[i * 2 + 2]; // x2 - x1
        int side1y = points[i * 2 + 5] - points[i * 2 + 3]; // y2 - y1

        // winding = side0 cross side1
        int this_winding = side0x * side1y - side0y * side1x;
        if (this_winding * winding < 0) {
            // if this winding and winding are of different sign,
            // shape is concave
            return (0);
        }
    }

    return (1);
}

#endif

/**
 * poly_winding_direction
 * Determines the winding direction of a set of points.
 *
 * If the points are wound clockwise, returns 1.
 * If the points are wound counter-clockwise, returns 0.
 */
static int
poly_winding_direction(int16_t *points, int npoints)
{
    // Use the Shoelace formula or whatever it's called

    int32_t area = points[2 * npoints - 2] * points[1] -
                   points[2 * npoints - 1] * points[0];
    for (int i = 0; i < npoints - 1; i++) {
        area += points[2 * i] * points[2 * i + 3] -
                points[2 * i + 1] * points[2 * i + 2];
    }

    // area is now > 0 if wound clockwise, and < 0 if wound counter-clockwise
    // you may note that this apparently should be the other way around,
    // but the fact that computer pixel coordinates are a left-handed system
    // (+y is to the right of +x) causes the sign to be inverted here

    return (area > 0);
}

/**
 * poly_get_bounding_box
 * Finds the bounding box of the polygon described by points.
 * Outputs the result to out.
 * Returns the index of the point with minimum y.
 * - If there are several points with this, then it returns the left-most.
 *
 * points - the list of points, ordered {x, y, x, y, x, y, ...}
 * npoints - the number of points
 * out - a pointer to the structure to output to
 */
static int
poly_get_bounding_box(int16_t *points, int npoints, poly_bounding_box *out)
{
    // easy initialization
    out->max_x = 0x8000;
    out->max_y = 0x8000;
    out->min_x = 0x7FFF;
    out->min_y = 0x7FFF;

    int ret_i = -1;

    for (int i = 0; i < npoints; i++) {
        if (points[i * 2] > out->max_x) {
            out->max_x = points[i * 2];
        }

        if (points[i * 2] < out->min_x) {
            out->min_x = points[i * 2];
        }

        if (points[i * 2 + 1] > out->max_y) {
            out->max_y = points[i * 2 + 1];
        }

        if (points[i * 2 + 1] <= out->min_y) {
            out->min_y = points[i * 2 + 1];

            if (ret_i == -1 || points[i * 2 + 1] < points[ret_i * 2 + 1] ||
                               points[i * 2] < points[ret_i * 2]) {
                ret_i = i;
            }
        }
    }

    return (ret_i);
}

#define abs(x) __builtin_abs(x)

/**
 * Following static functions are modifications of the basic Bresenham algorithm
 * https://en.wikipedia.org/wiki/Bresenham%27s_line_algorithm
 */

/**
 * Function for plotLineLow & plotLineHigh (refer to wikipedia article)
 * flag = 0 -> plotLineLow, on left side of polygon
 * flag = 1 -> plotLineLow, on right side of polygon
 * flag = 2 -> plotLineHigh, flip x & y arguments as well, e.g.
 * p_pl_s(bounds, y0, x0, y1, x1, 2)
 *
 * This function does the actual placing of points of the line
 *
 * The reason why we need to differentiate between the left and right side
 * of the polygon is because we want to start drawing from the lowest x
 * value on the left side and to the highest x-value on the right side
 */
static void
poly_plot_line_helper(int16_t *bounds, int16_t x0, int16_t y0, int16_t x1,
                      int16_t y1, int flag)
{
    int16_t dx = x1 - x0;
    int16_t dy = y1 - y0; // this is never 0

    int yi = 1;
    if (dy < 0) {
        yi = -1;
        dy = -dy;
    }
    int D = 2 * dy - dx;

    switch (flag) {
    break; case 0:
        bounds[y0] = x0;

        for (; x0 < x1; x0++) {
            if (D > 0) {
                y0 += yi;
                bounds[y0] = x0 + 1;
                D -= 2 * dx;
            }
            D += 2 * dy;
        }
    break; default:
        for (; x0 <= x1; x0++) {
            // this simplifies to (bounds[y0] = x0) if flag = 1 and
            // (bounds[x0] = y0) if flag = 2
            bounds[y0 * (2 - flag) + x0 * (flag - 1)] = x0 * (2 - flag) +
                                                        y0 * (flag - 1);
            if (D > 0) {
                y0 += yi;
                D -= 2 * dx;
            }
            D += 2 * dy;
        }
    }
}

/**
 * Wrapper function for poly_plot_line_helper
 * Does the handling of how points of the line should be placed and whatnot
 *
 * flag = 0 -> plotLine for left side of polygon
 * flag = 1 -> plotLine for right side of polygon
 */
static void
poly_plot_line(int16_t *bounds, int16_t x0, int16_t y0, int16_t x1,
               int16_t y1, int flag)
{
    int16_t dx = x1 - x0;
    int16_t dy = y1 - y0; // this is never 0

    if (dx == 0) { // optimization
        for (int y = y0; y <= y1; y++) {
            bounds[y] = x0;
        }
        return;
    }

    // guarantees: y is always increasing from y0 to y1
    if (dy < abs(dx)) {
        if (dx < 0) {
            poly_plot_line_helper(bounds, x1, y1, x0, y0, flag);
        } else {
            poly_plot_line_helper(bounds, x0, y0, x1, y1, flag);
        }
    } else {
        // here, we need a plotLineHigh
        poly_plot_line_helper(bounds, y0, x0, y1, x1, 2);
    }
}

/**
 * fill_polygon_cpu
 * Fills in the polygon described by points with the fill_rect_cpu function.
 *
 * Requirements:
 * The polygon described by points is wound clockwise.
 * The polygon described by points does not contain duplicate points.
 *
 * Note:
 * Currently does not support concave polygons all that well.
 *
 * vect - the list of points, ordered {x, y, x, y, x, y, ...}
 * count - the number of points
 */
void
fill_polygon_cpu(uint fgpen, uint count, int16_t *vect)
{
    // deduplicating vertices
    for (; count > 1 &&
           vect[count * 2 - 2] == vect[0] &&
           vect[count * 2 - 1] == vect[1]; count--) {
           ; // remove duplicates from end
    }
    for (; count > 1 &&
           vect[0] == vect[2] &&
           vect[1] == vect[3]; count--) {
           // remove duplicates from beginning
           vect += 2;
    }

    // this will now be 1 when clockwise, and -1 when counter clockwise
    int direction = poly_winding_direction(vect, count) * 2 - 1;

    poly_bounding_box bounding_box = { 0, 0, 0, 0 };
    int top_i = poly_get_bounding_box(vect, count, &bounding_box);

    // init memory to store x-positions of each line
    // inclusive of top and bottom
    int bounds_size = bounding_box.max_y - bounding_box.min_y + 1;
    int16_t *bounds = (int16_t *)malloc(sizeof (int16_t) * bounds_size * 2);
    int16_t *left_bounds = bounds;
    int16_t *right_bounds = bounds + bounds_size;

    // find the top points
    int left_i = top_i; // left point i
    int right_i = top_i; // (tentative) right point i

    // push right_i out
    for (int nright_i = right_i + direction; ; nright_i += direction) {
        if (nright_i == count) {
            nright_i = 0;
        }
        if (nright_i < 0) {
            nright_i = count - 1;
        }

        if (vect[nright_i * 2 + 1] > bounding_box.min_y) {
            break;
        }

        // nright_i is at min_y along with right_i
        // since this convex polygon is wound in the direction of the
        // direction variable, this implies that nright_i is to the right
        // of right_i
        right_i = nright_i;
    }

    // add left bounds to buffer
    for (int i = left_i; bounding_box.max_y > vect[i * 2 + 1]; ) {
        int ni = i - direction; // go counter-clockwise
        if (ni == count) {
            ni = 0;
        }
        if (ni < 0) {
            ni = count - 1;
        }

        poly_plot_line(left_bounds, vect[i * 2], vect[i * 2 + 1] -
                       bounding_box.min_y, vect[ni * 2], vect[ni * 2 + 1] -
                       bounding_box.min_y, 0);

        i = ni;
    }

    // add right bounds to buffer
    for (int i = right_i; bounding_box.max_y > vect[i * 2 + 1]; ) {
        int ni = i + direction; // go clockwise
        if (ni == count) {
            ni = 0;
        }
        if (ni < 0) {
            ni = count - 1;
        }

        poly_plot_line(right_bounds, vect[i * 2], vect[i * 2 + 1] -
                       bounding_box.min_y, vect[ni * 2], vect[ni * 2 + 1] -
                       bounding_box.min_y, 1);

        i = ni;
    }

    for (int i = 0; i < bounds_size; i++) {
        int y = i + bounding_box.min_y;
        fill_rect_cpu(fgpen, left_bounds[i], y, right_bounds[i], y);
    }

    free(bounds);
}
