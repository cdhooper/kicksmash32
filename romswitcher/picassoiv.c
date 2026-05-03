/*
 * Village Tronic Picasso IV initialization helpers.
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
#include "amiga_chipset.h"
#include "autoconfig.h"
#include "picassoiv.h"
#include "printf.h"
#include "timer.h"
#include "util.h"
#include "cache.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof (x) / sizeof ((x)[0]))
#endif

#define PICASSOIV_MFG          0x0877
#define PICASSOIV_PRODUCT_Z2FF 0x17
#define PICASSOIV_PRODUCT_Z3FF 0x18
#define P4_RGB_LOAD_NIBBLE     0x0f

typedef struct {
    uint32_t control_base;
    uint32_t regs_base;
    uint32_t post_base;
    uint32_t mem_base;
    uint8_t  variant;
    uint8_t  seq0f_18;
    uint8_t  product;
} p4_ff_t;

static const uint16_t p4_ff_pal[12] = {
    0x1c1c, 0x02f8, 0x030c, 0x0370, 0x0398, 0x025b,
    0x025c, 0x025e, 0x0271, 0x0310, 0x0270, 0x0000,
};

static const uint16_t p4_ff_ntsc[12] = {
    0x1c1c, 0x02f8, 0x0310, 0x0370, 0x0398, 0x01e9,
    0x01f0, 0x01f4, 0x020d, 0x0310, 0x01ea, 0x0000,
};

static void
p4_delay(void)
{
    (void) *CIAA_PRA;
}

static void
p4_delay_count(uint count)
{
    while (count-- != 0)
        p4_delay();
}

static void
p4_index_write(volatile uint8_t *regs, uint port, uint8_t idx, uint8_t value)
{
    *VADDR16((uintptr_t) regs + port) = (((uint16_t) idx) << 8) | value;
    p4_delay();
}

static uint8_t
crtc_read(volatile uint8_t *regs, uint8_t idx)
{
    regs[0x3d4] = idx;
    p4_delay();
    return (regs[0x3d5]);
}

static void
crtc_write(volatile uint8_t *regs, uint8_t idx, uint8_t value)
{
    p4_index_write(regs, 0x3d4, idx, value);
}

static void
crtc_set(volatile uint8_t *regs, uint8_t idx, uint8_t bits)
{
    crtc_write(regs, idx, crtc_read(regs, idx) | bits);
}

static void
crtc_clear(volatile uint8_t *regs, uint8_t idx, uint8_t bits)
{
    crtc_write(regs, idx, crtc_read(regs, idx) & (uint8_t) ~bits);
}

static uint8_t
seq_read(volatile uint8_t *regs, uint8_t idx)
{
    regs[0x3c4] = idx;
    p4_delay();
    return (regs[0x3c5]);
}

static void
seq_write(volatile uint8_t *regs, uint8_t idx, uint8_t value)
{
    p4_index_write(regs, 0x3c4, idx, value);
}

static void
gc_write(volatile uint8_t *regs, uint8_t idx, uint8_t value)
{
    p4_index_write(regs, 0x3ce, idx, value);
}

static void
p4_wait_input_status(volatile uint8_t *regs, uint8_t bit_set)
{
    uint timeout;

    for (timeout = 0; timeout < 2000; timeout++) {
        if (((regs[0x3da] & 0x01) != 0) == bit_set)
            return;
        p4_delay();
    }
}

static void
p4_dac_hidden_read4(volatile uint8_t *regs)
{
    uint x;

    /*
     * The S3/RAMDAC hidden command register is selected by four reads from
     * 0x3c6. Delays alone leave the native-video colour path misprogrammed.
     */
    for (x = 0; x < 4; x++) {
        (void) regs[0x3c6];
        p4_delay();
    }
}

static uint8_t
p4_ctrl_wait(volatile uint8_t *reg, uint8_t mask, uint8_t expected)
{
    uint timeout;
    uint8_t value = 0;

    for (timeout = 0; timeout < 10000; timeout++) {
        value = *reg & mask;
        if (value == expected)
            return (value);
        p4_delay();
    }
    return (value);
}

