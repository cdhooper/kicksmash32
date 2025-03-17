/*
 * Amiga chipset registers
 *
 * This header file is part of the code base for a simple Amiga ROM
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
#ifndef _AMIGA_CHIPSET_H
#define _AMIGA_CHIPSET_H

/* Amiga chipset registers */
#define GARY_BTIMEOUT  VADDR8(0x00de0000)  // Timeout type: 1=BERR 0=DSACK (b7)
#define GARY_BTOENB    VADDR8(0x00de0001)  // Timeout enable: 1=enable (bit 7)
#define COLDSTART      VADDR8(0x00de0002)  // Coldstart (bit 7)
#define DSPRST         VADDR8(0x00de1000)  // DSP reset line (bit 7)
#define KBRSTEN        VADDR8(0x00de1001)  // Keyboard reset control (bit 7)
#define GARY_ID        VADDR8(0x00de1002)  // Gary ID code
#define RAMSEY_CONTROL VADDR8(0x00de0003)  // Ramsey control
#define RAMSEY_VERSION VADDR8(0x00de0043)  // Ramsey version

/* Amiga graphics registers */
#define DMACONR   VADDR16(0x00dff002)  // DMA enable register (read)
#define VPOSR     VADDR32(0x00dff004)  // Light pen position high (read)
#define VHPOSR    VADDR16(0x00dff006)  // Light pen position low (read)
#define JOY0DAT   VADDR16(0x00dff00a)  // Counter for digital mouse input port 0
#define JOY1DAT   VADDR16(0x00dff00e)  // Counter for digital mouse input port 1
#define POT0DAT   VADDR16(0x00dff012)  // Counter for proportional input port 0
#define POT1DAT   VADDR16(0x00dff014)  // Counter for proportional input port 1
#define POTGOR    VADDR16(0x00dff016)  // Prop. pin and start counters (read)
#define POTGO     VADDR16(0x00dff034)  // Prop. pin and start counters (write)
#define ADKCONR   VADDR16(0x00dff010)  // Audio, disk control (read)
#define SERDATR   VADDR16(0x00dff018)  // Serial data and status (read)
#define SERDAT    VADDR16(0x00dff030)  // Serial port data stop bits (write)
#define SERPER    VADDR16(0x00dff032)  // Serial port period and control (write)
#define INTENAR   VADDR16(0x00dff01c)  // Interrupt enable register (read)
#define INTENA    VADDR16(0x00dff09a)  // Interrupt enable register (write)
#define INTREQR   VADDR16(0x00dff01e)  // Interrupt status register (read)
#define INTREQ    VADDR16(0x00dff09c)  // Interrupt status register (write)
#define DIWSTRT   VADDR16(0x00dff08e)  // Start of screen window
#define DIWSTOP   VADDR16(0x00dff090)  // End of screen window
#define DDFSTRT   VADDR16(0x00dff092)  // Bitplane DMA start
#define DDFSTOP   VADDR16(0x00dff094)  // Bitplane DMA end
#define DMACON    VADDR16(0x00dff096)  // DMA enable register (write)
#define BPL1PT    VADDR32(0x00dff0e0)  // Address of bitplane 1, bits 1-20
#define BPL1PTH   VADDR16(0x00dff0e0)  // Address of bitplane 1, bits 16-20
#define BPL1PTL   VADDR16(0x00dff0e2)  // Address of bitplane 1, bits 1-15
#define BPL2PT    VADDR32(0x00dff0e4)  // Address of bitplane 2, bits 1-20
#define BPL2PTH   VADDR16(0x00dff0e4)  // Address of bitplane 2, bits 16-20
#define BPL2PTL   VADDR16(0x00dff0e6)  // Address of bitplane 2, bits 1-15
#define BPL3PT    VADDR32(0x00dff0e8)  // Address of bitplane 3, bits 1-20
#define BPL3PTH   VADDR16(0x00dff0e8)  // Address of bitplane 3, bits 16-20
#define BPL3PTL   VADDR16(0x00dff0ea)  // Address of bitplane 3, bits 1-15
#define BPL4PT    VADDR32(0x00dff0ec)  // Address of bitplane 4, bits 1-20
#define BPL4PTH   VADDR16(0x00dff0ec)  // Address of bitplane 4, bits 16-20
#define BPL4PTL   VADDR16(0x00dff0ee)  // Address of bitplane 4, bits 1-15
#define BPL5PT    VADDR32(0x00dff0f0)  // Address of bitplane 5, bits 1-20
#define BPL5PTH   VADDR16(0x00dff0f0)  // Address of bitplane 5, bits 16-20
#define BPL5PTL   VADDR16(0x00dff0f2)  // Address of bitplane 5, bits 1-15
#define BPL6PT    VADDR32(0x00dff0f4)  // Address of bitplane 6, bits 1-20
#define BPL6PTH   VADDR16(0x00dff0f4)  // Address of bitplane 6, bits 16-20
#define BPL6PTL   VADDR16(0x00dff0f6)  // Address of bitplane 6, bits 1-15
#define BPL1MOD   VADDR16(0x00dff108)  // Odd-numbered bitplane modulo
#define BPL2MOD   VADDR16(0x00dff10A)  // Even-numbered bitplane modulo
#define BLTCON0   VADDR16(0x00dff040)  // Blitter control register 0
#define BLTCON1   VADDR16(0x00dff042)  // Blitter control register 1
#define BLTAFWM   VADDR16(0x00dff044)  // Blitter first word mask for source A
#define BLTALWM   VADDR16(0x00dff046)  // Blitter last word mask for source A
#define BLTAPT    VADDR32(0x00dff050)  // Blitter pointer to source A
#define BLTAPTH   VADDR32(0x00dff050)  // Blitter pointer to source A (hi)
#define BLTAPTL   VADDR16(0x00dff052)  // Blitter pointer to source A (lo)
#define BLTBPT    VADDR32(0x00dff04c)  // Blitter pointer to source B
#define BLTBPTH   VADDR32(0x00dff04c)  // Blitter pointer to source B (hi)
#define BLTBPTL   VADDR16(0x00dff04e)  // Blitter pointer to source B (lo)
#define BLTCPT    VADDR32(0x00dff048)  // Blitter pointer to source C
#define BLTCPTH   VADDR32(0x00dff048)  // Blitter pointer to source C (hi)
#define BLTCPTL   VADDR16(0x00dff04a)  // Blitter pointer to source C (lo)
#define BLTDPT    VADDR32(0x00dff054)  // Blitter pointer to destination D
#define BLTDPTH   VADDR32(0x00dff054)  // Blitter pointer to destination D (hi)
#define BLTDPTL   VADDR16(0x00dff056)  // Blitter pointer to destination D (lo)
#define BLTSIZE   VADDR16(0x00dff058)  // Blitter start and size (window w & h)
#define BLTAMOD   VADDR16(0x00dff064)  // Blitter modulo for source A
#define BLTBMOD   VADDR16(0x00dff062)  // Blitter modulo for source B
#define BLTCMOD   VADDR16(0x00dff060)  // Blitter modulo for source C
#define BLTDMOD   VADDR16(0x00dff066)  // Blitter modulo for destination D
#define BLTADAT   VADDR16(0x00dff074)  // Blitter source A data register
#define BLTBDAT   VADDR16(0x00dff072)  // Blitter source B data register
#define BLTCDAT   VADDR16(0x00dff070)  // Blitter source C data register
#define AUD0LC    VADDR32(0x00dff0a0)  // Audio channel 0 location
#define AUD0LCH   VADDR32(0x00dff0a0)  // Audio channel 0 location (hi)
#define AUD0LCL   VADDR16(0x00dff0a2)  // Audio channel 0 location (lo)
#define AUD0LEN   VADDR16(0x00dff0a4)  // Audio channel 0 length
#define AUD0PER   VADDR16(0x00dff0a6)  // Audio channel 0 period
#define AUD0VOL   VADDR16(0x00dff0a8)  // Audio channel 0 volume
#define AUD0DAT   VADDR16(0x00dff0aa)  // Audio channel 0 data
#define AUD1LC    VADDR32(0x00dff0b0)  // Audio channel 1 location
#define AUD1LCH   VADDR32(0x00dff0b0)  // Audio channel 1 location (hi)
#define AUD1LCL   VADDR16(0x00dff0b2)  // Audio channel 1 location (lo)
#define AUD1LEN   VADDR16(0x00dff0b4)  // Audio channel 1 length
#define AUD1PER   VADDR16(0x00dff0b6)  // Audio channel 1 period
#define AUD1VOL   VADDR16(0x00dff0b8)  // Audio channel 1 volume
#define AUD1DAT   VADDR16(0x00dff0ba)  // Audio channel 1 data
#define AUD2LC    VADDR32(0x00dff0c0)  // Audio channel 2 location
#define AUD2LCH   VADDR32(0x00dff0c0)  // Audio channel 2 location (hi)
#define AUD2LCL   VADDR16(0x00dff0c2)  // Audio channel 2 location (lo)
#define AUD2LEN   VADDR16(0x00dff0c4)  // Audio channel 2 length
#define AUD2PER   VADDR16(0x00dff0c6)  // Audio channel 2 period
#define AUD2VOL   VADDR16(0x00dff0c8)  // Audio channel 2 volume
#define AUD2DAT   VADDR16(0x00dff0ca)  // Audio channel 2 data
#define AUD3LC    VADDR32(0x00dff0d0)  // Audio channel 3 location
#define AUD3LCH   VADDR32(0x00dff0d0)  // Audio channel 3 location (hi)
#define AUD3LCL   VADDR16(0x00dff0d2)  // Audio channel 3 location (lo)
#define AUD3LEN   VADDR16(0x00dff0d4)  // Audio channel 3 length
#define AUD3PER   VADDR16(0x00dff0d6)  // Audio channel 3 period
#define AUD3VOL   VADDR16(0x00dff0d8)  // Audio channel 3 volume
#define AUD3DAT   VADDR16(0x00dff0da)  // Audio channel 3 data
#define BPLCON0   VADDR16(0x00dff100)  // Bitplane Control Register 0
#define BPLCON1   VADDR16(0x00dff102)  // Bitplane Control Register 1
#define BPLCON2   VADDR16(0x00dff104)  // Bitplane Control Register 2
#define BPLCON3   VADDR16(0x00dff106)  // Bitplane Control Register 3
#define BPL1DAT   VADDR16(0x00dff110)  // Bitplane 1 data
#define BPL2DAT   VADDR16(0x00dff112)  // Bitplane 2 data
#define BPL3DAT   VADDR16(0x00dff114)  // Bitplane 3 data
#define BPL4DAT   VADDR16(0x00dff116)  // Bitplane 4 data
#define BPL5DAT   VADDR16(0x00dff118)  // Bitplane 5 data
#define BPL6DAT   VADDR16(0x00dff11a)  // Bitplane 6 data
#define BPL7DAT   VADDR16(0x00dff11c)  // Bitplane 7 data
#define BPL8DAT   VADDR16(0x00dff11e)  // Bitplane 8 data
#define SPR0PT    VADDR32(0x00dff120)  // Sprite 0 pointer, bits 1-20
#define SPR0PTH   VADDR32(0x00dff120)  // Sprite 0 pointer, bits 16-20
#define SPR0PTL   VADDR16(0x00dff122)  // Sprite 0 pointer, bits 1-15
#define SPR1PT    VADDR32(0x00dff124)  // Sprite 1 pointer, bits 1-20
#define SPR1PTH   VADDR32(0x00dff124)  // Sprite 1 pointer, bits 16-20
#define SPR1PTL   VADDR16(0x00dff126)  // Sprite 1 pointer, bits 1-15
#define SPR2PT    VADDR32(0x00dff128)  // Sprite 2 pointer, bits 1-20
#define SPR2PTH   VADDR32(0x00dff128)  // Sprite 2 pointer, bits 16-20
#define SPR2PTL   VADDR16(0x00dff12a)  // Sprite 2 pointer, bits 1-15
#define SPR3PT    VADDR32(0x00dff12c)  // Sprite 3 pointer, bits 1-20
#define SPR3PTH   VADDR32(0x00dff12c)  // Sprite 3 pointer, bits 16-20
#define SPR3PTL   VADDR16(0x00dff12e)  // Sprite 3 pointer, bits 1-15
#define SPR4PT    VADDR32(0x00dff130)  // Sprite 4 pointer, bits 1-20
#define SPR4PTH   VADDR32(0x00dff130)  // Sprite 4 pointer, bits 16-20
#define SPR4PTL   VADDR16(0x00dff132)  // Sprite 4 pointer, bits 1-15
#define SPR5PT    VADDR32(0x00dff134)  // Sprite 5 pointer, bits 1-20
#define SPR5PTH   VADDR32(0x00dff134)  // Sprite 5 pointer, bits 16-20
#define SPR5PTL   VADDR16(0x00dff136)  // Sprite 5 pointer, bits 1-15
#define SPR6PT    VADDR32(0x00dff138)  // Sprite 6 pointer, bits 1-20
#define SPR6PTH   VADDR32(0x00dff138)  // Sprite 6 pointer, bits 16-20
#define SPR6PTL   VADDR16(0x00dff13a)  // Sprite 6 pointer, bits 1-15
#define SPR7PT    VADDR32(0x00dff13c)  // Sprite 7 pointer, bits 1-20
#define SPR7PTH   VADDR32(0x00dff13c)  // Sprite 7 pointer, bits 16-20
#define SPR7PTL   VADDR16(0x00dff13e)  // Sprite 7 pointer, bits 1-15
#define SPR0POS   VADDR16(0x00dff140)  // Sprite 0 vert-horiz start position
#define SPR0CTL   VADDR16(0x00dff142)  // Sprite 0 vert stop and control data
#define SPR0DATA  VADDR16(0x00dff144)  // Sprite 0 image data register A
#define SPR0DATB  VADDR16(0x00dff146)  // Sprite 0 image data register B
#define SPR0POS   VADDR16(0x00dff140)  // Sprite 1 vert-horiz start position
#define SPR0CTL   VADDR16(0x00dff142)  // Sprite 1 vert stop and control data
#define SPR0DATA  VADDR16(0x00dff144)  // Sprite 1 image data register A
#define SPR0DATB  VADDR16(0x00dff146)  // Sprite 1 image data register B
#define SPR0POS   VADDR16(0x00dff140)  // Sprite 2 vert-horiz start position
#define SPR0CTL   VADDR16(0x00dff142)  // Sprite 2 vert stop and control data
#define SPR0DATA  VADDR16(0x00dff144)  // Sprite 2 image data register A
#define SPR0DATB  VADDR16(0x00dff146)  // Sprite 2 image data register B
#define SPR0POS   VADDR16(0x00dff140)  // Sprite 3 vert-horiz start position
#define SPR0CTL   VADDR16(0x00dff142)  // Sprite 3 vert stop and control data
#define SPR0DATA  VADDR16(0x00dff144)  // Sprite 3 image data register A
#define SPR0DATB  VADDR16(0x00dff146)  // Sprite 3 image data register B
#define SPR0POS   VADDR16(0x00dff140)  // Sprite 4 vert-horiz start position
#define SPR0CTL   VADDR16(0x00dff142)  // Sprite 4 vert stop and control data
#define SPR0DATA  VADDR16(0x00dff144)  // Sprite 4 image data register A
#define SPR0DATB  VADDR16(0x00dff146)  // Sprite 4 image data register B
#define SPR0POS   VADDR16(0x00dff140)  // Sprite 5 vert-horiz start position
#define SPR0CTL   VADDR16(0x00dff142)  // Sprite 5 vert stop and control data
#define SPR0DATA  VADDR16(0x00dff144)  // Sprite 5 image data register A
#define SPR0DATB  VADDR16(0x00dff146)  // Sprite 5 image data register B
#define SPR0POS   VADDR16(0x00dff140)  // Sprite 6 vert-horiz start position
#define SPR0CTL   VADDR16(0x00dff142)  // Sprite 6 vert stop and control data
#define SPR0DATA  VADDR16(0x00dff144)  // Sprite 6 image data register A
#define SPR0DATB  VADDR16(0x00dff146)  // Sprite 6 image data register B
#define SPR0POS   VADDR16(0x00dff140)  // Sprite 7 vert-horiz start position
#define SPR0CTL   VADDR16(0x00dff142)  // Sprite 7 vert stop and control data
#define SPR0DATA  VADDR16(0x00dff144)  // Sprite 7 image data register A
#define SPR0DATB  VADDR16(0x00dff146)  // Sprite 7 image data register B
#define COLOR00   VADDR16(0x00dff180)  // Pallette color 0
#define COLOR01   VADDR16(0x00dff182)  // Pallette color 1
#define COLOR02   VADDR16(0x00dff184)  // Pallette color 2
#define COLOR03   VADDR16(0x00dff186)  // Pallette color 3
#define COLOR04   VADDR16(0x00dff188)  // Pallette color 4
#define COLOR05   VADDR16(0x00dff18a)  // Pallette color 5
#define COLOR06   VADDR16(0x00dff18c)  // Pallette color 6
#define COLOR07   VADDR16(0x00dff18e)  // Pallette color 7
#define COLOR08   VADDR16(0x00dff190)  // Pallette color 8
#define COLOR09   VADDR16(0x00dff192)  // Pallette color 9
#define COLOR10   VADDR16(0x00dff194)  // Pallette color 10
#define COLOR11   VADDR16(0x00dff196)  // Pallette color 11
#define COLOR12   VADDR16(0x00dff198)  // Pallette color 12
#define COLOR13   VADDR16(0x00dff19a)  // Pallette color 13
#define COLOR14   VADDR16(0x00dff19c)  // Pallette color 14
#define COLOR15   VADDR16(0x00dff19e)  // Pallette color 15
#define COLOR16   VADDR16(0x00dff1a0)  // Pallette color 16
#define COLOR17   VADDR16(0x00dff1a2)  // Pallette color 17 & Sprite 0+1 color 1
#define COLOR18   VADDR16(0x00dff1a4)  // Pallette color 18 & Sprite 0+1 color 2
#define COLOR19   VADDR16(0x00dff1a6)  // Pallette color 19 & Sprite 0+1 color 3
#define COLOR20   VADDR16(0x00dff1a8)  // Pallette color 20
#define COLOR21   VADDR16(0x00dff1aa)  // Pallette color 21 & Sprite 2+3 color 1
#define COLOR22   VADDR16(0x00dff1ac)  // Pallette color 22 & Sprite 2+3 color 2
#define COLOR23   VADDR16(0x00dff1ae)  // Pallette color 23 & Sprite 2+3 color 3
#define COLOR24   VADDR16(0x00dff1b0)  // Pallette color 24
#define COLOR25   VADDR16(0x00dff1b2)  // Pallette color 25 & Sprite 4+5 color 1
#define COLOR26   VADDR16(0x00dff1b4)  // Pallette color 26 & Sprite 4+5 color 2
#define COLOR27   VADDR16(0x00dff1b6)  // Pallette color 27 & Sprite 4+5 color 3
#define COLOR28   VADDR16(0x00dff1b8)  // Pallette color 28
#define COLOR29   VADDR16(0x00dff1ba)  // Pallette color 29 & Sprite 6+7 color 1
#define COLOR30   VADDR16(0x00dff1bc)  // Pallette color 30 & Sprite 6+7 color 2
#define COLOR31   VADDR16(0x00dff1be)  // Pallette color 31 & Sprite 6+7 color 3


