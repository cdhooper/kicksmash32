/*
 * Mouse handling functions.
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
#ifndef _MOUSE_H
#define _MOUSE_H

#define MOUSE_BUTTON_LEFT    0  // Left mouse button
#define MOUSE_BUTTON_RIGHT   1  // Right mouse button
#define MOUSE_BUTTON_MIDDLE  2  // Middle mouse button

#define MOUSE_BUTTON_PRESS   1  // Mouse button is held
#define MOUSE_BUTTON_RELEASE 0  // Mouse button is released

void mouse_init(void);
void mouse_poll(void);

extern int mouse_x;
extern int mouse_y;

#endif /* _MOUSE_H */
