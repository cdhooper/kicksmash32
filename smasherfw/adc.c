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
#include "gpio.h"
#include "config.h"
#include "clock.h"
#include "pin_tests.h"

#define TEMP_BASE          25000     // Base temperature is 25C

#if defined(STM32F407xx)
#define TEMP_V25           760       // 0.76V
#define TEMP_AVGSLOPE      25        // 2.5 mV/C
#define SCALE_VREF         12100000  // 1.21V

#elif defined(STM32F1)
/* Verified STM32F103xE and STM32F107xC are identical */
#define TEMP_V25           1410      // 1.34V-1.52V; 1.41V seems more accurate
#define TEMP_AVGSLOPE      43        // 4.3 mV/C
#define SCALE_VREF         12000000  // 1.20V

/* GD32F107 datasheet Table 4-25: V25 = 1.45 V, Avg_Slope = 4.1 mV/°C.
 * VREFINT is also typically 1.20 V (same as STM32F1). */
#define GD32_TEMP_V25      1450      // 1.45V
#define GD32_TEMP_AVGSLOPE 41        // 4.1 mV/C
#define GD32_SCALE_VREF    11700000  // 1.17V observed (datasheet says 1.20V)

#else
#error STM32 architecture temp sensor slopes must be known
#endif

#define V5_EXPECTED_MV     5000      // 5V expressed as millivolts
#define V5_DIVIDER         2         // (1k / 1k)

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
    9,                 // 2: PB1 - V5          (1k/1k divider)
};

typedef struct {
    uint32_t gpio_port;
    uint16_t gpio_pin;
} channel_gpio_t;
static const channel_gpio_t channel_gpios[] = {
    { GPIOB, GPIO1 },  // PB1 - V5
};

#define CHANNEL_COUNT ARRAY_SIZE(channel_defs)

/* Buffer to store the results of the ADC conversion */
static volatile uint16_t adc_buffer[CHANNEL_COUNT];

static uint avg_v5 = 0;
int8_t v5_stable = false;

static void
adc_enable(void)
{
    int p;
    for (p = 0; p < ARRAY_SIZE(channel_gpios); p++) {
        gpio_setmode(channel_gpios[p].gpio_port, channel_gpios[p].gpio_pin,
                     GPIO_SETMODE_INPUT_ANALOG);
    }
}

void
adc_init(void)
{
    uint32_t adcbase = ADC1;

    /* STM32F1... */
    uint32_t dma = DMA1;  // STM32F1xx RM Table 78 Summary of DMA1 requests...
    uint32_t channel = DMA_CHANNEL1;

    adc_enable();

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
        printf("%2d.%u %s", units, milli / 100, suffix);
    } else {
        printf("%d.%02u %s", units, milli / 10, suffix);
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

    if (is_gd32)
        tscale = GD32_SCALE_VREF / adc0_value;
    else
        tscale = SCALE_VREF / adc0_value;

    if (scale == 0)
        scale = tscale;
    else
        scale += (tscale - scale) / 16;

    return (scale);
}

static uint
adc_calc_v5(uint16_t adcval, uint scale)
{
    uint calc_v5 = adcval * scale * V5_DIVIDER / 10000;
    if (is_gd32 && (calc_v5 > 100))
        calc_v5 -= 100;  // Odd +0.10 V offset with GD32F107
    else
        calc_v5 = 0;
    return (calc_v5);
}

__attribute__((format(__printf__, 3, 4))) static void
printf_reading(const char *prefix, int value, const char *fmt, ...)
{
    char buf[64];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof (buf), fmt, args);
    va_end(args);

    printf("%s", prefix);
    print_reading(value, buf);
}

