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
#include "main.h"
#include "msg.h"
#include "ee_kicksmash.h"
#include "timer.h"
#include "utils.h"
#include "gpio.h"
#include "usb.h"
#include "version.h"
#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/cm3/nvic.h>

#define SWAP16(x)   __builtin_bswap16(x)
#define SWAP32(x)   __builtin_bswap32(x)
#define SWAP64(x)   __builtin_bswap64(x)

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* Flags to ks_reply() */
#define KS_REPLY_RAW    BIT(0)  // Don't emit header or CRC (raw data)
#define KS_REPLY_WE     BIT(1)  // Set up WE to trigger when host drives OE
#define KS_REPLY_WE_RAW (KS_REPLY_RAW | KS_REPLY_WE)

#undef CAPTURE_DMA_SWAP
#ifdef CAPTURE_DMA_SWAP
#define LOG_DMA_CONTROLLER DMA1
#define LOG_DMA_CHANNEL    DMA_CHANNEL5
#define LOG_DMA_NVIC_IRQ   NVIC_TIM2_IRQ
#define LOG_DMA_TIMER      TIM2
#else
#define LOG_DMA_CONTROLLER DMA2
#define LOG_DMA_CHANNEL    DMA_CHANNEL5
#define LOG_DMA_NVIC_IRQ   NVIC_TIM5_IRQ
#define LOG_DMA_TIMER      TIM5
#endif

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
#define nvic_enable_irq(irqn)   NVIC_ISER(irqn / 32) = (1 << (irqn % 32))
#define nvic_disable_irq(irqn)  NVIC_ICER(irqn / 32) = (1 << (irqn % 32))

extern uint8_t usb_serial_str[32];

static const uint16_t sm_magic[] = { 0x0204, 0x1017, 0x0119, 0x0117 };
static const uint8_t *sm_magic_b = (uint8_t *) sm_magic;
// static const uint16_t reset_magic_32[] = { 0x0000, 0x0001, 0x0034, 0x0035 };
// static const uint16_t reset_magic_16[] = { 0x0002, 0x0003, 0x0068, 0x0069 };

static const uint32_t testpatt_reply[] = {
    0x54534554, 0x54544150, 0x53202d20, 0x54524154,
    0xaaaa5555, 0xcccc3333, 0xeeee1111, 0x66669999,
    0x00020001, 0x00080004, 0x00200010, 0x00800040,
    0x02000100, 0x08000400, 0x20001000, 0x80004000,
    0xfffdfffe, 0xfff7fffb, 0xffdfffef, 0xff7fffbf,
    0xfdfffeff, 0xf7fffbff, 0xdfffefff, 0x7fffbfff,
    0x54534554, 0x54544150, 0x444e4520, 0x68646320,
};

#define REBOOT_MAGIC_NUM 8
static const uint16_t reboot_magic_32[] =
        { 0x0004, 0x0003, 0x0003, 0x0002, 0x0002, 0x0001, 0x0001, 0x0000 };
static const uint16_t reboot_magic_16[] =
        { 0x0007, 0x0006, 0x0005, 0x0004, 0x0003, 0x0002, 0x0001, 0x0000 };
static const uint16_t *reboot_magic;
static uint16_t reboot_magic_end;

static uint8_t  capture_mode = CAPTURE_ADDR;
static uint8_t  msg_lock;       // Bits !USB 0=atou 1=utoa, !Amiga 2=atou 3=utoa
static uint     consumer_wrap;
static uint     consumer_wrap_last_poll;
static uint     rx_consumer = 0;
uint64_t        amiga_time = 0;           // Seconds and microseconds
static uint64_t expire_update_amiga_app;  // Expiration time for last Amiga app
static uint64_t expire_update_usb_app;    // Expiration time for last USB app
static uint16_t state_amiga_app;          // Amiga app state
static uint16_t state_usb_app;            // USB app state

