/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Pin tests for board connectivity and soldering issues.
 */

#ifndef __PIN_TESTS_H
#define __PIN_TESTS_H

void check_board_standalone(void);
uint pin_tests(void);

extern uint8_t board_is_standalone;

#endif /* __PIN_TESTS_H */
