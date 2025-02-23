/*
 * Drawing code test functions.
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
#include "printf.h"
#include "draw.h"
#include "screen.h"
#include "intuition.h"
#include "testdraw.h"

/* Only one of these should be defined for test at a time */
#undef TEST_LINE_DRAW
#undef TEST_SIMPLE_RECT_LINE_DRAW
#undef TEST_RECT_BLIT_NO_OVERLAP
#undef TEST_RECT_BLIT_TWO_OVERLAP
#undef TEST_RECT_BLIT_OVERLAP_COLORS_LINE_OVERLAY
#undef TEST_RECT_CPU_OVERLAP_COLORS_LINE_OVERLAY
#undef TEST_RECT_BLIT_OVERLAP_MANY_SOLID
#undef TEST_TEXT
#undef TEST_POLYDRAW
#undef TEST_AREAFILL
#undef TEST_BLITFILL

#ifdef TEST_LINE_DRAW
static void
test_line_draw(void)
{
    struct RastPort *rp = &sscreen.RastPort;
    int x = 50;
    int y = 30;
    int cur;

    for (cur = 10; cur > 0; cur--) {
        /* Yellow down-to-right line */
        SetAPen(rp, 4);
        Move(rp, x,      y + cur);
        Draw(rp, x + 50, y + 50 + cur);
        x += cur;
    }

    x += 50;
    for (cur = 10; cur > 0; cur--) {
        /* Grey up-to-right line */
        SetAPen(rp, 6);
        Move(rp, x,      y + 50 + cur);
        Draw(rp, x + 50, y + cur);
        x += cur;
    }

    x += 50;
    for (cur = 10; cur > 0; cur--) {
        /* Blue up-to-left line */
        SetAPen(rp, 3);
        Move(rp, x + 50, y + 50 + cur);
        Draw(rp, x,      y + cur);
        x += cur;
    }

    x += 50;
    for (cur = 10; cur > 0; cur--) {
        /* Red down-to-left line */
        SetAPen(rp, 7);
        Move(rp, x + 50, y + cur);
        Draw(rp, x,      y + 50 + cur);
        x += cur;
    }

    const uint8_t pens[] = { 4, 3, 6, 7 };
    y += 100;
    x = 50;
    for (cur = 30; cur > 4; cur--) {
        /* Down-to-right line */
        SetAPen(rp, pens[cur & 3]);
        Move(rp, x,      y + cur / 4);
        Draw(rp, x + 50, y + 50 + cur / 4);
        x += cur / 4;
    }
    for (cur = 30; cur > 4; cur--) {
        /* Up-to-right line */
        SetAPen(rp, pens[cur & 3]);
        Move(rp, x,      y + 50 + cur / 4);
        Draw(rp, x + 50, y + cur / 4);
        x += cur / 4;
    }
    for (cur = 30; cur > 4; cur--) {
        /* Up-to-left lines */
        SetAPen(rp, pens[cur & 3]);
        Move(rp, x + 50, y + 50 + cur / 4);
        Draw(rp, x,      y + cur / 4);
        x += cur / 4;
    }
    for (cur = 30; cur > 4; cur--) {
        /* Down-to-left line */
        SetAPen(rp, pens[cur & 3]);
        Move(rp, x + 50, y + cur / 4);
        Draw(rp, x,      y + 50 + cur / 4);
        x += cur / 4;
    }
}
#endif

#ifdef TEST_SIMPLE_RECT_LINE_DRAW
static void
test_simple_rect_line_draw(void)
{
    struct RastPort *rp = &sscreen.RastPort;

    /* Black V */
    SetAPen(rp, 1);
    draw_line(1, 100, 100, 100, 150);
    draw_line(1, 50, 100, 100, 150);

    /* White L */
    SetAPen(rp, 2);
    Move(rp, 200, 150);
    Draw(rp, 200, 175);
    Draw(rp, 250, 175);

    /* Blue squares */
    SetAPen(rp, 3);
    Rect(rp, 200, 20, 250, 30);

    Rect(rp, 300, 20, 350, 30);  // Double wall blue square
    Rect(rp, 301, 21, 349, 29);

    SetAPen(rp, 7);  // Interior red
    Rect(rp, 302, 22, 348, 28);
    Rect(rp, 303, 23, 347, 27);
}
#endif