/* Message interface through Kicksmash between Amiga and USB host */
static uint     prod_atou;      // Producer for Amiga -> USB buffer
static uint     cons_atou;      // Consumer for Amiga -> USB buffer
static uint     prod_utoa;      // Producer for USB buffer -> Amiga
static uint     cons_utoa;      // Consumer for USB buffer -> Amiga
static uint     messages_atou;  // Count of Amiga-to-USB messages
static uint     messages_utoa;  // Count of USB-to-Amiga messages
static uint     messages_usb;   // Messages sent by USB Host
static uint     fail_crc_u;     // CRC message failures from USB Host
static uint     fail_cmd_u;     // Invalid command failures from USB Host

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
            if (diff & SOCKET_OE_PIN)
                printf(" F_OE=%u", !!(b & SOCKET_OE_PIN));
            if (diff & SOCKET_WE_PIN)
                printf(" WE=%u", !!(b & SOCKET_WE_PIN));
            if (diff & SOCKET_OEWE_PIN)
                printf(" OEWE=%u", !!(b & SOCKET_OEWE_PIN));
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
 * The Amiga-to-USB (atou) and USB-to-Amiga buffers are used to store data
 * which is to be moved between the Amiga and a USB host.
 *
 * Note that data is stored in byte-swapped order (B1 B0 B4 B3 B6 B5...).
 * This is done to help reduce latency in response to Amiga requests for
 * I/O, which are very timing-sensitive. The STM32 DMA hardware delivers
 * data to/from the GPIO ports in a byte-swapped manner.
 *
 * Compute buffer space available   Compute buffer space in use
 * (S-2)-(P-C)&(S-1)                (P-C)&(S-1)
 * 5 ops                            3 ops
 *
 * Producer / consumer scenarios
 *  _ _ _ _ _ _ _ _    _ _ _ _ _ _ _ _    _ _ _ _ _ _ _ _
 * |#|_|_|#|#|#|#|#|  |_|#|#|_|_|_|_|_|  |_|_|_|_|_|_|_|_|
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
 */
#define SPACE_INUSE_ATOU ((prod_atou - cons_atou) & (sizeof (msg_atou) - 1))
#define SPACE_INUSE_UTOA ((prod_utoa - cons_utoa) & (sizeof (msg_utoa) - 1))
#define SPACE_AVAIL_ATOU (sizeof (msg_atou) - 2 - SPACE_INUSE_ATOU)
#define SPACE_AVAIL_UTOA (sizeof (msg_utoa) - 2 - SPACE_INUSE_UTOA)

static uint
atou_add(uint len, void *ptr)
{
    uint xlen;
    uint8_t *sptr = ptr;
    len = (len + 1) & ~1;  // Round up to 16-bit alignment
    if (len > SPACE_AVAIL_ATOU) {
        /*
         * Should never get this failure on Amiga message side, as the
         * caller first checks for sufficient space.
         */
        return (1);
    }
    xlen = sizeof (msg_atou) - prod_atou;
    if (len <= xlen) {
        memcpy(msg_atou + prod_atou, sptr, len);
    } else {
        memcpy(msg_atou + prod_atou, sptr, xlen);
        memcpy(msg_atou, sptr + xlen, len - xlen);
    }
    prod_atou = (prod_atou + len) & (sizeof (msg_atou) - 1);
    messages_atou++;
    return (0);
}

static uint
utoa_add(uint len, void *ptr)
{
    uint xlen;
    uint8_t *sptr = ptr;
    len = (len + 1) & ~1;  // Round up to 16-bit alignment
    if (len > SPACE_AVAIL_UTOA)
        return (1);
    xlen = sizeof (msg_utoa) - prod_utoa;
    if (len <= xlen) {
        memcpy(msg_utoa + prod_utoa, sptr, len);
    } else {
        memcpy(msg_utoa + prod_utoa, sptr, xlen);
        memcpy(msg_utoa, sptr + xlen, len - xlen);
    }
    __asm__ volatile("dmb");
    prod_utoa = (prod_utoa + len) & (sizeof (msg_utoa) - 1);
    messages_utoa++;
    return (0);
}

static uint16_t
atou_next_msg_len(void)
{
    uint     len;
    uint     pos;
    uint     inuse = SPACE_INUSE_ATOU;
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
    len     = (len + 3) & ~3;  // Round up
    return (len + KS_HDR_AND_CRC_LEN);
}

