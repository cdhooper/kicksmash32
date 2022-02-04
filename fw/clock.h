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
#ifndef _CLOCK_H
#define _CLOCK_H

void clock_init(void);
uint32_t clock_get_hclk(void);
uint32_t clock_get_apb1(void);
uint32_t clock_get_apb2(void);

extern uint32_t rcc_pclk2_frequency;

#endif /* _CLOCK_H */