static uint8_t
p4_control_index_read(const p4_ff_t *ff, uint16_t index)
{
    volatile uint8_t *reg;

    reg = VADDR8(ff->control_base + 0x800 + ((uint32_t) index * 2));
    p4_delay();
    return (*reg);
}

static void
p4_control_index_write(const p4_ff_t *ff, uint16_t index, uint8_t value)
{
    volatile uint8_t *reg;

    reg = VADDR8(ff->control_base + 0x800 + ((uint32_t) index * 2));
    *reg = value;
    p4_delay();
}

static void
p4_program_rgb_loads(const p4_ff_t *ff)
{
    uint8_t sig22a;
    uint8_t stat22e;
    uint8_t rgb_load;
    uint ready = 0;
    uint poll;

    /*
     * Picasso IV ROM programs these board-side controls
     * through control_base + 0x800 + index * 2.
     */
    p4_control_index_write(ff, 0x226, 0x01);
    timer_delay_usec(4000);
    p4_control_index_write(ff, 0x226, 0x00);

    sig22a = 0xff;
    stat22e = 0xff;
    for (poll = 0; poll < 100; poll++) {
        timer_delay_usec(1000);
        stat22e = p4_control_index_read(ff, 0x22e);
        sig22a = p4_control_index_read(ff, 0x22a);
        if (((stat22e & 0x80) != 0) && (sig22a == 0xaa)) {
            ready = 1;
            break;
        }
    }

    rgb_load = (P4_RGB_LOAD_NIBBLE << 4) | P4_RGB_LOAD_NIBBLE;
    p4_control_index_write(ff, 0x224, 0x00);
    p4_control_index_write(ff, 0x225, 0x00);
    p4_control_index_write(ff, 0x224, 0x3e);
    p4_control_index_write(ff, 0x225, 0xff);
    p4_control_index_write(ff, 0x224, 0x36);
    p4_control_index_write(ff, 0x225, 0x88);
    p4_control_index_write(ff, 0x224, 0x32);
    p4_control_index_write(ff, 0x225, rgb_load);

    if (ready)
        timer_delay_msec(250);
}

static void
p4_program_rgb_load_post(const p4_ff_t *ff)
{
    /*
     * ROM re-applies these two control writes for boards connected to the
     * video slot. Keep this separate from the polled RGB load helper above
     * because the ROM does the late pass separately.
     */
    p4_control_index_write(ff, 0x224, 0x3e);
    p4_control_index_write(ff, 0x225, 0xff);
    p4_control_index_write(ff, 0x224, 0x32);
    p4_control_index_write(ff, 0x225, 0xff);
}

static void
p4_video_slot_cmd(const p4_ff_t *ff, uint16_t cmd_index,
                  uint8_t command, uint8_t value)
{
    p4_control_index_write(ff, cmd_index, command);
    timer_delay_usec(4);
    p4_control_index_write(ff, cmd_index + 1, value);
    timer_delay_usec(23);
}

static void
p4_program_video_slot_config(const p4_ff_t *ff)
{
    /*
     * ROM programs the video-slot colour path after the RGB load handshake
     * succeeds.
     */
    p4_video_slot_cmd(ff, 0x228, 0x01, 0x00);
    p4_video_slot_cmd(ff, 0x222, 0x05, 0x01);
    p4_video_slot_cmd(ff, 0x228, 0xc0, 0x31);
    p4_video_slot_cmd(ff, 0x228, 0x23, 0x21);
    p4_video_slot_cmd(ff, 0x228, 0x43, 0x00);
    p4_video_slot_cmd(ff, 0x228, 0x63, 0xff);
    p4_video_slot_cmd(ff, 0x228, 0x83, 0x05);
    p4_video_slot_cmd(ff, 0x228, 0x20, 0x20);
    p4_video_slot_cmd(ff, 0x228, 0x40, 0x3f);
    p4_video_slot_cmd(ff, 0x228, 0x60, 0x44);
    p4_video_slot_cmd(ff, 0x228, 0x80, 0x05);
    p4_video_slot_cmd(ff, 0x228, 0x23, 0x21);
    p4_video_slot_cmd(ff, 0x228, 0xa0, 0x40);
    p4_video_slot_cmd(ff, 0x228, 0xb0, 0x2e);
    timer_delay_msec(250);
    p4_video_slot_cmd(ff, 0x228, 0xb0, 0x0e);
    p4_video_slot_cmd(ff, 0x222, 0x05, 0x00);
}