void
adc_show_sensors(void)
{
    uint     scale;
    uint16_t adc[CHANNEL_COUNT];

    /*
     * raw / 4095 * 3.3V = voltage reading * resistor/div scale (2) = reading
     *
     * On STM32F407:
     *     ADC_U1 IN16 is STM32 Temperature (* 10000 / 25 - 279000)
     *     ADC_U1 IN17 is Vrefint 1.2V
     *     ADC_U1 IN18 is Vbat (* 2)
     *
     *     ADC_CHANNEL_TEMP
     *     ADC_CHANNEL_VREF
     *     ADC_CHANNEL_VBAT
     *
     *     We could use Vrefin_cal to get a more accurate expected Vrefint
     *     from factory-calibrated values when Vdda was 3.3V.
     *
     * Temperature sensor formula
     *      Temp = (V25 - VSENSE) / Avg_Slope + 25
     *
     *           STM32F407     STM32F1xx     GD32F107
     *      V25  0.76V         1.41V         1.45V
     * AvgSlope  2.5           4.3           4.1
     *
     * Channel order (STM32F1 / GD32F107):
     *     adc_buffer[0] = Vrefint
     *     adc_buffer[1] = Vtemperature
     *     adc_buffer[2] = 5V sense / 2
     *
     * Algorithm:
     *  * Vrefint tells us what 1.21V (STM32F407) or 1.20V (STM32F1/GD32)
     *    should be according to the ADC.
     *  1. scale = 1.2 / adc_buffer[0]
     *          Because: reading * scale = 1.2V
     *  2. Report V5:
     *          adc_buffer[2] * scale * 2
     */
    memcpy(adc, (void *)adc_buffer, sizeof (adc_buffer));
    scale = adc_get_scale(adc[0]);

    uint calc_temp;
    uint calc_vref;
    uint calc_v5;
    if (is_gd32) {
        calc_temp = ((int)(GD32_TEMP_V25 * 10000 - adc[1] * scale)) /
                    GD32_TEMP_AVGSLOPE + TEMP_BASE;
    } else {
        calc_temp = ((int)(TEMP_V25 * 10000 - adc[1] * scale)) /
                    TEMP_AVGSLOPE + TEMP_BASE;
    }

    /* Should be close to Vref, as average of adc[0] is used to compute scale */
    calc_vref = adc[0] * scale / 10000;

    /*
     * Calibrated VDDA from internal VREFINT (nominally 1.20 V).
     *   VDDA = VREFINT_NOM * 4096 / adc[0]
     * Using the running scale factor:
     *   VDDA_mV = scale * 4096 / 10000
     */
    uint calc_vdda = scale * 4096 / 10000;
    calc_v5 = adc_calc_v5(adc[2], scale);

    printf("Sensor     Reading  Calculation\n");
    printf_reading("Vrefint    ", calc_vref,
                   "V   ADC=%04x scale=%-4u\n", adc[0], scale);
    printf_reading("3.3 V      ", calc_vdda, "V\n");

    if (config.board_rev >= 2) {
        printf_reading("5.0 V      ", calc_v5,
                       "V   ADC=%04x %8u\n", adc[2], adc[2] * scale);
        printf_reading("5.0 V avg  ", avg_v5, "V\n");
    }

    if (adc[1] != 0xfff) {
        /* GD32F107 temperature sensor doesn't seem to work */
        printf_reading("Die Temp   ", calc_temp,
                       "C   ADC=%04x %8u\n", adc[1], adc[1] * scale);
    }
}

/*
 * adc_poll() will monitor the 5V sensor.
 */
void
adc_poll(int verbose, int force)
{
    uint            calc_v5;
    uint            scale;
    uint            v5_max;
    int             v5_good = false;
    uint16_t        adc[CHANNEL_COUNT];
    static uint8_t  deglitch = 0;
    static uint64_t next_check = 0;

    if ((timer_tick_has_elapsed(next_check) == false) && (force == false))
        return;
    next_check = timer_tick_plus_msec(1);  // Limit rate to prevent overshoot

    memcpy(adc, (void *)adc_buffer, sizeof (adc_buffer));
    scale = adc_get_scale(adc[0]);
    calc_v5 = adc_calc_v5(adc[2], scale);

    if (calc_v5 > 5800)
        printf("ADC bad read %d %04x\n", calc_v5, adc[2]);
    if (avg_v5 == 0)
        avg_v5 = calc_v5;
    else
        avg_v5 += ((int)calc_v5 - (int)avg_v5) / 4;

    /*
     * GD32F107 has been observed to report higher ADC readings than
     * STM32F107 with the same hardware; keep a more tolerant upper limit.
     */
    if (is_gd32)
        v5_max = 5800;  // 5.80V
    else
        v5_max = 5400;  // 5.40V

    if ((avg_v5 < 4250) || (avg_v5 > v5_max)) {  // 4.25V minimum
        v5_good = false;
    } else {
        v5_good = true;
    }

    if (v5_stable != v5_good) {
        if (deglitch == 0) {
            v5_stable = v5_good;
        } else {
            deglitch--;
            next_check = 0;
        }
    } else {
        deglitch = 3;
    }
}