#define BPLCON0_ECS       BIT(0)   // Enable ECS + bits in BPLCON3
#define BPLCON0_ERSY      BIT(1)   // External synchronization (genlock)
#define BPLCON0_LACE      BIT(2)   // Interlace mode enable
#define BPLCON0_LPEN      BIT(3)   // Light pen enable
#define BPLCON0_GAUD      BIT(8)   // Genlock audio enable
#define BPLCON0_COLORON   BIT(9)   // Composite mode color-burst enabled
#define BPLCON0_DBLPF     BIT(10)  // Double playfields
#define BPLCON0_HOMOD     BIT(11)  // Hold-and-modify mode
#define BPLCON0_BPU       BIT(12)  // Bits 12-14 set the number of bitplanes
#define BPLCON0_BPU0      BIT(12)  // Low bit of # of bitplanes
#define BPLCON0_BPU1      BIT(13)  // Middle bit of # of bitplanes
#define BPLCON0_BPU2      BIT(14)  // High bit of # of bitplanes
#define BPLCON0_HIGHRES   BIT(15)  // High resolution mode

#define BPLCON3_EXTBLKEN  BIT(0)   // BLANK output is programmable (ECS)
#define BPLCON3_BRDSPRT   BIT(1)   // Enable sprites in window border (ECS)
#define BPLCON3_ZDCLKEN   BIT(2)   // ZD pin outputs 14 MHz clock (ECS)
#define BPLCON3_BRDNTRANN BIT(4)   // Border area is non-minus transparent (ECS)
#define BPLCON3_BRDRBLNK  BIT(5)   // Border area is blanked (ECS)
#define BPLCON3_SPRES0    BIT(6)   // Sprite resolution 00=ECS, 01=LORES
#define BPLCON3_SPRES1    BIT(7)   // Sprite resolution 10=HIRES, 11=SHRES
#define BPLCON3_LOCT      BIT(9)   // Color values written to 2nd palette
#define BPLCON3_PF2OF0    BIT(10)  // Playfield 2 bitplane color table offset
#define BPLCON3_PF2OF1    BIT(11)  //   000=None 001=2  010=3  011=7
#define BPLCON3_PF2OF2    BIT(12)  //   100=16   101=32 110=64 111=128
#define BPLCON3_BANK0     BIT(13)  // These bits select one of eight
#define BPLCON3_BANK1     BIT(14)  // color banks.
#define BPLCON3_BANK2     BIT(15)  //   000=Bank 0  001=Bank 1  ...

