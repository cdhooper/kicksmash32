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
    regs[0x3c6] = 0xff;             // Palette Pixel Mask
    p4_delay_count(8);

    seq_write(regs, 0x08, 0x43);    // SR8 DDC2B DDCDAT=1 DDCCLK=1
    p4_delay_count(8);
    (void) seq_read(regs, 0x08);
    seq_write(regs, 0x08, 0x41);    // SR8 DDC2B DDCDAT=0 DDCCLK=1
    p4_delay_count(8);
    (void) seq_read(regs, 0x08);
    seq_write(regs, 0x08, 0x40);    // SR8 DDC2B DDCDAT=0 DDCCLK=0
    p4_delay_count(8);
    (void) seq_read(regs, 0x08);
    seq_write(regs, 0x08, 0x42);    // SR8 DDC2B DDCDAT=1 DDCCLK=0
    p4_delay_count(8);
    (void) seq_read(regs, 0x08);

    seq_write(regs, 0x08, 0x40);    // SR8 DDC2B DDCDAT=0 DDCCLK=0
    p4_delay_count(8);
    seq_write(regs, 0x08, 0x41);    // SR8 DDC2B DDCDAT=0 DDCCLK=1
    p4_delay_count(8);
    seq_write(regs, 0x08, 0x43);    // SR8 DDC2B DDCDAT=1 DDCCLK=1
    p4_delay_count(8);
}

