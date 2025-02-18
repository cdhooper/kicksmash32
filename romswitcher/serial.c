/*
 * Amiga serial port and debug text output.
 *
 * This source file is part of the code base for a simple Amiga ROM
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
#include <stdint.h>
#include "util.h"
#include "serial.h"
#include "keyboard.h"
#include "printf.h"
#include "screen.h"
#include "amiga_chipset.h"
#include "timer.h"

#define ECLOCK_NTSC 3579545
#define ECLOCK_PAL  3546895

static volatile uint ser_in_rb_producer;  // Console input current writer pos
static uint          ser_in_rb_consumer;  // Console input current reader pos
static uint16_t      ser_in_rb[64];       // Console input ring buffer (FIFO)
uint8_t              serial_active;       // Serial port is active
volatile uint8_t     gui_wants_all_input; // Non-zero if GUI wants all key input

static const uint8_t input_med_magic[] = { 0x0d, 0x05, 0x04 };  // ^M^E^D

/*
 * input_rb_put() stores a character in the input ring buffer.
 *
 * @param [in]  ch - The character to store in the input ring buffer.
 *
 * @return      None.
 */
void
input_rb_put(uint ch)
{
    static uint8_t magic_pos = 0;
    uint new_prod = ((ser_in_rb_producer + 1) % ARRAY_SIZE(ser_in_rb));

    if ((ch & 0xff) != 0) {
        if (((uint8_t) ch) == input_med_magic[magic_pos]) {
            if (++magic_pos == ARRAY_SIZE(input_med_magic)) {
                gui_wants_all_input ^= 1;
                if (gui_wants_all_input & 1) {
                    /* MED deactivated */
                    cursor_visible &= ~2;
                } else {
                    /* MED activated */
                    cursor_visible |= 2;
                    dbg_all_scroll = 25;
                    dbg_cursor_y = 25;
                }
                magic_pos = 0;
            }
        } else {
            magic_pos = 0;
            if (((uint8_t) ch) == input_med_magic[magic_pos])
                magic_pos = 1;
        }
    }
    if (new_prod == ser_in_rb_consumer) {
        // serial_putc('%');  // Emit here can lead to serial deadlock
        if ((ch & 0xff) != 0) {
            /* Buffer full: Always capture most recent key down input */
            uint last_prod = (ser_in_rb_producer - 1) % ARRAY_SIZE(ser_in_rb);
            ser_in_rb[last_prod] = (uint8_t) ch;
        }
        return;  // Would cause ring buffer overflow
    }

//  disable_irq();
    ser_in_rb[ser_in_rb_producer] = (uint16_t) ch;
    ser_in_rb_producer = new_prod;
//  enable_irq();
}

/*
 * input_rb_get() returns the next character in the input ring buffer.
 *                A value of -1 is returned if there are no characters
 *                waiting to be received in the input ring buffer.
 *
 * This function requires no arguments.
 *
 * @return      The next input character.
 * @return      -1 = No input character is pending.
 */
int
input_rb_get(void)
{
    uint ch;
    extern uint vblank_ints;
    vblank_ints = 0;  // XXX DEBUG
    if (ser_in_rb_consumer == ser_in_rb_producer)
        return (-1);  // Ring buffer empty

    ch = ser_in_rb[ser_in_rb_consumer];
    ser_in_rb_consumer = (ser_in_rb_consumer + 1) % ARRAY_SIZE(ser_in_rb);
    return (ch);
}

void
serial_init(void)
{
    uint bps = 9600;
    uint vid_clk = (vid_type == VID_NTSC) ? ECLOCK_NTSC : ECLOCK_PAL;
    uint serper_divisor = vid_clk / bps - 1;

    *INTENA = INTENA_INTEN;             // disable internal interrupt
    *SERPER = serper_divisor;
    *CIAB_PRA = 0x4f; // Set DTR
    *INTENA = INTENA_TBE | INTENA_RBF;  // disable interrupt
    *INTREQ = INTREQ_TBE | INTREQ_RBF;  // clear interrupt
}