#define BLTCON0_LF_MASK   0x00ff   // Area mode and line mode LF mask
#define BLTCON0_AREA_USED BIT(8)   // Area mode use D channel
#define BLTCON0_AREA_USEC BIT(9)   // Area mode use C channel
#define BLTCON0_AREA_USEB BIT(10)  // Area mode use B channel
#define BLTCON0_AREA_USEA BIT(11)  // Area mode use A channel
#define BLTCON0_LINE_REQD 0x0b00   // Line mode required bits (must be set)
#define BLTCON0_ASH_MASK  0xf000   // Area mode and line mode ASH mask

#define BLTCON1_LINE      BIT(0)   // 1=Line mode
#define BLTCON1_AREA_DESC BIT(1)   // Area mode Descending
#define BLTCON1_AREA_FCI  BIT(2)   // Area mode FCI
#define BLTCON1_AREA_IFE  BIT(4)   // Area mode IFE (inclusive)
#define BLTCON1_AREA_EFE  BIT(4)   // Area mode EFE (exclusive)
#define BLTCON1_AREA_DOFF BIT(7)   // Area mode DOFF
#define BLTCON1_SHF0      BIT(12)  // Area mode and line mode BSH0
#define BLTCON1_SHF1      BIT(13)  // Area mode and line mode BSH1
#define BLTCON1_SHF2      BIT(14)  // Area mode and line mode BSH2
#define BLTCON1_SHF3      BIT(15)  // Area mode and line mode BSH3
#define BLTCON1_BSH_MASK  0xf000   // Area mode and line mode BSH mask