static void
p4_vga_cold_init(const p4_ff_t *ff, volatile uint8_t *regs)
{
    static const uint8_t seq_init[][2] = {
        { 0x06, 0x12 },  // SR6 Key [4]=1=Unlock, [1]=1=Unlock
        { 0x01, 0x01 },  // SR1 Seq Clock Mode [0]=1=Div8 Dot Clock
        { 0x0f, 0x98 },  // SRF DRAM Ctrl [4:3]=11=32-bit, [7]=1=Bank Switching
        { 0x00, 0x03 },  // SR0 Sequencer Reset [0]=1=No Reset [1]=1=No Reset
        { 0x02, 0xff },  // SR2 Sequencer [3:0]=1111 Plane Mask Enable all
        { 0x03, 0x00 },  // SR3 Sequencer Char Map, Offset Pri=0K, Sec=0K
        { 0x04, 0x0e },  // SR4 Sequencer Memory Mode: [1]=AllMem [3]=UseA0
        { 0x08, 0x43 },  // SR8 EEPROM Ctrl: [6]=1=DDC2B [3]=DI [1]=Dat [0]=Clk
        { 0x16, 0x00 },  // SR16 Display FIFO Threshold [3:0]
        { 0x18, 0x02 },  // SR18 Signature Gen [1:0] Byte Select=10=2
        { 0x0e, 0x65 },  // SRE  VCLK3 Numerator   [6:0]=65 = d'101
        { 0x1e, 0x3b },  // SR1E VCLK3 Denominator [7:1]=0011101=d'29, [0]=P=1
                         //          14.31826 MHz * 101 / 29 / 2 = 24.93 MHz
        { 0x17, 0x04 },  // SR17 Conf Extended Ctrl [2]=1=Enable Mem-Mapped I/O
        { 0x12, 0x00 },  // SR12 Graphics [0]=0 Cursor Disable
        { 0x13, 0x3c },  // SR13 Graphics Cursor [5:0]=111100
        { 0x1f, 0x2d },  // SR1F MCLK [5:0]=101101=d'43
                         //      Ref=14.31826; MCLK=SR1F * Ref / 8 = 76.960 MHz
    };
    static const uint8_t crtc_init[][2] = {
        { 0x00, 0x5f },  // CR0 Horizontal Total = d'95
        { 0x01, 0x4f },  // CR1 Horz Display End = d'78
        { 0x02, 0x50 },  // CR2 Horz Blank Start = d'80
        { 0x03, 0x82 },  // CR3 Horz Blank End   = d'2   [7]=Compat Read
        { 0x04, 0x54 },  // CR4 Horz Sync Start  = d'84
        { 0x05, 0x80 },  // CR5 Horz Sync End=0, Sync Delay=0, Blank End[5]=1
        { 0x06, 0xbf },  // CR6 Vert Total       = d'191 + 0x100 = d'447
        { 0x07, 0x1f },  // CR7 Overflow bits [8]=1 for CR6 CR12 CR10 CR15 CR18
        { 0x08, 0x00 },  // CR8 Screen A Preset RScan
        { 0x09, 0xc0 },  // CR9 Scan Double, Line Compare[9]=1
        { 0x0a, 0x00 },  // CRA Text Cursor Start
        { 0x0b, 0x00 },  // CRB Text Cursor End
        { 0x0c, 0x00 },  // CRC Screen Addr Start [15:8]
        { 0x0d, 0x00 },  // CRD Screen Addr Start [7:0]
        { 0x0e, 0x00 },  // CRE Text Cursor Loc [15:8]
        { 0x0f, 0x00 },  // CRF Text Cursor Loc [7:0]
        { 0x10, 0x9c },  // CR10 Vert Sync Start [7:0] + 0x100 = d'412
        { 0x11, 0x3e },  // CR11 Vert Sync End [3:0]=110, Disable/Clar Vert IRS
        { 0x12, 0x8f },  // CR12 Vert Display End [7:0] + 0x100 = d'389
        { 0x13, 0x50 },  // CR13 Offset (Pirch) = d'80
        { 0x14, 0x00 },  // CR14 Underline Row Scanline
        { 0x15, 0x96 },  // CR15 Vert Blank Start [7:0] + 0x100 = d'406
        { 0x16, 0xb9 },  // CR16 Vert Blank End [7:0] + 0x100 = d'406
        { 0x17, 0xc3 },  // CR17 Mode Control: Timing En, Byte Mode, CGA Support
        { 0x18, 0xff },  // CR18 Line Compare = 0x3ff d'1023
        { 0x19, 0x00 },  // CR19 Interface End
        { 0x1a, 0x02 },  // CR1A Misc Control [1]=En Dbl Buffered Display Addr
        { 0x1b, 0xa2 },  // CR1B Ext Control  [1]=Ext Addr Wrap, [5]=Blank Ctrl
        { 0x1c, 0x00 },  // CR1C Sync Adjust / Genlock
        { 0x1d, 0x40 },  // CR1D Overlay Ext Control: [5]=Overlay Timing Select
        { 0x3e, 0x20 },  // CR3E Video Win Master Ctrl: [5]=Error Diffision En
        { 0x3f, 0x01 },  // CR3F Misc Ctrl: [0]=Auto-Decimation Mem Page Bit
        { 0x50, 0x01 },  // CR50 Video Cap [1:0]=1=Reserved
        { 0x51, 0x00 },  // CR51 Video Cap Data Fmt [2:0]=000=YUV16
        { 0x52, 0x00 },  // CR52 Video Cap Horz Data Reduct = Disable
        { 0x53, 0x00 },  // CR53 Video Cap Vert Data Reduct = Disable
        { 0x58, 0x40 },  // CR58 Video Cap Misc Ctrl [6]=Reverse Odd/Even Sense
        { 0x5c, 0x00 },  // CR5C Luminance-Only Control
        { 0x5d, 0x00 },  // CR5D Window Pixel Alignment
        { 0x5e, 0x00 },  // CR5E Double Buffer Ctrl
    };
    static const uint8_t gc_init[][2] = {
        { 0x00, 0x00 },  // GR0 Reset Background color [3:0] Plane
        { 0x01, 0x00 },  // GR1 Enable Foreground color [3:0] Plane
        { 0x02, 0x00 },  // GR2 Color Compare [3:0] Plane
        { 0x03, 0x00 },  // GR3 Data Rotate [2:0] Count, [4:3] Function Select
        { 0x04, 0x00 },  // GR4 Read Map Select [1:0] Plane
        { 0x05, 0x00 },  // GR5 Graphics Controller Mode
        { 0x09, 0x00 },  // GR9 Offset Register 0 [7:0]
        { 0x0a, 0x00 },  // GRa Offset Register 1 [7:0]
        { 0x06, 0x05 },  // GR6 Misc [0]=1=VGA Graphics, [3:2]=01=Extended Map
        { 0x07, 0x0f },  // GR7 Color Don't Care [3:0] Plane
        { 0x08, 0xff },  // GR8 Write Enable [7:0]
        { 0x0b, 0x28 },  // GRB CRTC Text Crsr End[4:0]=01000=d'8, Skew[6:5]=01
        { 0x0e, 0x20 },  // GRE Power Mgmt [5]=Enable Write to GR33
        { 0x31, 0x04 },  // GR31 BLT Start [4]=1=Reset
        { 0x31, 0x80 },  // GR31 BLT Start [7]=1=Enable Autostart
        { 0x17, 0x0c },  // GR17 Active Display [2]=1=No INTR#, [3]=1=No Feature
        { 0x18, 0x04 },  // GR18 [2]=1=Enable 8-MCLK EDO Timing
        { 0x19, 0x00 },  // GR19 GPIO Port Config
    };
    static const uint8_t attr_init[][2] = {
        /* 0x20 in index is Display Enable */
        { 0x30, 0x01 },  // AR0 Palette 0 = Dark Blue
        { 0x31, 0x00 },  // AR1 Palette 1 = Black
        { 0x32, 0x0f },  // AR2 Palette 2 = Light Blue
        { 0x33, 0x00 },  // AR3 Palette 3 = Black
        { 0x34, 0x00 },  // AR4 Palette 4 = Black
    };
    uint x;

    regs[0x3c6] = 0xff;  // Palette Pixel mask
    regs[0x3c2] = 0x6f;  // Select Clock (VCLK3, CRTC at 0x3da)
    for (x = 0; x < ARRAY_SIZE(seq_init); x++)
        seq_write(regs, seq_init[x][0], seq_init[x][1]);
    for (x = 0; x < ARRAY_SIZE(crtc_init); x++)
        crtc_write(regs, crtc_init[x][0], crtc_init[x][1]);
    for (x = 0; x < ARRAY_SIZE(gc_init); x++)
        gc_write(regs, gc_init[x][0], gc_init[x][1]);

    /* Initialize 16-color palette to unique colors */
    (void) regs[0x3da];     // Force 0x3c0 toggle to Index
    for (x = 0; x < 16; x++) {
        regs[0x3c0] = x;    // Index
        p4_delay();
        regs[0x3c0] = x;    // Data
        p4_delay();
    }

    /* Load some elements of 16-color palette */
    (void) regs[0x3da];     // Force 0x3c0 toggle to Index
    for (x = 0; x < ARRAY_SIZE(attr_init); x++) {
        regs[0x3c0] = attr_init[x][0];  // Index
        p4_delay();
        regs[0x3c0] = attr_init[x][1];  // Data
        p4_delay();
    }

    seq_write(regs, 0x07, 0x21);  // SR7 Ext Seq Mode [0]=En Ext Disp [5]=En FB1
    seq_write(regs, 0x16, 0x00);  // SR16 Display FIFO Threshold [3:0]=0
    if (ff->seq0f_18)
        seq_write(regs, 0x0f, 0x18);  // SRF DRAM CTRL [4:3]=11 Bus Width=64-bit

    /* XXX: Should call p4_dac_hidden_write() here */
    regs[0x3c6] = 0x00; // Begin sequence
    p4_delay();
    p4_dac_hidden_read4(regs);
    regs[0x3c6] = 0x00; // Hidden DAC=00 Disable Extended modes, 5:5:5, VGA
    p4_delay();
    regs[0x3c6] = 0x00; // Hidden DAC=00 (Bug workaround? Just to be sure??)
    p4_delay();
    /* XXX: Should call p4_dac_hidden_write() here */

    p4_wait_input_status(regs, 1);
    p4_wait_input_status(regs, 0);

    /* Gray */
    regs[0x3c8] = 0x00;  // Palette Address (write-only)
    p4_delay();
    regs[0x3c9] = 0x10;  // Palette Data: Red=0x10
    p4_delay();
    regs[0x3c9] = 0x10;  // Palette Data: Green=0x10
    p4_delay();
    regs[0x3c9] = 0x10;  // Palette Data: Blue=0x10
    p4_delay();
    p4_wait_input_status(regs, 1);
    p4_wait_input_status(regs, 0);

    if ((regs[0x3c2] & 0x10) == 0) {  // Input Status Register 0 [4]=DAC Sensing
        /* Dark green */
        regs[0x3c8] = 0x00; // Palette Address (write-only)
        p4_delay();
        regs[0x3c9] = 0x04; // Palette Data: Red=0x04
        p4_delay();
        regs[0x3c9] = 0x10; // Palette Data: Green=0x10
        p4_delay();
        regs[0x3c9] = 0x04; // Palette Data: Blue=0x04
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
        {           // VARIANT 0
            0x02,   // CR13 Timing divisor = 4
            0x01,   // CR13 CRTC Offset (putch) = 1 * timing / 4
            0xa0,   // Hidden DAC=ExtColor, Clk2, 5:5:5 Sierra
            0x06    // SR7 Ext Seq [3:1]=011=16bpp
        }, {        // VARIANT 1 (aa_video)
            0x03,   // CR13 Timing divisor = 8
            0x03,   // CR13 CRTC Offset (putch) = 3 * timing / 8
            0xe5,   // Hidden DAC=ExtColor, Ext, Clk2, 8:8:8 16M
            0x04    // SR7 Ext Seq [3:1]=010=24bpp
        },
    };
    static const uint8_t crtc_1c[8] = {  // CR1C Horz Total Adjust
        0x00,  // [5:3]=000 =  0 VCLKs (normal)
        0x20,  // [5:3]=100 = +1 VCLKs
        0x28,  // [5:3]=101 = +2 VCLKs
        0x30,  // [5:3]=110 = +3 VCLKs
        0x38,  // [5:3]=110 = +3 VCLKs
        0x08,  // [5:3]=001 = -3 VCLKs
        0x10,  // [5:3]=010 = -2 VCLKs
        0x18,  // [5:3]=011 = -1 VCLKs
    };
    const uint8_t *cur_cfg = cfg[variant ? 1 : 0];
    uint8_t old;
    uint8_t value;
    uint x;
    uint shift;

    regs[0x3c6] = 0xff;        // Palette Pixel Mask
    crtc_write(regs, 0x14, crtc_read(regs, 0x14) & 0x9f);
                               // CR14 Disable DoubleWord Mode and CounntByFour
    crtc_write(regs, 0x17, 0xc3); // CR17 Timing Enable, Byte Mode, CGA Support
    gc_write(regs, 0x05, 0x40);   // GR5 [6]=256-color Mode

    /* XXX: Should turn below into p4_dac_hidden_write() */
    old = regs[0x3c6];         // Save Palette Pixel Mask
    regs[0x3c6] = 0x00;        // Begin sequence
    p4_delay();
    p4_dac_hidden_read4(regs);
    regs[0x3c6] = cur_cfg[2];  // Hidden DAC=0xa0 ExtColor, Clk2, 5:5:5 Sierra
                               // Hidden DAC=0xe5 ExtColor, Ext, Clk2, 8:8:8 16M
    p4_delay();
    regs[0x3c6] = old;         // Palette Pixel Mask
    p4_delay();
    /* XXX: Should turn above into p4_dac_hidden_write() */

    seq_write(regs, 0x02, 0xff);            // SR2 Seq Plane [3:0]=1111 En All
    seq_write(regs, 0x04, seq_read(regs, 0x04) | 0x08);
                                            // SR4 Seq Mode [3]=Use A0
    seq_write(regs, 0x07, seq_read(regs, 0x07) | 0x01);
                                            // SR7 Ext Seq [0]=En Packed

    x = timing[0];                          // 0x1c1c both PAL and NTSC
    seq_write(regs, 0x1e, x & 0x3f);        // SR1E VCLK3 Denmoninator
    seq_write(regs, 0x0e, (x >> 8) & 0x7f); // SRE VCLK3 Numerator
    seq_write(regs, 0x07, (seq_read(regs, 0x07) & 0xf1) | cur_cfg[3]);
                                    // SR7 Ext Seq [3:1]=011=16bpp or 010=24bpp

    crtc_write(regs, 0x00, (((timing[4] + 3) >> 3) - 5) & 0xff);
                                        // CR0 CRTC Horizontal Total
    value = (crtc_read(regs, 0x1c) & 0x11) | crtc_1c[timing[4] & 7];
    crtc_write(regs, 0x1c, value);      // CR1C Horz Total Adjust
    crtc_write(regs, 0x01, (((timing[1] + 7) >> 3) - 1) & 0xff);
                                        // CR1 CRTC Horz Display End
    crtc_write(regs, 0x04, (timing[2] >> 3) & 0xff);
                                        // CR4 Horz Sync Start
    crtc_write(regs, 0x1c, timing[2] & 7);
    value = (crtc_read(regs, 0x05) & 0xe0) | ((timing[3] >> 3) & 0x1f);
    crtc_write(regs, 0x05, value);      // CR5 Horz Sync End

    shift = (timing[11] & 4) >> 2;

    x = (timing[8] >> shift) - 2;
    crtc_write(regs, 0x06, x & 0xff);   // CR6 Vert Total [7:0]
    value = crtc_read(regs, 0x07) & 0xde;
    if (x & 0x100)
        value |= 0x01;
    if (x & 0x200)
        value |= 0x20;
    crtc_write(regs, 0x07, value);      // CR7 Vert Total [9,8]

    x = (timing[5] >> shift) - 1;
    crtc_write(regs, 0x12, x & 0xff);   // CR12 Vert Display End [7:0]
    value = crtc_read(regs, 0x07) & 0xbd;
    if (x & 0x100)
        value |= 0x02;
    if (x & 0x200)
        value |= 0x40;
    crtc_write(regs, 0x07, value);      // CR7 Vert Display End [9,8]

    x = (timing[6] >> shift) - 1;
    crtc_write(regs, 0x10, x & 0xff);   // CR10 Vert Sync Start [7:0]
    value = crtc_read(regs, 0x07) & 0x7b;
    if (x & 0x100)
        value |= 0x04;
    if (x & 0x200)
        value |= 0x80;
    crtc_write(regs, 0x07, value);      // CR7 Vert Sync Start [9,8]

    x = (timing[7] >> shift) - 1;
    crtc_write(regs, 0x11, (crtc_read(regs, 0x11) & 0xf0) | (x & 0x0f));
                                        // CR11 Vert Sync End [3:0]

    regs[0x3c2] = ((regs[0x3cc] & 0x2f) | ((timing[11] & 3) << 6)) ^ 0xc0;
                                        // Misc Sync Polarity [6]=Horz [7]=Vert

    crtc_write(regs, 0x1a, (crtc_read(regs, 0x1a) & 0xfe) | (shift & 1));
                                        // CR1A Misc Enable Interlaced
    if (shift != 0)
        crtc_write(regs, 0x19, crtc_read(regs, 0x04) >> 1);
                                        // CR19 Interlace End = CR4 H Sync Start

    x = (timing[1] * cur_cfg[1]) >> cur_cfg[0];
    crtc_write(regs, 0x13, x & 0xff);   // CR13 CRTC Offset (pitch) [7:0]
    crtc_write(regs, 0x1b, (crtc_read(regs, 0x1b) & 0xef) |
                         ((x >> 4) & 0x10));
                                        // CR1B CRTC Offset (pitch) [8]

    (void) regs[0x3da];                 // Force 0x3c0 toggle to Index
    regs[0x3c0] = 0x20;                 // Turn display on (undocumented?)
                                        // by leaving it pointing to data
}

