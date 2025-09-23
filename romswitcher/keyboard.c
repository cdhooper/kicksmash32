/*
 * Amiga Keyboard handling.
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
#include "amiga_chipset.h"
#include "util.h"
#include "printf.h"
#include "serial.h"
#include "timer.h"
#include "vectors.h"

#undef KEYBOARD_DEBUG
#ifdef KEYBOARD_DEBUG
#define DEBUG_COLOR(x) *COLOR00 = (x)
#else
#define DEBUG_COLOR(x)
#endif

#define KEY_CTRL_A           0x01  /* ^A Line begin */
#define KEY_CTRL_B           0x02  /* ^B Cursor left */
#define KEY_CTRL_E           0x05  /* ^E Line end */
#define KEY_CTRL_F           0x06  /* ^F Cursor right */
#define KEY_CTRL_P           0x10  /* ^P Cursor up */
#define KEY_CTRL_N           0x0e  /* ^N Cursor down */
#define KEY_CTRL_O           0x0f  /* ^O ASCII Shift In, reused as Shift-Tab */

#define KEY_LINE_BEGIN       KEY_CTRL_A
#define KEY_LINE_END         KEY_CTRL_E
#define KEY_CURSOR_LEFT      KEY_CTRL_B
#define KEY_CURSOR_RIGHT     KEY_CTRL_F
#define KEY_CURSOR_UP        KEY_CTRL_P
#define KEY_CURSOR_DOWN      KEY_CTRL_N
#define KEY_SHIFT_TAB        KEY_CTRL_O

const struct {
    uint8_t scancode;
    uint8_t ascii;
    uint8_t ascii_shifted;
} key_scancode_to_ascii[] = {
    { 0x00,  '`',  '~' },
    { 0x01,  '1',  '!' },
    { 0x02,  '2',  '@' },
    { 0x03,  '3',  '#' },
    { 0x04,  '4',  '$' },
    { 0x05,  '5',  '%' },
    { 0x06,  '6',  '^' },
    { 0x07,  '7',  '&' },
    { 0x08,  '8',  '*' },
    { 0x09,  '9',  '(' },
    { 0x0a,  '0',  ')' },
    { 0x0b,  '-',  '_' },
    { 0x0c,  '=',  '+' },
    { 0x0d, '\\',  '|' },
    { 0x0e,    0,   0  },  // Undefined
    { 0x0f,  '0',  '0' },  // Keypad 0
    { 0x10,  'q',  'Q' },
    { 0x11,  'w',  'W' },
    { 0x12,  'e',  'E' },
    { 0x13,  'r',  'R' },
    { 0x14,  't',  'T' },
    { 0x15,  'y',  'Y' },
    { 0x16,  'u',  'U' },
    { 0x17,  'i',  'I' },
    { 0x18,  'o',  'O' },
    { 0x19,  'p',  'P' },
    { 0x1a,  '[',  '{' },
    { 0x1b,  ']',  '}' },  // Keypad bracket
    { 0x1c,    0,   0  },  // Undefined
    { 0x1d,  '1',  '1' },  // Keypad 1
    { 0x1e,  '2',  '2' },  // Keypad 2
    { 0x1f,  '3',  '3' },  // Keypad 3
    { 0x20,  'a',  'A' },
    { 0x21,  's',  'S' },
    { 0x22,  'd',  'D' },
    { 0x23,  'f',  'F' },
    { 0x24,  'g',  'G' },
    { 0x25,  'h',  'H' },
    { 0x26,  'j',  'J' },
    { 0x27,  'k',  'K' },
    { 0x28,  'l',  'L' },
    { 0x29,  ';',  ':' },
    { 0x2a,  ',',  '"' },
    { 0x2b,    0,   0  },  // Key next to Return (regional)
    { 0x2c,    0,   0  },  // Undefined
    { 0x2d,  '4',  '4' },  // Keypad 4
    { 0x2e,  '5',  '5' },  // Keypad 5
    { 0x2f,  '6',  '6' },  // Keypad 6
    { 0x30,    0,   0  },  // Key next to left Shift (regional)
    { 0x31,  'z',  'Z' },
    { 0x32,  'x',  'X' },
    { 0x33,  'c',  'C' },
    { 0x34,  'v',  'V' },
    { 0x35,  'b',  'B' },
    { 0x36,  'n',  'N' },
    { 0x37,  'm',  'M' },
    { 0x38,  ',',  '<' },
    { 0x39,  '.',  '>' },
    { 0x3a,  '/',  '?' },
    { 0x3b,    0,   0  },  // Spare
    { 0x3c,  '.',  '.' },  // Keypad .
    { 0x3d,  '7',  '7' },  // Keypad 7
    { 0x3e,  '8',  '8' },  // Keypad 8
    { 0x3f,  '9',  '9' },  // Keypad 9
    { 0x40,  ' ',  ' ' },  // Space bar
    { 0x41, '\b', '\b' },  // Backspace
    { 0x42, '\t', KEY_SHIFT_TAB },  // Tab
    { 0x43, '\r', '\r' },  // Keypad Enter
    { 0x44, '\r', '\r' },  // Return
    { 0x45,   27,   27 },  // ESC
    { 0x46,  127,  127 },  // Delete
    { 0x47,    0,   0  },  // Undefined
    { 0x48,    0,   0  },  // Undefined
    { 0x49,    0,   0  },  // Undefined
    { 0x4a,  '-',  '-' },  // Keypad -
    { 0x4b,    0,   0  },  // Undefined
    { 0x4c,  KEY_CURSOR_UP,    0 },              // Cursor up
    { 0x4d,  KEY_CURSOR_DOWN,  0 },              // Cursor down
    { 0x4e,  KEY_CURSOR_RIGHT, KEY_LINE_END },   // Cursor right
    { 0x4f,  KEY_CURSOR_LEFT,  KEY_LINE_BEGIN }, // Cursor left
    { 0x50,    0,   0  },  // F1
    { 0x51,    0,   0  },  // F2
    { 0x52,    0,   0  },  // F3
    { 0x53,    0,   0  },  // F4
    { 0x54,    0,   0  },  // F5
    { 0x55,    0,   0  },  // F6
    { 0x56,    0,   0  },  // F7
    { 0x57,    0,   0  },  // F8
    { 0x58,    0,   0  },  // F9
    { 0x59,    0,   0  },  // F10
    { 0x5a,  '(',  '(' },  // Unknown (
    { 0x5b,  ')',  ')' },  // Unknown )
    { 0x5c,  '/',  '/' },  // Keypad /
    { 0x5d,  '*',  '*' },  // Keypad *
    { 0x5e,  '+',  '+' },  // Keypad +
    { 0x5f,    0,   0  },  // HELP
/*
 * Special codes
 * 0x60 - Left shift
 * 0x61 - Right shift
 * 0x62 - Caps lock
 * 0x63 - Control
 * 0x64 - Left alt
 * 0x65 - Right alt
 * 0x66 - Left Amiga
 * 0x67 - Right Amiga
 *
 * 0x78 - Reset warning
 * 0xf9 - Last key code bad, next key code is retransmit of it
 * 0xfa - Keyboard output buffer overflow
 * 0xfb - Unused (was controller failure)
 * 0xfc - Keyboard selftest failed
 * 0xfd - Initiate power-up key stream (keys pressed at powerup)
 * 0xfe - Terminate power-up key strem
 * 0xff - Unused (was interrupt)
 */
};

