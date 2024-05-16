/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * Analog to digital conversion for sensors.
 */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "board.h"
#include "cmdline.h"
#include "printf.h"
#include "main.h"
#include "prom_access.h"
#include "adc.h"
#include "timer.h"

#define TEMP_BASE          25000     // Base temperature is 25C

#if defined(STM32F407xx)
#define TEMP_V25           760       // 0.76V
#define TEMP_AVGSLOPE      25        // 2.5
#define SCALE_VREF         12100000  // 1.21V

#elif defined(STM32F1)
/* Verified STM32F103xE and STM32F107xC are identical */
#define TEMP_V25           1410      // 1.34V-1.52V; 1.41V seems more accurate
#define TEMP_AVGSLOPE      43        // 4.3
#define SCALE_VREF         12000000  // 1.20V

#else
#error STM32 architecture temp sensor slopes must be known
#endif

#define V5_EXPECTED_MV     5000      // 5V expressed as millivolts
#define V3P3_DIVIDER_SCALE 2 / 10000 // (1k / 1k)
#define V5_DIVIDER_SCALE   2 / 10000 // (1k / 1k)

#include <libopencm3/stm32/adc.h>
#include <libopencm3/stm32/dac.h>
#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#if defined(STM32F407xx)
#define ADC_CHANNEL_TEMP ADC_CHANNEL_TEMP_F40
#endif

static const uint8_t channel_defs[] = {
    ADC_CHANNEL_VREF,  // 0: Vrefint (used to calibrate other readings
    ADC_CHANNEL_TEMP,  // 1: Vtemp Temperature sensor
    8,                 // 2: PB0 - V5          (1k/1k divider)
#if 0
    3,                 // 2: PA3 - EEPROM V10  (10k/1k divider)
    1,                 // 3: PA1 - V3P3        (1k/1k divider)
    14,                // 4: PC4 - V5          (1k/1k divider)
    15,                // 5: PC5 - EEPROM V5CL (1k/1k divider)
    2,                 // 6: PA2 - V10FB (V10 feedback for regulator)
#endif
};

typedef struct {
    uint32_t gpio_port;
    uint16_t gpio_pin;
} channel_gpio_t;
static const channel_gpio_t channel_gpios[] = {
    { GPIOB, GPIO0 },  // PB0 - V5
};

#define CHANNEL_COUNT ARRAY_SIZE(channel_defs)

/* Buffer to store the results of the ADC conversion */
volatile uint16_t adc_buffer[CHANNEL_COUNT];

int v5_stable = false;

void
adc_init(void)
{
    uint32_t adcbase = ADC1;
    size_t   p;

    /* STM32F1... */
    uint32_t dma = DMA1;  // STM32F1xx RM Table 78 Summary of DMA1 requests...
    uint32_t channel = DMA_CHANNEL1;

    for (p = 0; p < ARRAY_SIZE(channel_gpios); p++) {
        gpio_set_mode(channel_gpios[p].gpio_port, GPIO_MODE_INPUT,
                      GPIO_CNF_INPUT_ANALOG, channel_gpios[p].gpio_pin);
    }

    rcc_periph_clock_enable(RCC_ADC1);
    rcc_periph_clock_enable(RCC_DMA1);
    adc_power_off(adcbase);  // Turn off ADC during configuration
    rcc_periph_reset_pulse(RST_ADC1);
    adc_disable_dma(adcbase);

    dma_disable_channel(dma, channel);
    dma_channel_reset(dma, channel);
    dma_set_peripheral_address(dma, channel, (uintptr_t)&ADC_DR(adcbase));
    dma_set_memory_address(dma, channel, (uintptr_t)adc_buffer);
    dma_set_read_from_peripheral(dma, channel);
    dma_set_number_of_data(dma, channel, CHANNEL_COUNT);
    dma_disable_peripheral_increment_mode(dma, channel);
    dma_enable_memory_increment_mode(dma, channel);
    dma_set_peripheral_size(dma, channel, DMA_CCR_PSIZE_16BIT);
    dma_set_memory_size(dma, channel, DMA_CCR_MSIZE_16BIT);
    dma_enable_circular_mode(dma, channel);
    dma_set_priority(dma, channel, DMA_CCR_PL_MEDIUM);
    dma_enable_channel(dma, channel);

    adc_set_dual_mode(ADC_CR1_DUALMOD_IND);  // Independent ADCs

    adc_enable_scan_mode(adcbase);

    adc_set_continuous_conversion_mode(adcbase);
    adc_set_sample_time_on_all_channels(adcbase, ADC_SMPR_SMP_239DOT5CYC);
    adc_disable_external_trigger_regular(adcbase);
    adc_disable_external_trigger_injected(adcbase);
    adc_set_right_aligned(adcbase);
    adc_enable_external_trigger_regular(adcbase, ADC_CR2_EXTSEL_SWSTART);

    adc_set_regular_sequence(adcbase, CHANNEL_COUNT, (uint8_t *)channel_defs);
    adc_enable_temperature_sensor();

    adc_enable_dma(adcbase);

    adc_power_on(adcbase);
    adc_reset_calibration(adcbase);
    adc_calibrate(adcbase);

    /* Start the ADC and triggered DMA */
    adc_start_conversion_regular(adcbase);
}

void
adc_shutdown(void)
{
    dma_disable_channel(DMA1, DMA_CHANNEL1);
}

