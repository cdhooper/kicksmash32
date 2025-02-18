/*
 * Sprite functions.
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
#ifndef _SPRITE_H
#define _SPRITE_H

void sprite_init(void);
uint sprite_calcpos(uint x_start, uint y_start, uint y_end);

extern uint32_t *sprite0_data;
extern uint32_t *sprite1_data;
extern uint32_t *spritex_data;

#endif /* _SPRITE_H */
