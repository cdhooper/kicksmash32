/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Amiga message interface.
 */
#include "board.h"
#include "main.h"
#include "printf.h"
#include "uart.h"
#include <stdbool.h>
#include <string.h>
#include "irq.h"
#include "config.h"
#include "crc32.h"
#include "kbrst.h"
#include "msg.h"
#include "m29f160xt.h"
#include "timer.h"
#include "utils.h"
#include "gpio.h"
#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/cm3/nvic.h>

#define SWAP16(x) __builtin_bswap16(x)
#define SWAP32(x) __builtin_bswap32(x)
#define SWAP64(x) __builtin_bswap64(x)

/* Flags to ks_reply() */
#define KS_REPLY_RAW    BIT(0)  // Don't emit header or CRC (raw data)
#define KS_REPLY_WE     BIT(1)  // Set up WE to trigger when host drives OE
#define KS_REPLY_WE_RAW (KS_REPLY_RAW | KS_REPLY_WE)

#define LOG_DMA_CONTROLLER DMA2
#define LOG_DMA_CHANNEL    DMA_CHANNEL5

#define CAPTURE_SW       0
#define CAPTURE_ADDR     1
#define CAPTURE_DATA_LO  2
#define CAPTURE_DATA_HI  3

/* Inline speed-critical code */
#define dma_get_number_of_data(x, y) DMA_CNDTR(x, y)

static const uint16_t sm_magic[] = { 0x0117, 0x0119, 0x1017, 0x0204 };
// static const uint16_t reset_magic_32[] = { 0x0000, 0x0001, 0x0034, 0x0035 };
// static const uint16_t reset_magic_16[] = { 0x0002, 0x0003, 0x0068, 0x0069 };

#define REBOOT_MAGIC_NUM 8
static const uint16_t reboot_magic_32[] =
        { 0x0004, 0x0003, 0x0003, 0x0002, 0x0002, 0x0001, 0x0001, 0x0000 };
static const uint16_t reboot_magic_16[] =
        { 0x0007, 0x0006, 0x0005, 0x0004, 0x0003, 0x0002, 0x0001, 0x0000 };
static const uint16_t *reboot_magic;
static uint16_t reboot_magic_end;

static uint     consumer_spin;
static uint8_t  capture_mode = CAPTURE_ADDR;
static uint     consumer_wrap;
static uint     consumer_wrap_last_poll;
static uint     rx_consumer = 0;
static uint     message_count = 0;

/* Buffers for DMA from/to GPIOs and Timer event generation registers */
#define ADDR_BUF_COUNT 1024
#define ALIGN  __attribute__((aligned(16)))
ALIGN volatile uint16_t buffer_rxa_lo[ADDR_BUF_COUNT];
ALIGN volatile uint16_t buffer_rxd[ADDR_BUF_COUNT];
ALIGN volatile uint16_t buffer_txd_lo[ADDR_BUF_COUNT * 2];
ALIGN volatile uint16_t buffer_txd_hi[ADDR_BUF_COUNT];


#ifdef CAPTURE_GPIOS
ALIGN uint16_t buffer_a[ADDR_BUF_COUNT];
ALIGN uint16_t buffer_b[ADDR_BUF_COUNT];
ALIGN uint16_t buffer_c[ADDR_BUF_COUNT];
ALIGN uint16_t buffer_d[ADDR_BUF_COUNT];

static uint
gpio_watch(void)
{
    uint pos = 0;
    uint16_t gpioa;
    uint16_t gpiob;
    uint16_t gpioc;
    uint16_t gpiod;
    uint16_t l_gpioa = 0;
    uint16_t l_gpiob = 0;
    uint16_t l_gpioc = 0;
    uint16_t l_gpiod = 0;

    while (1) {
        gpioa = GPIO_IDR(GPIOA);  // PA0 + A13-A19
        gpiob = GPIO_IDR(GPIOB);  // WE=PB14 OEWE=PB9
        gpioc = GPIO_IDR(GPIOC);  // A0-A15
        gpiod = GPIO_IDR(GPIOD);  // D0-D15
        if ((gpioa != l_gpioa) ||
            (gpiob != l_gpiob) ||
            (gpioc != l_gpioc) ||
            (gpiod != l_gpiod)) {
            buffer_a[pos] = gpioa;
            buffer_b[pos] = gpiob;
            buffer_c[pos] = gpioc;
            buffer_d[pos] = gpiod;
            if (pos++ > 400)
                break;
            l_gpioa = gpioa;
            l_gpiob = gpiob;
            l_gpioc = gpioc;
            l_gpiod = gpiod;
        }
    }
    return (pos);
}

static void
gpio_showbuf(uint count)
{
    uint     pos;
    uint16_t last_a = 0;
    uint16_t last_b = 0;
    uint16_t last_c = 0;
    uint16_t last_d = 0;
    uint16_t diff;

    last_a = ~buffer_a[0];
    last_b = ~buffer_b[0];
    last_c = ~buffer_c[0];
    last_d = ~buffer_d[0];
    for (pos = 0; pos < count; pos++) {
        uint printed_a = 0;
        uint16_t a = buffer_a[pos];
        uint16_t b = buffer_b[pos];
        uint16_t c = buffer_c[pos];
        uint16_t d = buffer_d[pos];
        printf(" %04x %04x %04x %04x", a, b, c, d);
        if (a != last_a) {
            diff = a ^ last_a;
            if (diff & SOCKET_OE_PIN)
                printf(" S_OE=%u", !!(a & SOCKET_OE_PIN));
            if (diff & 0x00f0) {
                /* A16-A19 */
                printf(" A=%05x", c | ((a & 0xf0) << (16 - 4)));
                printed_a++;
            }
        }
        if (b != last_b) {
            diff = b ^ last_b;
            if (diff & FLASH_OE_PIN)
                printf(" F_OE=%u", !!(b & FLASH_OE_PIN));
            if (diff & FLASH_WE_PIN)
                printf(" WE=%u", !!(b & FLASH_WE_PIN));
            if (diff & FLASH_OEWE_PIN)
                printf(" OEWE=%u", !!(b & FLASH_OEWE_PIN));
        }
        if (c != last_c) {
            if (!printed_a)
                printf(" A=%05x", c | ((a & 0xf0) << (16 - 4)));
        }
        if (d != last_d) {
            diff = d ^ last_d;
            printf(" D=%x", d);
        }
        printf("\n");
        last_a = buffer_a[pos];
        last_b = buffer_b[pos];
        last_c = buffer_c[pos];
        last_d = buffer_d[pos];
    }
}
#endif

/*
 * oe_input
 * --------
 * Return the current value of the SOCKET_OE pin (either 0 or non-zero).
 */
static uint
oe_input(void)
{
    return (GPIO_IDR(SOCKET_OE_PORT) & SOCKET_OE_PIN);
}

