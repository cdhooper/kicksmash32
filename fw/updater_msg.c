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
#include <stdint.h>
#include "board.h"
#include "main.h"
#include "uart.h"
#include <stdbool.h>
#include <string.h>
#include "irq.h"
#include "clock.h"
#include "config.h"
#include "crc32.h"
#include "kbrst.h"
#include "main.h"
#include "msg.h"
#include "m29f160xt.h"
#include "timer.h"
#include "utils.h"
#include "gpio.h"
#include "persist.h"
#include "main.h"
#include "version.h"
#include "cmdline.h"
#include "stm32flash.h"
#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>

#define SWAP16(x)   __builtin_bswap16(x)
#define SWAP32(x)   __builtin_bswap32(x)
#define SWAP64(x)   __builtin_bswap64(x)

#define FW_READ_MAX   2048     // Maximum bytes per KS_CMD_FW_READ
#define FW_WRITE_MAX  2048     // Maximum bytes per KS_CMD_FW_WRITE
#define FW_FLASH_SIZE 0x40000  // Writable STM32 flash (see stm32flash.c)

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* Flags to ks_reply() */
#define KS_REPLY_RAW    BIT(0)  // Don't emit header or CRC (raw data)
#define KS_REPLY_WE     BIT(1)  // Set up WE to trigger when host drives OE
#define KS_REPLY_WE_RAW (KS_REPLY_RAW | KS_REPLY_WE)

#define LOG_DMA_CONTROLLER DMA2
#define LOG_DMA_CHANNEL    DMA_CHANNEL5
#define LOG_DMA_NVIC_IRQ   NVIC_TIM5_IRQ
#define LOG_DMA_TIMER      TIM5

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

/* Linker script symbol: value is the firmware image size in flash */
extern uint8_t _fw_size;

extern uint8_t usb_serial_str[32];

static const uint16_t sm_magic[] = { 0x0204, 0x1017, 0x0119, 0x0117 };

static uint64_t ks_timeout_timer = 0;  // timer too frequent complaint message
static uint     ks_timeout_count = 0;  // count of complaint messages

static uint     consumer_wrap;
static uint     consumer_wrap_last_poll;
static uint     rx_consumer = 0;

/* Buffers for DMA from/to GPIOs and Timer event generation registers */
#define ADDR_BUF_COUNT 1200
#define ALIGN  __attribute__((aligned(16)))
ALIGN volatile uint16_t buffer_rxa_lo[ADDR_BUF_COUNT];
ALIGN volatile uint16_t buffer_rxd[ADDR_BUF_COUNT];
ALIGN volatile uint16_t          buffer_txd_lo[ADDR_BUF_COUNT * 2];
ALIGN volatile uint16_t          buffer_txd_hi[ADDR_BUF_COUNT];

/*
 * gpio_set_1
 * ----------
 * Drives the specified GPIO bits to 1 values without affecting other bits.
 */
static void
gpio_set_1(uint32_t GPIOx, uint16_t GPIO_Pins)
{
    GPIO_BSRR(GPIOx) = GPIO_Pins;
}

/*
 * gpio_set_0
 * ----------
 * Drives the specified GPIO bits to 0 values without affecting other bits.
 */
static void
gpio_set_0(uint32_t GPIOx, uint16_t GPIO_Pins)
{
    GPIO_BSRR(GPIOx) = GPIO_Pins << 16;
}

/*
 * gpio_setv
 * ---------
 * Sets the specified GPIO bits to 0 or 1 values without affecting other bits.
 */
