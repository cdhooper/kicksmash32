/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * STM32 USART and basic input / output handling.
 */

#ifndef _UART_H
#define _UART_H

/**
 * putchar() is a stdio compatible function which operates on a single
 *           character, sending output to the serial console.
 *
 * @param [in]  ch - The character to output.
 *
 * @return      0 = Success.
 * @return      1 = Failure.
 */
int putchar(int ch);

/**
 * puts() is a stdio compatible function which operates on a string
 *        buffer, sending output to the serial console.  A newline is
 *        automatically appended to the output.
 *
 * @param [in]  str - The string to output.
 *
 * @return      0 = Success.
 * @return      1 = Failure.
 */
int puts(const char *str);

/**
 * getchar() is a stdio-like function which will acquire a single character
 *           from the serial console.  This is a non-blocking implementation.
 *           If a character is not available, the function will return
 *           immediately to the caller with a 0 value.
 *
 * This function requires no arguments.
 *
 * @return      The received character.
 * @return      0 = No character is available.
 */
int getchar(void);

/*
 * uart_init() initializes the serial console uart.
 */
void uart_init(void);

void usb_rb_put(uint ch);

/*
 * input_break_pending() returns true if a ^C is pending in the input buffer.
 *
 * This function requires no arguments.
 *
 * @return      1 - break (^C) is pending.
 * @return      0 - no break (^C) is pending.
 */
int input_break_pending(void);

int uart_putchar(int ch);
void uart_flush(void);
int puts_binary(void *buf, uint32_t len);

#define SOURCE_UART 0  // Last input source was serial UART
#define SOURCE_USB  1  // Last input source was USB virtual serial port

extern uint8_t last_input_source;

#endif /* _UART_H */