static void
p4_control_cold_init(p4_ff_t *ff)
{
    volatile uint8_t *control = VADDR8(ff->control_base);
    volatile uint8_t *ctrl400 = VADDR8(ff->control_base + 0x400);
    volatile uint8_t *mem1000 = VADDR8(ff->mem_base + 0x1000);
    volatile uint8_t *mem0800 = VADDR8(ff->mem_base + 0x0800);
    uint8_t chip;
    uint8_t ctrl404;
    uint8_t revision;
    uint8_t aa_video = 0;

    control[0] = 0x00;
    p4_delay();
    control[0] = 0x00;
    p4_delay();
    control[0] = 0x03;
    p4_delay();

    if (ff->product == PICASSOIV_PRODUCT_Z3FF) {
        *VADDR32(ff->mem_base + 0x1000 + 0x14) = 0xc1030000;
        *VADDR32(ff->mem_base + 0x1000 + 0x10) = 0x08000080;
        *VADDR32(ff->mem_base + 0x1000 + 0x08) = 0x00000003;
        *VADDR32(ff->mem_base + 0x1000 + 0x04) = 0x03000002;
    } else {
        mem1000[0x13] = 0xff;
        p4_delay();
        if (mem1000[0x13] != 0xff) {
            *VADDR32(ff->mem_base + 0x1000 + 0x14) = 0x00800b00;
            ff->post_base += 0x100;
        } else {
            *VADDR32(ff->mem_base + 0x1000 + 0x14) = 0xc0030000;
        }
        *VADDR32(ff->mem_base + 0x1000 + 0x10) = 0x00000080;
        *VADDR32(ff->mem_base + 0x1000 + 0x04) = 0x03000000;
    }

    *VADDR32(ff->mem_base + 0x0800 + 0x14) = 0x00002080;
    *VADDR32(ff->mem_base + 0x0800 + 0x10) = 0x00000080;
    if (ff->product != PICASSOIV_PRODUCT_Z3FF) {
        *VADDR32(ff->mem_base + 0x0800 + 0x1c) = 0x00000080;
        *VADDR32(ff->mem_base + 0x0800 + 0x18) = 0x00800b00;
    }

    chip = mem0800[0x02];
    if (chip == 0x02)
        chip = mem0800[0x08];

    control[0] = 0x07;
    if (chip >= 0x03) {
        control[0x10] = 0x0c;
        control[0x14] = 0x0c;
        control[0x18] = 0x0c;
        control[0x1c] = 0x0c;
    }

    ctrl404 = ctrl400[0x04];
    revision = ctrl404 >> 4;
    if (ctrl404 & 0x04) {
        aa_video = 1;
        if (revision < 4)
            aa_video = 0;
    }
    ff->variant = aa_video;

    ctrl400[0x00] = 0x00;
    ctrl400[0x06] = 0x03;
    p4_delay_count(8);
    (void) p4_ctrl_wait(&ctrl400[0x06], 0x87, 0x87);
    ctrl400[0x06] = 0x01;
    p4_delay_count(8);
    (void) p4_ctrl_wait(&ctrl400[0x06], 0x87, 0x05);
    ctrl400[0x06] = 0x00;
    p4_delay_count(8);
    (void) p4_ctrl_wait(&ctrl400[0x06], 0x87, 0x00);
    ctrl400[0x06] = 0x02;
    p4_delay_count(8);
    (void) p4_ctrl_wait(&ctrl400[0x06], 0x87, 0x82);

    ctrl400[0x06] = 0x00;
    p4_delay_count(8);
    ctrl400[0x06] = 0x01;
    p4_delay_count(8);
    ctrl400[0x06] = 0x03;
    p4_delay_count(8);
}

