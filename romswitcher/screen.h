/*
 * Amiga screen functions, including screen scroll and text rendering.
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
#ifndef _SCREEN_H
#define _SCREEN_H

#define SCREEN_WIDTH      640
#define SCREEN_HEIGHT     200
#define SCREEN_BITPLANES    3  // 8 colors
#define FONT_WIDTH          8  // pixels
#define FONT_HEIGHT         8  // pixels

#define BITPLANE_OFFSET   (SCREEN_WIDTH / 8 * (SCREEN_HEIGHT + 64))
#define BITPLANE_0_BASE   0x00020000
#define BITPLANE_1_BASE   (BITPLANE_0_BASE + BITPLANE_OFFSET)
#define BITPLANE_2_BASE   (BITPLANE_1_BASE + BITPLANE_OFFSET)

#define TEXTPEN      1  // Black
#define HIGHLIGHTPEN 4  // Gold

void dbg_show_char(uint ch);
void dbg_show_string(const uint8_t *str);
void render_text_at(const char *str, uint maxlen, uint x, uint y,
                    uint fg_color, uint bg_color);
void screen_init(void);
void screen_beep_handle(void);
void screen_displaybeep(void);
void WaitBlit(void);
void SetRGB4(void *vp, int32_t index, uint32_t red, uint32_t green,
             uint32_t blue);

extern uint cursor_x_start;  // Upper left pixel of editor area
extern uint cursor_y_start;  // Upper right pixel of editor area
extern uint cursor_x;        // Cursor left-right column position in editor area
extern uint cursor_y;        // Cursor top-down row position in editor area
extern uint cursor_visible;  // Cursor is visible on screen
extern uint dbg_cursor_x;    // Debug cursor column position on screen
extern uint dbg_cursor_y;    // Debug cursor row position on screen
extern uint dbg_all_scroll;  // Count of lines where all bitplanes should scroll
extern uint displaybeep;     // DisplayBeep is active when non-zero

#endif /* _SCREEN_H */