#define BLTCON1_LINE_SING BIT(1)   // Line mode SING
#define BLTCON1_LINE_AUL  BIT(2)   // Line mode AUL
#define BLTCON1_LINE_SUL  BIT(3)   // Line mode SUL
#define BLTCON1_LINE_SUD  BIT(4)   // Line mode SUD
#define BLTCON1_LINE_OVF  BIT(5)   // Line mode OVF
#define BLTCON1_LINE_SIGN BIT(6)   // Line mode SIGN
#define BLTCON1_LINE_DPFF BIT(7)   // Line mode DPFF

#define DMACON_AUD0EN     BIT(0)   // Audio channel 0 DMA enable
#define DMACON_AUD1EN     BIT(1)   // Audio channel 1 DMA enable
#define DMACON_AUD2EN     BIT(2)   // Audio channel 2 DMA enable
#define DMACON_AUD3EN     BIT(3)   // Audio channel 3 DMA enable
#define DMACON_DSKEN      BIT(4)   // Disk DMA enable
#define DMACON_SPREN      BIT(5)   // Sprite DMA enable
#define DMACON_BLTEN      BIT(6)   // Blitter DMA enable
#define DMACON_COPEN      BIT(7)   // Copper DMA enable
#define DMACON_BPLEN      BIT(8)   // Bitplane DMA enable
#define DMACON_DMAEN      BIT(9)   // Enable all DMA below
#define DMACON_BLTPRI     BIT(10)  // Blitter DMA priority (over CPU)
#define DMACON_BZERO      BIT(13)  // Blitter logic zero status (read-only)
#define DMACON_BBUSY      BIT(14)  // Blitter busy (read-only)
#define DMACON_SET        BIT(15)  // When 1, sets bits. When 0, clears bits

