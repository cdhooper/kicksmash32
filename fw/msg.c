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

/* Inline speed-critical functions */
#define dma_get_number_of_data(dma, channel)        DMA_CNDTR(dma, channel)
#define dma_enable_channel(dma, channel) \
                DMA_CCR(dma, channel) |= DMA_CCR_EN
#define dma_disable_channel(dma, channel) \
                DMA_CCR(dma, channel) &= ~DMA_CCR_EN
#define dma_set_peripheral_address(dma, channel, address) \
                DMA_CPAR(dma, channel) = (uint32_t) address
#define dma_set_memory_address(dma, channel, address) \
                DMA_CMAR(dma, channel) = (uint32_t) address
#define dma_set_read_from_memory(dma, channel) \
                DMA_CCR(dma, channel) |= DMA_CCR_DIR
#define dma_set_number_of_data(dma, channel, number) \
                DMA_CNDTR(dma, channel) = number
#define dma_set_peripheral_size(dma, channel, size) \
                DMA_CCR(dma, channel) = (DMA_CCR(dma, channel) & \
                                         ~DMA_CCR_PSIZE_MASK) | size
#define dma_set_memory_size(dma, channel, size) \
                DMA_CCR(dma, channel) = (DMA_CCR(dma, channel) & \
                                         ~DMA_CCR_MSIZE_MASK) | size
#define timer_enable_irq(timer, irq)          TIM_DIER(timer) |= (irq)
#define timer_disable_irq(timer, irq)         TIM_DIER(timer) &= ~(irq)
#define timer_set_dma_on_compare_event(timer) TIM_CR2(timer) &= ~TIM_CR2_CCDS
#define timer_set_ti1_ch1(timer)              TIM_CR2(timer) &= ~TIM_CR2_TI1S


static const uint16_t sm_magic[] = { 0x0204, 0x1017, 0x0119, 0x0117 };
// static const uint16_t reset_magic_32[] = { 0x0000, 0x0001, 0x0034, 0x0035 };
// static const uint16_t reset_magic_16[] = { 0x0002, 0x0003, 0x0068, 0x0069 };

#define REBOOT_MAGIC_NUM 8
static const uint16_t reboot_magic_32[] =
        { 0x0004, 0x0003, 0x0003, 0x0002, 0x0002, 0x0001, 0x0001, 0x0000 };
static const uint16_t reboot_magic_16[] =
        { 0x0007, 0x0006, 0x0005, 0x0004, 0x0003, 0x0002, 0x0001, 0x0000 };
static const uint16_t *reboot_magic;
static uint16_t reboot_magic_end;

static uint32_t ticks_per_200_nsec;
static uint64_t ks_timeout_timer = 0;  // timer too frequent complaint message
static uint     ks_timeout_count = 0;  // count of complaint messages

static uint     consumer_spin;
static uint8_t  capture_mode = CAPTURE_ADDR;
static uint     consumer_wrap;
static uint     consumer_wrap_last_poll;
static uint     rx_consumer = 0;
static uint     message_count = 0;
static uint64_t amiga_time = 0;     // Seconds and microseconds

/* Message interface through Kicksmash between Amiga and USB host */
static uint     prod_atou;   // Producer for Amiga -> USB buffer
static uint     cons_atou;   // Consumer for Amiga -> USB buffer
static uint     prod_utoa;   // Producer for USB buffer -> Amiga
static uint     cons_utoa;   // Consumer for USB buffer -> Amiga
static uint8_t  msg_lock;    // Bits !USB 0=atou 1=utoa, !Amiga 2=atou 3=utoa

/* Buffers for DMA from/to GPIOs and Timer event generation registers */
#define ADDR_BUF_COUNT 1024
#define ALIGN  __attribute__((aligned(16)))
ALIGN volatile uint16_t buffer_rxa_lo[ADDR_BUF_COUNT];
ALIGN volatile uint16_t buffer_rxd[ADDR_BUF_COUNT];
ALIGN volatile uint16_t          buffer_txd_lo[ADDR_BUF_COUNT * 2];
ALIGN volatile uint16_t          buffer_txd_hi[ADDR_BUF_COUNT];

