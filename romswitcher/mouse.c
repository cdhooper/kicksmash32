/*
 * Mouse handling functions.
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
#include "intuition.h"
#include "gadget.h"
#include "mouse.h"

#define MOUSE_DEBUG
#ifdef MOUSE_DEBUG
#define DPRINTF(fmt, ...) printf(fmt, ...)
#else
#define DPRINTF(fmt, ...)
#endif

int mouse_x;
int mouse_y;
int mouse_x_last;
int mouse_y_last;
uint mouse_left;
uint mouse_right;

static void
mouse_poll_buttons(void)
{
    static uint8_t mouse_left_last;
    static uint8_t mouse_right_last;

    mouse_left  = !(*CIAA_PRA & BIT(6));
    mouse_right = !(*POTGOR & BIT(10));
//  mouse_middle = !!(*POTGOR & BIT(8));
    if (mouse_left_last != mouse_left) {
//      printf(" LMB%u", mouse_left);
        mouse_left_last = mouse_left;
        gadget_mouse_button(MOUSE_BUTTON_LEFT, mouse_left);
    }
    if (mouse_right_last != mouse_right) {
//      printf(" RMB%u", mouse_right);
        mouse_right_last = mouse_right;
        gadget_mouse_button(MOUSE_BUTTON_RIGHT, mouse_right);
    }
}

void
mouse_poll(void)
{
    if ((mouse_x != mouse_x_last) || (mouse_y != mouse_y_last)) {
        /* Mouse moved -- check for gadget hover change */
        gadget_mouse_move(mouse_x, mouse_y);
        mouse_x_last = mouse_x;
        mouse_y_last = mouse_y;
//      printf("Mouse %x %x\n", mouse_x, mouse_y);
    }
    mouse_poll_buttons();
}

void
mouse_init(void)
{
    *POTGO = 0xff00;  // Make right mouse button readable
}