#define SERDATR_STP1      BIT(8)   // Stop bit or 9th data bit
#define SERDATR_STP2      BIT(9)   // Stop bit if 9th data bit mode
#define SERDATR_RSVD      BIT(10)  // Unused
#define SERDATR_RXD       BIT(11)  // Paula RXD input pin
#define SERDATR_TSRE      BIT(12)  // Transmit shift register empty
#define SERDATR_TBE       BIT(13)  // Transmit buffer empty
#define SERDATR_RBF       BIT(14)  // Read buffer full
#define SERDATR_OVRUN     BIT(15)  // Overrun

#define ADKCON_SETCLR     BIT(15)  // 1=Set, 0=Clear
#define ADKCON_UARTBRK    BIT(11)  // Force transmit pin to zero

/* INTREQ INTREQR INTENA INTENAR */
#define INTREQ_TBE        BIT(0)   // Serial transmit buffer empty
#define INTREQ_DSKBLK     BIT(1)   // Disk DMA transfer done
#define INTREQ_SOFT       BIT(2)   // Software interrupt
#define INTREQ_PORTS      BIT(3)   // CIA-A or expansion port
#define INTREQ_COPER      BIT(4)   // Copper interrupt
#define INTREQ_VERTB      BIT(5)   // Start of vertical blanking gap reached
#define INTREQ_BLIT       BIT(6)   // Blitter ready
#define INTREQ_AUD0       BIT(7)   // Output audio data channel 0
#define INTREQ_AUD1       BIT(8)   // Output audio data channel 1
#define INTREQ_AUD2       BIT(9)   // Output audio data channel 2
#define INTREQ_AUD3       BIT(10)  // Output audio data channel 3
#define INTREQ_RBF        BIT(11)  // Serial receive buffer full
#define INTREQ_DSKSYN     BIT(12)  // Disk sync value recognized
#define INTREQ_EXTERN     BIT(13)  // Interrupt from CIA-B or expansion port
#define INTREQ_INTEN      BIT(14)  // Enable interrupts
#define INTREQ_SETCLR     BIT(15)  // Set=1 Clear=0 interrupt

