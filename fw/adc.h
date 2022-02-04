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

#ifndef _ADC_H
#define _ADC_H

void adc_init(void);
void adc_show_sensors(void);
void adc_poll(int verbose, int force);
void dac_setvalue(uint32_t value);

extern int v5_overcurrent; // true = V5 drawing too much current
extern int v5_stable;      // true = V5 is within 5 percent of expected
extern int v10_stable;     // true = V10 is within 5 percent of expected

#endif /* _ADC_H */