static void
config_dma(uint32_t dma, uint32_t channel, uint to_periph, uint mode,
           volatile void *dst, volatile void *src, uint32_t wraplen)
{
    dma_disable_channel(dma, channel);
    dma_channel_reset(dma, channel);
    dma_set_peripheral_address(dma, channel, (uintptr_t)dst);
    dma_set_memory_address(dma, channel, (uintptr_t)src);
    if (to_periph)
        dma_set_read_from_memory(dma, channel);
    else
        dma_set_read_from_peripheral(dma, channel);
    dma_set_number_of_data(dma, channel, wraplen);
    dma_disable_peripheral_increment_mode(dma, channel);
    dma_enable_memory_increment_mode(dma, channel);
    switch (mode) {
        case 8:
            dma_set_peripheral_size(dma, channel, DMA_CCR_PSIZE_8BIT);
            dma_set_memory_size(dma, channel, DMA_CCR_MSIZE_8BIT);
            break;
        case 16:
            dma_set_peripheral_size(dma, channel, DMA_CCR_PSIZE_16BIT);
            dma_set_memory_size(dma, channel, DMA_CCR_MSIZE_16BIT);
            break;
        default: // 32
            dma_set_peripheral_size(dma, channel, DMA_CCR_PSIZE_32BIT);
            dma_set_memory_size(dma, channel, DMA_CCR_MSIZE_32BIT);
            break;
    }
    dma_enable_circular_mode(dma, channel);
    dma_set_priority(dma, channel, DMA_CCR_PL_VERY_HIGH);

    dma_enable_channel(dma, channel);
}

static void
config_tim5_ch1_dma(bool verbose)
{
    if (verbose) {
        memset((void *) buffer_rxa_lo, 0, sizeof (buffer_rxa_lo));
//      printf("Addr lo capture %08x (t5c1) 16-bit\n",
//             (uintptr_t) buffer_rxa_lo);
    }

    /* DMA from address GPIOs A0-A15 to memory */
    config_dma(DMA2, DMA_CHANNEL5, 0, 16,
               &GPIO_IDR(SOCKET_A0_PORT),
               buffer_rxa_lo, ADDR_BUF_COUNT);

    /* Set up TIM5 CH1 to trigger DMA based on external PA0 pin */
    timer_disable_oc_output(TIM5, TIM_OC1);

    /* Enable capture compare CC1 DMA and interrupt */
    timer_enable_irq(TIM5, TIM_DIER_CC1DE | TIM_DIER_CC1IE);

    timer_set_ti1_ch1(TIM5);               // Capture input from channel 1 only

    timer_set_oc_polarity_low(TIM5, TIM_OC1);
    timer_set_oc_value(TIM5, TIM_OC1, 0);

    /* Select the Input and set the filter off */
    TIM5_CCMR1 &= ~(TIM_CCMR1_CC1S_MASK | TIM_CCMR1_IC1F_MASK);
    TIM5_CCMR1 |= TIM_CCMR1_CC1S_IN_TI1 | TIM_CCMR1_IC1F_OFF;

    TIM5_SMCR = TIM_SMCR_ECE;       // external clock mode 2 (ETR input)

    /*
     * TIM5
     * PA0  TIM5_CH1      Filter    Polarity   Trigger  Clock
     *      CC1S_IN_TI1   IC1F_OFF  CCER_CC1P  TI1FP1   ECE
     *                    None      High       TI1      ETR(2)
     *
     * TIM2
     * PA0  TIM2_CH1_ETR  Filter    Polarity   Trigger  Clock
     *      CC1S_IN_TI1   IC1F_OFF  !CCER_CC1P          ECE
     *                    None      Low                 ETR(2)
     *
     * TIM3
     *                            ITR1|ITR2    ECM1
     *
     * Ext clock mode 1 = external input pin (TIx)
     * Ext clock mode 2 = external trigger input (ETR)
     */

//  timer_enable_oc_output(TIM5, TIM_OC1);
}

static void
config_tim2_ch1_dma(bool verbose)
{
    volatile void *src;

    if (verbose) {
        memset((void *) buffer_rxd, 0, sizeof (buffer_rxd));
//      printf("Addr hi capture %08x (t2c1) 16-bit\n",
//             (uintptr_t) buffer_rxd);
    }

    timer_disable_oc_output(TIM2, TIM_OC1);

    /* Word-wide DMA from data GPIOs D0-D15 to memory */
    switch (capture_mode) {
        default:
        case CAPTURE_ADDR:
            src = &GPIO_IDR(SOCKET_A16_PORT);
            break;
        case CAPTURE_DATA_LO:
            src = &GPIO_IDR(FLASH_D0_PORT);
            break;
        case CAPTURE_DATA_HI:
            src = &GPIO_IDR(FLASH_D16_PORT);
            break;
    }
    config_dma(DMA1, DMA_CHANNEL5, 0, 16, src, buffer_rxd, ADDR_BUF_COUNT);

    timer_set_ti1_ch1(TIM2);        // Capture input from channel 1 only

    if (capture_mode == CAPTURE_ADDR)
        timer_set_oc_polarity_low(TIM2, TIM_OC1);
    else
        timer_set_oc_polarity_high(TIM2, TIM_OC1);

    /* Select the Input and set the filter off */
    TIM2_CCMR1 &= ~(TIM_CCMR1_CC1S_MASK | TIM_CCMR1_IC1F_MASK);
    TIM2_CCMR1 |= TIM_CCMR1_CC1S_IN_TI1 | TIM_CCMR1_IC1F_OFF;

    TIM2_SMCR = // TIM_SMCR_ETP      | // falling edge detection
                TIM_SMCR_ECE      | // external clock mode 2 (ETR input)
                TIM_SMCR_ETPS_OFF | // no prescaler
                TIM_SMCR_ETF_OFF;   // no filter
    TIM2_DIER = 0;
    timer_enable_irq(TIM2, TIM_DIER_CC1DE); // DMA on capture/compare event

    timer_set_dma_on_compare_event(TIM2);  // DMA on CCx event occurs
}

static void
configure_oe_capture_rx(bool verbose)
{
    consumer_wrap = 0;
    rx_consumer = 0;
    config_tim2_ch1_dma(verbose);
    config_tim5_ch1_dma(verbose);

    disable_irq();
// Not enough memory bandwidth on at least one CPU I have to have both
// DMAs active and STM32 keep up with the Amiga. That particular STM32 does
// not have DFU, so it might be a remarked STM32F103 or something else.
    TIM_CCER(TIM2) |= TIM_CCER_CC1E;  // timer_enable_oc_output()
    TIM_CCER(TIM5) |= TIM_CCER_CC1E;
    enable_irq();
}

/*
 * ks_reply
 * --------
 * This function sends a reply message to the Amiga host operating system.
 * It will disable flash output and drive data lines directly from the STM32.
 * This routine is called from interrupt context.
 */