static void
p4_unlock_extended(volatile uint8_t *regs)
{
    regs[0x3c6] = 0xff;
    p4_delay_count(8);

    seq_write(regs, 0x08, 0x43);
    p4_delay_count(8);
    (void) seq_read(regs, 0x08);
    seq_write(regs, 0x08, 0x41);
    p4_delay_count(8);
    (void) seq_read(regs, 0x08);
    seq_write(regs, 0x08, 0x40);
    p4_delay_count(8);
    (void) seq_read(regs, 0x08);
    seq_write(regs, 0x08, 0x42);
    p4_delay_count(8);
    (void) seq_read(regs, 0x08);

    seq_write(regs, 0x08, 0x40);
    p4_delay_count(8);
    seq_write(regs, 0x08, 0x41);
    p4_delay_count(8);
    seq_write(regs, 0x08, 0x43);
    p4_delay_count(8);
}

static void
p4_vga_cold_init(const p4_ff_t *ff, volatile uint8_t *regs)
{
    static const uint8_t seq_init[][2] = {
        { 0x06, 0x12 }, { 0x01, 0x01 }, { 0x0f, 0x98 },
        { 0x00, 0x03 }, { 0x02, 0xff }, { 0x03, 0x00 },
        { 0x04, 0x0e }, { 0x08, 0x43 }, { 0x16, 0x00 },
        { 0x18, 0x02 }, { 0x0e, 0x65 }, { 0x1e, 0x3b },
        { 0x17, 0x04 }, { 0x12, 0x00 }, { 0x13, 0x3c },
        { 0x1f, 0x2d },
    };
    static const uint8_t crtc_init[][2] = {
        { 0x00, 0x5f }, { 0x01, 0x4f }, { 0x02, 0x50 },
        { 0x03, 0x82 }, { 0x04, 0x54 }, { 0x05, 0x80 },
        { 0x06, 0xbf }, { 0x07, 0x1f }, { 0x08, 0x00 },
        { 0x09, 0xc0 }, { 0x0a, 0x00 }, { 0x0b, 0x00 },
        { 0x0c, 0x00 }, { 0x0d, 0x00 }, { 0x0e, 0x00 },
        { 0x0f, 0x00 }, { 0x10, 0x9c }, { 0x11, 0x3e },
        { 0x12, 0x8f }, { 0x13, 0x50 }, { 0x14, 0x00 },
        { 0x15, 0x96 }, { 0x16, 0xb9 }, { 0x17, 0xc3 },
        { 0x18, 0xff }, { 0x19, 0x00 }, { 0x1a, 0x02 },
        { 0x1b, 0xa2 }, { 0x1c, 0x00 }, { 0x1d, 0x40 },
        { 0x3e, 0x20 }, { 0x3f, 0x01 }, { 0x50, 0x01 },
        { 0x51, 0x00 }, { 0x52, 0x00 }, { 0x53, 0x00 },
        { 0x58, 0x40 }, { 0x5c, 0x00 }, { 0x5d, 0x00 },
        { 0x5e, 0x00 },
    };
    static const uint8_t gc_init[][2] = {
        { 0x00, 0x00 }, { 0x01, 0x00 }, { 0x02, 0x00 },
        { 0x03, 0x00 }, { 0x04, 0x00 }, { 0x05, 0x00 },
        { 0x09, 0x00 }, { 0x0a, 0x00 }, { 0x06, 0x05 },
        { 0x07, 0x0f }, { 0x08, 0xff }, { 0x0b, 0x28 },
        { 0x0e, 0x20 }, { 0x31, 0x04 }, { 0x31, 0x80 },
        { 0x17, 0x0c }, { 0x18, 0x04 }, { 0x19, 0x00 },
    };
    static const uint8_t attr_init[][2] = {
        { 0x30, 0x01 }, { 0x31, 0x00 }, { 0x32, 0x0f },
        { 0x33, 0x00 }, { 0x34, 0x00 },
    };
    uint x;

    regs[0x3c6] = 0xff;
    regs[0x3c2] = 0x6f;
    for (x = 0; x < ARRAY_SIZE(seq_init); x++)
        seq_write(regs, seq_init[x][0], seq_init[x][1]);
    for (x = 0; x < ARRAY_SIZE(crtc_init); x++)
        crtc_write(regs, crtc_init[x][0], crtc_init[x][1]);
    for (x = 0; x < ARRAY_SIZE(gc_init); x++)
        gc_write(regs, gc_init[x][0], gc_init[x][1]);

    (void) regs[0x3da];
    for (x = 0; x < 16; x++) {
        regs[0x3c0] = x;
        p4_delay();
        regs[0x3c0] = x;
        p4_delay();
    }
    (void) regs[0x3da];
    for (x = 0; x < ARRAY_SIZE(attr_init); x++) {
        regs[0x3c0] = attr_init[x][0];
        p4_delay();
        regs[0x3c0] = attr_init[x][1];
        p4_delay();
    }

    seq_write(regs, 0x07, 0x21);
    seq_write(regs, 0x16, 0x00);
    if (ff->seq0f_18)
        seq_write(regs, 0x0f, 0x18);

    regs[0x3c6] = 0x00;
    p4_delay();
    p4_dac_hidden_read4(regs);
    regs[0x3c6] = 0x00;
    p4_delay();
    regs[0x3c6] = 0x00;
    p4_delay();
    p4_wait_input_status(regs, 1);
    p4_wait_input_status(regs, 0);

    regs[0x3c8] = 0x00;
    p4_delay();
    regs[0x3c9] = 0x10;
    p4_delay();
    regs[0x3c9] = 0x10;
    p4_delay();
    regs[0x3c9] = 0x10;
    p4_delay();
    p4_wait_input_status(regs, 1);
    p4_wait_input_status(regs, 0);

    if ((regs[0x3c2] & 0x10) == 0) {
        regs[0x3c8] = 0x00;
        p4_delay();
        regs[0x3c9] = 0x04;
        p4_delay();
        regs[0x3c9] = 0x10;
        p4_delay();
        regs[0x3c9] = 0x04;
        p4_delay();
        p4_wait_input_status(regs, 1);
        p4_wait_input_status(regs, 0);
    }

    p4_unlock_extended(regs);
}