static uint16_t
utoa_next_msg_len(void)
{
    uint     len;
    uint     pos;
    uint     inuse = SPACE_INUSE_UTOA;
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
    len     = (len + 3) & ~3;  // Round up
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

/*
 * address_input
 * -------------
 * Returns the current value present on the address pins.
 */
static uint32_t
address_input(void)
{
    uint32_t addr = GPIO_IDR(SOCKET_A0_PORT);
    addr |= ((GPIO_IDR(SOCKET_A16_PORT) & 0x00f0) << (16 - 4));
    return (addr);
}

/*
 * data_input
 * ----------
 * Returns the current value present on the data pins.
 */
static uint32_t
data_input(void)
{
    /*
     * Board Rev 2+
     *
     * D0-D15  = PD0-PD15
     * D16-D15 = PE0-PE15
     */
    return (GPIO_IDR(SOCKET_D0_PORT) | (GPIO_IDR(SOCKET_D16_PORT) << 16));
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

#ifdef CAPTURE_DMA_SWAP
    /* DMA from address GPIOs A16-A31 to memory */
    config_dma(DMA2, DMA_CHANNEL5, 0, 16,
               &GPIO_IDR(SOCKET_A16_PORT),
               buffer_rxd, ADDR_BUF_COUNT);
#else
#undef DMA_DEBUG
#ifdef DMA_DEBUG
    /* DMA from local variable to GPIO B, alternating PB5 low and high */
    static uint16_t values[2] = { BIT(5), 0 };
    config_dma(DMA2, DMA_CHANNEL5, 1, 16, &GPIO_ODR(BOOT0_PORT), values, 2);
#else
    /* DMA from address GPIOs A0-A15 to memory */
    config_dma(DMA2, DMA_CHANNEL5, 0, 16,
               &GPIO_IDR(SOCKET_A0_PORT),
               buffer_rxa_lo, ADDR_BUF_COUNT);
#endif
#endif

    /* Set up TIM5 CH1 to trigger DMA based on external PA0 pin */
    timer_disable_oc_output(TIM5, TIM_OC1);

#ifdef CAPTURE_DMA_SWAP
    /* Enable capture compare CC1 DMA */
    timer_enable_irq(TIM5, TIM_DIER_CC1DE);
#else
    /* Enable capture compare CC1 DMA and interrupt */
    timer_enable_irq(TIM5, TIM_DIER_CC1DE | TIM_DIER_CC1IE);
#endif

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
#ifdef CAPTURE_DMA_SWAP
        case CAPTURE_ADDR:
            src = &GPIO_IDR(SOCKET_A0_PORT);
            break;
#else
        case CAPTURE_ADDR:
            src = &GPIO_IDR(SOCKET_A16_PORT);
            break;
#endif
        case CAPTURE_DATA_LO:
            src = &GPIO_IDR(SOCKET_D0_PORT);
            break;
        case CAPTURE_DATA_HI:
            src = &GPIO_IDR(SOCKET_D16_PORT);
            break;
    }
#ifdef CAPTURE_DMA_SWAP
    config_dma(DMA1, DMA_CHANNEL5, 0, 16, src, buffer_rxa_lo, ADDR_BUF_COUNT);
#else
    config_dma(DMA1, DMA_CHANNEL5, 0, 16, src, buffer_rxd, ADDR_BUF_COUNT);
#endif

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
#ifdef CAPTURE_DMA_SWAP
    /* Enable capture compare CC1 DMA and interrupt */
    timer_enable_irq(TIM2, TIM_DIER_CC1DE | TIM_DIER_CC1IE);
#else
    /* Enable capture compare CC1 DMA */
    timer_enable_irq(TIM2, TIM_DIER_CC1DE);
#endif

    timer_set_dma_on_compare_event(TIM2);  // DMA on CCx event occurs
#define DISABLE_TIM2_DMA
#ifdef DISABLE_TIM2_DMA
    dma_disable_channel(DMA1, DMA_CHANNEL5);
#endif
}

static void
configure_oe_capture_rx(bool verbose)
{
    consumer_wrap = 0;
    rx_consumer = 0;
    config_tim2_ch1_dma(verbose);
    config_tim5_ch1_dma(verbose);

    /*
     * Not enough memory bandwidth on at least one CPU I have to have both
     * DMAs active and STM32 keep up with the Amiga. That particular STM32
     * does not have DFU, so it might be a remarked STM32F103 or something
     * else.
     */
#ifdef DISABLE_TIM2_DMA
    dma_disable_channel(DMA1, DMA_CHANNEL5);  // TIM2
#endif
    TIM_CCER(LOG_DMA_TIMER) |= TIM_CCER_CC1E;  // timer_enable_oc_output()
}