static void
ks_reply(uint flags, uint status, uint rlen1, const void *rbuf1,
         uint rlen2, const void *rbuf2)
{
    uint      count;
    uint      pos = 0;
    uint      tlen = (ee_mode == EE_MODE_32) ? 4 : 2;
    uint      dma_left;
    uint      dma_last;
    uint16_t  rlen = rlen1 + rlen2;

    /* FLASH_OE=1 disables flash from driving data pins */
    oe_output(1);
    oe_output_enable();

    /*
     * Board rev 3 and higher have external bus tranceiver, so STM32 can
     * always drive data bus so long as FLASH_OE is disabled.
     */
    data_output_enable();  // Drive data pins
    if (flags & KS_REPLY_WE) {
        we_output(1);
        we_enable(0);      // Pull up
        oewe_output(1);
    }

    /*
     * Configure DMA hardware to drive data pins from RAM when OE goes high
     *
     * In 32-bit mode, we need to copy the 16-bit high word of each 32-bit
     * long to a buffer and then DMA that to the D16-D31 output data register.
     * The D0-D15 values can be DMA as 32-bit values from the source buffer,
     * as the output data register does nothing with writes to the upper
     * 16 bits.
     *
     * In 16-bit low mode, we just need to do 16-bit DMA from the source
     * data to the D0-D15 data register.
     *
     * In 16-bit high mode, we need to do 16-bit DMA from the source data
     * to the D16-D31 data register.
     */

    /* Stop timer DMA triggers */
    TIM_CCER(TIM2) = 0;  // Disable everything
    TIM_CCER(TIM5) = 0;
    timer_disable_irq(TIM5, TIM_DIER_CC1IE);

#if 0
    /* Stop DMA engines */
    timer_disable_oc_output(TIM2, TIM_OC1);
    timer_disable_oc_output(TIM5, TIM_OC1);
#endif

    if (ee_mode == EE_MODE_32) {
        /*
         * For 32-bit mode, we need to separate low and high 16 bits as
         * they are driven out by separate DMA engines (TIM5 and TIM2).
         */
        uint32_t *rbp;
        if (flags & KS_REPLY_RAW) {
            rbp = (uint32_t *) rbuf1;
            rlen1 /= 4;
            for (count = 0; count < rlen1; count++, pos++) {
                uint32_t val = *(rbp++);
                buffer_txd_lo[pos] = val >> 16;
                buffer_txd_hi[pos] = (uint16_t) val;
            }
            if (rlen2 != 0) {
                rbp = (uint32_t *) rbuf2;
                rlen2 /= 4;
                for (count = 0; count < rlen2; count++, pos++) {
                    uint32_t val = *(rbp++);
                    buffer_txd_lo[pos] = val >> 16;
                    buffer_txd_hi[pos] = (uint16_t) val;
                }
            }
        } else {
            uint32_t crc;
            for (count = 0; count < ARRAY_SIZE(sm_magic); pos++) {
                buffer_txd_hi[pos] = sm_magic[count++];
                buffer_txd_lo[pos] = sm_magic[count++];
            }
            buffer_txd_hi[pos] = rlen;
            buffer_txd_lo[pos] = status;
            pos++;
            crc = crc32r(0, &rlen, 2);
            crc = crc32r(crc, &status, 2);
            crc = crc32(crc, rbuf1, rlen1);
            rlen1 /= tlen;
            rbp = (uint32_t *) rbuf1;
            for (count = 0; count < rlen1; count++, pos++) {
                uint32_t val = *(rbp++);
                buffer_txd_hi[pos] = (val << 8)  | ((val >> 8) & 0x00ff);
                buffer_txd_lo[pos] = (val >> 24) | ((val >> 8) & 0xff00);
            }
            if (rlen2 != 0) {
                crc = crc32(crc, rbuf2, rlen2);
                rlen2 /= tlen;
                rbp = (uint32_t *) rbuf2;
                for (count = 0; count < rlen2; count++, pos++) {
                    uint32_t val = *(rbp++);
                    buffer_txd_hi[pos] = (val << 8)  | ((val >> 8) & 0x00ff);
                    buffer_txd_lo[pos] = (val >> 24) | ((val >> 8) & 0xff00);
                }
            }
            buffer_txd_hi[pos] = crc >> 16;
            buffer_txd_lo[pos] = (uint16_t) crc;
            pos++;  // Include CRC
        }

        TIM5_SMCR = TIM_SMCR_ETP      | // falling edge detection
                    TIM_SMCR_ECE;       // external clock mode 2 (ETR input)

        /* TIM5 DMA drives low 16 bits */
        dma_disable_channel(DMA2, DMA_CHANNEL5);  // TIM5
        dma_set_peripheral_address(DMA2, DMA_CHANNEL5,
                                   (uintptr_t) &GPIO_ODR(FLASH_D0_PORT));
        dma_set_memory_address(DMA2, DMA_CHANNEL5, (uintptr_t)buffer_txd_lo);
        dma_set_read_from_memory(DMA2, DMA_CHANNEL5);
        dma_set_number_of_data(DMA2, DMA_CHANNEL5, pos + 1);
        dma_set_peripheral_size(DMA2, DMA_CHANNEL5, DMA_CCR_PSIZE_16BIT);
        dma_set_memory_size(DMA2, DMA_CHANNEL5, DMA_CCR_MSIZE_16BIT);
        DMA_CCR(DMA2, DMA_CHANNEL5) &= ~DMA_CCR_CIRC;
        dma_enable_channel(DMA2, DMA_CHANNEL5);

        /* TIM2 DMA drives high 16 bits */
        dma_disable_channel(DMA1, DMA_CHANNEL5);  // TIM2
        dma_set_peripheral_address(DMA1, DMA_CHANNEL5,
                                   (uintptr_t) &GPIO_ODR(FLASH_D16_PORT));
        dma_set_memory_address(DMA1, DMA_CHANNEL5, (uintptr_t)buffer_txd_hi);
        dma_set_read_from_memory(DMA1, DMA_CHANNEL5);
        dma_set_number_of_data(DMA1, DMA_CHANNEL5, pos + 1);
        dma_set_peripheral_size(DMA1, DMA_CHANNEL5, DMA_CCR_PSIZE_16BIT);
        dma_set_memory_size(DMA1, DMA_CHANNEL5, DMA_CCR_MSIZE_16BIT);
        DMA_CCR(DMA1, DMA_CHANNEL5) &= ~DMA_CCR_CIRC;
        dma_enable_channel(DMA1, DMA_CHANNEL5);

        /* Send out first data and enable capture */
        disable_irq();
        TIM2_EGR = TIM_EGR_CC1G;
        TIM5_EGR = TIM_EGR_CC1G;
        TIM_CCER(TIM2) = TIM_CCER_CC1E;  // Enable, rising edge
        TIM_CCER(TIM5) = TIM_CCER_CC1E;  // Enable, rising edge
        enable_irq();
    } else {
        /* For 16-bit mode, a single DMA engine can be used */
        if (flags & KS_REPLY_RAW) {
            memcpy((void *)buffer_txd_lo, rbuf1, rlen1);
            if (rlen2 != 0) {
                memcpy(((uint8_t *)buffer_txd_lo) + rlen1, rbuf2, rlen2);
            }
            pos = rlen / 2;
        } else {
            uint32_t crc;
            memcpy((void *)buffer_txd_lo, sm_magic, sizeof (sm_magic));
            pos = sizeof (sm_magic) / 2;
            buffer_txd_lo[pos++] = rlen1;
            buffer_txd_lo[pos++] = status;
            crc = crc32r(0, &rlen1, 2);
            crc = crc32r(crc, &status, 2);
            crc = crc32(crc, rbuf1, rlen1);
            if (rlen1 != 0) {
                memcpy((void *)&buffer_txd_lo[pos], rbuf1, rlen1);
                pos += rlen1 / 2;
            }
            if (rlen2 != 0) {
                memcpy((void *)&buffer_txd_lo[pos], rbuf2, rlen2);
                pos += rlen2 / 2;
            }
            buffer_txd_lo[pos++] = crc >> 16;
            buffer_txd_lo[pos++] = (uint16_t) crc;
        }

        TIM5_SMCR = TIM_SMCR_ETP      | // falling edge detection
                    TIM_SMCR_ECE;       // external clock mode 2 (ETR input)

        /* TIM5 DMA drives low 16 bits */
        dma_disable_channel(DMA2, DMA_CHANNEL5);  // TIM5
        dma_set_peripheral_address(DMA2, DMA_CHANNEL5,
                                   (uintptr_t) &GPIO_ODR(FLASH_D0_PORT));
        dma_set_memory_address(DMA2, DMA_CHANNEL5, (uintptr_t)buffer_txd_lo);
        dma_set_read_from_memory(DMA2, DMA_CHANNEL5);
        dma_set_number_of_data(DMA2, DMA_CHANNEL5, pos + 1);
        dma_set_peripheral_size(DMA2, DMA_CHANNEL5, DMA_CCR_PSIZE_16BIT);
        dma_set_memory_size(DMA2, DMA_CHANNEL5, DMA_CCR_MSIZE_16BIT);
        DMA_CCR(DMA2, DMA_CHANNEL5) &= ~DMA_CCR_CIRC;
        dma_enable_channel(DMA2, DMA_CHANNEL5);

        dma_disable_channel(DMA1, DMA_CHANNEL5);  // TIM2

        /* Send out first data and enable capture */
        disable_irq();
        TIM5_EGR = TIM_EGR_CC1G;
        TIM_CCER(TIM5) = TIM_CCER_CC1E;  // Enable, rising edge
        enable_irq();
    }

//  enable_oe_capture_tx();

#ifdef CAPTURE_GPIOS
    if (flags & KS_REPLY_RAW) {
        count = gpio_watch();
    } else {
#endif
    dma_last = dma_get_number_of_data(DMA2, DMA_CHANNEL5);

    while (dma_last != 0) {
        dma_left = dma_get_number_of_data(DMA2, DMA_CHANNEL5);
        while (dma_last == dma_left) {
            for (count = 0; dma_last == dma_left; count++) {
                if (count > 100000) {
                    printf(" KS timeout 0: %u reads left\n", dma_left);
                    goto oe_reply_end;
                }
                __asm__ volatile("dmb");
                dma_left = dma_get_number_of_data(DMA1, DMA_CHANNEL5);
            }
        }
        dma_last = dma_left;
    }
//  timer_delay_ticks(ticks_per_200_nsec);
#ifdef CAPTURE_GPIOS
}
#endif

oe_reply_end:
    if (flags & KS_REPLY_WE) {
        oewe_output(0);
    }

    data_output_disable();
    oe_output_disable();

    configure_oe_capture_rx(false);
    timer_enable_irq(TIM5, TIM_DIER_CC1IE);
    data_output(0xffffffff);    // Return to pull-up of data pins

#ifdef CAPTURE_GPIOS
    if (flags & KS_REPLY_RAW) {
        gpio_showbuf(count);
    }
#endif
}