#define FLAG_LSHIFT    0x01
#define FLAG_RSHIFT    0x02
#define FLAG_LOCKSHIFT 0x04
#define FLAG_CONTROL   0x08
#define FLAG_LALT      0x10
#define FLAG_RALT      0x20
#define FLAG_LAMIGA    0x40
#define FLAG_RAMIGA    0x80

static uint8_t shift_state;
static uint8_t keyboard_init_done;

static uint8_t
scan_convert_to_ascii(uint8_t scancode)
{
    uint8_t ch;

    if (scancode >= ARRAY_SIZE(key_scancode_to_ascii))
        return (0);

    if (shift_state & (FLAG_LSHIFT | FLAG_RSHIFT | FLAG_LOCKSHIFT))
        return (key_scancode_to_ascii[scancode].ascii_shifted);

    ch = key_scancode_to_ascii[scancode].ascii;
    if (shift_state & FLAG_CONTROL) {
        if ((ch >= 'a') && (ch <= 'z'))
            return (ch - 'a' + 1);
        switch (ch) {
            case KEY_CURSOR_LEFT:
                return (KEY_LINE_BEGIN);
            case KEY_CURSOR_RIGHT:
                return (KEY_LINE_END);
        }
    }
    return (ch);
}

static uint
shift_state_flag(uint8_t scancode)
{
    uint bit = 0;
    switch (scancode & 0x7f) {
        case 0x60:  // Left shift
            bit = FLAG_LSHIFT;
            break;
        case 0x61:  // Right shift
            bit = FLAG_RSHIFT;
            break;
        case 0x62:  // Caps lock
            bit = FLAG_LOCKSHIFT;
            break;
        case 0x63:  // Control
            bit = FLAG_CONTROL;
            break;
        case 0x64:  // Left Alt
            bit = FLAG_LALT;
            break;
        case 0x65:  // Right Alt
            bit = FLAG_RALT;
            break;
        case 0x66:  // Left Amiga
            bit = FLAG_LAMIGA;
            break;
        case 0x67:  // Right Amiga
            bit = FLAG_RAMIGA;
            break;
    }
    if (scancode & 0x80)
        shift_state &= ~bit;
    else
        shift_state |= bit;
    return (bit);
}

