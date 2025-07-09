/*
 * Amiga Keyboard handling.
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
#ifndef _KBD_H
#define _KBD_H

#define RAWKEY_SPACE     0x40
#define RAWKEY_TAB       0x42
#define RAWKEY_ENTER     0x43  // Numeric keypad
#define RAWKEY_RETURN    0x44
#define RAWKEY_ESC       0x45
#define RAWKEY_CRSRUP    0x4C
#define RAWKEY_CRSRDOWN  0x4D
#define RAWKEY_CRSRRIGHT 0x4E
#define RAWKEY_CRSRLEFT  0x4F

void keyboard_init(void);
void keyboard_irq(void);
void keyboard_poll(void);

#endif /* _KBD_H */
