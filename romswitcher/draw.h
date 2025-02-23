/*
 * Drawing functions.
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
#ifndef _DRAW_H
#define _DRAW_H

#include "exec_types.h"
#include "intuition.h"

/* drawing modes */
#define JAM1        0         // jam 1 color into raster
#define JAM2        1         // jam 2 colors into raster
#define COMPLEMENT  2         // XOR bits into raster
#define INVERSVID   4         // inverse video for drawing modes

/* RastPort.Flags */
#define FRST_DOT    0x01      // draw the first dot of this line ?
#define ONE_DOT     0x02      // use one dot mode for drawing lines
#define DBUFFER     0x04      // flag set when RastPorts
#define AREAOUTLINE 0x08      // areafill: draw outline
#define NOCROSSFILL 0x20      // areafill: no crossovers present

#define GetAPen(rp)            (rp)->FgPen
#define GetBPen(rp)            (rp)->BgPen
#define GetOutlinePen(rp)      (rp)->AOlPen
#define GetDrMd(rp)            (rp->DrawMode)
#define SetAPen(rp, pen)       do { (rp)->FgPen = (pen); } while (0)
#define SetBPen(rp, pen)       do { (rp)->BgPen = (pen); } while (0)
#define SetOutLinePen(rp, pen) do { (rp)->AOlPen = (pen); \
                                    (rp)->Flags |= AREAOUTLINE; } while (0)
#define SetDrMd(rp, mode)      do { (rp)->DrawMode = (mode); } while (0)
#define BNDRYOFF(rp)           do { {(rp)->Flags &= ~AREAOUTLINE; } while (0)

/* areainfo.FlagPtr */
#define AREAINFOFLAG_MOVE       0x00
#define AREAINFOFLAG_DRAW       0x01
#define AREAINFOFLAG_CLOSEDRAW  0x02
#define AREAINFOFLAG_ELLIPSE    0x03

/* Primitives */
void draw_test(void);
void draw_line(uint fgpen, int x1, int y1, int x2, int y2);
void draw_rect(uint fgpen, int x1, int y1, int x2, int y2);
void fill_rect(uint fgpen, uint x1, uint y1, uint x2, uint y2);
void fill_rect_cpu(uint fgpen, uint x1, uint y1, uint x2, uint y2);
void fill_rect_blit(uint fgpen, uint x1, uint y1, uint x2, uint y2,
                    uint8_t xor, uint8_t fill_carry_input);
void gray_rect(uint fgpen, uint x1, uint y1, uint x2, uint y2);
void blit_fill(APTR dst_base, UWORD dst_stride_b, UWORD x, UWORD y,
               UWORD width, UWORD height);

void     Move(RastPort *rp, int x, int y);
void     Draw(RastPort *rp, int x, int y);
void     PolyDraw(RastPort *rp, int count, int16_t *da);
void     InitArea(AreaInfo *areaInfo, APTR vectorBuffer, LONG maxVectors);
int32_t  AreaMove(RastPort *rp, int x, int y);
int32_t  AreaDraw(struct RastPort *rp, int x, int y);
int32_t  AreaEnd(struct RastPort *rp);
void     Rect(RastPort *rp, int xmin, int ymin, int xmax, int ymax);
void     RectFill(RastPort *rp, int xmin, int ymin, int xmax, int ymax);
void     Text(RastPort *rp, const char * text, uint len);
uint16_t TextLength(struct RastPort *rp, const char *text, uint count);
PLANEPTR AllocRaster(uint width, uint height);
void     FreeRaster(PLANEPTR p, uint width, uint height);

#endif /* _DRAW_H */