void
gpio_setv(uint32_t GPIOx, uint16_t GPIO_Pins, int value)
{
    if (value == 0)
        gpio_set_0(GPIOx, GPIO_Pins);
    else
        gpio_set_1(GPIOx, GPIO_Pins);
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
 * oe_output_enable
 * ----------------
 * Enable drive of the FLASH_OE pin, which is flash output enable OE#.
 */
void
oe_output_enable(void)
{
    /* FLASH_OE = PB13 */
    GPIO_CRH(FLASH_OE_PORT) = (GPIO_CRH(FLASH_OE_PORT) & 0xff0fffff) |
                              0x00100000;  // Output

//  gpio_setmode(FLASH_OE_PORT, FLASH_OE_PIN, GPIO_SETMODE_OUTPUT_PPULL_50);
}

/*
 * oe_output_disable
 * -----------------
 * Disable drive of the FLASH_OE pin, which is flash output enable OE#.
 */
void
oe_output_disable(void)
{
    /* FLASH_OE = PB13 */
    GPIO_CRH(FLASH_OE_PORT) = (GPIO_CRH(FLASH_OE_PORT) & 0xff0fffff) |
                              0x00400000;  // Input

//  gpio_setmode(FLASH_OE_PORT, FLASH_OE_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
}

/*
 * oe_output
 * ---------
 * Drives the OE# (flash output enable) pin with the specified value.
 */
void
oe_output(uint value)
{
#ifdef DEBUG_SIGNALS
    uart_puts(" OE=");
    uart_puthex(value);
#endif
    gpio_setv(FLASH_OE_PORT, FLASH_OE_PIN, value);
}

/*
 * data_output
 * -----------
 * Writes the specified value to the data output pins.
 */
void
data_output(uint32_t data)
{
    GPIO_ODR(FLASH_D0_PORT)  = data;                      // Set D0-D15
    GPIO_ODR(FLASH_D16_PORT) = (data >> 16);              // Set D16-D31
}

/*
 * oewe_output
 * -----------
 * Drives the OEWE (flash write enable on output enable) pin with the
 * specified value. If this pin is high, when the host drives OE# low,
 * it will result in WE# being driven low.
 */
static void
oewe_output(uint value)
{
    gpio_setv(FLASH_OEWE_PORT, FLASH_OEWE_PIN, value);
}

/*
 * we_enable
 * ---------
 * Enables or disables WE# pin output
 */
static void
we_enable(uint value)
{
    gpio_setmode(FLASH_WE_PORT, FLASH_WE_PIN,
                 (value != 0) ? GPIO_SETMODE_OUTPUT_PPULL_50 :
                                GPIO_SETMODE_INPUT_PULLUPDOWN);
}

/*
 * data_output_enable
 * ------------------
 * Enables the data pins for output.
 */
void
data_output_enable(void)
{
    GPIO_CRL(FLASH_D0_PORT)  = 0x33333333; // Output Push-Pull
    GPIO_CRH(FLASH_D0_PORT)  = 0x33333333;
    GPIO_CRL(FLASH_D16_PORT) = 0x33333333;
    GPIO_CRH(FLASH_D16_PORT) = 0x33333333;
}

/*
 * data_output_disable
 * -------------------
 * Reverts the data pins back to input (don't drive).
 */
void
data_output_disable(void)
{
    /* D0-D15 = PD0-PD15, D16-D31=PE0-PE15 */
    GPIO_CRL(FLASH_D0_PORT)  = 0x88888888; // Input Pull-Up / Pull-Down
    GPIO_CRH(FLASH_D0_PORT)  = 0x88888888;
    GPIO_CRL(FLASH_D16_PORT) = 0x88888888;
    GPIO_CRH(FLASH_D16_PORT) = 0x88888888;
    data_output(0);
}

/*
 * flash_oe_input
 * --------------
 * Return the current value of the FLASH_OE pin (either 0 or non-zero).
 */
static uint
flash_oe_input(void)
{
    return (GPIO_IDR(FLASH_OE_PORT) & FLASH_OE_PIN);
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
//      uart_puts("Addr lo capture %08x (t5c1) 16-bit\n",
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
}

static void
config_tim2_ch1_dma(bool verbose)
{
    volatile void *src;

    if (verbose) {
        memset((void *) buffer_rxd, 0, sizeof (buffer_rxd));
//      uart_puts("Addr hi capture %08x (t2c1) 16-bit\n",
//             (uintptr_t) buffer_rxd);
    }

    timer_disable_oc_output(TIM2, TIM_OC1);

    /* Word-wide DMA from data GPIOs D0-D15 to memory */
    src = &GPIO_IDR(SOCKET_A16_PORT);

    config_dma(DMA1, DMA_CHANNEL5, 0, 16, src, buffer_rxd, ADDR_BUF_COUNT);

    timer_set_ti1_ch1(TIM2);        // Capture input from channel 1 only

    timer_set_oc_polarity_low(TIM2, TIM_OC1);

    /* Select the Input and set the filter off */
    TIM2_CCMR1 &= ~(TIM_CCMR1_CC1S_MASK | TIM_CCMR1_IC1F_MASK);
    TIM2_CCMR1 |= TIM_CCMR1_CC1S_IN_TI1 | TIM_CCMR1_IC1F_OFF;

    TIM2_SMCR = // TIM_SMCR_ETP      | // falling edge detection
                TIM_SMCR_ECE      | // external clock mode 2 (ETR input)
                TIM_SMCR_ETPS_OFF | // no prescaler
                TIM_SMCR_ETF_OFF;   // no filter
    TIM2_DIER = 0;
    /* Enable capture compare CC1 DMA */
    timer_enable_irq(TIM2, TIM_DIER_CC1DE);

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

static int
atoi(const char *str)
{
    int value;

    for (value = 0; (*str >= '0') && (*str <= '9'); str++)
        value = value * 10 + (*str - '0');

    return (value);
}

/*
 * ks_reply
 * --------
 * This function sends a reply message to the Amiga host operating system.
 * It will disable flash output and drive data lines directly from the STM32.
 * This routine is called from interrupt context.
 *
 * flags  - message send flags
 *          0               - normal message; encapsulate with header + CRC
 *          KS_REPLY_RAW    - do not encapsulate the message with header + CRC
 *          KS_REPLY_WR     - enable flash write latch during transfer
 *          KS_REPLY_WR_RAW - do not encapsulate and enable flash write latch
 * status - reply status, stored in the cmd field of the message header.
 * rlen1  - Length of the reply message (first part).
 *          This length may be zero if the reply is just status.
 * rbuf1  - Reply message buffer (first part).
 * rlen2  - Length of the second part of the reply message (optional).
 * rbuf2  - Second part of the reply message buffer (optional).
 */
static void
ks_reply(uint flags, uint status, uint rlen1, const void *rbuf1,
         uint rlen2, const void *rbuf2)
{
    uint      count;
    uint      pos = 0;
    uint      dma_left;
    uint      dma_last;
    uint16_t  rlen = rlen1 + rlen2;

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

    if ((persist.sp_ee_mode == EE_MODE_32) ||
        (persist.sp_ee_mode == EE_MODE_32_SWAP)) {
        /*
         * For 32-bit mode, separate low and high 16 bits as they are
         * driven out by separate DMA engines (TIM5 and TIM2).
         */
        uint32_t *rbp;
        uint32_t  val;
        uint      rlen1_odd = (rlen1 + 1) & 2;
        /*
         * Calculation for rlen alignment:
         *     (rlen1 & 3) == 0: EVEN (ends on 32-bit boundary)
         *     (rlen1 & 3) == 1: ODD
         *     (rlen1 & 3) == 2: ODD
         *     (rlen1 & 3) == 3: EVEN (ends on 32-bit boundary)
         */
        if (flags & KS_REPLY_RAW) {
            /* Raw message -- send without header or CRC */
            uint16_t *txl = (uint16_t *)buffer_txd_lo;
            uint16_t *txh = (uint16_t *)buffer_txd_hi;

            if (persist.sp_ee_mode == EE_MODE_32_SWAP) {
                txl = (uint16_t *)buffer_txd_hi;
                txh = (uint16_t *)buffer_txd_lo;
            }

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
                uint      rlen2_odd = (rlen2 + 1) & 2;

                rlen2 = (rlen2 + 3) / 4;

                if (rlen1_odd == 0) {
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
                    if (rlen2_odd)
                        txh--;          // Odd first + odd second = even
                }
            }
            pos = (rlen + 3) / 4;
        } else {
            /* Encapsulate reply data in header + CRC */
            uint16_t *txl = (uint16_t *)buffer_txd_lo;
            uint16_t *txh = (uint16_t *)buffer_txd_hi;
            uint32_t crc;
            for (count = 0; count < ARRAY_SIZE(sm_magic); ) {
                *(txh++) = sm_magic[count++];
                *(txl++) = sm_magic[count++];
            }
            *(txh++) = rlen;
            *(txl++) = status;
            rbp = (uint32_t *) rbuf1;
            crc = crc32r(0, &rlen, 2);
            crc = crc32r(crc, &status, 2);
            crc = crc32(crc, rbuf1, rlen1);

            for (count = (rlen1 + 3) / 4; count > 0; count--) {
                val = *(rbp++);
                *(txh++) = (val << 8)  | ((val >> 8) & 0x00ff);
                *(txl++) = (val >> 24) | ((val >> 8) & 0xff00);
            }
            if (rlen2 != 0) {
                uint rlen2_odd = (rlen2 + 1) & 2;

                crc = crc32(crc, rbuf2, rlen2);
                rbp = (uint32_t *) rbuf2;
                if (rlen1_odd == 0) {
                    /* First chunk was even multiple of 4 bytes */
                    for (count = (rlen2 + 3) / 4; count > 0; count--) {
                        val = *(rbp++);
                        *(txh++) = (val << 8)  | ((val >> 8) & 0x00ff);
                        *(txl++) = (val >> 24) | ((val >> 8) & 0xff00);
                    }
                } else {
                    /* First chunk was not even multiple of 4 bytes */
                    txl--;   // Fixup first chunk overrun

                    for (count = (rlen2 + 3) / 4; count > 0; count--) {
                        val = *(rbp++);
                        *(txl++) = (val << 8)  | ((val >> 8) & 0x00ff);
                        *(txh++) = (val >> 24) | ((val >> 8) & 0xff00);
                    }
                    if (rlen2_odd) {
                        txh--;              // Odd first + odd second = even
                    } else {
                        *(txl++) = 0xaaaa;  // Align for CRC
                    }
                }
            } else if (rlen1_odd) {
                txl[-1] = 0xbbbb;  // Alignment data (DEBUG)
            }
            *(txh++) = crc >> 16;
            *(txl++) = (uint16_t) crc;
            pos = (rlen + 3 + KS_HDR_AND_CRC_LEN) / 4;
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
            buffer_txd_lo[pos++] = rlen;
            buffer_txd_lo[pos++] = status;
            crc = crc32r(0, &rlen, 2);
            crc = crc32r(crc, &status, 2);
            crc = crc32(crc, rbuf1, rlen1);
            if (rlen1 != 0) {
                memcpy((void *)&buffer_txd_lo[pos], rbuf1, rlen1);
                pos += rlen1 / 2;
            }
            if (rlen2 != 0) {
                crc = crc32(crc, rbuf2, rlen2);
                memcpy((void *)&buffer_txd_lo[pos], rbuf2, rlen2);
                pos += (rlen2 + 1) / 2;
            }
            buffer_txd_lo[pos++] = crc >> 16;
            buffer_txd_lo[pos++] = (uint16_t) crc;
            pos = (rlen + 3 + KS_HDR_AND_CRC_LEN) / 2;
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
    }

    /* FLASH_OE=1 disables flash from driving data pins */
    oe_output(1);
    oe_output_enable();  // Enable override of FLASH_OE

    /*
     * Board rev 3 and higher have external bus tranceiver, so STM32 can
     * always drive data bus so long as FLASH_OE is disabled.
     */
    data_output_enable();  // Drive data pins

    if (flags & KS_REPLY_WE)
        we_enable(0);      // Pull up WE instead of driving it high

    disable_irq();

    /* Wait for OE to go low */
    count = 0;
    while (oe_input() != 0) {
        if (count++ > 100000) {
            enable_irq();
            if (timer_tick_has_elapsed(ks_timeout_timer))
                ks_timeout_count = 0;
            if (ks_timeout_count++ < 4)
                uart_puts("OE low timeout\n");
            ks_timeout_timer = timer_tick_plus_msec(1000);
            goto oe_reply_end;
        }
    }

    /* Wait for OE to go high before enabling DMA */
    count = 0;
    while ((oe_input() == 0) || (flash_oe_input() == 0)) {
        if (count++ > 100000) {
            enable_irq();
            if (timer_tick_has_elapsed(ks_timeout_timer))
                ks_timeout_count = 0;
            if (ks_timeout_count++ < 4)
                uart_puts("OE high timeout\n");
            ks_timeout_timer = timer_tick_plus_msec(1000);
            goto oe_reply_end;
        }
    }

    if (flags & KS_REPLY_WE)
        oewe_output(1);  // Allow SOCKET_OE to drive WE

    if ((persist.sp_ee_mode == EE_MODE_32) ||
        (persist.sp_ee_mode == EE_MODE_32_SWAP)) {
        TIM2_EGR = TIM_EGR_CC1G;         // Generate first DMA trigger
        TIM5_EGR = TIM_EGR_CC1G;         // Generate first DMA trigger
        TIM_CCER(TIM2) = TIM_CCER_CC1E;  // Enable DMA, rising edge
        TIM_CCER(TIM5) = TIM_CCER_CC1E;  // Enable DMA, rising edge
    } else {
        TIM5_EGR = TIM_EGR_CC1G;         // Generate first DMA trigger
        TIM_CCER(TIM5) = TIM_CCER_CC1E;  // Enable DMA, rising edge
    }
    enable_irq();

    dma_last = dma_get_number_of_data(DMA2, DMA_CHANNEL5);

    while (dma_last != 0) {
        dma_left = dma_get_number_of_data(DMA2, DMA_CHANNEL5);
        if (dma_last == dma_left) {
            for (count = 0; dma_last == dma_left; count++) {
                if (count > 100000) {
                    if (flags & KS_REPLY_WE)
                        oewe_output(0);  // Disconnect SOCKET_OE from WE
                    data_output_disable();
                    oe_output_disable();

                    if (timer_tick_has_elapsed(ks_timeout_timer))
                        ks_timeout_count = 0;
                    if (ks_timeout_count++ < 4) {
                        uart_puts(" KS timeout 0: ");
                        uart_puthex(dma_left);
                        uart_puts(" reads left\n");
                    }
                    ks_timeout_timer = timer_tick_plus_msec(1000);
                    goto oe_reply_end;
                }
                __asm__ volatile("dmb");
                dma_left = dma_get_number_of_data(DMA1, DMA_CHANNEL5);
            }
        }
        dma_last = dma_left;
    }

oe_reply_end:
    if (flags & KS_REPLY_WE)
        oewe_output(0);    // Disconnect SOCKET_OE from WE

    data_output_disable(); // Stop driving data lines
    oe_output_disable();   // Stop doing override of FLASH_OE

    configure_oe_capture_rx(false);
    timer_set_oc_polarity_low(TIM5, TIM_OC1);

    nvic_enable_irq(LOG_DMA_NVIC_IRQ);
    data_output(0x00000000);    // Return to pull-down of data pins
}

static const fw_info_t *fw_info_cache;
static uint             fw_info_cache_valid;

static void
fw_info_invalidate(void)
{
    fw_info_cache_valid = 0;
}

/*
 * fw_info_find
 * ------------
 * Locate the firmware identification structure (see main.h) in flash.
 * The result is cached, as the scan is slow; fw_info_invalidate() must
 * be called whenever flash is modified. Returns NULL if no valid
 * structure is found.
 */
static const fw_info_t *
fw_info_find(void)
{
    /* Flash size register is in Kbytes */
    uint32_t flash_size = *(volatile uint16_t *) DESIG_FLASH_SIZE_BASE * 1024;
    uintptr_t addr;

    if (fw_info_cache_valid)
        return (fw_info_cache);

    if ((flash_size == 0) || (flash_size > FW_FLASH_SIZE))
        flash_size = FW_FLASH_SIZE;

    fw_info_cache = NULL;
    for (addr = FLASH_BASE;
         addr <= FLASH_BASE + flash_size - sizeof (fw_info_t);
         addr += 4) {
        const fw_info_t *fi = (const fw_info_t *) addr;
        if ((fi->fi_magic == FW_INFO_MAGIC) &&
            (fi->fi_self == fi) &&
            (fi->fi_struct_size >= sizeof (fw_info_t)) &&
            (fi->fi_fw_size <= flash_size) &&
            (addr - FLASH_BASE + fi->fi_struct_size <= fi->fi_fw_size)) {
            fw_info_cache = fi;
            break;
        }
    }
    fw_info_cache_valid = 1;
    return (fw_info_cache);
}

/*
 * rx_payload_start
 * ----------------
 * Returns the index in buffer_rxa_lo[] of the first 16-bit word of the
 * payload of the command which was just received.
 */
static uint
rx_payload_start(uint16_t cmd_len)
{
    uint cons_s = rx_consumer - (cmd_len + 3) / 4 * 2 - 1;
    if ((int) cons_s < 0)
        cons_s += ARRAY_SIZE(buffer_rxa_lo);
    return (cons_s);
}

/*
 * rx_payload_get
 * --------------
 * Returns the 16-bit payload word at the specified position, and advances
 * the position (with wrap) to the next word.
 */
static uint16_t
rx_payload_get(uint *cons_s)
{
    uint16_t val = buffer_rxa_lo[*cons_s];
    if (++*cons_s == ARRAY_SIZE(buffer_rxa_lo))
        *cons_s = 0;
    return (val);
}

/*
 * rx_fw_rw_req
 * ------------
 * Extracts the ks_fw_rw_t request at the start of the command payload.
 * The offset and size are returned in native byte order. The position
 * of the payload word following the request is also returned.
 */
static void
rx_fw_rw_req(uint16_t cmd_len, uint32_t *offset, uint32_t *len, uint *cons_s)
{
    uint     pos;
    uint16_t adata[sizeof (ks_fw_rw_t) / 2];

    *cons_s = rx_payload_start(cmd_len);
    for (pos = 0; pos < ARRAY_SIZE(adata); pos++)
        adata[pos] = rx_payload_get(cons_s);
    *offset = (adata[0] << 16) | adata[1];
    *len    = adata[2];
}

static void
execute_cmd(uint16_t cmd, uint16_t cmd_len)
{
    switch ((uint8_t) cmd) {
        case KS_CMD_NULL:
            /* Do absolutely nothing (discard command) */
            break;
        case KS_CMD_NOP:
            /* Do nothing but reply */
            ks_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            break;  // End processing
        case KS_CMD_ID: {
            /* Send KickSmash identification and configuration */
            smash_id_t reply;
            const fw_info_t *fi;
            uint temp[3];
            uint pos = 0;
            memset(&reply, 0, sizeof (reply));
            temp[0] = version_str[8 + 0] - '0';
            temp[1] = version_str[8 + 2] - '0';
            reply.si_ks_version[0] = SWAP16(temp[0]);
            reply.si_ks_version[1] = SWAP16(temp[1]);
            if (pos == 0)
                pos = 18;
            else
                pos += 8 + 7;
            /* Year-Month-Day */
            temp[0] = atoi(&version_str[19]);
            temp[1] = atoi(&version_str[24]);
            temp[2] = atoi(&version_str[27]);
            reply.si_ks_date[0] = temp[0] / 100;
            reply.si_ks_date[1] = temp[0] % 100;
            reply.si_ks_date[2] = temp[1];
            reply.si_ks_date[3] = temp[2];
            pos += 11;
            /* Hour:Minute:Second */
            temp[0] = atoi(&version_str[30]);
            temp[1] = atoi(&version_str[33]);
            temp[2] = atoi(&version_str[36]);
            reply.si_ks_time[0] = temp[0];
            reply.si_ks_time[1] = temp[1];
            reply.si_ks_time[2] = temp[2];
            reply.si_ks_time[3] = 0;
            strcpy(reply.si_serial, (const char *)usb_serial_str);
            reply.si_rev      = SWAP16(0x0003);     // Protocol version 0.3
            reply.si_features = SWAP16(0x000f);     // Features
            reply.si_usbid    = SWAP32(0x12091610); // Matches USB ID
            reply.si_mode     = persist.sp_ee_mode;
            reply.si_cpu      = is_gd32;
            reply.si_usbdev   = 0;
            reply.si_cpufreq  = clock_get_hclk() / 1000000;
            reply.si_cpufreq  = SWAP16(reply.si_cpufreq);
            reply.si_busfreq  = clock_get_apb2() / 1000000;
            reply.si_busfreq  = SWAP16(reply.si_busfreq);
            reply.si_state    = 0;
            reply.si_name[0] = '\0';
            fi = fw_info_find();
            reply.si_size     = (fi == NULL) ? 0 : SWAP32(fi->fi_fw_size);
            memset(reply.si_unused, 0, sizeof (reply.si_unused));
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
        case KS_CMD_FW_UNLOCK:
            /* Updater always allows firmware writes */
            ks_reply(0, KS_STATUS_BADARG, 0, NULL, 0, NULL);
            break;
        case KS_CMD_FW_LOCK:
            /* Done with update: reboot into new firmware */
            ks_reply(0, KS_STATUS_OK, 0, NULL, 0, NULL);
            uart_puts("\nUpdater rebooting\n");
            timer_delay_msec(10);
            msg_shutdown();
            scb_reset_system();
            break;
        case KS_CMD_FW_READ: {
            ks_fw_rw_t       req;
            const fw_info_t *fi;
            uint32_t         offset;
            uint32_t         len;
            uint             cons_s;

            if (cmd_len < sizeof (req)) {
                ks_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
                break;
            }
            rx_fw_rw_req(cmd_len, &offset, &len, &cons_s);
            fi = fw_info_find();
            if ((fi == NULL) || (offset >= fi->fi_fw_size)) {
                ks_reply(0, KS_STATUS_FAIL, 0, NULL, 0, NULL);
                break;
            }
            if (len > FW_READ_MAX)
                len = FW_READ_MAX;
            if (len > fi->fi_fw_size - offset)
                len = fi->fi_fw_size - offset;
            memset(&req, 0, sizeof (req));
            req.kfrw_offset = SWAP32(offset);
            req.kfrw_size   = SWAP16(len);
            ks_reply(0, KS_STATUS_OK, sizeof (req), &req,
                     len, (const void *) (FLASH_BASE + offset));
            break;
        }
        case KS_CMD_FW_WRITE: {
            static uint8_t fw_wbuf[FW_WRITE_MAX];
            ks_fw_rw_t req;
            uint32_t   offset;
            uint32_t   len;
            uint       cons_s;
            uint       pos;
            int        rc;

            if (cmd_len < sizeof (req)) {
                ks_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
                break;
            }
            rx_fw_rw_req(cmd_len, &offset, &len, &cons_s);
            if ((cmd_len < sizeof (req) + len) || (len > sizeof (fw_wbuf))) {
                /* Not enough data following request, or too much */
                ks_reply(0, KS_STATUS_BADLEN, 0, NULL, 0, NULL);
                break;
            }
            if ((offset > FW_FLASH_SIZE) || (len > FW_FLASH_SIZE - offset)) {
                ks_reply(0, KS_STATUS_BADARG, 0, NULL, 0, NULL);
                break;
            }

            /*
             * Copy data out of the circular receive buffer. Each 16-bit
             * word holds two bytes in big-endian order.
             */
            for (pos = 0; pos < len; pos += 2) {
                uint16_t val = rx_payload_get(&cons_s);
                fw_wbuf[pos] = val >> 8;
                if (pos + 1 < len)
                    fw_wbuf[pos + 1] = (uint8_t) val;
            }

            /* Each page which begins within the write area is erased first */
            fw_info_invalidate();
            rc = stm32flash_write(offset, len, fw_wbuf,
                                  STM32FLASH_FLAG_AUTOERASE);
            if (rc != RC_SUCCESS) {
                ks_reply(0, KS_STATUS_FAIL, 0, NULL, 0, NULL);
                break;
            }
            memset(&req, 0, sizeof (req));
            req.kfrw_offset = SWAP32(offset);
            req.kfrw_size   = SWAP16(len);
            ks_reply(0, KS_STATUS_OK, sizeof (req), &req, 0, NULL);
            if (offset == 0)
                uart_puts("Firmware update: ");
            else if ((offset & 0xfff) == 0)
                uart_putchar('.');  // Every 4 KB
            break;
        }
        default:
            /* Unknown command */
            ks_reply(0, KS_STATUS_UNKCMD, 0, NULL, 0, NULL);
            uart_puts("KS cmd ");
            uart_puthex(cmd);
            uart_puts("?\n");
            break;
    }
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
        rx_consumer += 2;
        if (rx_consumer >= ARRAY_SIZE(buffer_rxa_lo))
            rx_consumer -= ARRAY_SIZE(buffer_rxa_lo);
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
    static uint     cons_start = 0;
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
            case 0:
                if (fast_magic_search(prod))
                    break;
                magic_pos = 1;
                break;
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
                cons_start = rx_consumer;
                cmd_len = buffer_rxa_lo[rx_consumer];
                if (cmd_len >= sizeof (buffer_rxa_lo) - 16) {
                    uart_puts("BAD msg len=");
                    uart_puthex(cmd_len);
                    uart_putchar('\n');
                    magic_pos = 0;  // Invalid length
                    break;
                }
                len = (cmd_len + 3) / 4 * 2;  // Even number of 16-bit words
                magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic) + 1:
                /* Command phase */
                cmd = buffer_rxa_lo[rx_consumer];
                if (len == 0)
                    magic_pos++;  // Skip following Data Phase
                magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic) + 2:
                /* Data phase */
                len--;
                if (unlikely(len == 0))
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
                uint len1 = cmd_len + 4;
                uint32_t ncrc;

                /* Stop timer DMA triggers */
                TIM_CCER(TIM2) = 0;
                TIM_CCER(TIM5) = 0;

                if (len1 > sizeof (buffer_rxa_lo) - cons_start * 2) {
                    uint len2;
                    len1 = sizeof (buffer_rxa_lo) - cons_start * 2;
                    len2 = cmd_len - len1 + 4;
                    ncrc = crc32s(0, (void *) &buffer_rxa_lo[cons_start], len1);
                    ncrc = crc32s(ncrc, (void *) buffer_rxa_lo, len2);
                } else {
                    ncrc = crc32s(0, (void *) &buffer_rxa_lo[cons_start], len1);
                }
                crc = ncrc;
                if (crc_rx != crc) {
                    uint16_t error[2];
                    error[0] = KS_STATUS_CRC;
                    error[1] = crc;
                    /* First capture the log */
                    static uint16_t tempcap[16];
                    uint c = rx_consumer;
                    int pos;
                    for (pos = ARRAY_SIZE(tempcap) - 1; pos >= 0; pos--) {
                        tempcap[pos] = buffer_rxa_lo[c];
                        if (c-- == 0)
                            c = ARRAY_SIZE(buffer_rxa_lo) - 1;
                    }
                    ks_reply(0, KS_STATUS_CRC, sizeof (error), &error, 0, NULL);
                    uart_puts("cmd=");
                    uart_puthex(cmd);
                    uart_puts(" l=");
                    uart_puthex(cmd_len);
                    uart_puts(" CRC ");
                    uart_puthex(crc_rx);
                    uart_puts(" != ");
                    uart_puthex(crc);
                    uart_puts(" calc ");
                    for (pos = 0; pos < ARRAY_SIZE(tempcap); pos++) {
                        uart_putchar(' ');
                        uart_puthex(tempcap[pos]);
                        if (((pos & 0xf) == 0xf) &&
                            (pos != ARRAY_SIZE(tempcap) - 1)) {
                            uart_putchar('\n');
                        }
                    }
                    uart_putchar('\n');
                } else {
                    /* Execution phase */
                    execute_cmd(cmd, cmd_len);
                }

                magic_pos = 0;  // Restart magic detection

                /* Clobber magic so it doesn't get executed again */
                if (cons_start == 0)
                    cons_start = ARRAY_SIZE(buffer_rxa_lo) - 1;
                else
                    cons_start--;
                buffer_rxa_lo[cons_start] = 0;
#if 0
                /*
                 * XXX: Branching back to new_cmd makes a huge performance
                 *      difference in IOPS. I don't understand why. It seems
                 *      to be the increment of rx_consumer causing slowness.
                 */
                if (++rx_consumer == ARRAY_SIZE(buffer_rxa_lo)) {
                    rx_consumer = 0;
                }
#endif
                goto new_cmd;
                break;
            default:
                uart_putchar('?');
                magic_pos = 0;  // Restart magic detection
                break;
        }

        if (++rx_consumer == ARRAY_SIZE(buffer_rxa_lo)) {
            rx_consumer = 0;
            if (++consumer_wrap - consumer_wrap_last_poll > 1) {
                /*
                 * Spinning too much in interrupt context.
                 * Disable interrupt -- it will be re-enabled in msg_poll().
                 */
                nvic_disable_irq(LOG_DMA_NVIC_IRQ);
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

    configure_oe_capture_rx(true);
}
