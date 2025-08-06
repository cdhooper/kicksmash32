/*
 * Amiga serial port and debug text output.
 *
 * This header file is part of the code base for a simple Amiga ROM
 * replacement sufficient to allow programs using some parts of GadTools
 * to function.
 *
 * Copyright 2025 Chris Hooper. This program and source may be used
 * and distributed freely, for any purpose which benefits the Amiga
 * community. All redistributions must retain this Copyright notice.
 *
 * DISCLAIMER: THE SOFTWARE IS PROVIDED "AS-IS", WITHOUT ANY WARRANTY.
 * THE AUTHOR ASSUMES NO LIABILITY FOR ANY DAMAGE ARISING OUT OF THE USE
 * OR MISUSE OF THIS UTILITY OR INFORMATION REPORTED BY THIS UTILITY.
 */
#ifndef _SERIAL
#define _SERIAL

void serial_init(void);
void serial_putc(unsigned int ch);
void serial_puts(const char *str);
void serial_flush(void);
void serial_poll(void);              // poll for serial input
void serial_replay_output(void);     // re-show all previous serial output
void input_rb_put(unsigned int ch);  // push to keyboard input buffer
int  input_rb_get(void);             // get raw keycode from input buffer
int  input_break_pending(void);      // ^C pressed

extern uint8_t serial_active;        // Serial port is active

#endif /* _SERIAL */