static void
execute_cmd(uint16_t cmd, uint16_t cmd_len)
{
    uint cons_s;

    switch ((uint8_t) cmd) {
        case KS_CMD_NULL:
            /* Do absolutely nothing (discard command) */
            break;
        case KS_CMD_NOP:
            /* Do nothing but reply */
            ks_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            break;  // End processing
        case KS_CMD_ID: {
            /* Send Kickflash identification and configuration */
            static const uint32_t reply[] = {
                0x12091610,  // Matches USB ID
                0x00000001,  // Protocol version 0.1
                0x00000001,  // Features
                0x00000000,  // Unused
                0x00000000,  // Unused
            };
            ks_reply(0, KS_STATUS_OK, sizeof (reply), &reply, 0, NULL);
            break;
        }
        case KS_CMD_UPTIME: {
            uint64_t now = timer_tick_get();
            uint64_t usec = timer_tick_to_usec(now);
            usec = SWAP64(usec);  // Big endian format
            ks_reply(0, KS_STATUS_OK, sizeof (usec), &usec, 0, NULL);
            break;
        }
        case KS_CMD_TESTPATT: {
            /* Send special data pattern (for test / diagnostic) */
            static const uint32_t reply[] = {
                0x54534554, 0x54544150, 0x53202d20, 0x54524154,
                0xaaaa5555, 0xcccc3333, 0xeeee1111, 0x66669999,
                0x00020001, 0x00080004, 0x00200010, 0x00800040,
                0x02000100, 0x08000400, 0x20001000, 0x80004000,
                0xfffdfffe, 0xfff7fffb, 0xffdfffef, 0xff7fffbf,
                0xfdfffeff, 0xf7fffbff, 0xdfffefff, 0x7fffbfff,
                0x54534554, 0x54544150, 0x444e4520, 0x68646320,
            };
            ks_reply(0, KS_STATUS_OK, sizeof (reply), &reply, 0, NULL);
            break;
        }
        case KS_CMD_LOOPBACK: {
            /* Answer back with loopback data (for test / diagnostic) */
            uint8_t *buf1;
            uint8_t *buf2;
            uint len1;
            uint len2;
            uint raw_len = cmd_len + 8 + 2 + 2 + 4;  // Magic + len + cmd + CRC
            cons_s = rx_consumer - (raw_len - 1) / 2;
            if ((int) cons_s >= 0) {
                /* Send data doesn't wrap */
                len1 = raw_len;
                buf1 = (uint8_t *) &buffer_rxa_lo[cons_s];
                ks_reply(KS_REPLY_RAW, 0, len1, buf1, 0, NULL);
            } else {
                /* Send data from end of buffer + beginning of buffer */
                cons_s += ARRAY_SIZE(buffer_rxa_lo);

                len1 = (ARRAY_SIZE(buffer_rxa_lo) - cons_s) * 2;
                buf1 = (uint8_t *) &buffer_rxa_lo[cons_s];

                len2 = raw_len - len1;
                buf2 = (uint8_t *) buffer_rxa_lo;

                ks_reply(KS_REPLY_RAW, 0, len1, buf1, len2, buf2);
            }
            break;
        }
        case KS_CMD_FLASH_READ: {
            /* Send command sequence for flash read array command */
            uint32_t addr = SWAP32(0x00555);
            ks_reply(0, KS_STATUS_OK, sizeof (addr), &addr, 0, NULL);
            if (ee_mode == EE_MODE_32) {
                uint32_t data = 0x00f000f0;
                ks_reply(KS_REPLY_WE_RAW, 0, sizeof (data), &data, 0, NULL);
            } else {
                uint16_t data = 0x00f0;
                ks_reply(KS_REPLY_WE_RAW, 0, sizeof (data), &data, 0, NULL);
            }
            break;
        }
        case KS_CMD_FLASH_ID: {
            /* Send command sequence to put it in identify mode */
            static const uint32_t addr[] = {
                SWAP32(0x00555), SWAP32(0x002aa), SWAP32(0x00555)
            };
            ks_reply(0, KS_STATUS_OK, sizeof (addr), &addr, 0, NULL);
            if (ee_mode == EE_MODE_32) {
                static const uint32_t data[] = {
                    0x00aa00aa, 0x00550055, 0x00900090
                };
                ks_reply(KS_REPLY_WE_RAW, 0, sizeof (data), &data, 0, NULL);
            } else {
                static const uint16_t data[] = {
                    0x00aa, 0x0055, 0x0090
                };
                ks_reply(KS_REPLY_WE_RAW, 0, sizeof (data), &data, 0, NULL);
            }
            break;
        }
        case KS_CMD_FLASH_WRITE: {
            /* Send command sequence to perform flash write */
            static const uint32_t addr[] = {
                SWAP32(0x00555), SWAP32(0x002aa), SWAP32(0x00555)
            };
            uint32_t wdata;

            cons_s = rx_consumer - (cmd_len + 1) / 2 - 1;
            if ((int) cons_s < 0)
                cons_s += ARRAY_SIZE(buffer_rxa_lo);

            if (ee_mode == EE_MODE_32) {
                if (cmd_len != 4) {
                    ks_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
                    break;
                }
                wdata = buffer_rxa_lo[cons_s];
                cons_s++;
                if (cons_s == ARRAY_SIZE(buffer_rxa_lo))
                    cons_s = 0;
                wdata |= (buffer_rxa_lo[cons_s] << 16);
            } else {
                if (cmd_len != 2) {
                    ks_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
                    break;
                }
                wdata = buffer_rxa_lo[cons_s];
            }

            ks_reply(0, KS_STATUS_OK, sizeof (addr), &addr, 0, NULL);
            if (ee_mode == EE_MODE_32) {
                static uint32_t data[] = {
                    0x00aa00aa, 0x00550055, 0x00a000a0, 0
                };
                data[3] = wdata;
                ks_reply(KS_REPLY_WE_RAW, 0, sizeof (data), &data, 0, NULL);
            } else {
                static uint16_t data[] = {
                    0x00aa, 0x0055, 0x00a0, 0
                };
                data[3] = wdata;
                ks_reply(KS_REPLY_WE_RAW, 0, sizeof (data), &data, 0, NULL);
            }
            break;
        }
        case KS_CMD_FLASH_ERASE: {
            static const uint32_t addr[] = {
                SWAP32(0x00555), SWAP32(0x002aa), SWAP32(0x00555),
                SWAP32(0x00555), SWAP32(0x002aa),
            };
            if (cmd_len != 0) {
                ks_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
                break;
            }
            ks_reply(0, KS_STATUS_OK, sizeof (addr), &addr, 0, NULL);
            if (ee_mode == EE_MODE_32) {
                static const uint32_t data[] = {
                    0x00aa00aa, 0x00550055, 0x00800080,
                    0x00aa00aa, 0x00550055, 0x00300030,
                };
                ks_reply(KS_REPLY_WE_RAW, 0, sizeof (data), &data, 0, NULL);
            } else {
                static const uint16_t data[] = {
                    0x00aa, 0x0055, 0x00a0,
                    0x00aa, 0x0055, 0x0030
                };
                ks_reply(KS_REPLY_WE_RAW, 0, sizeof (data), &data, 0, NULL);
            }
            break;
        }
        case KS_CMD_BANK_INFO:
            /* Get bank info */
            ks_reply(0, KS_STATUS_OK, sizeof (config.bi), &config.bi, 0, NULL);
            break;
        case KS_CMD_BANK_SET: {
            /* Set ROM bank (options in high bits of command) */
            uint16_t bank;
            cons_s = rx_consumer - (cmd_len + 1) / 2 - 1;
            if ((int) cons_s < 0)
                cons_s += ARRAY_SIZE(buffer_rxa_lo);
            bank = buffer_rxa_lo[cons_s];

            if (cmd_len != 2) {
                ks_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
                break;
            }
            if (bank >= ROM_BANKS) {
                ks_reply(0, KS_STATUS_BADARG, 0, NULL, 0, NULL);
                break;
            }
            ks_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            if (cmd & KS_BANK_SETCURRENT) {
                ee_set_bank(bank);
            }
            if (cmd & KS_BANK_SETTEMP) {
                ee_address_override((bank << 4) | 0x7, 0);
            }
            if (cmd & KS_BANK_UNSETTEMP) {
                ee_set_bank(config.bi.bi_bank_current);
            }
            if (cmd & KS_BANK_SETRESET) {
                config.bi.bi_bank_nextreset = bank;
            }
            if (cmd & KS_BANK_SETPOWERON) {
                config.bi.bi_bank_poweron = bank;
                config_updated();
            }
            if (cmd & KS_BANK_REBOOT) {
                kbrst_amiga(0, 0);
            }
            break;
        }
        case KS_CMD_BANK_MERGE: {
            /* Merge or unmerge multiple ROM banks */
            uint     bank;
            uint8_t  bank_start;  // first bank number
            uint8_t  bank_end;    // last bank number
            uint     banks_add;   // additional banks past first
            cons_s = rx_consumer - (cmd_len + 1) / 2 - 1;
            if ((int) cons_s < 0)
                cons_s += ARRAY_SIZE(buffer_rxa_lo);
            bank = buffer_rxa_lo[cons_s];

            if (cmd_len != 2) {
                ks_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
                break;
            }
            bank_start = (uint8_t) bank;
            bank_end   = bank >> 8;
            banks_add  = bank_end - bank_start;

            /*
             * Bank range must be within 0 to max bank.
             * Bank sizes must be a power of 2 (1, 2, 4, 8).
             * Two-bank ranges must start with an even bank number.
             * Four-bank ranges must start with either bank 0 or 4.
             * Eight-bank ranges must start with bank 0.
             */
            if ((bank_start > bank_end) || (bank_end >= ROM_BANKS) ||
                ((banks_add != 0) && (banks_add != 1) &&
                 (banks_add != 3) && (banks_add != 7)) ||
                ((banks_add == 1) && (bank_start & 1)) ||
                ((banks_add == 3) && (bank_start != 0) && (bank_start != 4)) ||
                ((banks_add == 7) && (bank_start != 0))) {
                ks_reply(0, KS_STATUS_BADARG, 0, NULL, 0, NULL);
                break;
            }
            for (bank = bank_start; bank <= bank_end; bank++) {
                if ((((cmd & KS_BANK_UNMERGE) == 0) &&
                     (config.bi.bi_merge[bank] != 0)) ||
                    (((cmd & KS_BANK_UNMERGE) != 0) &&
                     (config.bi.bi_merge[bank] == 0))) {
                    break;  // Stop early -- invalid bank choice
                }
            }
            if (bank < bank_end) {
                ks_reply(0, KS_STATUS_FAIL, 0, NULL, 0, NULL);
                break;
            }
            ks_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            for (bank = bank_start; bank <= bank_end; bank++) {
                if (cmd & KS_BANK_UNMERGE) {
                    config.bi.bi_merge[bank] = 0;
                } else {
                    config.bi.bi_merge[bank] = (banks_add << 4) |
                                               (bank - bank_start);
                }
            }
            config_updated();
            break;
        }
        case KS_CMD_BANK_COMMENT: {
            /* Add comment (description) to the specified bank */
            uint16_t bank;
            uint     slen;
            uint     pos = 0;
            uint8_t *ptr;
            cons_s = rx_consumer - (cmd_len + 1) / 2 - 1;
            if ((int) cons_s < 0)
                cons_s += ARRAY_SIZE(buffer_rxa_lo);
            bank = buffer_rxa_lo[cons_s];
            slen = cmd_len - 2;
            if (bank >= ROM_BANKS) {
                ks_reply(0, KS_STATUS_BADARG, 0, NULL, 0, NULL);
                break;
            }
            if (slen > sizeof (config.bi.bi_desc[0])) {
                ks_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
                break;
            }
            ks_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            while (slen > 0) {
                if (++cons_s >= ARRAY_SIZE(buffer_rxa_lo))
                    cons_s = 0;
                ptr = (uint8_t *) &buffer_rxa_lo[cons_s];
                config.bi.bi_desc[bank][pos++] = ptr[1];
                config.bi.bi_desc[bank][pos++] = ptr[0];

                if (slen == 1)
                    slen = 0;
                else
                    slen -= 2;
            }
            config_updated();
            break;
        }
        case KS_CMD_BANK_LRESET: {
            /* Update long reset sequence of banks */
            uint    bank;
            uint8_t banks[ROM_BANKS];

            cons_s = rx_consumer - (cmd_len + 1) / 2 - 1;
            if ((int) cons_s < 0)
                cons_s += ARRAY_SIZE(buffer_rxa_lo);
            if (cmd_len != sizeof (config.bi.bi_longreset_seq)) {
                /*
                 * All bytes of sequence must be specified.
                 * Unused sequence bytes at the end are 0xff.
                 */
                ks_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
                break;
            }
            for (bank = 0; bank < ROM_BANKS; bank += 2) {
                banks[bank + 0] = buffer_rxa_lo[cons_s] >> 8;
                banks[bank + 1] = (uint8_t) buffer_rxa_lo[cons_s];
                if (++cons_s >= ARRAY_SIZE(buffer_rxa_lo))
                    cons_s = 0;
            }
            for (bank = 0; bank < ROM_BANKS; bank++) {
                if ((banks[bank] >= ROM_BANKS) && (banks[bank] != 0xff))
                    break;  // Invalid position
                if ((config.bi.bi_merge[banks[bank]] & 0x0f) != 0)
                    break;  // Not start of bank
            }
            if (bank < ROM_BANKS) {
                ks_reply(0, KS_STATUS_BADARG, 0, NULL, 0, NULL);
                break;
            }
            ks_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            memcpy(config.bi.bi_longreset_seq, banks, ROM_BANKS);
            config_updated();
            break;
        }
        case KS_CMD_REMOTE_MSG:
            ks_reply(0, KS_STATUS_FAIL, 0, NULL, 0, NULL);
            break;
        default:
            /* Unknown command */
            ks_reply(0, KS_STATUS_UNKCMD, 0, NULL, 0, NULL);
            printf("KS cmd %x?\n", cmd);
            break;
    }
}