static void
p4_program_vga_timing(volatile uint8_t *regs, const uint16_t timing[12],
                      uint variant)
{
    static const uint8_t cfg[2][4] = {
        { 0x02, 0x01, 0xa0, 0x06 },
        { 0x03, 0x03, 0xe5, 0x04 },
    };
    static const uint8_t crtc_1c[8] = {
        0x00, 0x20, 0x28, 0x30, 0x38, 0x08, 0x10, 0x18,
    };
    const uint8_t *cur_cfg = cfg[variant ? 1 : 0];
    uint8_t old;
    uint8_t value;
    uint x;
    uint shift;

    regs[0x3c6] = 0xff;
    crtc_write(regs, 0x14, crtc_read(regs, 0x14) & 0x9f);
    crtc_write(regs, 0x17, 0xc3);
    gc_write(regs, 0x05, 0x40);

    old = regs[0x3c6];
    regs[0x3c6] = 0x00;
    p4_delay();
    p4_dac_hidden_read4(regs);
    regs[0x3c6] = cur_cfg[2];
    p4_delay();
    regs[0x3c6] = old;
    p4_delay();

    seq_write(regs, 0x02, 0xff);
    seq_write(regs, 0x04, seq_read(regs, 0x04) | 0x08);
    seq_write(regs, 0x07, seq_read(regs, 0x07) | 0x01);

    x = timing[0];
    seq_write(regs, 0x1e, x & 0x3f);
    seq_write(regs, 0x0e, (x >> 8) & 0x7f);
    seq_write(regs, 0x07, (seq_read(regs, 0x07) & 0xf1) | cur_cfg[3]);

    crtc_write(regs, 0x00, (((timing[4] + 3) >> 3) - 5) & 0xff);
    value = (crtc_read(regs, 0x1c) & 0x11) | crtc_1c[timing[4] & 7];
    crtc_write(regs, 0x1c, value);
    crtc_write(regs, 0x01, (((timing[1] + 7) >> 3) - 1) & 0xff);
    crtc_write(regs, 0x04, (timing[2] >> 3) & 0xff);
    crtc_write(regs, 0x1c, timing[2] & 7);
    value = (crtc_read(regs, 0x05) & 0xe0) | ((timing[3] >> 3) & 0x1f);
    crtc_write(regs, 0x05, value);

    shift = (timing[11] & 4) >> 2;

    x = (timing[8] >> shift) - 2;
    crtc_write(regs, 0x06, x & 0xff);
    value = crtc_read(regs, 0x07) & 0xde;
    if (x & 0x100)
        value |= 0x01;
    if (x & 0x200)
        value |= 0x20;
    crtc_write(regs, 0x07, value);

    x = (timing[5] >> shift) - 1;
    crtc_write(regs, 0x12, x & 0xff);
    value = crtc_read(regs, 0x07) & 0xbd;
    if (x & 0x100)
        value |= 0x02;
    if (x & 0x200)
        value |= 0x40;
    crtc_write(regs, 0x07, value);

    x = (timing[6] >> shift) - 1;
    crtc_write(regs, 0x10, x & 0xff);
    value = crtc_read(regs, 0x07) & 0x7b;
    if (x & 0x100)
        value |= 0x04;
    if (x & 0x200)
        value |= 0x80;
    crtc_write(regs, 0x07, value);

    x = (timing[7] >> shift) - 1;
    crtc_write(regs, 0x11, (crtc_read(regs, 0x11) & 0xf0) | (x & 0x0f));

    regs[0x3c2] = ((regs[0x3cc] & 0x2f) | ((timing[11] & 3) << 6)) ^ 0xc0;

    crtc_write(regs, 0x1a, (crtc_read(regs, 0x1a) & 0xfe) | (shift & 1));
    if (shift != 0)
        crtc_write(regs, 0x19, crtc_read(regs, 0x04) >> 1);

    x = (timing[1] * cur_cfg[1]) >> cur_cfg[0];
    crtc_write(regs, 0x13, x & 0xff);
    crtc_write(regs, 0x1b, (crtc_read(regs, 0x1b) & 0xef) |
                         ((x >> 4) & 0x10));

    (void) regs[0x3da];
    regs[0x3c0] = 0x20;
}