/*
 * fast_magic_search() looks for the next occurrence of the start of the
 *                     magic address sequence for when an Amiga program
 *                     wants to send a message to Kicksmash.
 *
 * The algorithm is implemented in a manner to reduce the SRAM bandwidth
 * required so that the DMA engine has lower latency. This is done by
 * fetching a 32-bit value at a time, and then comparing the entire
 * value against the first two 16-bit magic values, or the high 16 bits
 * against the first 16-bit magic value. Since a single fetch is done,
 * and the loop is small, memory bandwidth should be lower.
 */
static inline uint
fast_magic_search(uint prod)
{
    uint count;
    uint32_t *ptr;

    if (rx_consumer & 1) {
        /* Not 32-bit aligned */
        if (buffer_rxa_lo[rx_consumer] == sm_magic[0])
            return (0);  // found
        else
            return (1);  // not found
    }

    if (prod > rx_consumer)
        count = prod - rx_consumer;
    else
        count = ARRAY_SIZE(buffer_rxa_lo) - rx_consumer;

    ptr = (void *) &buffer_rxa_lo[rx_consumer];
    while (count > 1) {
        uint32_t value = *ptr;
        if (value == (((uint32_t) sm_magic[1] << 16) | sm_magic[0])) {
            return (0);
        }
        if ((value >> 16) == sm_magic[0]) {
            rx_consumer++;
            return (0);
        }
        count       -= 2;
        rx_consumer  = (rx_consumer + 2) % ARRAY_SIZE(buffer_rxa_lo);
        ptr++;
    }

    if (count == 1) {
        /* Not 32-bit aligned */
        if (buffer_rxa_lo[rx_consumer] == sm_magic[0])
            return (0);  // found
        else
            return (1);  // not found
    }

    rx_consumer--;  // Back up, since it will be incremented later
    return (1);
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
}

void
tim2_isr(void)
{
    TIM_SR(TIM2) = 0;  /* Clear all TIM2 interrupt status */

    process_addresses();
}

void
tim5_isr(void)
{
    TIM_SR(TIM5) = 0;  /* Clear all TIM5 interrupt status */

    process_addresses();
}

/*
 * bus_snoop
 * ---------
 * Capture bus address and/or data values which occur during Amiga
 * fetches of Kickstart ROM.
 *
 * This function either uses DMA hardware to do captures or the CPU will
 * directly poll GPIOs. The advantage of polling is that the full address
 * and data values can be captured. The disadvantage of polling is that
 * multiple fast accesses can be missed by software.
 */
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
        TIM_CCER(TIM2) |= TIM_CCER_CC1E;  // timer_enable_oc_output()
        dma_left = dma_get_number_of_data(LOG_DMA_CONTROLLER, LOG_DMA_CHANNEL);
        prod = ARRAY_SIZE(buffer_rxa_lo) - dma_left;
        if (prod > ARRAY_SIZE(buffer_rxa_lo))
            prod = 0;
        cons = prod;

        while (1) {
            if ((count++ & 0x0fff) == 0) {
                usb_poll();
                if (getchar() > 0)
                    break;
            }
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
                    if ((count++ & 0x0fff) == 0) {
                        usb_poll();
                        if (getchar() > 0)
                            goto snoop_abort;
                    }
                }
                printf("\n");
            }
        }