/*
 * process_addresses
 * -----------------
 * Walk the ring of captured ROM addresses to detect and act upon commands
 * from the running operating system. This routine is called from interrupt
 * context.
 */
static inline void
process_addresses(void)
{
    static uint     magic_pos = 0;
    static uint16_t len = 0;
    static uint16_t cmd = 0;
    static uint16_t cmd_len = 0;
    static uint32_t crc;
    static uint32_t crc_rx;
    uint            dma_left;
    uint            prod;

new_cmd:
    dma_left = dma_get_number_of_data(LOG_DMA_CONTROLLER, LOG_DMA_CHANNEL);
    prod     = ARRAY_SIZE(buffer_rxa_lo) - dma_left;

    while (rx_consumer != prod) {
        switch (magic_pos) {
            case 0: {
                uint16_t val = buffer_rxa_lo[rx_consumer];
                /* Check for reboot sequence */
                if (val == reboot_magic_end) {
                    int pos;
                    uint c = rx_consumer;

                    for (pos = 1; pos < REBOOT_MAGIC_NUM; pos++) {
                        if (c-- == 0)
                            c = ARRAY_SIZE(buffer_rxa_lo) - 1;
                        if (reboot_magic[pos] != buffer_rxa_lo[c])
                            break;
                    }
                    if (pos < REBOOT_MAGIC_NUM)
                        break;
                    amiga_reboot_detect++;
                    break;
                }
                /* Look for start of Magic sequence (needs to be fast) */
                if (val != sm_magic[0])
                    break;
                magic_pos = 1;
                break;
            }
            case 1:
            case 2:
            case 3:  // 1 ... ARRAY_SIZE(sm_magic) - 1
                /* Magic phase */
                if (buffer_rxa_lo[rx_consumer] != sm_magic[magic_pos]) {
                    magic_pos = 0;  // No match
                    break;
                }
                magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic):
                /* Length phase */
                message_count++;
                len = cmd_len = buffer_rxa_lo[rx_consumer];
                crc = crc32r(0, &len, sizeof (uint16_t));
                magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic) + 1:
                /* Command phase */
                cmd = buffer_rxa_lo[rx_consumer];
                crc = crc32r(crc, &cmd, sizeof (uint16_t));
                if (len == 0)
                    magic_pos++;  // Skip following Data Phase
                magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic) + 2:
                /* Data phase */
                len--;
                if (len != 0) {
                    crc = crc32r(crc, (void *) &buffer_rxa_lo[rx_consumer], 2);
                    len--;
                } else {
                    /* Special case -- odd byte at end */
                    crc = crc32r(crc, (void *) &buffer_rxa_lo[rx_consumer], 1);
                }
                if (len == 0)
                    magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic) + 3:
                /* Top half of CRC */
                crc_rx = buffer_rxa_lo[rx_consumer] << 16;
                magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic) + 4:
                /* Bottom half of CRC */
                crc_rx |= buffer_rxa_lo[rx_consumer];
                if (crc_rx != crc) {
                    uint16_t error[2];
                    error[0] = KS_STATUS_CRC;
                    error[1] = crc;
#undef CRC_DEBUG
#ifdef CRC_DEBUG
                {
                // XXX: not fast enough to deal with log
                    static uint16_t tempcap[16];
                    uint c = rx_consumer;
                    int pos;
                    for (pos = ARRAY_SIZE(tempcap) - 1; pos > 0; pos--) {
                        tempcap[pos] = buffer_rxa_lo[c];
                        if (c-- == 0)
                            c = ARRAY_SIZE(buffer_rxa_lo) - 1;
                    }
#endif  // CRC_DEBUG
                    ks_reply(0, KS_STATUS_CRC, sizeof (error), &error, 0, NULL);
                    magic_pos = 0;  // Reset magic sequencer

                    printf("cmd=%x l=%04x CRC %08lx != calc %08lx\n",
                           cmd, cmd_len, crc_rx, crc);
#ifdef CRC_DEBUG
                        for (pos = 0; pos < ARRAY_SIZE(tempcap); pos++) {
                            printf(" %04x", tempcap[pos]);
                            if (((pos & 0xf) == 0xf) &&
                                (pos != ARRAY_SIZE(tempcap) - 1))
                                printf("\n");
                        }
                        printf("\n");
                    }
#endif  // CRC_DEBUG
                    magic_pos = 0;  // Restart magic detection
                    break;
                }

                /* Execution phase */
                execute_cmd(cmd, cmd_len);
                magic_pos = 0;  // Restart magic detection
                goto new_cmd;
            default:
                printf("?");
                magic_pos = 0;  // Restart magic detection
                goto new_cmd;
        }

        if (++rx_consumer == ARRAY_SIZE(buffer_rxa_lo)) {
            rx_consumer = 0;
            if (++consumer_wrap - consumer_wrap_last_poll > 10) {
                /*
                 * Spinning too much in interrupt context.
                 * Disable interrupt -- it will be re-enabled in ee_poll().
                 */
                timer_disable_irq(TIM5, TIM_DIER_CC1IE);
                consumer_spin++;
                return;
            }
        }
    }

    dma_left = dma_get_number_of_data(LOG_DMA_CONTROLLER, LOG_DMA_CHANNEL);
    prod = ARRAY_SIZE(buffer_rxa_lo) - dma_left;
    if (rx_consumer != prod)
        goto new_cmd;
}

