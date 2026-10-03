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
#include "main.h"
#include "vectors.h"

#define ECLOCK_NTSC 3579545
#define ECLOCK_PAL  3546895

static volatile uint ser_in_rb_producer;  // Console input current writer pos
static uint          ser_in_rb_consumer;  // Console input current reader pos
static uint16_t      ser_in_rb[64];       // Console input ring buffer (FIFO)
uint8_t              serial_active;       // Serial port is active
volatile uint8_t     gui_wants_all_input; // Non-zero if GUI wants all key input
static uint8_t       ser_out_wrapped;     // Serial output wrapped buffer
static uint8_t       ser_out_buf[4096];   // Serial output
static volatile uint ser_out_prod;        // Serial output producer
static uint8_t       serial_output_enabled = 1;  // Console text writes serial
static uint8_t       screen_output_enabled = 0;  // Console text draws to screen

static const uint8_t input_med_magic[] = { 0x0d, 0x05, 0x04,         // ^M^E^D
                                           0x13, 0x14, 0x0f, 0x10 }; // ^S^T^O^P

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
    static uint8_t isr_stopped = 0;
    uint new_prod = ((ser_in_rb_producer + 1) % ARRAY_SIZE(ser_in_rb));

    if ((ch & 0xff) != 0) {
        if (((uint8_t) ch) == input_med_magic[magic_pos]) {
            if (++magic_pos == 3) {
                /* ^M ^E ^D */
                if (isr_stopped == 0) {
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
                }
            } else if (magic_pos == ARRAY_SIZE(input_med_magic)) {
                /* ^M ^E ^D ^S ^T ^O ^P */
                isr_stopped = 1;
                while (main_poll() == 0)
                    ;
                magic_pos = 0;
                isr_stopped = 0;
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
            ser_in_rb[last_prod] = (uint16_t) ch;
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

    vblank_ints = 0;     // Tell VBlank code we are still running
    INT_COUNTER(8) = 0;  // Clear spurious interrupt counter

    if (ser_in_rb_consumer == ser_in_rb_producer)
        return (-1);  // Ring buffer empty

    ch = ser_in_rb[ser_in_rb_consumer];
    ser_in_rb_consumer = (ser_in_rb_consumer + 1) % ARRAY_SIZE(ser_in_rb);
    return (ch);
}

void
serial_input_rb_put(uint ch)
{
    ch = scan_convert_from_ascii(ch);

    if ((ch & 0xff00) == 0) {
        /* No scancode translation for this ASCII */
        input_rb_put(ch);
        return;
    }

    if (ch & 0x10000)
        input_rb_put(0x6000);  // Press shift key
    if (ch & 0x20000)
        input_rb_put(0x6300);  // Press control key

    input_rb_put(ch & 0xffff);  // Key down
    input_rb_put((ch & 0xff00) | 0x8000);  // Key up

    if (ch & 0x10000)
        input_rb_put(0x6000 | 0x8000);  // Release shift key
    if (ch & 0x20000)
        input_rb_put(0x6300 | 0x8000);  // Release control key
}

void
serial_init(void)
{
    uint bps = 9600;
    uint vid_clk = (vid_type == VID_NTSC) ? ECLOCK_NTSC : ECLOCK_PAL;
    uint serper_divisor = vid_clk / bps - 1;

    *INTENA = INTENA_RBF;                  // disable serial interrupt
    *SERPER = serper_divisor;
    *CIAB_PRA = 0x4f;                      // Set DTR
    *INTREQ = INTREQ_RBF;                  // clear interrupt
    *INTENA = INTENA_SETCLR | INTENA_RBF;  // enable interrupt
}

void
serial_putc(unsigned int ch)
{
    ser_out_buf[ser_out_prod] = ch;
    ser_out_prod = (ser_out_prod + 1) % sizeof (ser_out_buf);

    if ((serial_active == 0) && (eclk_ticks_per_sec != 0) &&
        (timer_tick_get() >> 25))
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
            serial_input_rb_put(sdat & 0xff);
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

void
serial_output_set(uint enabled)
{
    serial_output_enabled = (enabled != 0);
}

void
screen_output_set(uint enabled)
{
    screen_output_enabled = (enabled != 0);
}

void
serial_poll(void)
{
    int ch;
    uint32_t sr = irq_disable();
    ch = serial_getc();
    if (ch != -1) {
        serial_active = 1;
        serial_input_rb_put(ch);
    }
    irq_restore(sr);
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
        serial_poll();
        ch = input_rb_get();
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
        if (serial_output_enabled)
            serial_putc('\r');
        if (screen_output_enabled)
            show_char('\r');
    }

    if (serial_output_enabled)
        serial_putc((uint) ch);
    if (screen_output_enabled)
        show_char((uint) ch);
    return (ch);
}

int
puts(const char *str)
{
    if (serial_output_enabled) {
        serial_puts(str);
        serial_putc('\r');
        serial_putc('\n');
    }
    if (screen_output_enabled) {
        show_string(str);
        show_string("\r\n");
    }
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

    vblank_ints = 0;     // Tell VBlank code we are still running
    INT_COUNTER(8) = 0;  // Clear spurious interrupt counter

    for (cur = ser_in_rb_consumer; cur != ser_in_rb_producer; cur = next) {
        next = (cur + 1) % ARRAY_SIZE(ser_in_rb);
        if (ser_in_rb[cur] == 0x03) {  /* ^C is abort key */
            ser_in_rb_consumer = next;
            return (1);
        }
    }
    serial_poll();
#ifdef KEYBOARD_POLL
    keyboard_poll();
#endif

    return (0);
}

void
serial_replay_output(void)
{
    uint prod = ser_out_prod;
    uint cons = 0;
    if (ser_out_wrapped) {
        cons = (prod + 1) % sizeof (ser_out_buf);
    }
    while (cons != prod) {
        putchar(ser_out_buf[cons]);
        cons = (cons + 1) % sizeof (ser_out_buf);
    }
    ser_out_prod = prod;  // Reset producer
}

void
serial_report(uint bank, const char *action, const char *str)
{
    serial_puts(action);
    serial_putc(' ');
    serial_putc('0' + bank);
    serial_putc(' ');
    serial_puts(str);
    serial_putc('\n');
    timer_delay_msec(10);
}