snoop_abort:
        return;
    }

    nvic_disable_irq(LOG_DMA_NVIC_IRQ);
    while (1) {
        if (oe_input() == 0) {
            /* Capture address on falling edge of OE */
            if (last_oe == 1) {
                uint32_t nprod = prod + 1;
                if (nprod >= ARRAY_SIZE(cap_addr))
                    nprod = 0;
                if (nprod != cons) {
                    /* FIFO has space, capture address */
                    oprod = prod;
                    prod = nprod;
                    no_data = 0;
                }
                last_oe = 0;
            }
            cap_addr[oprod] = address_input();  // Capture address
            cap_data[oprod] = data_input();     // Capture data
        } else {
            /* Capture data on rising edge of OE */
            if (last_oe == 0) {
                last_oe = 1;
                continue;
            }
        }
        if ((count++ & 0x0fff) == 0) {
            usb_poll();
            if (getchar() > 0)
                break;
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
    }
    nvic_enable_irq(LOG_DMA_NVIC_IRQ);
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
        nvic_enable_irq(LOG_DMA_NVIC_IRQ);
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

static uint8_t usb_msg_buffer[2048];

static void
usb_msg_reply(uint flags, uint status, uint rlen1, const void *rbuf1,
              uint rlen2, const void *rbuf2)
{
    uint rlen = rlen1 + rlen2;

    if (flags & KS_REPLY_RAW) {
        /* raw mode sends an already constructed message */
        if (rlen1 != 0) {
            if (puts_binary(rbuf1, rlen1))
                printf("puts_binary %u fail\n", rlen1);
        }
        if (rlen2 != 0) {
            if (puts_binary(rbuf2, rlen2))
                printf("puts_binary %u fail\n", rlen2);
        }
    } else {
        uint16_t data[2];
        uint32_t crc;
        if (puts_binary(sm_magic, sizeof (sm_magic)))
            printf("puts_binary %u fail\n", sizeof (sm_magic));
        data[0] = rlen;
        data[1] = status;
        crc = crc32s(0, data, sizeof (data));
        if (puts_binary(data, sizeof (data)))
            printf("puts_binary %u fail\n", sizeof (data));
        if (rlen1 != 0) {
            if (puts_binary(rbuf1, rlen1))
                printf("puts_binary %u fail\n", rlen1);
            crc = crc32s(crc, rbuf1, rlen1);
        }
        if (rlen2 != 0) {
            if (puts_binary(rbuf2, rlen2))
                printf("puts_binary %u fail\n", rlen2);
            crc = crc32s(crc, rbuf2, rlen2);
        }
        crc = (crc << 16) | (crc >> 16);  // Convert to match Amiga format
        if (puts_binary(&crc, sizeof (crc)))
            printf("puts_binary %u fail\n", sizeof (crc));
    }
}

static void
execute_usb_cmd(uint16_t cmd, uint16_t cmd_len, uint8_t *rawbuf)
{
    uint8_t *buf = rawbuf + 12;
    switch ((uint8_t) cmd) {
        case KS_CMD_NULL:
            /* Do absolutely nothing (discard command) */
            break;
        case KS_CMD_NOP:
            /* Do nothing but reply */
            usb_msg_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            break;  // End processing
        case KS_CMD_ID: {
            /* Send KickSmash identification and configuration */
            smash_id_t reply;
            uint temp[3];
            int  pos = 0;
            memset(&reply, 0, sizeof (reply));
            sscanf(version_str + 8, "%u.%u%n", &temp[0], &temp[1], &pos);
            reply.si_ks_version[0] = SWAP16(temp[0]);
            reply.si_ks_version[1] = SWAP16(temp[1]);
            if (pos == 0)
                pos = 18;
            else
                pos += 8 + 7;
            sscanf(version_str + pos, "%04u-%02u-%02u",
                   &temp[0], &temp[1], &temp[2]);
            reply.si_ks_date[0] = temp[0] / 100;
            reply.si_ks_date[1] = temp[0] % 100;
            reply.si_ks_date[2] = temp[1];
            reply.si_ks_date[3] = temp[2];
            pos += 11;
            sscanf(version_str + pos, "%02u:%02u:%02u",
                   &temp[0], &temp[1], &temp[2]);
            reply.si_ks_time[0] = temp[0];
            reply.si_ks_time[1] = temp[1];
            reply.si_ks_time[2] = temp[2];
            reply.si_ks_time[3] = 0;
            strcpy(reply.si_serial, (const char *)usb_serial_str);
            reply.si_rev      = SWAP16(0x0001);     // Protocol version 0.1
            reply.si_features = SWAP16(0x0001);     // Features
            reply.si_usbid    = SWAP32(0x12091610); // Matches USB ID
            reply.si_mode     = ee_mode;
            reply.si_unused1  = 0;
            reply.si_usbdev   = usb_current_address();
            strcpy(reply.si_name, config.name);
            memset(reply.si_unused, 0, sizeof (reply.si_unused));
            usb_msg_reply(0, KS_STATUS_OK, sizeof (reply), &reply, 0, NULL);
            break;
        }
        case KS_CMD_UPTIME: {
            uint64_t now = timer_tick_get();
            uint64_t usec = timer_tick_to_usec(now);
            usec = SWAP64(usec);  // Big endian format
            usb_msg_reply(0, KS_STATUS_OK, sizeof (usec), &usec, 0, NULL);
            break;
        }
        case KS_CMD_TESTPATT: {
            /* Send special data pattern (for test / diagnostic) */
            usb_msg_reply(0, KS_STATUS_OK, sizeof (testpatt_reply),
                          &testpatt_reply, 0, NULL);
            break;
        }
        case KS_CMD_LOOPBACK: {
            uint raw_len = cmd_len + KS_HDR_AND_CRC_LEN;  // Magic+len+cmd+CRC
            usb_msg_reply(1, 0, raw_len, rawbuf, 0, NULL);
            break;
        }
        case KS_CMD_SET:
            if (cmd & KS_SET_NAME) {
                if (cmd_len != 16) {
                    usb_msg_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
                    break;
                }
                config_name((char *)buf);
                usb_msg_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            } else {
                usb_msg_reply(0, KS_STATUS_BADARG, 0, NULL, 0, NULL);
            }
            break;
#if 0
        case KS_CMD_BANK_INFO:
            /* Get bank info */
            usb_msg_reply(0, KS_STATUS_OK, sizeof (config.bi),
                          &config.bi, 0, NULL);
            break;
#endif
        case KS_CMD_MSG_STATE: {
            uint16_t reply[2];
            if (cmd & KS_MSG_STATE_SET) {
                uint16_t mask;
                uint16_t state;
                uint16_t expire = 10000;  // 10 seconds
                if ((cmd_len != 4) && (cmd_len != 6)) {
                    usb_msg_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
                    break;
                }
                mask = (buf[0] << 8) | buf[1];
                state = (buf[2] << 8) | buf[3];
                if (cmd_len == 6) {
                    expire = (buf[4] << 8) | buf[5];
                }
                state_usb_app = (state_usb_app & ~mask) | (state & mask);
                expire_update_usb_app = timer_tick_plus_msec(expire);
            }
            reply[0] = SWAP16(state_amiga_app);
            reply[1] = SWAP16(state_usb_app);
            usb_msg_reply(0, KS_STATUS_OK, sizeof (reply), &reply, 0, NULL);
            break;
        }
        case KS_CMD_MSG_INFO: {
            smash_msg_info_t reply;
            uint16_t         avail_atou;
            uint16_t         avail_utoa;
            uint16_t         inuse_atou;
            uint16_t         inuse_utoa;

            if (msg_lock & BIT(0)) {
                inuse_atou = 0;
                avail_atou = 0;
            } else {
                inuse_atou = SPACE_INUSE_ATOU;
                avail_atou = SPACE_AVAIL_ATOU;
                if (avail_atou >= KS_HDR_AND_CRC_LEN)
                    avail_atou -= KS_HDR_AND_CRC_LEN;
                else
                    avail_atou = 0;
            }

            if (msg_lock & BIT(1)) {
                inuse_utoa = 0;
                avail_utoa = 0;
            } else {
                inuse_utoa = SPACE_INUSE_UTOA;
                avail_utoa = SPACE_AVAIL_UTOA;
                if (avail_utoa >= KS_HDR_AND_CRC_LEN)
                    avail_utoa -= KS_HDR_AND_CRC_LEN;
                else
                    avail_utoa = 0;
            }

            if (timer_tick_has_elapsed(expire_update_amiga_app))
                state_amiga_app = 0;
            if (timer_tick_has_elapsed(expire_update_usb_app))
                state_usb_app = 0;

            reply.smi_atou_inuse  = SWAP16(inuse_atou);
            reply.smi_atou_avail  = SWAP16(avail_atou);
            reply.smi_utoa_inuse  = SWAP16(inuse_utoa);
            reply.smi_utoa_avail  = SWAP16(avail_utoa);
            reply.smi_state_amiga = SWAP16(state_amiga_app);
            reply.smi_state_usb   = SWAP16(state_usb_app);
            memset(reply.smi_unused, 0, sizeof (reply.smi_unused));
            usb_msg_reply(0, KS_STATUS_OK, sizeof (reply), &reply, 0, NULL);
            break;
        }
        case KS_CMD_MSG_SEND: {
            uint64_t new_expire;
            uint rc;
            uint raw_len = cmd_len + KS_HDR_AND_CRC_LEN;  // Magic+len+cmd+CRC
            raw_len = (raw_len + 3) & ~3;                 // round up

            if ((((cmd & KS_MSG_ALTBUF) == 0) && (msg_lock & BIT(1))) ||
                (((cmd & KS_MSG_ALTBUF) != 0) && (msg_lock & BIT(0)))) {
                usb_msg_reply(0, KS_STATUS_LOCKED, 0, NULL, 0, NULL);
                break;
            }
            if ((cmd & KS_MSG_ALTBUF) == 0)
                rc = utoa_add(raw_len, rawbuf);
            else
                rc = atou_add(raw_len, rawbuf);

            if (rc != 0)
                usb_msg_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
            else
                usb_msg_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);

            /* Extend state expiration when transfer in progress */
            new_expire = timer_tick_plus_msec(1000);
            if (expire_update_usb_app < new_expire)
                expire_update_usb_app = new_expire;
#ifdef UMSG_DEBUG
            char sbuf[16];
//          sprintf(sbuf, " US%u,%u", raw_len, SPACE_INUSE_UTOA);
            sprintf(sbuf, " US%u", raw_len);
            uart_puts(sbuf);
#endif
            break;
        }
        case KS_CMD_MSG_RECEIVE: {
            uint64_t new_expire;
            uint     len;
            uint     len1;
            uint     len2;
            uint8_t *buf1;
            uint8_t *buf2;

            if ((((cmd & KS_MSG_ALTBUF) == 0) && (msg_lock & BIT(0))) ||
                (((cmd & KS_MSG_ALTBUF) != 0) && (msg_lock & BIT(1)))) {
                usb_msg_reply(0, KS_STATUS_LOCKED, 0, NULL, 0, NULL);
                break;
            }

            if ((cmd & KS_MSG_ALTBUF) == 0) {
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
            } else {
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
            }
            if (len == 0) {
                usb_msg_reply(0, KS_STATUS_NODATA, 0, NULL, 0, NULL);
                break;
            }

            usb_msg_reply(KS_REPLY_RAW, 0, len1, buf1, len2, buf2);
            if ((cmd & KS_MSG_ALTBUF) == 0)
                cons_atou = (cons_atou + len) & (sizeof (msg_atou) - 1);
            else
                cons_utoa = (cons_utoa + len) & (sizeof (msg_utoa) - 1);

            /* Extend state expiration when transfer in progress */
            new_expire = timer_tick_plus_msec(1000);
            if (expire_update_usb_app < new_expire)
                expire_update_usb_app = new_expire;
#ifdef UMSG_DEBUG
            char sbuf[16];
            sprintf(sbuf, " UR%u", len1 + len2);
            uart_puts(sbuf);
            if (len2 != 0)
                uart_putchar('*');
#endif
            break;
        }
        case KS_CMD_MSG_LOCK: {
            uint lockbits = (buf[0] << 8) | buf[1];

            if (cmd & KS_MSG_UNLOCK) {
                msg_lock &= ~lockbits;
            } else {
                if (((lockbits & BIT(2)) && (msg_lock & BIT(0))) ||
                    ((lockbits & BIT(3)) && (msg_lock & BIT(1)))) {
                    /* Attempted to lock resource owned by the other side */
                    usb_msg_reply(0, KS_STATUS_LOCKED, 0, NULL, 0, NULL);
                    break;
                }
                msg_lock |= lockbits;
            }
            usb_msg_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            break;
        }
        case KS_CMD_MSG_FLUSH:
            if ((((cmd & KS_MSG_ALTBUF) == 0) && (msg_lock & BIT(0))) ||
                (((cmd & KS_MSG_ALTBUF) != 0) && (msg_lock & BIT(1)))) {
                usb_msg_reply(0, KS_STATUS_LOCKED, 0, NULL, 0, NULL);
                break;
            }
            if ((cmd & KS_MSG_ALTBUF) == 0)
                cons_atou = prod_atou;  // default: flush "my" receive buffer
            else
                cons_utoa = prod_utoa;
            usb_msg_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            break;
        case KS_CMD_CLOCK: {
            uint64_t  now  = timer_tick_get();
            uint64_t  usec = timer_tick_to_usec(now);
            uint32_t  am_time[2];

            if (cmd & (KS_CLOCK_SET | KS_CLOCK_SET_IFNOT)) {
                uint32_t t_usec;
                uint32_t t_sec;

                if (cmd_len != 8) {
                    usb_msg_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
                    break;
                }
                memcpy(am_time, buf, sizeof (am_time));
                t_sec  = am_time[0];
                t_usec = am_time[1];
                if (((cmd & KS_CLOCK_SET_IFNOT) == 0) || (amiga_time == 0))
                    amiga_time = t_sec * 1000000ULL + t_usec - usec;
                usb_msg_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            } else {
                if (amiga_time == 0) {
                    am_time[0] = 0;
                    am_time[1] = 0;
                } else {
                    uint64_t both   = usec + amiga_time;
                    uint32_t t_usec = both % 1000000;
                    uint32_t t_sec  = both / 1000000;
                    am_time[0] = t_sec;
                    am_time[1] = t_usec;
                }
                usb_msg_reply(0, KS_STATUS_OK, sizeof (am_time), &am_time,
                              0, NULL);
            }
            break;
        }
        default:
            fail_cmd_u++;
            break;
    }
}