static uint64_t key_repeat_timer;
static uint16_t key_held;

void
keyboard_irq(void)
{
    // Set SPMOD
    //    cb bfee01 48
    // Clear SPMOD
    //    cb bfee01 0
    // Read SP
    //    db bfec01 1
    // Dump CIAA registers
    //      loop 16 dbA bfe$a01 1

    uint8_t scan_orig;
    uint8_t scan_conv;
    static uint8_t scan_last = 0;
    static volatile uint8_t running = 0;

    if (keyboard_init_done == 0)
        return;

    uint32_t sr = irq_disable();

    if (running) {
        DEBUG_COLOR(0xf00);  // Red (background)
        irq_restore(sr);
        return;
    }
    running = 1;
    DEBUG_COLOR(0x0c4);   // Aqua-Green (background)
    irq_restore(sr);

    *CIAA_CRA = CIA_CRA_SPMOD;  // Set handshake bit

    /* Read keyboard */
    timer_delay_usec(75);
    scan_orig = *CIAA_SP;

    if (scan_last == scan_orig)
        goto finish;

    scan_last = scan_orig;
    scan_conv = ~((scan_orig >> 1) | (scan_orig << 7));

    if ((shift_state_flag(scan_conv) == 0) &&
        ((scan_conv & BIT(7)) == 0)) {
        uint8_t ascii = scan_convert_to_ascii(scan_conv);
        if (ascii != 0) {
//          printf("RB put '%c' %02x %02x\n", ascii, ascii, scan_conv & 0x7f);
            DEBUG_COLOR(0x00f);   // Bright blue (background)
            key_held = ascii | (scan_conv << 8);
            input_rb_put(key_held);
        } else {
            key_held = 0;
        }
    } else {
        input_rb_put(scan_conv << 8);
        key_held = 0;
    }

    if (scan_conv & BIT(7)) {
//      printf("key up %02x %02x\n", scan_orig, scan_conv);
        goto finish;
    }

#if 0
    timer_delay_msec(2);
#endif

#ifdef KEYBOARD_DEBUG
    printf("[%04x]", scan_conv);
#endif
//  printf("key down %02x %02x\n", scan_orig, scan_conv);

finish:
//  timer_delay_usec(75);
    *CIAA_CRA = 0;        // Clear handshake bit
    DEBUG_COLOR(0x77c);   // Blue (background)
    running = 0;
    key_repeat_timer = 0;
}

void
keyboard_poll(void)
{
    /* Handle key repeats */
    if (key_held != 0) {
        if (key_repeat_timer == 0) {
            key_repeat_timer = timer_tick_plus_msec(500);
            return;
        }
        if (timer_tick_has_elapsed(key_repeat_timer)) {
            key_repeat_timer = timer_tick_plus_msec(70);
            input_rb_put(key_held);
        }
    }
    vblank_ints = 0;
}

/*
 * To begin transmission, you must first set up Timer A in continuous mode,
 * and start the timer. Transmission will start following a write to the
 * serial data register. The clock signal derived from Timer A appears as
 * an output on the CNT pin. The data in the serial data register will be
 * loaded into the shift register, then shifted out to the SP pin when a
 * CNT pulse occurs. Data shifted out becomes valid on the next falling
 * edge of CNT and remains valid until the next falling edge. After eight
 * CNT pulses, an interrupt is generated to indicate that more data can
 * be sent. If the serial data register was reloaded with new information
 * prior to this interrupt, the new data will automatically be loaded into
 * the shift register and transmission will continue.
 */

void
keyboard_init(void)
{
    *INTENA = INTENA_PORTS;  // Disable interrupts
    *CIAA_ICR = CIA_ICR_SET | CIA_ICR_SP;  // enable SP interrupt
    timer_delay_msec(5);

    keyboard_init_done = 1;

    *CIAA_CRA = 0;                           // Clear handshake bit
    *INTENA = INTENA_SETCLR | INTENA_PORTS;  // Enable interrupts

    /*
     * Dump CIAA registers
     *      loop 16 dbA bfe$a01 1
     */
}