#ifdef TEST_RECT_BLIT_NO_OVERLAP
static void
test_rect_blit_no_overlap(void)
{
    /* Non-overlapping solid color rectangles */
    struct RastPort *rp = &sscreen.RastPort;

    int p;
    for (p = 0; p < 8; p++) {
        int startx = 50 + p * 30;
        int starty = 40 + p * 20;

        fill_rect_blit(p, startx, starty,
                       startx + 20 * p, starty + 10 + p, FALSE, 1);
//      SetAPen(rp, p);
//      Rect(rp, startx, starty, startx + 20 * p, starty + 10 + p);

        SetAPen(rp, 2);
    }
}
#endif

#ifdef TEST_RECT_BLIT_TWO_OVERLAP
static void
test_rect_blit_two_overlap(void)
{
    /* Two overlapping solid color rectangles */
    struct RastPort *rp = &sscreen.RastPort;

    fill_rect_blit(1, 100, 50, 200, 70, FALSE, 1);
    fill_rect_blit(3, 150, 100, 250, 150, FALSE, 1);
    fill_rect_blit(2, 200, 90, 260, 120, FALSE, 1);
    fill_rect_blit(4, 110, 60, 140, 65, FALSE, 1);
}
#endif

#ifdef TEST_RECT_BLIT_OVERLAP_COLORS_LINE_OVERLAY
static void
test_rect_blit_overlap_colors_line_overlay(void)
{
    /* All 8 colors as overlapping rectangles, outlined with next color */
    struct RastPort *rp = &sscreen.RastPort;

    uint x = 32;
    uint y = 48;
    for (int p = 0; p < 35; p++) {
//      SetAPen(rp, (p % 7) + 1);
        fill_rect_cpu(2, 128 + x, y, 128 + x + 48, y + 6);
        fill_rect_blit(1, x, y, x + 48, y + 6, FALSE, 1);
//      fill_rect_cpu(1, x + 256, y, x + 256 + 48, y + 6);
//      blit_fill((APTR)BITPLANE_0_BASE, SCREEN_WIDTH, x, y, 48, 4);
        x += 1;
        y += 5;
    }
}
#endif

#ifdef TEST_RECT_CPU_OVERLAP_COLORS_LINE_OVERLAY
static void
test_rect_cpu_overlap_colors_line_overlay(void)
{
    /* All 8 colors as overlapping rectangles, outlined with next color */
    struct RastPort *rp = &sscreen.RastPort;

    uint x = 32;
    uint y = 50;
    for (int p = 0; p < 16; p++) {
//      fill_rect_blit(p & 7, x, y, x + 70, y + 30, FALSE, 1);
        fill_rect_cpu(p & 7, x, y, x + 70, y + 30);
        x += 60;
        if (x > 500) {
            x = 32;
            y += 35;
        }
        y += 8;
    }
    /* Add boxes around the first 8, offset down a bit */
    x = 32;
    y = 54;
    for (int p = 0; p < 8; p++) {
        SetAPen(rp, (p + 1) & 7);
        Rect(rp, x, y, x + 70, y + 30);
        x += 60;
        if (x > 500) {
            x = 20;
            y += 35;
        }
        y += 8;
    }
    draw_line(7, 32 + 60, 96, 32 + 60, 156);
    draw_line(7, 32 + 60, 96, 32 + 60, 156);
}
#endif

#ifdef TEST_RECT_BLIT_OVERLAP_MANY_SOLID
static void
test_rect_blit_overlap_many_solid(void)
{
    /* Many overlapping solid color boxes */
    struct RastPort *rp = &sscreen.RastPort;

    uint x = 20;
    uint y = 20;
    for (int p = 0; p < 40; p++) {
        fill_rect_blit(p & 7, x, y, x + 70 + p, y + 30, FALSE, 1);
        x += 65 + p;
        if (x > 500) {
            x = 20;
        }
        y += 4;
    }
}
#endif