/* The message buffers must be a power-of-2 in size */
ALIGN uint8_t  msg_atou[0x1000];  // Amiga -> USB buffer
ALIGN uint8_t  msg_utoa[0x1000];  // USB -> Amiga buffer

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
 * Compute buffer space available   Compute buffer space in use
 * (S-1)-(P-C)&(S-1)                (P-C)&(S-1)
 * 4 ops                            3 ops
 *
 * Producer / consumer scenarios
 *  _ _ _ _ _ _ _ _    _ _ _ _ _ _ _ _    _ _ _ _ _ _ _ _
 * |.|_|_|.|.|.|.|.|  |_|.|.|_|_|_|_|_|  |_|_|_|_|_|_|_|_|
 *    P   C              C   P              C
 *    r   o              o   r              o
 *    o   n              n   o              P
 *    d   s              s   d              R
 *    =   =              =   =              =
 *    1   3              1   3              1
 *
 * (1-3)&7            (3-1)&7            (1-1)&7
 * (0xfe&7)=6 in use  2&7=2 in use       0&7=0 in use
 * 1 available        5 available        7 available
 *
 * First scenario P-C will result in a negative, which is then masked
 * against the total_size - 1. That will yield a positive which is the
 * number of elements in use.
 *
 */
#define MBUF1_SPACE_INUSE ((prod_atou - cons_atou) & (sizeof (msg_atou) - 1))
#define MBUF2_SPACE_INUSE ((prod_utoa - cons_utoa) & (sizeof (msg_utoa) - 1))
#define MBUF1_SPACE_AVAIL (sizeof (msg_atou) - 2 - MBUF1_SPACE_INUSE)
#define MBUF2_SPACE_AVAIL (sizeof (msg_utoa) - 2 - MBUF2_SPACE_INUSE)

static uint
atou_add(uint len, void *ptr)
{
    uint xlen;
    uint8_t *sptr = ptr;
    len = (len + 1) & ~1;  // Round up to 16-bit alignment
    if (MBUF1_SPACE_AVAIL < len)
        return (1);
    xlen = sizeof (msg_atou) - prod_atou;
    if (len <= xlen) {
        memcpy(msg_atou + prod_atou, sptr, len);
    } else {
        memcpy(msg_atou + prod_atou, sptr, xlen);
        memcpy(msg_atou, sptr + xlen, len - xlen);
    }
    prod_atou = (prod_atou + len) & (sizeof (msg_atou) - 1);
    return (0);
}

static uint
utoa_add(uint len, void *ptr)
{
    uint xlen;
    uint8_t *sptr = ptr;
    len = (len + 1) & ~1;  // Round up to 16-bit alignment
    if (MBUF2_SPACE_AVAIL < len)
        return (1);
    xlen = sizeof (msg_utoa) - prod_utoa;
    if (len <= xlen) {
        memcpy(msg_utoa + prod_utoa, sptr, len);
    } else {
        memcpy(msg_utoa + prod_utoa, sptr, xlen);
        memcpy(msg_utoa, sptr + xlen, len - xlen);
    }
    prod_utoa = (prod_utoa + len) & (sizeof (msg_utoa) - 1);
    return (0);
}

static uint16_t
atou_next_msg_len(void)
{
    uint     len;
    uint     pos;
    uint     inuse = MBUF1_SPACE_INUSE;
    uint     count;
    uint16_t magic;

    if (inuse < KS_HDR_AND_CRC_LEN) {
        /* Invalid */
        cons_atou = prod_atou;
        return (0);
    }

    /* Check magic */
    for (pos = cons_atou, count = 0; count < ARRAY_SIZE(sm_magic); count++) {
        magic = *(uint16_t *) (msg_atou + pos);
        if (magic != sm_magic[count]) {
            printf("Bad msg %u %04x != %04x\n", count, magic, sm_magic[count]);
            cons_atou = prod_atou;
            return (0);
        }
        pos = (pos + 2) & (sizeof (msg_atou) - 1);
    }

    len     = *(uint16_t *) (msg_atou + pos);
    len     = (len + 1) & ~1;  // Round up
    return (len + KS_HDR_AND_CRC_LEN);
}