static void
print_reading(int value, char *suffix)
{
    int  units = value / 1000;
    uint milli = abs(value) % 1000;

    if (*suffix == 'C') {
        printf("%3d.%u %s", units, milli / 100, suffix);
    } else {
        printf("%2d.%02u %s", units, milli / 10, suffix);
    }
}

/*
 * adc_get_scale
 * -------------
 * Captures the current scale value from the sensor table.  This value is
 * based on the internal reference voltage and is then used to appropriately
 * scale all other ADC readings.
 */
static uint
adc_get_scale(uint16_t adc0_value)
{
    static int scale = 0;
    int tscale;

    if (adc0_value == 0)
        adc0_value = 1;

    tscale = SCALE_VREF / adc0_value;

    if (scale == 0)
        scale = tscale;
    else
        scale += (tscale - scale) / 16;

    return (scale);
}

void
adc_show_sensors(void)
{
    uint     scale;
    uint16_t adc[CHANNEL_COUNT];

    /*
     * raw / 4095 * 3V = voltage reading * resistor/div scale (8.5) = reading
     *      10K / 1.33K divider: 10V -> 1.174V (multiply reading by 8.51788756)
     *      So, if reading is 1614:
     *              1608 / 4096 * 3 = 1.177734375V
     *              1.177734375 * 8.51788756 = 10.0318
     *
     * PC4 ADC_U1 IN14 is V10SENSE (nominally 10V)
     *     ADC_U1 IN16 is STM32 Temperature (* 10000 / 25 - 279000)
     *     ADC_U1 IN17 is Vrefint 1.2V
     *     ADC_U1 IN18 is Vbat (* 2)
     *
     *     ADC_CHANNEL_TEMPSENSOR
     *     ADC_CHANNEL_VREFINT
     *     ADC_CHANNEL_VBAT
     *
     * We could use Vrefin_cal to get a more accurate expected Vrefint
     * from factory-calibrated values when Vdda was 3.3V.
     *
     * Temperature sensor formula
     *      Temp = (V25 - VSENSE) / Avg_Slope + 25
     *
     *           STM32F407               STM32F1xx
     *      V25  0.76V                   1.43V
     * AvgSlope  2.5                     4.3
     * Calc      * 10000 / 25 - 279000   * 10000 / 43 - 279000
     *
     * Channel order (STM32F1):
     *     adc_buffer[0] = Vrefint
     *     adc_buffer[1] = Vtemperature
     *
     * Algorithm:
     *  * Vrefint tells us what 1.21V (STM32F407) or 1.20V (STM32F1xx) should
     *  be according to ADCs.
     *  1. scale = 1.2 / adc_buffer[0]
     *          Because: reading * scale = 1.2V
     *  2. Report Vbat:
     *          adc_buffer[1] * scale * 2
     */
    memcpy(adc, (void *)adc_buffer, sizeof (adc_buffer));
    scale = adc_get_scale(adc[0]);

    uint calc_temp;
    uint calc_vref;
    uint calc_v5;
    calc_temp = ((int)(TEMP_V25 * 10000 - adc[1] * scale)) / TEMP_AVGSLOPE +
                TEMP_BASE;
    calc_vref = adc[0] * 3300 / 4096;
    calc_v5   = adc[2] * scale * V5_DIVIDER_SCALE;

    printf("Vrefint=%04x scale=%-4u ", adc[0], scale);
    print_reading(calc_vref, "V\n");
    printf("  Vtemp=%04x %8u   ", adc[1], adc[1] * scale);
    print_reading(calc_temp, "C\n");
#if BOARD_REV >= 4
    printf("     5V=%04x %8u   ", adc[2], adc[2] * scale);
    print_reading(calc_v5, "V\n");
#else
    (void) calc_v5;
#endif
}

/*
 * adc_poll() will capture the current readings from the sensors and take
 *            action to maintain the 10V rail as close as possible to 10V.
 */
void
adc_poll(int verbose, int force)
{
    static uint     avg_v5 = 0;
    uint            calc_v5;
    uint            scale;
    int             percent5;   // 0.1 percent voltage deviation for 5V
    uint16_t        adc[CHANNEL_COUNT];
    static uint64_t next_check = 0;

    if ((timer_tick_has_elapsed(next_check) == false) && (force == false))
        return;
    next_check = timer_tick_plus_msec(1);  // Limit rate to prevent overshoot

    memcpy(adc, (void *)adc_buffer, sizeof (adc_buffer));
    scale = adc_get_scale(adc[0]);
    calc_v5 = adc[2] * scale * V5_DIVIDER_SCALE;
    if (avg_v5 == 0)
        avg_v5 = calc_v5;
    else
        avg_v5 += ((int)calc_v5 - (int)avg_v5) / 4;

    percent5 = avg_v5 * 100 / V5_EXPECTED_MV;
    if ((percent5 < 90) || (percent5 > 105)) {  // 4.5V - 5.25V
        if ((v5_stable == true) && verbose) {
#if BOARD_REV >= 4
            printf("Amiga V5 not stable at ");
            print_reading(avg_v5, "V\n");
#endif
        }
        v5_stable = false;
    } else {
        if ((v5_stable == false) && verbose) {
#if BOARD_REV >= 4
            printf("Amiga V5 stable at ");
            print_reading(avg_v5, "V\n");
#endif
        }
        v5_stable = true;
    }

//  scale = adc_get_scale(adc[0]);
}