#ifdef TEST_TEXT
static void
test_text(void)
{
    /* Text at various offsets and colors */
    struct RastPort *rp = &sscreen.RastPort;

    uint x = 33;
    uint y = 70;
    uint cur;

    SetAPen(rp, 1);
    SetBPen(rp, 0);

    Move(rp, x, y); y += 8;
    Text(rp, "0", 1);
    Move(rp, x, y); y += 8;
    Text(rp, "0123", 4);
    Move(rp, x, y); y += 8;
    Text(rp, "01234", 5);
    Move(rp, x, y); y += 8;
    Text(rp, "0123456789", 10);

    SetAPen(rp, 7);
    RectFill(rp, x - 10, y + 4, x + 190, y + 24);
    SetAPen(rp, 3);
    RectFill(rp, x - 10, y + 44, x + 190, y + 64);
    SetAPen(rp, 5);
    RectFill(rp, x - 10, y + 84, x + 190, y + 104);
    y += 8;
    for (cur = 0; cur < 20; cur++) {
        SetAPen(rp, 1);
        SetBPen(rp, 2);
        Move(rp, x, y);
        Text(rp, "0123456789", 10);
        SetAPen(rp, 6);
        SetBPen(rp, 4);
        Move(rp, 160 - x, y);
        Text(rp, "0123456789", 10);
        x += 1;
        y += 4;
    }
    uint row;
    for (row = 0; row < 8; row++) {
        uint col;
        for (col = 0; col < 8; col++) {
            SetAPen(rp, row);
            SetBPen(rp, col);
            Move(rp, 400 + col * 8, 70 + row * 8);  // unaligned
            Text(rp, "A", 1);

            Move(rp, 401 + col * 9, 140 + row * 9);  // unaligned
            if (((401 + col * 9) & 7) == 0)
                Text(rp, "A", 1);
            else
                Text(rp, "U", 1);
        }
    }
}
#endif

#ifdef TEST_POLYDRAW
static void
test_polydraw(void)
{
    /* Polygon */
    struct RastPort *rp = &sscreen.RastPort;
    int16_t da[20];
    uint    dc;
    uint    x;
    uint    y;

    SetAPen(rp, 4);
    Move(rp, 68, 40); Text(rp, "PolyDraw", 8);
    SetAPen(rp, 6);
    Move(rp, 270, 40); Text(rp, "PolyDraw", 8);

    x = 100;
    y = 50;
    dc = 0;
    da[dc++] = x;       da[dc++] = y;
    da[dc++] = x + 50;  da[dc++] = y + 25;
    da[dc++] = x;       da[dc++] = y + 50;
    da[dc++] = x - 50;  da[dc++] = y + 25;
    da[dc++] = x;       da[dc++] = y;
    SetAPen(rp, 4);
    Move(rp, da[0], da[1]);
    PolyDraw(rp, dc / 2, da);

    x = 200;
    y = 50;
    dc = 0;
    da[dc++] = x;       da[dc++] = y;
    da[dc++] = x + 50;  da[dc++] = y + 25;
    da[dc++] = x + 100; da[dc++] = y + 25;
    da[dc++] = x + 150; da[dc++] = y + 10;
    da[dc++] = x + 100; da[dc++] = y + 10;
    da[dc++] = x + 50;  da[dc++] = y;
    da[dc++] = x + 25;  da[dc++] = y - 10;
    da[dc++] = x;       da[dc++] = y;
    SetAPen(rp, 6);
    Move(rp, da[0], da[1]);
    PolyDraw(rp, dc / 2, da);
}
#endif

#ifdef TEST_AREAFILL

#define AREA_SIZE 40
static WORD areabuffer[AREA_SIZE];
static struct AreaInfo areaInfo = {0};
static struct TmpRas tmpras;

static void
init_tmpras(void)
{
    struct RastPort *rp = &sscreen.RastPort;
    InitArea(&areaInfo, areabuffer, AREA_SIZE * 2 / 5);
    rp->AreaInfo = &areaInfo;

    tmpras.RasPtr = (BYTE *) AllocRaster(SCREEN_WIDTH, SCREEN_HEIGHT);
    tmpras.Size = (long) RASSIZE(SCREEN_WIDTH, SCREEN_HEIGHT);
    rp->TmpRas = &tmpras;
}