#define INTENA_TBE        BIT(0)   // Serial port transmit buffer empty
#define INTENA_DSKBLK     BIT(1)   // Disk block finished
#define INTENA_SOFT       BIT(2)   // Software-initiated interrupt
#define INTENA_PORTS      BIT(3)   // I/O ports and timers
#define INTENA_COPER      BIT(4)   // Copper
#define INTENA_VERTB      BIT(5)   // Start of vertical blank
#define INTENA_BLIT       BIT(6)   // Blitter finished
#define INTENA_AUD0       BIT(7)   // Audio channel 0 block finished
#define INTENA_AUD1       BIT(8)   // Audio channel 1 block finished
#define INTENA_AUD2       BIT(9)   // Audio channel 2 block finished
#define INTENA_AUD3       BIT(10)  // Audio channel 3 block finished
#define INTENA_RBF        BIT(11)  // Serial port receive buffer full
#define INTENA_DSKSYN     BIT(12)  // Disk sync register matches disk data
#define INTENA_EXTER      BIT(13)  // External interrupt
#define INTENA_INTEN      BIT(14)  // Internal interrupt
#define INTENA_SETCLR     BIT(15)  // 1=Set, 0=Clear


/* Amiga CIA (8520) registers */
#define CIA_A_BASE    0x00bfe001
#define CIA_B_BASE    0x00bfd000