void
serial_putc(unsigned int ch)
{
    if ((serial_active == 0) && (timer_tick_get() >> 25))
        return;  // No serial input and it's past 45 seconds

#if 0
    const uint parity = 0;
    const uint parity_odd = 0;
    if (parity) {
        uint8_t p = ch ^ (ch >> 4);
        p ^= (p >> 2);
        p ^= (p >> 1);
        if (p & 1)
            ch |= BIT(7);  // make it even
        if (parity_odd)
            ch ^= BIT(7);  // make it odd
    }
#endif
    ch |= 0x100; // stop bit

    uint timeout = 50000;
    uint16_t sdat;
    do {
        uint32_t sr = irq_disable();
        sdat = *SERDATR;
        if (sdat & SERDATR_RBF) {
            if ((sdat & 0xff) == 0x7f)  // Map other delete to backspace
                sdat = 0x08;
            input_rb_put(sdat);
            serial_active = 1;
            *INTREQ = INTREQ_RBF;  // Clear interrupt status
        }
        irq_restore(sr);
        if (timeout-- == 0)
            break;
    } while ((sdat & SERDATR_TBE) == 0);

    *SERDAT = ch;
    *INTREQ = INTREQ_TBE;
}

/*
 * serial_flush() waits for all pending serial output to be transmitted
 */
void
serial_flush(void)
{
    uint timeout = 50000;
    uint16_t sdat;
    do {
        sdat = *SERDATR;
        if (timeout-- == 0)
            break;
    } while ((sdat & SERDATR_TSRE) == 0);
}

int
serial_getc(void)
{
    uint16_t sdat = *SERDATR;
    if ((sdat & SERDATR_RBF) == 0)
        return (-1);  // No input bytes pending
    *INTREQ = INTREQ_RBF;  // Clear interrupt status
    return (sdat & 0xff);
}

void
serial_puts(const char *str)
{
    while (*str != '\0') {
        char ch = *(str++);
        if (ch == '\n')
            serial_putc('\r');
        serial_putc(ch);
    }
}

int
getchar(void)
{
    static int ch_prev = 0;
    int ch = input_rb_get();  // Pull from serial input ring buffer

    if (ch == -1) {
        /* Attempt to pull directly from keyboard */
#undef KEYBOARD_POLL
#ifdef KEYBOARD_POLL
        // XXX: Seems broken on real hardware -- maybe delays not long enough
        keyboard_poll();
#endif
        ch = input_rb_get();
    }
    if (ch == -1) {
        /* Attempt to pull directly from serial port */
        ch = serial_getc();
        if (ch != -1)
            serial_active = 1;
    }
    if (ch == -1)
        return (ch);

    if ((ch_prev == '\r') && (ch == '\n'))
        return (-1);  // CRLF: skip LF
    ch_prev = ch;
    return (ch & 0xff);
}

int
putchar(int ch)
{
    if (ch == '\n') {
        serial_putc('\r');
        dbg_show_char('\r');
    }
    serial_putc((uint) ch);
    dbg_show_char((uint) ch);
    return (ch);
}

int
puts(const char *str)
{
    serial_puts(str);
    serial_putc('\r');
    serial_putc('\n');
    dbg_show_string(str);
    dbg_show_string("\r\n");
    return (0);
}

/*
 * input_break_pending() returns true if a ^C is pending in the input buffer.
 *
 * This function requires no arguments.
 *
 * @return      1 - break (^C) is pending.
 * @return      0 - no break (^C) is pending.
 */
int
input_break_pending(void)
{
    uint cur;
    uint next;

    extern uint vblank_ints;
    vblank_ints = 0;  // XXX DEBUG
    for (cur = ser_in_rb_consumer; cur != ser_in_rb_producer; cur = next) {
        next = (cur + 1) % ARRAY_SIZE(ser_in_rb);
        if (ser_in_rb[cur] == 0x03) {  /* ^C is abort key */
            ser_in_rb_consumer = next;
            return (1);
        }
    }
    cur = serial_getc();
    if (cur != (uint)-1)
        input_rb_put(cur);
    if (cur == 0x03)
        return (1);
#ifdef KEYBOARD_POLL
    keyboard_poll();
#endif

    return (0);
}
