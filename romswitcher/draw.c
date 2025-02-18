/*
 * Drawing functions.
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
 *
 * Portions of the area fill code below was taken from sources on the
 * Internet. Area fill is not working yet, and further work is
 * required to make it functional.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "amiga_chipset.h"
#include "util.h"
#include "printf.h"
#include "draw.h"
#include "screen.h"
#include "intuition.h"
#include "timer.h"
#include "vectors.h"
#include <hardware/blit.h>

void BltPattern(struct RastPort *rp, const PLANEPTR mask, LONG xMin, LONG yMin,
                LONG xMax, LONG yMax, ULONG byteCnt);


void
draw_rect(uint fgpen, int x1, int y1, int x2, int y2)
{
    draw_line(fgpen, x1, y1, x2, y1);   // top right
    draw_line(fgpen, x2, y1, x2, y2);   // bottom right
    draw_line(fgpen, x2, y2, x1, y2);   // bottom left
    draw_line(fgpen, x1, y2, x1, y1);   // top left
}

void
Move(RastPort *rp, int x, int y)
{
    rp->cp_x = x;
    rp->cp_y = y;
}

void
Draw(RastPort *rp, int x, int y)
{
    draw_line(rp->FgPen, rp->cp_x, rp->cp_y, x, y);
    rp->cp_x = x;
    rp->cp_y = y;
}

void
PolyDraw(RastPort *rp, int count, WORD *da)
{
    while (count-- > 0) {
        Draw(rp, da[0], da[1]);
        da += 2;
    }
}

void
Rect(RastPort *rp, int xmin, int ymin, int xmax, int ymax)
{
    rp->cp_x = xmin;                         // start: top left
    rp->cp_y = ymin;
    draw_rect(rp->FgPen, xmin, ymin, xmax, ymax);
}

void
RectFill(RastPort *rp, int xmin, int ymin, int xmax, int ymax)
{
    fill_rect_cpu(rp->FgPen, xmin, ymin, xmax, ymax);
}

void
DrawEllipse(RastPort *rp, int x, int y, int a, int b)
{
    (void) rp;
    (void) x;
    (void) y;
    (void) a;
    (void) b;
}

#define iPTR uintptr_t
void
InitArea(AreaInfo *areainfo, APTR vectbuf, LONG maxvec)
{
    areainfo->VctrTbl  = vectbuf;
    areainfo->VctrPtr  = vectbuf;
    areainfo->FlagTbl  = (int8_t *) (((uintptr_t) vectbuf) + (2 * 2 * maxvec));
    areainfo->FlagPtr  = (int8_t *) (((uintptr_t) vectbuf) + (2 * 2 * maxvec));
    areainfo->Count    = 0;
    areainfo->MaxCount = maxvec;
}

PLANEPTR
AllocRaster(uint width, uint height)
{
    return ((PLANEPTR) malloc_chipmem(RASSIZE(width, height)));
}

void
FreeRaster(PLANEPTR p, uint width, uint height)
{
    (void) width;
    (void) height;
    free_chipmem(p);
}

static void
areaclosepolygon(AreaInfo *areainfo)
{
    if (areainfo->FlagPtr[-1] != AREAINFOFLAG_DRAW)
        return;  // Wrong type

    if ((areainfo->VctrPtr[-1] != areainfo->FirstY) ||
        (areainfo->VctrPtr[-2] != areainfo->FirstX)) {
        areainfo->Count++;
        areainfo->VctrPtr[0] = areainfo->FirstX;
        areainfo->VctrPtr[1] = areainfo->FirstY;
        areainfo->FlagPtr[0] = AREAINFOFLAG_CLOSEDRAW;

        areainfo->VctrPtr = &areainfo->VctrPtr[2];
        areainfo->FlagPtr++;
    } else {
        areainfo->FlagPtr[-1] = AREAINFOFLAG_CLOSEDRAW;
    }
}

int32_t
AreaMove(RastPort *rp, int x, int y)
{
    AreaInfo *areainfo = rp->AreaInfo;

    if (areainfo->Count >= areainfo->MaxCount) {
        /* Out of space in the area info buffer */
        return (-1);
    }

    if (areainfo->Count == 0) {
        /* First entry */
        areainfo->FirstX = x;
        areainfo->FirstY = y;

        /* Insert the new point */
        areainfo->VctrPtr[0] = x;
        areainfo->VctrPtr[1] = y;
        areainfo->VctrPtr    = &areainfo->VctrPtr[2];

        areainfo->FlagPtr[0] = AREAINFOFLAG_MOVE;
        areainfo->FlagPtr++;

        areainfo->Count++;
    } else {
        /* Following entry */
        if (AREAINFOFLAG_MOVE == areainfo->FlagPtr[-1]) {
            /* Previous entry was also area move, so replace it */
            areainfo->FirstX = x;
            areainfo->FirstY = y;

            /* replace the previous point */
            areainfo->VctrPtr[-2] = x;
            areainfo->VctrPtr[-1] = y;
        } else {
            /* Not the first command not the previous wasn't area move */
            areaclosepolygon(areainfo);

            if (areainfo->Count + 1 > areainfo->MaxCount)
                return (-1);

            areainfo->FirstX = x;
            areainfo->FirstY = y;

            /* Insert the new point into the matrix */
            areainfo->VctrPtr[0] = x;
            areainfo->VctrPtr[1] = y;
            areainfo->VctrPtr    = &areainfo->VctrPtr[2];

            areainfo->FlagPtr[0] = AREAINFOFLAG_MOVE;
            areainfo->FlagPtr++;
            areainfo->Count++;
        }
    }

    Move(rp, x, y);
    return (0);
}

