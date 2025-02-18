/*
 * Intuition and Exec API.
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
#include <stdlib.h>
#include "amiga_chipset.h"
#include "util.h"
#include "printf.h"
#include "draw.h"
#include "screen.h"
#include "intuition.h"
#include "draw.h"
#include "reset.h"

struct Screen sscreen;
struct Window swindow;

static void
init_screen_struct(void)
{
    sscreen.Width = SCREEN_WIDTH;
    sscreen.Height = SCREEN_HEIGHT;
    sscreen.DetailPen = 1;
    sscreen.BlockPen = 2;
}

struct Screen *
OpenScreenTagList(struct NewScreen *, struct TagItem *)
{
    init_screen_struct();
    return (&sscreen);
}

struct Screen *
OpenScreenTags(struct NewScreen *, ULONG, ULONG, ...)
{
    init_screen_struct();
    return (&sscreen);
}

int
CloseScreen(struct Screen *screen)
{
    (void) screen;
    return (0);
}

Window *
OpenWindowTags(const struct NewWindow *newWindow, ULONG tag1Type, ...)
{
    (void) newWindow;
    (void) tag1Type;
    swindow.WScreen = &sscreen;
    swindow.Width = sscreen.Width;
    swindow.Height = sscreen.Height;
    swindow.DetailPen = sscreen.DetailPen;
    swindow.BlockPen = sscreen.BlockPen;
    swindow.RPort = &sscreen.RastPort;
    return (&swindow);
}

void
CloseWindow(struct Window *window)
{
    (void) window;
}

void *
GetVisualInfoA(Screen *screen, const struct TagItem *taglist)
{
    (void) screen;
    (void) taglist;
    return (NULL);
}

void
FreeVisualInfo(void *vi)
{
    (void) vi;
}

void
DisplayBeep(Screen *screen)
{
    (void) screen;
    screen_displaybeep();
}

void
ColdReboot(void)
{
    reset_cpu();
}