void
tim5_isr(void)
{
#if 0
    uint flags = TIM_SR(TIM5) & TIM_DIER(TIM5);
    TIM_SR(TIM5) = ~flags;  /* Clear interrupt */
#else
    TIM_SR(TIM5) = 0;  /* Clear all TIM5 interrupt status */
#endif

    process_addresses();
}

int
address_log_replay(uint max)
{
    uint dma_left;
    uint prod;
    uint cons;
    uint addr;
    uint data;
    uint count = 0;
#if 0
    uint flags = TIM_SR(TIM5) & TIM_DIER(TIM5);
    TIM_SR(TIM5) = ~flags;  /* Clear interrupt */
#endif

    dma_left = dma_get_number_of_data(LOG_DMA_CONTROLLER, LOG_DMA_CHANNEL);
    prod = ARRAY_SIZE(buffer_rxa_lo) - dma_left;

    if (prod >= ARRAY_SIZE(buffer_rxa_lo)) {
        printf("Invalid producer=%x left=%x\n", prod, dma_left);
        return (1);
    }
    if (max >= 999) {
        printf("T2C1=%04x %08x\n"
               "T5C1=%04x %08x\n"
               "Wrap=%u\n"
               "Spin=%u\n"
               "Msgs=%u\n",
               (uint) DMA_CNDTR(DMA1, DMA_CHANNEL5), (uintptr_t)buffer_rxd,
               (uint) DMA_CNDTR(DMA2, DMA_CHANNEL5), (uintptr_t)buffer_rxa_lo,
               consumer_wrap, consumer_spin, message_count);
        consumer_wrap = 0;
        consumer_spin = 0;
        message_count = 0;
        return (0);
    }
    if (max > ARRAY_SIZE(buffer_rxa_lo) - 1)
        max = ARRAY_SIZE(buffer_rxa_lo) - 1;

    cons = prod - max;
    if (cons >= ARRAY_SIZE(buffer_rxa_lo)) {
        if (consumer_wrap == 0) {
            cons = 0;
            if (prod == 0) {
                printf("No log entries\n");
                return (1);
            }
        } else {
            cons += ARRAY_SIZE(buffer_rxa_lo);  // Fix negative wrap
        }
    }

    printf("Ent ROMAddr AmigaAddr");
    if (capture_mode == CAPTURE_DATA_LO)
        printf(" DataLo");
    else if (capture_mode == CAPTURE_DATA_HI)
        printf(" DataHi");
    printf("\n");

    while (cons != prod) {
        addr = buffer_rxa_lo[cons];
        data = buffer_rxd[cons];
        if (capture_mode == CAPTURE_ADDR) {
            addr |= ((data & 0xf0) << (16 - 4));
            printf("%3u %05x   %05x\n", cons, addr,
                   (ee_mode == EE_MODE_32) ? (addr << 2) : (addr << 1));
        } else {
            printf("%3u _%04x   %05x     %04x\n", cons, addr,
                   (ee_mode == EE_MODE_32) ? (addr << 2) : (addr << 1), data);
        }
        cons++;
        if (cons >= ARRAY_SIZE(buffer_rxa_lo))
            cons = 0;
        if (count++ > max) {
            printf("bug: count=%u cons=%x prod=%x\n", count, cons, prod);
            break;
        }
    }
    return (0);
}