static uint16_t
utoa_next_msg_len(void)
{
    uint     len;
    uint     pos;
    uint     inuse = MBUF2_SPACE_INUSE;
    uint     count;
    uint16_t magic;

    if (inuse < KS_HDR_AND_CRC_LEN) {
        cons_utoa = prod_utoa;
        return (0);
    }
    /* Check magic */
    for (pos = cons_utoa, count = 0; count < ARRAY_SIZE(sm_magic); count++) {
        magic = *(uint16_t *) (msg_utoa + pos);
        if (magic != sm_magic[count]) {
            printf("bad msg %u %04x != %04x\n", count, magic, sm_magic[count]);
            cons_utoa = prod_utoa;
            return (0);
        }
        pos = (pos + 2) & (sizeof (msg_utoa) - 1);
    }

    len     = *(uint16_t *) (msg_utoa + pos);
    len     = (len + 1) & ~1;  // Round up
    return (len + KS_HDR_AND_CRC_LEN);
}

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
            uint     rlen1_orig = rlen1;
            uint16_t *txl = (uint16_t *)buffer_txd_lo;
            uint16_t *txh = (uint16_t *)buffer_txd_hi;
            uint32_t val;

            /* Copy first chunk */
            rbp = (uint32_t *) rbuf1;
            rlen1 = (rlen1 + 3) / 4;
            for (count = rlen1; count != 0; count--) {
                val = *(rbp++);
                *(txh++) = (uint16_t) val;
                *(txl++) = val >> 16;
            }
            if (rlen2 != 0) {
                uint32_t *rbp2 = (uint32_t *) rbuf2;
                uint      rlen2_orig = rlen2;

                rlen2 = (rlen2 + 3) / 4;

                if ((rlen1_orig & 3) == 0) {
                    /* First chunk was even multiple of 4 bytes */
                    for (count = rlen2; count != 0; count--) {
                        val = *(rbp2++);
                        *(txh++) = (uint16_t) val;
                        *(txl++) = val >> 16;
                    }
                } else {
                    /* First chunk was not even multiple of 4 bytes */
                    txl--;   // Fixup first chunk overrun

                    for (count = rlen2; count != 0; count--) {
                        val = *(rbp2++);
                        *(txl++) = (uint16_t) val;
                        *(txh++) = val >> 16;
                    }
                    if ((rlen2_orig & 3) != 0)
                        txh--;          // Odd first + odd second = even
                }
            }
            pos = txh - buffer_txd_hi;
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
            pos = (rlen + 1) / 2;
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
                    if (flags & KS_REPLY_WE)
                        oewe_output(0);

                    data_output_disable();
                    oe_output_disable();
                    if (timer_tick_has_elapsed(ks_timeout_timer))
                        ks_timeout_count = 0;
                    ks_timeout_timer = timer_tick_plus_msec(1000);
                    if (ks_timeout_count++ < 4)
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

    if (flags & KS_REPLY_WE)
        oewe_output(0);

    data_output_disable();
    oe_output_disable();

oe_reply_end:
    configure_oe_capture_rx(false);
    timer_enable_irq(TIM5, TIM_DIER_CC1IE);
    data_output(0xffffffff);    // Return to pull-up of data pins

