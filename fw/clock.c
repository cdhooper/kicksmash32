/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * Clock functions.
 */

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/flash.h>
#include "board.h"
#include "clock.h"

uint32_t rcc_pclk2_frequency = 0;

/*
 * STM32F107 Clock structure
 *
 *                                        /--PLLCLK
 *                                       /   72MHz
 *                                      /
 *    HSE---Prediv1_Mux--PREDIV1--PLLMul----PLL_VCO---USB Prescaler---USB
 *    8MHz               /1       x9   \   72MHz x2   /3              48MHz
 *                                      \
 *                                       \--PLLCLK
 *                                          72MHz
 *
 *
 *                                          /-----HCLK core 72MHz
 *                                         /--/1--Cortex timer 72MHz
 *                                        /-------FCLK 72MHz
 *                                       /
 *    PLLCLK--SYSCLK--AHB Prescaler--HCLK-----APB1 Pre------PCLK1 36 MHz
 *            72MHz   /1             72MHz\   /2      \--x2-APB1 Timer 72 MHz
 *                                         \
 *                                          \-APB2 Pre------PCLK2 72 MHz
 *                                            /1      \
 *                                                     \----APB2 Timer 72 MHz
 *                                                      \
 *                                                       \--ADC Pre---ADC 1,2
 *                                                          /6        12MHz
 *
 *    HSE---PREDIV2---VCOinput2------PLL2MUl----PLL2CLK (ignored)
 *    8MHz  /1         8MHz      \    x8         64
 *                                \
 *                                 \--PLL3Mul-Mul-VCO    (ignored)
 *                                    x8      x2  128MHz
 */
#ifdef STM32F1
static const struct rcc_clock_scale rcc_clock_config = {
    /* HSE=8 PLL=72 USB=48 APB1=36 APB2=72 ADC=12 */
/*
 *  These don't need to be set because they are not driving a
 *  peripheral that this program requires.
 *
 *  .pll2_mul         = RCC_CFGR2_PLL2MUL_PLL2_CLK_MUL8, // 8 * 8 = 64 MHz
 *  .pll3_mul         = RCC_CFGR2_PLL3MUL_PLL3_CLK_MUL8, // 8 * 8 * 2 = 128 MHz
 *  .prediv2          = RCC_CFGR2_PREDIV2_NODIV,         // 8 / 1 = 8 MHz
 */

    .prediv1_source   = RCC_CFGR2_PREDIV1SRC_HSE_CLK,  // 8 MHz
    .prediv1          = RCC_CFGR2_PREDIV_NODIV,        // 8 / 1 = 8 MHz
    .pll_source       = RCC_CFGR_PLLSRC_PREDIV1_CLK,   // 8 / 1 = 8 MHz
    .pll_mul          = RCC_CFGR_PLLMUL_PLL_CLK_MUL9,  // 9 * 8 = 72 MHz

    .hpre             = RCC_CFGR_HPRE_NODIV,           // 72 / 1 = 72 MHz Core
    .ppre1            = RCC_CFGR_PPRE_DIV2,            // 72 / 2 = 36 MHz APB1
    .ppre2            = RCC_CFGR_PPRE_NODIV,           // 72 / 1 = 72 MHz APB2
    .adcpre           = RCC_CFGR_ADCPRE_DIV6,          // 72 / 6 = 12 MHz ADC
    .usbpre           = RCC_CFGR_USBPRE_PLL_VCO_CLK_DIV3, // 72 * 2 / 3 = 48 MHz

    .flash_waitstates = 2,
    .ahb_frequency    = 72000000,
    .apb1_frequency   = 36000000,
    .apb2_frequency   = 72000000,
};
#else
static const struct rcc_clock_scale rcc_clock_config = {
    /* HSE=8 USB=48 APB1=42 APB2=84 */
    .pllm             = 8,                      // 8 MHz / 8 = 1 MHz
    .plln             = 336,                    // 1 * 336 = 336 MHz
    .pllp             = 2,                      // 336 / 2 = 168 MHz core clock
    .pllq             = 7,                      // 336 / 7 = 48 MHz USB
    .pllr             = 0,
    .pll_source       = RCC_CFGR_PLLSRC_HSE_CLK,
    .hpre             = RCC_CFGR_HPRE_NODIV,
    .ppre1            = RCC_CFGR_PPRE_DIV4,
    .ppre2            = RCC_CFGR_PPRE_DIV2,
    .voltage_scale    = PWR_SCALE1,
    .flash_config     = FLASH_ACR_DCEN | FLASH_ACR_ICEN |
                        FLASH_ACR_LATENCY_5WS,
    .ahb_frequency    = 168000000,
    .apb1_frequency   = 42000000,
    .apb2_frequency   = 84000000,
};
#endif

void
clock_init(void)
{
#ifdef STM32F4
    rcc_clock_setup_pll(&rcc_hse_8mhz_3v3[RCC_CLOCK_3V3_168MHZ]);
    rcc_pclk2_frequency = rcc_clock_config.apb2_frequency;
#else
    rcc_clock_setup_pll(&rcc_clock_config);
    rcc_pclk2_frequency = rcc_clock_config.apb2_frequency;
#endif
}

uint32_t
clock_get_hclk(void)
{
    return (rcc_clock_config.ahb_frequency);
}

uint32_t
clock_get_apb1(void)
{
    return (rcc_clock_config.apb1_frequency);
}

uint32_t
clock_get_apb2(void)
{
    return (rcc_clock_config.apb2_frequency);
}