static void
p4_program_ff_ext(volatile uint8_t *regs, const uint16_t timing[12],
                  uint capture_mode)
{
    uint x;
    uint8_t value;

    crtc_write(regs, 0x5b, crtc_read(regs, 0x5b) & 0x1f);
    crtc_write(regs, 0x5d, crtc_read(regs, 0x5d) & 0x33);
    crtc_write(regs, 0x59, 0x00);
    crtc_write(regs, 0x5a, 0x00);
    crtc_write(regs, 0x58, crtc_read(regs, 0x58) & 0x60);

    x = (((capture_mode + 2) * timing[9]) & 0x0fff) >> 3;
    crtc_write(regs, 0x3d, x & 0xff);
    value = (crtc_read(regs, 0x3c) & 0x0f) | ((x & 0x100) ? 0x20 : 0x00);
    crtc_write(regs, 0x3c, value);
    crtc_write(regs, 0x13, x & 0xff);
    value = (crtc_read(regs, 0x1b) & 0xef) | ((x & 0x100) ? 0x10 : 0x00);
    crtc_write(regs, 0x1b, value);

    x = (timing[10] & 0x03ff) >> 1;
    crtc_write(regs, 0x57, x & 0xff);
    value = crtc_read(regs, 0x58) & 0x4f;
    if (x & 0x100)
        value |= 0x20;
    crtc_write(regs, 0x58, value);

    crtc_write(regs, 0x5b, 0x00);
    crtc_write(regs, 0x51, 0x01);
    crtc_write(regs, 0x56, 0x00);
    crtc_write(regs, 0x54, 0x00);
    crtc_write(regs, 0x3e, 0x00);
    crtc_write(regs, 0x50, capture_mode ? 0x0a : 0x12);
    crtc_write(regs, 0x51, 0x09);
}