#ifdef CAPTURE_GPIOS
    if (flags & KS_REPLY_RAW)
        gpio_showbuf(count);
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
            uint raw_len = cmd_len + KS_HDR_AND_CRC_LEN;  // Magic+len+cmd+CRC
            cons_s = rx_consumer - (raw_len - 2 + 1) / 2;
            if ((int) cons_s >= 0) {
                /* Send data doesn't wrap */
                len1 = raw_len;
                buf1 = (uint8_t *) &buffer_rxa_lo[cons_s];
                ks_reply(KS_REPLY_RAW, 0, len1, buf1, 0, NULL);
// printf("e=%02x%02x%02x%02x\n", buf1[len1 - 4], buf1[len1 - 3], buf1[len1 - 2], buf1[len1 - 1]);
            } else {
                /* Send data from end of buffer + beginning of buffer */
                cons_s += ARRAY_SIZE(buffer_rxa_lo);

                len1 = (ARRAY_SIZE(buffer_rxa_lo) - cons_s) * 2;
                buf1 = (uint8_t *) &buffer_rxa_lo[cons_s];

                len2 = raw_len - len1;
                buf2 = (uint8_t *) buffer_rxa_lo;
                ks_reply(KS_REPLY_RAW, 0, len1, buf1, len2, buf2);
// printf("c=%u l=%u+%u e1=%02x%02x e=%02x%02x%02x%02x\n", cons_s, len1, len2, buf1[len1 - 2], buf1[len1 - 1], buf2[len2 - 4], buf2[len2 - 3], buf2[len2 - 2], buf2[len2 - 1]);
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
        case KS_CMD_BANK_NAME: {
            /* Set bank name (description) for the specified bank */
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
            if (slen > sizeof (config.bi.bi_name[0])) {
                ks_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
                break;
            }
            ks_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            while (slen > 0) {
                if (++cons_s >= ARRAY_SIZE(buffer_rxa_lo))
                    cons_s = 0;
                ptr = (uint8_t *) &buffer_rxa_lo[cons_s];
                config.bi.bi_name[bank][pos++] = ptr[1];
                config.bi.bi_name[bank][pos++] = ptr[0];

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
                if (banks[bank] == 0xff)
                    continue;
                if (banks[bank] >= ROM_BANKS)
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
        case KS_CMD_MSG_INFO: {
            smash_msg_info_t reply;
            uint16_t         temp;
            uint16_t         avail1;
            uint16_t         avail2;

            avail1 = MBUF1_SPACE_AVAIL;
            avail2 = MBUF2_SPACE_AVAIL;
            if (avail1 >= KS_HDR_AND_CRC_LEN)
                avail1 -= KS_HDR_AND_CRC_LEN;
            else
                avail1 = 0;
            if (avail2 >= KS_HDR_AND_CRC_LEN)
                avail2 -= KS_HDR_AND_CRC_LEN;
            else
                avail2 = 0;

            temp                 = MBUF1_SPACE_INUSE;
            reply.smi_atou_inuse = SWAP16(temp);
            reply.smi_atou_avail = SWAP16(avail1);
            temp                 = MBUF2_SPACE_INUSE;
            reply.smi_utoa_inuse = SWAP16(temp);
            reply.smi_utoa_avail = SWAP16(avail2);
            ks_reply(0, KS_STATUS_OK, sizeof (reply), &reply, 0, NULL);
            break;
        }
        case KS_CMD_MSG_SEND: {
            uint raw_len = cmd_len + KS_HDR_AND_CRC_LEN;  // Magic+len+cmd+CRC
            uint8_t *buf1;
            uint8_t *buf2;
            uint len1;
            uint len2;
            uint rc;
            cons_s = rx_consumer - (raw_len - 1) / 2;
//printf("[%u]", raw_len);
            if ((int) cons_s >= 0) {
                /* Receive data doesn't wrap */
                len1 = raw_len;
                buf1 = (uint8_t *) &buffer_rxa_lo[cons_s];
                if (cmd & KS_MSG_ALTBUF) {
                    rc = utoa_add(len1, buf1);
                } else {
                    rc = atou_add(len1, buf1);
//printf("pb1=%u\n", prod_atou);
                }
            } else {
                /* Send data from end of buffer + beginning of buffer */
                cons_s += ARRAY_SIZE(buffer_rxa_lo);

                len1 = (ARRAY_SIZE(buffer_rxa_lo) - cons_s) * 2;
                buf1 = (uint8_t *) &buffer_rxa_lo[cons_s];

                len2 = raw_len - len1;
                buf2 = (uint8_t *) buffer_rxa_lo;

                if (cmd & KS_MSG_ALTBUF) {
                    if (len1 + len2 > MBUF2_SPACE_AVAIL) {
                        rc = 1;
                    } else {
                        rc = utoa_add(len1, buf1);
                        if (rc == 0)
                            rc = utoa_add(len2, buf2);
                    }
                } else {
                    if (len1 + len2 > MBUF1_SPACE_AVAIL) {
                        rc = 1;
                    } else {
                        rc = atou_add(len1, buf1);
//printf("Pb1=%u\n", prod_atou);
                        if (rc == 0)
{
                            rc = atou_add(len2, buf2);
//printf("PB1=%u\n", prod_atou);
}
                    }
                }
            }
            if (rc != 0)
                ks_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
            else
                ks_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            break;
        }
        case KS_CMD_MSG_RECEIVE: {
            uint     len;
            uint     len1;
            uint     len2;
            uint8_t *buf1;
            uint8_t *buf2;
            if (cmd & KS_MSG_ALTBUF) {
                if (msg_lock & BIT(2)) {
                    ks_reply(0, KS_STATUS_LOCKED, 0, NULL, 0, NULL);
                    break;
                }
                len = atou_next_msg_len();
                len1 = sizeof (msg_atou) - cons_atou;
                if (len1 > len) {
                    /* Send data doesn't wrap */
                    len1 = len;
                    len2 = 0;
                } else {
                    /* Send data from end + beginning of circular buffer */
                    len2 = len - len1;
                }
                buf1 = msg_atou + cons_atou;
                buf2 = msg_atou;
                cons_atou = (cons_atou + len) & (sizeof (msg_atou) - 1);
            } else {
                if (msg_lock & BIT(3)) {
                    ks_reply(0, KS_STATUS_LOCKED, 0, NULL, 0, NULL);
                    break;
                }
                len = utoa_next_msg_len();
                len1 = sizeof (msg_utoa) - cons_utoa;
                if (len1 > len) {
                    /* Send data doesn't wrap */
                    len1 = len;
                    len2 = 0;
                } else {
                    /* Send data from end + beginning of circular buffer */
                    len2 = len - len1;
                }
                buf1 = msg_utoa + cons_utoa;
                buf2 = msg_utoa;
                cons_utoa = (cons_utoa + len) & (sizeof (msg_utoa) - 1);
            }
            if (len == 0) {
                ks_reply(0, KS_STATUS_NODATA, 0, NULL, 0, NULL);
printf("nd%s\n", (cmd & KS_MSG_ALTBUF) ? " alt" : "");
            } else {
                ks_reply(KS_REPLY_RAW, 0, len1, buf1, len2, buf2);
//              printf("l=%u+%u cb1=%u pb1=%u %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x%s\n", len1, len2, cons_atou, prod_atou, buf1[0], buf1[1], buf1[2], buf1[3], buf1[4], buf1[5], buf1[6], buf1[7], buf1[9], buf1[9], buf1[10], buf1[11], (cmd & KS_MSG_ALTBUF) ? " alt" : "");
            }
            break;
        }
        case KS_CMD_MSG_LOCK: {
            uint lockbits;
            cons_s = rx_consumer - (cmd_len + 1) / 2 - 1;
            if ((int) cons_s < 0)
                cons_s += ARRAY_SIZE(buffer_rxa_lo);
            lockbits = buffer_rxa_lo[cons_s];

            if (cmd & KS_MSG_UNLOCK) {
                msg_lock &= ~lockbits;
            } else {
                if (((lockbits & BIT(0)) && (msg_lock & BIT(2))) ||
                    ((lockbits & BIT(1)) && (msg_lock & BIT(3)))) {
                    /* Attempted to lock resource owned by the other side */
                    ks_reply(0, KS_STATUS_LOCKED, 0, NULL, 0, NULL);
                    break;
                }
                msg_lock |= lockbits;
            }
            ks_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            break;
        }
        case KS_CMD_CLOCK: {
            uint64_t  now  = timer_tick_get();
            uint64_t  usec = timer_tick_to_usec(now);

            if (cmd & (KS_CLOCK_SET | KS_CLOCK_SET_IFNOT)) {
                uint     pos;
                uint16_t adata[4];
                uint32_t t_usec;
                uint32_t t_sec;

                if (cmd_len != 8) {
                    ks_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
                    break;
                }

                cons_s = rx_consumer - (cmd_len + 1) / 2 - 1;
                if ((int) cons_s < 0)
                    cons_s += ARRAY_SIZE(buffer_rxa_lo);
                for (pos = 0; pos < 4; pos++) {
                    adata[pos] = buffer_rxa_lo[cons_s];
                    if (++cons_s == ARRAY_SIZE(buffer_rxa_lo))
                        cons_s = 0;
                }
                t_sec  = (adata[0] << 16) | adata[1];
                t_usec = (adata[2] << 16) | adata[3];
                if (((cmd & KS_CLOCK_SET_IFNOT) == 0) || (amiga_time == 0))
                    amiga_time = t_sec * 1000000ULL + t_usec - usec;
                ks_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            } else {
                uint am_time[2];
                if (amiga_time == 0) {
                    am_time[0] = 0;
                    am_time[1] = 0;
                } else {
                    uint64_t both   = usec + amiga_time;
                    uint32_t t_usec = both % 1000000;
                    uint32_t t_sec  = both / 1000000;
                    am_time[0] = SWAP32(t_sec);
                    am_time[1] = SWAP32(t_usec);
                }
                ks_reply(0, KS_STATUS_OK, sizeof (am_time), &am_time, 0, NULL);
            }
            break;
        }
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

new_cmd_post:
    while (rx_consumer != prod) {
        switch (magic_pos) {
            case 0: {
                uint16_t val = buffer_rxa_lo[rx_consumer];
#if 0
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
#endif
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
                cmd_len = (cmd_len + 1) & ~1;  // round up
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
                    uint8_t *ptr = (uint8_t *) &buffer_rxa_lo[rx_consumer];
                    crc = crc32r(crc, ptr + 1, 1);
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
#define CRC_DEBUG
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
                    goto new_cmd;
                }

                /* Execution phase */
                execute_cmd(cmd, cmd_len);
                magic_pos = 0;  // Restart magic detection
                goto new_cmd;
            default:
                printf("?");
                magic_pos = 0;  // Restart magic detection
                break;
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
        goto new_cmd_post;
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
    if (max == 0x999) {  // magic value
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

    ticks_per_200_nsec = timer_nsec_to_tick(200);
}