static void
test_areafill(void)
{
    struct RastPort *rp = &sscreen.RastPort;
    uint count;
    int16_t da[20];
    uint    dc;
    uint    x;
    uint    y;

    SetAPen(rp, 2);
    Move(rp, 63, 140); Text(rp, "Area fill", 9);
    SetAPen(rp, 3);
    Move(rp, 270, 140); Text(rp, "Area fill", 9);

    init_tmpras();

    x = 100;
    y = 150;
    dc = 0;
    da[dc++] = x;       da[dc++] = y;
    da[dc++] = x + 50;  da[dc++] = y + 25;
    da[dc++] = x;       da[dc++] = y + 50;
    da[dc++] = x - 50;  da[dc++] = y + 25;
    da[dc++] = x;       da[dc++] = y;
    SetAPen(rp, 2);
    AreaMove(rp, da[0], da[1]);
    for (count = 0; count < dc; count += 2)
        AreaDraw(rp, da[count], da[count + 1]);
    AreaEnd(rp);

    x = 200;
    y = 150;
    dc = 0;
    da[dc++] = x;       da[dc++] = y;
    da[dc++] = x + 50;  da[dc++] = y + 25;
    da[dc++] = x + 100; da[dc++] = y + 25;
    da[dc++] = x + 150; da[dc++] = y + 10;
    da[dc++] = x + 100; da[dc++] = y + 10;
    da[dc++] = x + 50;  da[dc++] = y;
    da[dc++] = x + 25;  da[dc++] = y - 10;
    da[dc++] = x;       da[dc++] = y;
    SetAPen(rp, 3);
    AreaMove(rp, da[0], da[1]);
    for (count = 0; count < dc; count += 2)
        AreaDraw(rp, da[count], da[count + 1]);
    AreaEnd(rp);
}
#endif

#ifdef TEST_BLITFILL
static void
test_blitfill(void)
{
    struct RastPort *rp = &sscreen.RastPort;
    SetAPen(rp, 2);
    Move(rp, 300, 40); Text(rp, "Blit Fill inside rect", 21);

    draw_rect(3, 50, 10, 150, 40);
    blit_fill((APTR)BITPLANE_0_BASE, SCREEN_WIDTH / 8, 40, 14, 200, 20);

    SetAPen(rp, 2);
    Move(rp, 300, 100); Text(rp, "Blit Fill inside polygon", 24);

    SetAPen(rp, 7);
    Move(rp, 20, 80);
    Draw(rp, 60, 60);
    Draw(rp, 110, 80);
    Draw(rp, 150, 110);
    Draw(rp, 110, 130);
    Draw(rp, 90, 100);
    Draw(rp, 50, 150);
    Draw(rp, 40, 90);
    Draw(rp, 20, 80);
//  blit_fill((APTR)BITPLANE_0_BASE, SCREEN_WIDTH / 8, 10, 62, 160, 20);
    blit_fill((APTR)BITPLANE_0_BASE, SCREEN_WIDTH / 8, 10, 60, 160, 90);
}
#endif

static void (*test_handlers[])(void) = {
#ifdef TEST_LINE_DRAW
    test_line_draw,
#endif
#ifdef TEST_SIMPLE_RECT_LINE_DRAW
    test_simple_rect_line_draw,
#endif
#ifdef TEST_RECT_BLIT_NO_OVERLAP
    test_rect_blit_no_overlap,
#endif
#ifdef TEST_RECT_BLIT_TWO_OVERLAP
    test_rect_blit_two_overlap,
#endif
#ifdef TEST_RECT_BLIT_OVERLAP_COLORS_LINE_OVERLAY
    test_rect_blit_overlap_colors_line_overlay,
#endif
#ifdef TEST_RECT_CPU_OVERLAP_COLORS_LINE_OVERLAY
    test_rect_cpu_overlap_colors_line_overlay,
#endif
#ifdef TEST_RECT_BLIT_OVERLAP_MANY_SOLID
    test_rect_blit_overlap_many_solid,
#endif
#ifdef TEST_TEXT
    test_text,
#endif
#ifdef TEST_POLYDRAW
    test_polydraw,
#endif
#ifdef TEST_AREAFILL
    test_areafill,
#endif
#ifdef TEST_BLITFILL
    test_blitfill,
#endif
    NULL,  // Must be last
};

void
test_draw(void)
{
    uint pos;
    for (pos = 0; test_handlers[pos] != NULL; pos++)
        test_handlers[pos]();
    if (pos > 0) {
        while (1)
            getchar();
    }
}