void
bus_snoop(uint mode)
{
    uint     last_oe = 1;
    uint     count = 0;
    uint     cons = 0;
    uint     prod = 0;
    uint     oprod = 0;
    uint     no_data = 0;
    uint32_t cap_addr[32];
    uint32_t cap_data[32];

    if (mode != CAPTURE_SW)
        printf("Press any key to exit\n");

    address_output_disable();
    if (mode != CAPTURE_SW) {
        /* Use hardware DMA for capture */
        uint dma_left;
        capture_mode = mode;
        configure_oe_capture_rx(false);
        dma_left = dma_get_number_of_data(LOG_DMA_CONTROLLER, LOG_DMA_CHANNEL);
        prod = ARRAY_SIZE(buffer_rxa_lo) - dma_left;
        if (prod > ARRAY_SIZE(buffer_rxa_lo))
            prod = 0;
        cons = prod;

        while (1) {
            if (((count++ & 0xff) == 0) && getchar() > 0)
                break;
            dma_left = dma_get_number_of_data(LOG_DMA_CONTROLLER,
                                              LOG_DMA_CHANNEL);
            prod = ARRAY_SIZE(buffer_rxa_lo) - dma_left;
            if (prod > ARRAY_SIZE(buffer_rxa_lo))
                prod = 0;
            if (cons != prod) {
                while (cons != prod) {
                    uint addr = buffer_rxa_lo[cons];
                    uint data = buffer_rxd[cons];
                    if (mode == CAPTURE_ADDR) {
                        addr |= ((data & 0xf0) << (16 - 4));
                        printf(" %05x", addr);
                    } else {
                        printf(" %04x[%04x]", addr, data);
                    }
                    cons++;
                    if (cons >= ARRAY_SIZE(buffer_rxa_lo))
                        cons = 0;
                }
                printf("\n");
            }
        }
        return;
    }

    timer_disable_irq(TIM5, TIM_DIER_CC1IE);
    while (1) {
        if (oe_input() == 0) {
            /* Capture address on falling edge of OE */
            if (last_oe == 1) {
                uint32_t addr = address_input();
                uint nprod = prod + 1;
                if (nprod >= ARRAY_SIZE(cap_addr))
                    nprod = 0;
                if (nprod != cons) {
                    /* FIFO has space, capture address */
                    cap_addr[prod] = addr;
                    oprod = prod;
                    prod = nprod;
                    no_data = 0;
                    continue;
                }
                last_oe = 0;
            }
            continue;
        } else {
            /* Capture data on rising edge of OE */
            if (last_oe == 0) {
                cap_data[oprod] = data_input();
                last_oe = 1;
                continue;
            }
        }
        if ((no_data++ & 0x1ff) != 0)
            continue;
        if (cons != prod) {
            while (cons != prod) {
                printf(" %lx[%08lx]", cap_addr[cons], cap_data[cons]);
                if (++cons >= ARRAY_SIZE(cap_addr))
                    cons = 0;
            }
            printf("\n");
        }
        if ((no_data & 0xffff) != 1)
            continue;
        if (getchar() > 0)
            break;
        no_data = 0;
    }
    timer_enable_irq(TIM5, TIM_DIER_CC1IE);
    printf("\n");
}