int32_t
AreaDraw(struct RastPort *rp, int x, int y)
{
    struct AreaInfo *areainfo = rp->AreaInfo;

    if (areainfo->Count >= areainfo->MaxCount) {
        /* Out of space in the area info buffer */
        return (-1);
    }

    areainfo->Count++;
    areainfo->VctrPtr[0] = x;
    areainfo->VctrPtr[1] = y;
    areainfo->FlagPtr[0] = AREAINFOFLAG_DRAW;
    areainfo->VctrPtr    = &areainfo->VctrPtr[2];
    areainfo->FlagPtr++;

    Draw(rp, x, y);
    return (0);
}

int32_t
AreaEnd(struct RastPort *rp)
{
    struct AreaInfo * areainfo = rp->AreaInfo;

    if ((areainfo->Count == 0) || (rp->TmpRas == NULL))
        return (0);  // Nothing to do

    WORD first_idx = 0;
    WORD last_idx  = -1;
    ULONG BytesPerRow;
    UWORD * CurVctr  = areainfo -> VctrTbl;
    BYTE  * CurFlag  = areainfo -> FlagTbl;
    uint16_t count;
    UWORD Rem_APen   = GetAPen(rp);
    UWORD Rem_Flags = rp->Flags;
    /*
     * I don't know whether this function may corrupt the
     * cursor position of the rastport. So I save it for later
     */

    UWORD Rem_cp_x   = rp->cp_x;
    UWORD Rem_cp_y   = rp->cp_y;
    /* This rectangle serves as a "frame" for the tmpras for filling */
    struct Rectangle bounds;

    bounds.MinX = 0;
    bounds.MaxX = 0;
    bounds.MinY = 0;
    bounds.MaxY = 0;

    areaclosepolygon(areainfo);

    count = areainfo->Count;

    // kprintf("%d coord to process\n", count);

    /* process the list of vectors */
    while (count > 0) {
        // kprintf("\n******** Flags:%d Coord: (%d,%d)\n",
        //         CurFlag[0], CurVctr[0],CurVctr[1]);

        last_idx ++;
        switch ((unsigned char)CurFlag[0]) {
            case AREAINFOFLAG_MOVE:
                /* set the graphical cursor to a starting position */
                Move(rp, CurVctr[0], CurVctr[1]);

                bounds.MinX = CurVctr[0];
                bounds.MaxX = CurVctr[0];
                bounds.MinY = CurVctr[1];
                bounds.MaxY = CurVctr[1];

                CurVctr = &CurVctr[2];
                CurFlag = &CurFlag[1];
                break;

            case AREAINFOFLAG_CLOSEDRAW:
                /*
                 * this indicates that this Polygon is closed with
                 * this coordinate
                 */
                /*
                 * Must draw from lower y's to higher ones otherwise
                 * the fill algo does not work nicely.
                 */
#if 1
                if (rp->cp_y <= CurVctr[1]) {
                    Draw(rp, CurVctr[0], CurVctr[1]);
                } else {
                    int _x = rp->cp_x;
                    int _y = rp->cp_y;
                    rp->cp_x = CurVctr[0];
                    rp->cp_y = CurVctr[1];
                    Draw(rp, _x, _y);
                    rp->cp_x = CurVctr[0];
                    rp->cp_y = CurVctr[1];
                }
#endif
                CurVctr = &CurVctr[2];
                CurFlag = &CurFlag[1];
                /*
                 * no need to set the boundaries here like in case above as
                 * this coord closes the polygon and therefore is the same
                 * one as the first coordinate of the polygon.
                 *
                 * check whether there's anything to fill at all. I cannot
                 * fill a line (=3 coordinates)
                 */
                if (first_idx+2 <= last_idx) {
                    /* BytesPerRow must be a multiple of 2 bytes */

                    BytesPerRow = bounds.MaxX - bounds.MinX + 1;
                    if (0 != (BytesPerRow & 0x0f))
                        BytesPerRow = ((BytesPerRow >> 3) & 0xfffe) + 2;
                    else
                        BytesPerRow = (BytesPerRow >> 3) & 0xfffe;

                    if ((ULONG)rp->TmpRas->Size <
                        BytesPerRow * (bounds.MaxY - bounds.MinY + 1))
                        return (-1);

        /*
         *          kprintf("first: %d, last: %d\n", first_idx, last_idx);
         *          kprintf("(%d,%d)-(%d,%d)\n", bounds.MinX, bounds.MinY,
         *                                       bounds.MaxX, bounds.MaxY);
         *          kprintf("width: %d, bytesperrow: %d\n",
         *                  bounds.MaxX-bounds.MinX+1, BytesPerRow);
         */
#if 0
                    if (areafillpolygon(rp, &bounds, first_idx, last_idx,
                                        BytesPerRow)) {
                        /*
                         * Blit the area fill pattern through the mask
                         * provided by rp->TmpRas.
                         */

                        BltPattern(rp,
                                   rp->TmpRas->RasPtr,
                                   bounds.MinX,
                                   bounds.MinY,
                                   bounds.MaxX,
                                   bounds.MaxY,
                                   BytesPerRow);

                        if (rp->Flags & AREAOUTLINE) {
                            SetAPen(rp, GetOutlinePen(rp));
                            PolyDraw(rp, last_idx - first_idx + 1,
                                     &areainfo->VctrTbl[first_idx * 2]);
                            SetAPen(rp, Rem_APen);
                            rp->Flags = Rem_Flags;
                        }

                    }
#endif
                }
                /* set first_idx for a possible next polygon to draw */
                first_idx = last_idx + 1;
                break;

            case AREAINFOFLAG_DRAW:
                /* Draw a line to new position */
#if 1
                /*
                 * Must draw from lower y's to higher ones otherwise
                 * the fill algo does not work nicely.
                 */
                if (rp->cp_y <= CurVctr[1]) {
                    Draw(rp, CurVctr[0], CurVctr[1]);
                } else {
                    int _x = rp->cp_x;
                    int _y = rp->cp_y;
                    rp->cp_x = CurVctr[0];
                    rp->cp_y = CurVctr[1];
                    Draw(rp, _x, _y);
                    rp->cp_x = CurVctr[0];
                    rp->cp_y = CurVctr[1];
                }
#endif
                if (bounds.MinX > CurVctr[0])
                    bounds.MinX = CurVctr[0];
                if (bounds.MaxX < CurVctr[0])
                    bounds.MaxX = CurVctr[0];
                if (bounds.MinY > CurVctr[1])
                    bounds.MinY = CurVctr[1];
                if (bounds.MaxY < CurVctr[1])
                    bounds.MaxY = CurVctr[1];
                CurVctr = &CurVctr[2];
                CurFlag = &CurFlag[1];
                break;

            case AREAINFOFLAG_ELLIPSE:
                bounds.MinX = CurVctr[0] - CurVctr[2];
                bounds.MaxX = CurVctr[0] + CurVctr[2];
                bounds.MinY = CurVctr[1] - CurVctr[3];
                bounds.MaxY = CurVctr[1] + CurVctr[3];
                BytesPerRow = bounds.MaxX - bounds.MinX + 1;

                if (0 != (BytesPerRow & 0x0f))
                    BytesPerRow = ((BytesPerRow >> 3) & 0xfffe) + 2;
                else
                    BytesPerRow = (BytesPerRow >> 3) & 0xfffe;

                if ((ULONG) rp->TmpRas->Size <
                    BytesPerRow * (bounds.MaxY - bounds.MinY + 1))
                    return (-1);

                /* Draw an Ellipse and fill it */
                /* see how the data are stored by the second entry */
                /* I get cx, cy, cx+a, cy+b */

                DrawEllipse(rp, CurVctr[0],
                                CurVctr[1],
                                CurVctr[2],
                                CurVctr[3]);

                /*
                 * area-fill the ellipse with the pattern given
                 * in rp->AreaPtrn, AreaPtSz
                 */
#if 0
                areafillellipse(rp,
                                &bounds,
                                CurVctr,
                                BytesPerRow);
                /*
                 * Blit the area fill pattern through the mask provided
                 * by rp->TmpRas.
                 */
                BltPattern(rp,
                           rp->TmpRas->RasPtr,
                           bounds.MinX,
                           bounds.MinY,
                           bounds.MaxX,
                           bounds.MaxY,
                           BytesPerRow);
#endif

                if (rp->Flags & AREAOUTLINE) {
                    SetAPen(rp, GetOutlinePen(rp));

                    DrawEllipse(rp, CurVctr[0],
                                    CurVctr[1],
                                    CurVctr[2],
                                    CurVctr[3]);

                    SetAPen(rp, Rem_APen);
                    rp->Flags = Rem_Flags;
                }

                CurVctr = &CurVctr[4];
                CurFlag = &CurFlag[2];
                count--;
                last_idx++; /* there were two coords here! */

                /* set first_idx for a possible next polygon to draw */
                first_idx = last_idx + 1;
                break;

            default:
                /* this is an error */
                SetAPen(rp, Rem_APen);
                rp->Flags = Rem_Flags;

                /* also restore old graphics cursor position */
                rp->cp_x = Rem_cp_x;
                rp->cp_y = Rem_cp_y;
                return (-1);
        }
        count--;
    }

    /* restore areainfo structure for a new beginning */
    areainfo->VctrPtr = areainfo->VctrTbl;
    areainfo->FlagPtr = areainfo->FlagTbl;
    areainfo->Count   = 0;

    /* restore old graphics cursor position */
    rp->cp_x = Rem_cp_x;
    rp->cp_y = Rem_cp_y;

    return (0);
}

uint16_t
TextLength(struct RastPort *rp, const char *text, uint count)
{
    (void) rp;
    (void) text;
    return (count * FONT_WIDTH);
}

void
Text(RastPort *rp, const char * text, uint len)
{
    int16_t y = rp->cp_y - (FONT_HEIGHT - 1);  // Text location is bottom left
    render_text_at(text, len, rp->cp_x, y, rp->FgPen, rp->BgPen);
    rp->cp_x += 8 * len;
    if (rp->cp_x > SCREEN_WIDTH) {
        rp->cp_x = 0;
        rp->cp_y += 8;
    }
}