void
msg_usb_service(void)
{
    uint     ch;
    uint     len = 0;
    uint     len_rounded = 0;
    uint     pos = 0;
    uint32_t crc;

    while (1) {
        ch = getchar();
        if ((int)ch == -1) {
            /* Timeout will clobber received data and reset */
            uint64_t timeout = timer_tick_plus_msec(200);
            while ((int)(ch = getchar()) == -1) {
                main_poll();
                if (timer_tick_has_elapsed(timeout)) {
                    pos = 0;
                    break;
                }
            }
            if ((int)ch == -1) {
                continue;
            }
        }
        usb_msg_buffer[pos] = ch;
        switch (pos) {
            case 0:  // Magic start
                if ((ch == 0x3) || (ch == '\n') || (ch == '\r'))
                    return;  // Abort received ^C, LF, or CR
                /* FALLTHROUGH */
            case 1:  // Magic
            case 2:  // Magic
            case 3:  // Magic
            case 4:  // Magic
            case 5:  // Magic
            case 6:  // Magic
            case 7:  // Magic
                if (ch != sm_magic_b[pos])
                    pos = 0;
                else
                    pos++;
                break;
            case 8:  // Length phase 1
                messages_usb++;
                len = ch;
                pos++;
                break;
            case 9:  // Length phase 2
                len |= (ch << 8);
                len_rounded = (len + 3) & ~3;
                if (len > sizeof (usb_msg_buffer) - 16) {
                    /* Bad length */
                    pos = 0;
                    break;
                }
                pos++;
                break;
            case 10:  // Command phase 1
            case 11:  // Command phase 2
                pos++;
                break;
            default: {  // Data and CRC phase
                uint32_t crc_rx;
                uint     cmd;
                if (pos != len_rounded + 15) {
                    /* More data pending */
                    pos++;
                    break;
                }

                /*
                 * Last byte of CRC received. CRC region begins after
                 * sm_magic (8 bytes) and includes length (2) + cmd (2).
                 */
                crc = crc32s(0, usb_msg_buffer + 8, len + 4);
                cmd = usb_msg_buffer[10] | (usb_msg_buffer[11] << 8);
                crc_rx = (usb_msg_buffer[12 + 1 + len_rounded] << 24) |
                         (usb_msg_buffer[12 + 0 + len_rounded] << 16) |
                         (usb_msg_buffer[12 + 3 + len_rounded] << 8) |
                         (usb_msg_buffer[12 + 2 + len_rounded]);
                if (crc != crc_rx) {
                    uint16_t error[2];
                    error[0] = KS_STATUS_CRC;
                    error[1] = crc;
                    usb_msg_reply(0, KS_STATUS_CRC, sizeof (error),
                                  &error, 0, NULL);
                    fail_crc_u++;
                    printf("Ucmd=%x l=%04x CRC %08lx != calc %08lx\n",
                           cmd, len, crc_rx, crc);
                    pos = 0;
                    break;
                }
                execute_usb_cmd(cmd, len, usb_msg_buffer);

                pos = 0;
                break;
            }
        }
    }
}

void
msg_shutdown(void)
{
    nvic_disable_irq(LOG_DMA_NVIC_IRQ);
    timer_disable_irq(TIM2, TIM_DIER_CC1IE);
    timer_disable_irq(TIM5, TIM_DIER_CC1IE);
    dma_disable_channel(DMA1, DMA_CHANNEL5);  // TIM2
    dma_disable_channel(DMA2, DMA_CHANNEL5);  // TIM5
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
    nvic_set_priority(LOG_DMA_NVIC_IRQ, 0x20);
    nvic_enable_irq(LOG_DMA_NVIC_IRQ);

    capture_mode = CAPTURE_ADDR;
    configure_oe_capture_rx(true);
#ifdef DMA_DEBUG
    gpio_setv(BOOT0_PORT, BOOT0_PIN, 0);
    gpio_setmode(BOOT0_PORT, BOOT0_PIN, GPIO_SETMODE_OUTPUT_PPULL_50);
#endif
}