void
msg_poll(void)
{
    if (consumer_wrap_last_poll != consumer_wrap) {
        consumer_wrap_last_poll = consumer_wrap;
        /*
         * Re-enable message interrupt if it was disabled during
         * interrupt processing due to excessive time.
         */
        timer_enable_irq(TIM5, TIM_DIER_CC1IE);
    }
}

void
msg_mode(uint mode)
{
    if (mode == 16)
        reboot_magic = reboot_magic_16;
    else
        reboot_magic = reboot_magic_32;
    reboot_magic_end = reboot_magic[0];
}

void
msg_init(void)
{
    /*
     * Configure DMA on SOCKET_OE going low
     *
     * ---------------- STM32F1 Table 78 lists DMA1 channels ----------------
     * CH1      CH2       CH3       CH4       CH5        CH6       CH7
     * ADC1     -         -         -         -          -         -
     * -        SPI1_RX   SPI1_TX   SPI2_RX   SPI2_TX    -         -
     *                              I2S2_RX   I2S2_TX
     * -        USART3_TX USART3_RX USART1_TX USART1_RX  USART2_RX USART2_TX
     * -        -         -         I2C2_TX   I2C2_RX    I2C1_TX   I2C1_RX
     * -        TIM1_CH1  -         TIM1_CH4  TIM1_UP    TIM1_CH3  -
     *                              TIM1_COM
     *                              TIM1_TRIG
     * TIM2_CH3 TIM2_UP   -         -         TIM2_CH1   -         TIM2_CH2
     *                                                             TIM2_CH4
     * -        TIM3_CH3  TIM3_CH4  -         -          TIM3_CH1  -
     *                    TIM3_UP                        TIM3_TRIG
     * TIM4_CH1 -         -         TIM4_CH2  TIM4_CH3   -         TIM4_UP
     *
     *
     * ---------------- STM32F1 Table 79 lists DMA2 channels ----------------
     * CH1       CH2       CH3       CH4       CH5
     * -         -         -         -         -
     * SPI_RX    SPI_TX    -         -         -
     * I2S3_RX   I2S3_TX   -         -         -
     * -         -         UART4_RX  -         UART4_TX
     * -         -         -         SDIO      -
     * TIM5_CH4  TIM5_CH3  -         TIM5_CH2  TIM5_CH1
     * TIM5_TRIG TIM5_UP
     * -         -         TIM6_UP   -         -
     *                     DAC_CH1
     * -         -         -         TIM7_UP   -
     *                               DAC_CH2
     * TIM8_CH3  TIM8_CH4  TIM8_CH1  -         TIM8_CH2
     *           TIM8_UP   TIM8_TRIG
     *                     TIM8_COM
     *
     * PA0: WKUP/USART2_CTS, ADC12_IN0/TIM2_CH1_ETR, TIM5_CH1/ETH_MII_CRS_WKUP
     *
     * DMA1 Channel 1 used by ADC1
     * DMA1 Channel 5 used by TIM2_TRG (ROM OE DMA from ext pin)
     * DMA2 Channel 5 used by TIM5_CH1 (ROM OE DMA from ext pin)
     * DMA2 Channel 6 used by TIM3     (slave of TIM2 or TIM5)
     *
     * Only one channel may be active per stream.
     */

    /*
     * 2023-12-22
     * Read-from-STM32 sequence:
     *      Setup (as soon as OE high after command determined)
     *              Set flash WE high
     *              Set flash OE high
     *              Set data pins to output
     *              Set up DMA from reply buffer
     *      OE high
     *              loop_count--
     *              If loop_count == 0
     *                  Disable DMA
     *                  Set data pins to input
     *                  Flash OE = input
     *              else
     *                  write data pins
     * Write to flash sequence
     *      Setup (as soon as OE high after command determined)
     *              Set flash WE input pullup
     *              Set flash OEWE high
     *              Set flash OE high
     *              Set data pins to output
     *              Set up DMA from reply buffer
     *      OE high
     *              loop_count--
     *              If loop_count == 0
     *                  Disable DMA
     *                  Set data pins to input
     *                  Flash OEWE = low
     *                  Flash OE = input
     *              else
     *                  write data pins
     */
    rcc_periph_clock_enable(RCC_DMA1);
    rcc_periph_clock_enable(RCC_DMA2);

    rcc_periph_clock_enable(RCC_TIM2);
    rcc_periph_clock_enable(RCC_TIM5);

    rcc_periph_reset_pulse(RST_TIM2);
    rcc_periph_reset_pulse(RST_TIM5);

//  timer_clear_flag(TIM5, TIM_SR(TIM5) & TIM_DIER(TIM5));
    nvic_set_priority(NVIC_TIM5_IRQ, 0x20);
    nvic_enable_irq(NVIC_TIM5_IRQ);

    configure_oe_capture_rx(true);

    capture_mode = CAPTURE_ADDR;
    configure_oe_capture_rx(true);
}