#define CIAA_PRA      VADDR8(0x00bfe001)  // Port Register A
#define CIAA_PRB      VADDR8(0x00bfe101)  // Port Register B
#define CIAA_DDRA     VADDR8(0x00bfe201)  // Data Direction Register A
#define CIAA_DDRB     VADDR8(0x00bfe301)  // Data Direction Register B
#define CIAA_TALO     VADDR8(0x00bfe401)  // Timer A low byte
#define CIAA_TAHI     VADDR8(0x00bfe501)  // Timer A high byte
#define CIAA_TBLO     VADDR8(0x00bfe601)  // Timer B low byte
#define CIAA_TBHI     VADDR8(0x00bfe701)  // Timer B high byte
#define CIAA_ELSB     VADDR8(0x00bfe801)  // Event counter bits 0-7
#define CIAA_EMID     VADDR8(0x00bfe901)  // Event counter bits 8-15
#define CIAA_EMSB     VADDR8(0x00bfea01)  // Event counter bits 16-23
#define CIAA_RSVD     VADDR8(0x00bfeb01)  // Unused
#define CIAA_SP       VADDR8(0x00bfec01)  // Serial Port Data register (SDR)
#define CIAA_ICR      VADDR8(0x00bfed01)  // Interrupt Control Register
#define CIAA_CRA      VADDR8(0x00bfee01)  // Control Register A
#define CIAA_CRB      VADDR8(0x00bfef01)  // Control Register B

