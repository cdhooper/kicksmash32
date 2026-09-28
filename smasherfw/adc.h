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
void adc_shutdown(void);
void adc_show_sensors(void);
void adc_poll(int verbose, int force);

#endif /* _ADC_H */