static void
p4_program_ff_ext(volatile uint8_t *regs, const uint16_t timing[12],
                  uint capture_mode)
{
    uint x;
    uint8_t value;

    crtc_write(regs, 0x5b, crtc_read(regs, 0x5b) & 0x1f);  // CR5B Brightness
    crtc_write(regs, 0x5d, crtc_read(regs, 0x5d) & 0x33);  // Start Addr [1:0]
    crtc_write(regs, 0x59, 0x00);         // CR59 Video Buf 2 Start Addr [9:3]
    crtc_write(regs, 0x5a, 0x00);         // CR5A Video Buf 2 Start Addr [17:10]
    crtc_write(regs, 0x58, crtc_read(regs, 0x58) & 0x60);  // Start Addr [21:18]

    x = (((capture_mode + 2) * timing[9]) & 0x0fff) >> 3;
    crtc_write(regs, 0x3d, x & 0xff);  // CR3D Video Buffer Addr [10:3]
    value = (crtc_read(regs, 0x3c) & 0x0f) | ((x & 0x100) ? 0x20 : 0x00);
    crtc_write(regs, 0x3c, value);     // CR3C Video Buffer Addr [3:0]
    crtc_write(regs, 0x13, x & 0xff);  // CR13 CRTC Offset (display pitch) [7:0]
    value = (crtc_read(regs, 0x1b) & 0xef) | ((x & 0x100) ? 0x10 : 0x00);
    crtc_write(regs, 0x1b, value);     // CR1B CRTC Offset (display pitch) [8]

    x = (timing[10] & 0x03ff) >> 1;
    crtc_write(regs, 0x57, x & 0xff);  // CR57 Capture height [7:0]
    value = crtc_read(regs, 0x58) & 0x4f;
    if (x & 0x100)
        value |= 0x20;
    crtc_write(regs, 0x58, value); // CR5B Capture Height [8]

    crtc_write(regs, 0x5b, 0x00);  // CR5B Brightness
    crtc_write(regs, 0x51, 0x01);  // CR51 Video Capture [2:0]001=RGB16
    crtc_write(regs, 0x56, 0x00);  // CR56 Capture Vert Delay 0=no delay
    crtc_write(regs, 0x54, 0x00);  // CR54 Capture Horz Delay 0=no delay
    crtc_write(regs, 0x3e, 0x00);  // CR3E Vid Win Control [0]=0 Disable
    crtc_write(regs, 0x50, capture_mode ? 0x0a : 0x12);  // CR50 Capture Control
                                   // Capture Control [4]=16-bit capture port
                                   // Capture Control [3]=Double Edge
                                   // Capture Control [1:0]=10=Rising HREF edge
    crtc_write(regs, 0x51, 0x09);  // CR51 Video Capture: Enable Capture RGB16
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
    crtc_clear(regs, 0x09, 0x80);  // CR9 CTRC Scan Double disabled [7] = 0
    crtc_set(regs, 0x50, 0x40);    // CR50 Video Capture [6]=1 Interlaced
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
        ff->regs_base = dev.ac_addr + 0x00010000;  // GD5446 registers
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