#define CIAB_PRA      VADDR8(0x00bfd000)  // Port Register A
#define CIAB_PRB      VADDR8(0x00bfd100)  // Port Register B
#define CIAB_DDRA     VADDR8(0x00bfd200)  // Data Direction Register A
#define CIAB_DDRB     VADDR8(0x00bfd300)  // Data Direction Register B
#define CIAB_TALO     VADDR8(0x00bfd400)  // Timer A low byte
#define CIAB_TAHI     VADDR8(0x00bfd500)  // Timer A high byte
#define CIAB_TBLO     VADDR8(0x00bfd600)  // Timer B low byte
#define CIAB_TBHI     VADDR8(0x00bfd700)  // Timer B high byte
#define CIAB_ELSB     VADDR8(0x00bfd800)  // Event counter bits 0-7
#define CIAB_EMID     VADDR8(0x00bfd900)  // Event counter bits 8-15
#define CIAB_EMSB     VADDR8(0x00bfda00)  // Event counter bits 16-23
#define CIAB_RSVD     VADDR8(0x00bfdb00)  // Unused
#define CIAB_SP       VADDR8(0x00bfdc00)  // Serial Port Data register (SDR)
#define CIAB_ICR      VADDR8(0x00bfdd00)  // Interrupt Control Register
#define CIAB_CRA      VADDR8(0x00bfde00)  // Control Register A
#define CIAB_CRB      VADDR8(0x00bfdf00)  // Control Register B

#define CIA_ICR_TA      BIT(0) // Timer A timeout
#define CIA_ICR_TB      BIT(1) // Timer B timeout
#define CIA_ICR_ALARM   BIT(2) // Alarm
#define CIA_ICR_SP      BIT(3) // Shift register full (input) or empty (output)
#define CIA_ICR_FLAG    BIT(4) // Flag
#define CIA_ICR_IR      BIT(7) // Interrupt request (read)
#define CIA_ICR_SET     BIT(7) // 1=Set 0=Clear (write)

#define CIA_CRA_START   BIT(0) // Start timer
#define CIA_CRA_PBON    BIT(1) // 1=PB6on
#define CIA_CRA_OUTMODE BIT(2) // 0=pulse, 1=toggle
#define CIA_CRA_RUNMODE BIT(3) // 0=continuous, 1=one-shot
#define CIA_CRA_LOAD    BIT(4) // 1=Force load (strobe)
#define CIA_CRA_INMODE  BIT(5) // 0=clock, 1=CNT
#define CIA_CRA_SPMOD   BIT(6) // 0=input, 1=output
#define CIA_CRA_RSVD    BIT(7) // Unused

#define CIA_CRB_START   BIT(0) // Start timer
#define CIA_CRB_PBON    BIT(1) // 1=PB7on
#define CIA_CRB_OUTMODE BIT(2) // 0=pulse, 1=toggle
#define CIA_CRB_RUNMODE BIT(3) // 0=continuous, 1=one-shot
#define CIA_CRB_LOAD    BIT(4) // 1=Force load (strobe)
#define CIA_CRB_INMODE0 BIT(5) // 00=clock, 01=CNT
#define CIA_CRB_INMODE1 BIT(6) // 10=timer A, 11=timer A+
#define CIA_CRB_ALARM   BIT(7) // 0=TOD, 1=Alarm

#define RAMSEY_CONTROL_PAGE     BIT(0) // 1=Page mode enabled
#define RAMSEY_CONTROL_BURST    BIT(1) // 1=Burst mode enabled
#define RAMSEY_CONTROL_WRAP     BIT(2) // 1=wrap, 0=no backward bursts
#define RAMSEY_CONTROL_RAMSIZE  BIT(3) // 1=1Mx4 (4MB), 0=256x4 (1MB)
#define RAMSEY_CONTROL_RAMWIDTH BIT(4) // Ramsey-4 1=4-bit, 0=1=bit
#define RAMSEY_CONTROL_SKIP     BIT(4) // Ramsey-7 1=4-clocks, 0=5 clocks
#define RAMSEY_CONTROL_REFRESH0 BIT(5) // 00=154, 01=238, 10=380, 11=Off
#define RAMSEY_CONTROL_REFRESH1 BIT(6) // 00=154, 01=238, 10=380, 11=Off
#define RAMSEY_CONTROL_TEST     BIT(7) // 1=Test mode

#endif /* _AMIGA_CHIPSET_H */