static void
p4_post_init(volatile uint32_t *post)
{
    post[0] = 0x00000000;
    post[1] = 0x00000000;
    post[2] = 0xff0fff03;
    post[3] = 0x00100010;
    post[5] = 0x00000000;
    post[7] = 0x00000000;
    post[6] = 0x00000000;
    p4_delay();
    post[4] = 0x00000000;
    p4_delay();
}

static void
p4_route_native_video(volatile uint8_t *regs)
{
    crtc_clear(regs, 0x09, 0x80);
    crtc_set(regs, 0x50, 0x40);
}

static void
p4_enable(const p4_ff_t *ff)
{
    p4_ff_t state = *ff;
    volatile uint8_t *control = VADDR8(state.control_base);
    volatile uint8_t *regs = VADDR8(state.regs_base);
    volatile uint32_t *post;
    const uint16_t *timing;
    uint capture_mode;

    timing = (vid_type == VID_PAL) ? p4_ff_pal : p4_ff_ntsc;
    capture_mode = state.variant;

    p4_control_cold_init(&state);
    p4_program_rgb_loads(&state);
    p4_program_video_slot_config(&state);
    p4_vga_cold_init(&state, regs);
    post = VADDR32(state.post_base);

    p4_post_init(post);

    p4_program_vga_timing(regs, timing, state.variant);

    control[0x404] = (((state.variant ^ 1) << 1) | 1);
    p4_delay();

    p4_program_ff_ext(regs, timing, capture_mode);

    p4_post_init(post);

    p4_route_native_video(regs);
    p4_program_rgb_load_post(&state);
}

static int
p4_find_ff(p4_ff_t *ff)
{
    autoconfig_dev_t dev;

    if (autoconfig_find(PICASSOIV_MFG, PICASSOIV_PRODUCT_Z2FF, &dev)) {
        ff->control_base = dev.ac_addr;
        ff->regs_base = dev.ac_addr + 0x00010000;
        ff->post_base = dev.ac_addr + 0x00008000;
        ff->mem_base = dev.ac_addr;
        ff->variant = 0;
        ff->seq0f_18 = 1;
        ff->product = dev.ac_product;
        return (1);
    }

    if (autoconfig_find(PICASSOIV_MFG, PICASSOIV_PRODUCT_Z3FF, &dev)) {
        ff->control_base = dev.ac_addr;
        ff->regs_base = dev.ac_addr + 0x00600000;
        ff->post_base = dev.ac_addr + 0x00200000;
        ff->mem_base = dev.ac_addr + 0x00400000;
        ff->variant = 0;
        ff->seq0f_18 = 0;
        ff->product = dev.ac_product;
        return (1);
    }

    return (0);
}

rc_t
picassoiv_enable_flicker_fixer(void)
{
    p4_ff_t ff;

    if (!p4_find_ff(&ff))
        return (RC_NO_DATA);

    if (ff.control_base >= 0x10000000)
        cache_data_noncache_16m(ff.control_base);

    printf("PicassoIV flicker fixer: product 0x%02x regs=%08x %s\n",
           ff.product, ff.regs_base, (vid_type == VID_PAL) ? "PAL" : "NTSC");
    p4_enable(&ff);
    return (RC_SUCCESS);
}
