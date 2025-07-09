/*
 * GadTools API and main gadget handling functions.
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
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include "util.h"
#include "printf.h"
#include "draw.h"
#include "mouse.h"
#include "intuition.h"
#include "gadget.h"
#include "screen.h"
#include "serial.h"
#include "timer.h"
#include "main.h"

static void GT_PutIMsg(IntuiMessage *imsg);

static Gadget *mouse_cur_gadget = NULL;
static Gadget *click_cur_gadget = NULL;
static Gadget *active_gadget    = NULL;

static IntuiText *
create_intuitext(const char *str)
{
    IntuiText *it = malloc(sizeof (*it));
    memset(it, 0, sizeof (*it));
    it->IText = strdup(str);
    it->FrontPen = TEXTPEN;
    it->BackPen  = 0;       // Screen background
    it->TopEdge  = 1;
    return (it);
}

struct Gadget *
CreateGadget(uint32_t kind, Gadget *pgad, struct NewGadget *ng, ...)
{
    uint scan_again = 0;
    uint scans = 0;
    StringInfo *si = NULL;
    MxInfo     *mx = NULL;
    Gadget g;  // many fields come from the ng structure
    uint tag;
    uint arg;
    uint extrasize = 0;
    va_list ap;

    memset(&g, 0, sizeof (g));
    g.LeftEdge = ng->ng_LeftEdge;
    g.TopEdge = ng->ng_TopEdge;
    g.Width = ng->ng_Width;
    g.Height = ng->ng_Height;

    /*
     *  Not currently supported
     *  if (ng->ng_Flags & PLACETEXT_LEFT)
     *  if (ng->ng_Flags & PLACETEXT_RIGHT)
     *  if (ng->ng_Flags & PLACETEXT_ABOVE)
     *  if (ng->ng_Flags & PLACETEXT_BELOW)
     *  if (ng->ng_Flags & PLACETEXT_IN)
     *  if (ng->ng_Flags & NG_HIGHLABEL)
     *  if (ng->ng_Flags & NG_GRIDLAYOUT)
     */
//  g.Flags =
//  g.Activation =          // Set if GA_Immediate specified as a tag
    g.GadgetType = kind;
//  g.GadgetRender =
//  g.SelectRender =
    if (ng->ng_GadgetText != NULL) {
        IntuiText *it = create_intuitext(ng->ng_GadgetText);
        g.GadgetText = it;
        if (ng->ng_TextAttr != NULL)
            it->ITextFont = ng->ng_TextAttr;
    }
    // ng->ng_VisualInfo is ignored as it's an abstract handle
//  g.MutualExclude =               // deprecated
    g.GadgetID = ng->ng_GadgetID;
    g.UserData = ng->ng_UserData;

    switch (kind) {
        case STRING_KIND:
        case INTEGER_KIND:
            si = malloc(sizeof (StringInfo));
            if (si != NULL) {
                memset(si, 0, sizeof (*si));
                g.SpecialInfo = (void *) si;
                si->CLeft = 6;  // String starts # pixels from gadget left
            }
            g.Flags |= GFLG_TABCYCLE;  // Strings default to tab cycle list
            g.Flags |= GFLG_GADGHBOX;  // Default to draw border
            break;
        case MX_KIND:
            mx = malloc(sizeof (MxInfo));
            if (mx != NULL) {
                memset(mx, 0, sizeof (*mx));
                g.SpecialInfo = (void *) mx;
            }
            break;
    }

creategadget_scan_again:
    va_start(ap, ng);
    while ((tag = va_arg(ap, ULONG)) != TAG_DONE) {
        arg = va_arg(ap, ULONG);
        switch (tag) {
            case GA_Border:  // Border width?
                break;
            case GA_Disabled:
                if (arg)
                    g.Flags |= GFLG_DISABLED;
                else
                    g.Flags &= ~GFLG_DISABLED;
                break;
            // XXX: What about GFLG_SELECTED
            case GTST_EditHook:  // edit callback
                break;
            case GT_ExtraSize:
                extrasize = arg;
                break;
            case GA_Immediate:
                g.Activation |= GACT_IMMEDIATE;
                break;
            case GTMX_Active:
                if (mx != NULL) {
                    mx->mx_selected = arg;
                    mx->mx_seldisplay = arg;
                }
                break;
            case GTMX_Labels:  // Array of MX select label strings
                if (mx != NULL) {
                    uint count = 0;
                    if ((char **) arg != NULL) {
                        /*
                         * Need to copy the caller's data structure, as it
                         * may have been allocated on the stack.
                         *
                         * First get a count of the number of entries
                         */
                        char **ptr = (char **) arg;
                        while (ptr[count] != NULL)
                            count++;
                        mx->mx_labels = malloc((count + 1) * sizeof (char *));

                        count = 0;
                        while (ptr[count] != NULL) {
                            uint len = strlen(ptr[count]);
                            if (mx->mx_max_len < len)
                                mx->mx_max_len = len;
                            mx->mx_labels[count] = strdup(ptr[count]);
                            count++;
                        }
                        mx->mx_labels[count] = NULL;
                    }
                    mx->mx_count = count;
                }
                break;
            case GTCB_Scaled:  // Used on checkbox to scale imagry
            case GTMX_Scaled:  // Used on MX Radio to scale imagry
                if (mx != NULL)
                    mx->mx_scaled = arg;
                break;
            case GTMX_Spacing: // Min space between each choice
                if (mx != NULL)
                    mx->mx_spacing = arg;
                break;
            case GTST_MaxChars:
            case GTIN_MaxChars:
            case GTNM_MaxNumberLen:
                if ((si != NULL) && (arg > 0) && (arg < 256) &&
                    (si->MaxChars != arg)) {
                    si->MaxChars = arg;
                    si->DispCount = arg;
                    if (arg < 2)
                        arg = 2;  // Space for at least 2 characters and NULL
                    if ((kind != STRING_KIND) && (arg < 8))
                        arg = 8;  // Space for large number
                    si->Buffer = malloc(arg + 1);
                    if (si->Buffer == NULL) {
                        printf("Malloc %u fail\n");
                        si->MaxChars = 0;
                        break;
                    }
                    memset(si->Buffer, 0, arg + 1);
                    scan_again = 1;  // Scan again for string tag
                }
                break;
            case GTST_String:  // Pointer to string text
                if ((si != NULL) & (si->Buffer != NULL)) {
                    char *str = (char *) arg;
                    uint len = strlen(str);
                    if (len > si->MaxChars)
                        len = si->MaxChars;
                    strncpy(si->Buffer, str, len);
                    si->Buffer[len] = '\0';
                    si->NumChars = len;
                    si->DispPos = 0;
                    si->BufferPos = len;
                }
                break;
            case GTIN_Number:
            case GTNM_Number:  // Wrong tag, but support it anyway
                if (kind == NUMBER_KIND) {
                    char buf[32];
                    sprintf(buf, "%d", arg);
                    if (g.GadgetText != NULL)
                        free(g.GadgetText);
                    g.GadgetText = create_intuitext(buf);
                } else if ((si != NULL) & (si->Buffer != NULL)) {
                    sprintf(si->Buffer, "%u", arg);
                    si->NumChars = strlen(si->Buffer);
                    si->DispPos = si->NumChars;
                }
                break;
            case GA_TabCycle:
                if (arg)
                    g.Flags |= GFLG_TABCYCLE;
                else
                    g.Flags &= ~GFLG_TABCYCLE;
                break;
            case GA_Underscore:  // Underscore character
            case GT_Underscore:  // Underscore character
                break;
            case GTTX_Border:  // Border
            case GTNM_Border:  // Border
                if (arg)
                    g.Flags |= GFLG_GADGHBOX;  // not AmigaOS standard
                else
                    g.Flags &= ~GFLG_GADGHBOX;  // not AmigaOS standard
                break;
            case STRINGA_Justification:
                g.Activation |= arg;
                break;
        }
//      printf("Tag=%x,arg=%x ", tag, arg);
    }
    va_end(ap);
    if (scan_again && (scans++ < 1))
        goto creategadget_scan_again;

    if (mx != NULL) {
        /* Initialize multiple-selection sizes */
        if (mx->mx_scaled) {
            mx->mx_sel_height = g.Height;
            mx->mx_sel_width  = g.Width - mx->mx_max_len * FONT_WIDTH - 4;
        } else {
            mx->mx_sel_height = FONT_HEIGHT;
            mx->mx_sel_width  = FONT_WIDTH;
        }
        mx->mx_sel_height--;
        mx->mx_spacing++;
//      printf("Height=%u %u ", mx->mx_sel_height, g.Height);
        g.Height = (g.Height + mx->mx_spacing) * mx->mx_count;
//      printf("%u ", g.Height);
    }

    Gadget *newgad = malloc(sizeof (Gadget) + extrasize);
    memcpy(newgad, &g, sizeof (*newgad));
    if (pgad != NULL)
        pgad->NextGadget = newgad;

//  printf("CGkind=%x %x\n", gad->GadgetType, g.GadgetType);
    return (newgad);
}

static const char * const gad_kinds[] = {
    "GENERIC",
    "BUTTON",
    "CHECKBOX",
    "INTEGER",
    "LISTVIEW",
    "MX",
    "NUMBER",
    "CYCLE",
    "PALETTE",
    "SCROLLER",
    "RSVD10",
    "SLIDER",
    "STRING",
    "TEXT",
};

void
show_gadlist(Gadget *gad_list)
{
    printf("Gad list\n");
    while (gad_list != NULL) {
        uint id   = gad_list->GadgetID;
        uint kind = gad_list->GadgetType;
        printf("  ID %5d (0x%04x) kind=%04x %-8s fl=%04x x=%d y=%d w=%u h=%u\n",
               id, id, kind,
               (kind < ARRAY_SIZE(gad_kinds)) ? gad_kinds[kind] : "Unknown",
               gad_list->Flags,
               gad_list->LeftEdge, gad_list->TopEdge,
               gad_list->Width, gad_list->Height);
        gad_list = gad_list->NextGadget;
    }
}

static GadContext *gad_context_head = NULL;

struct Gadget *
CreateContext(Gadget **gad_list)
{
    struct NewGadget ng;
    Gadget *gad;
    GadContext *cgad = malloc(sizeof (GadContext));

#define CONTEXT_SIZE (sizeof (GadContext) - sizeof (Gadget))

    memset(&ng, 0, sizeof (ng));
    ng.ng_LeftEdge = -1;
    ng.ng_TopEdge = -1;
    ng.ng_GadgetID = -1;
    gad = CreateGadget(GENERIC_KIND, NULL, &ng,
                       GT_ExtraSize, CONTEXT_SIZE, TAG_DONE, NULL);
    memcpy(&cgad->gc_Gadget, gad, sizeof (*gad));
    free(gad);
    cgad->gc_next = gad_context_head;
    gad_context_head = cgad;

    *gad_list = &cgad->gc_Gadget;
    return (*gad_list);
}

uint16_t
AddGList(Window *window, Gadget *gadget, uint position, int numGad,
         Requester *requester)
{
    (void) window;
    (void) gadget;
    (void) position;
    (void) numGad;
    (void) requester;
    return (0);
}

uint16_t
RemoveGList(struct Window *remPtr, struct Gadget *gadget, LONG numGad)
{
    (void) remPtr;
    (void) gadget;
    (void) numGad;
    return (0);
}

void
DrawBevelBoxA(RastPort *rp, long left, long top,
              long width, long height, struct TagItem *taglist)
{
    (void) rp;
    (void) left;
    (void) top;
    (void) width;
    (void) height;
    (void) taglist;
}

void
DrawBevelBox(RastPort *rp, long left, long top, long width, long height,
             Tag tag1, ...)
{
    int top_pen = 2;   // white (default to raised box)
    int bot_pen = 1;   // black (default to raised box)
    uint boxtype = BBFT_BUTTON;  // Button (default)
    va_list ap;
    (void) rp;

    /*
     * GTBB_Recessed
     * Raised has white on top and left.
     * Recessed (lowered) has white on bottom and right.
     *
     * Tags:
     *     GT_VisualInfo (APTR)
     *     GTBB_Recessed (BOOL)
     */
    va_start(ap, tag1);
    while (tag1 != TAG_DONE) {
        uint arg = va_arg(ap, ULONG);
        if (tag1 == GTBB_Recessed) {
            if (arg) {
                top_pen = 1;
                bot_pen = 2;
            } else {
                top_pen = 2;
                bot_pen = 1;
            }
        } else if (tag1 == GTBB_FrameType) {
            boxtype = arg;
        }
        tag1 = va_arg(ap, ULONG);
    }

    switch (boxtype) {
        default:
        case BBFT_ICONDROPBOX:
            /* box for icon drop box imagery */
        case BBFT_BUTTON: {
            /* box for BUTTON_KIND gadgets (default) */
            int x1 = left;
            int x2 = left + width - 1;
            int y1 = top;
            int y2 = top + height - 1;
            draw_line(bot_pen, x1, y2, x2, y2);
            draw_line(bot_pen, x2, y1, x2, y2);
            draw_line(top_pen, x1, y1, x2, y1);
            draw_line(top_pen, x1, y1, x1, y2);
            boxtype = 0;
            break;
        }
        case BBFT_RIDGE: {
            /* box for STRING_KIND and INTEGER_KIND gadgets */
            int x1 = left;
            int x2 = left + width - 1;
            int y1 = top + 1;
            int y2 = top + height;
            if (y1 - y2 > FONT_HEIGHT + 1) {
                draw_line(bot_pen, x1, y2 + 1, x2, y2 + 1);
                draw_line(bot_pen, x2, y1 - 1, x2, y2 + 1);
                draw_line(top_pen, x1, y2, x2 - 1, y2);
                draw_line(top_pen, x2 - 1, y1 - 1, x2 - 1, y2);
            } else {
                draw_line(bot_pen, x1, y2, x2 - 1, y2);
                draw_line(bot_pen, x2 - 1, y1 - 1, x2 - 1, y2);
            }
// printf("[%u]", y2 - y1);
            if (y1 - y2 > FONT_HEIGHT + 1) {
                draw_line(bot_pen, x1 + 1, y1, x2 - 1, y1 + 1);
                draw_line(bot_pen, x1 + 1, y1, x1 + 1, y2 + 1);
            }
            draw_line(top_pen, x1, y1 - 1, x2, y1 - 1);
            draw_line(top_pen, x1, y1 - 1, x1, y2 + 1);
            break;
        }
    }
}

static void
gadget_draw_bounding_box(Gadget *gad, uint boxtype, uint is_recessed)
{
    struct RastPort *rp = &sscreen.RastPort;
    uint x = gad->LeftEdge;
    uint y = gad->TopEdge;
    uint h = gad->Height;

    DrawBevelBox(rp, x, y, gad->Width, h,
                 GTBB_FrameType, boxtype, GTBB_Recessed, is_recessed, TAG_DONE);
}

// "KickSmash ROM switcher" top box has black on top/left and white on bot/right
//      GTBB_Recessed
// Save/Cancel/Reboot buttons have white on top/left and black on bot/right
// Board name box has two layers of different colors at each border.
//      White is always left/top
//      Black is always right/bottom
// Table exterior box has two layers, maybe done manually
//      Outside has black left/top

static void
gadget_draw_button(Gadget *gad, uint activated)
{
    uint x = gad->LeftEdge;
    uint y = gad->TopEdge;
    struct IntuiText *it = gad->GadgetText;
    uint textlen_max = gad->Width / FONT_WIDTH;
    uint width;
    uint xoff;
    uint yoff = (gad->Height - FONT_HEIGHT) / 2;  // Vert. center text in button
    uint act = gad->Activation;

    if ((act & (GACT_STRINGLEFT | GACT_STRINGCENTER | GACT_STRINGRIGHT)) == 0)
        act |= GACT_STRINGCENTER;  // Center button by default

    if (it != NULL) {
        uint fg_pen = 1;
        uint bg_pen = it->BackPen;
        uint len = strlen(it->IText);
        const char *underscore = strchr(it->IText, '_');
        if (underscore != NULL)
            len--;

        if (activated) {
            bg_pen = 3;  // Blue
        }

        if (len > textlen_max)
            len = textlen_max;
        width = len * 8;
        if (act & GACT_STRINGLEFT) {
            xoff = it->LeftEdge;
        } else if (act & GACT_STRINGRIGHT) {
            xoff = gad->Width - width + it->LeftEdge;
        } else {  // GACT_STRINGCENTER
            xoff = (gad->Width - width) / 2;  // Default: center text in button
        }

        // XXX: Where is GT_Underscore saved?

        /* First fill gadget with bg color */
        fill_rect(bg_pen, x, y, x + gad->Width, y + gad->Height - 1);

        if (underscore != NULL) {
            uint us_len = underscore - it->IText;
            uint us_y;
            uint us_x;
            if (us_len != 0) {
                render_text_at(it->IText, us_len,
                               x + xoff, y + yoff + it->TopEdge,
                               fg_pen, bg_pen);
            }
            render_text_at(it->IText + us_len + 1, textlen_max - us_len,
                           x + xoff + FONT_WIDTH * us_len,
                           y + yoff + it->TopEdge, fg_pen, bg_pen);

            us_x = x + xoff + FONT_WIDTH * us_len;
            us_y = y + yoff + it->TopEdge + FONT_HEIGHT - 1;
            if (gad->Height >= FONT_HEIGHT + 5)
                us_y++;  // More room available for the underscore

            /* Black underline */
            draw_line(1, us_x, us_y, us_x + FONT_WIDTH, us_y);
        } else {
            /* No underscore present */
            render_text_at(it->IText, textlen_max,
                           x + xoff, y + yoff + it->TopEdge,
                           fg_pen, bg_pen);
        }
    }
    if (gad->Flags & GFLG_DISABLED) {
        gray_rect(6, x, y, x + gad->Width, y + gad->Height - 2);
    }

    gadget_draw_bounding_box(gad, BBFT_BUTTON, activated);
}

static void
gadget_update_mx(Gadget *gad)
{
    uint cur;
    uint ydist;
    uint w;
    uint h;
    uint x;
    uint y = gad->TopEdge;
    MxInfo *mx = gad->SpecialInfo;

    if (mx == NULL)
        return;

    w = mx->mx_sel_width;
    h = mx->mx_sel_height;
    if (mx->mx_scaled) {
        x = gad->LeftEdge + 2;
    } else {
        x = gad->LeftEdge + (gad->Width - w) / 2;
    }
    ydist = h + mx->mx_spacing;
    if ((mx->mx_max_len > 0) && (ydist < FONT_HEIGHT))
        ydist = FONT_HEIGHT;
    for (cur = 0; cur < mx->mx_count; cur++) {
        uint fill_pen = (mx->mx_seldisplay == cur) ? 3 : 0;
        draw_rect(1, x, y, x + w, y + h);
        fill_rect(fill_pen, x + 1, y + 1, x + w - 1, y + h - 1);
        y += ydist;
    }

    /*
     * When CreateGadget is called:
     *     Gadget Height is height of individual selection
     *     Gadget Width sets the selection width. Not just the middle gets
     *     wider.
     * For MX gadgets, the Height gets changed by CreateGadget() to be the
     *     height of the entire gadget.
     * The mx structure is created by CreateGadget()
     *     mx->scaled says if selection width should be scaled to match Width
     *     mx->scaled says if selection height should be scaled to match Height
     *     If mx->scaled is not set, the selection is 8x8 (font size)
     */
}

static void
gadget_update_mx_mouse(Gadget *gad)
{
    MxInfo *mx = gad->SpecialInfo;
    if (mx != NULL) {
        uint sel_height = mx->mx_sel_height + mx->mx_spacing;
        uint yoff = mouse_y - gad->TopEdge;
        uint newsel = yoff / sel_height;
// printf("sh=%u,yo=%u,nsel=%u", sel_height, yoff, newsel);
        if (mx->mx_seldisplay != newsel) {
            mx->mx_seldisplay = newsel;
            gadget_update_mx(gad);  // update selection
        }
    }
}

static void
gadget_draw_mx(Gadget *gad)
{
    MxInfo *mx = gad->SpecialInfo;
    struct IntuiText *it = gad->GadgetText;

    if (it != NULL) {
        uint fg_pen = 1;
        uint bg_pen = it->BackPen;
        uint len = strlen(it->IText);
        uint x = gad->LeftEdge;
        uint y = gad->TopEdge - FONT_HEIGHT;

        render_text_at(it->IText, len, x, y, fg_pen, bg_pen);
    }
    gadget_update_mx(gad);

    if (mx != NULL) {
        uint x     = gad->LeftEdge + mx->mx_sel_width + 4;
        uint y     = gad->TopEdge + mx->mx_spacing;
        uint h     = mx->mx_sel_height;
        uint ydist = h + mx->mx_spacing;
        uint cur;
        if ((mx->mx_max_len > 0) && (ydist < FONT_HEIGHT))
            ydist = FONT_HEIGHT;
        for (cur = 0; cur < mx->mx_count; cur++) {
            char *ptr = mx->mx_labels[cur];
            uint  len = strlen(ptr);
            if (len != 0)
                render_text_at(ptr, strlen(ptr), x, y, 1, 0);
            y += ydist;
        }
    }
}

#define GADGET_STRING_UPDATE_ALL             0
#define GADGET_STRING_UPDATE_RIGHT_OF_CURSOR 1

/*
 * gadget_update_string_mouse() updates the cursor position of a string
 * based on the mouse current position.
 */
static void
gadget_update_string_mouse(Gadget *gad)
{
    StringInfo *si = gad->SpecialInfo;

    cursor_x = (mouse_x - cursor_x_start) / FONT_WIDTH;
    if (si != NULL) {
        uint len = strlen(si->Buffer);
        if (cursor_x > len)
            cursor_x = len;
        si->BufferPos = cursor_x;
    }
}

uint
gadget_string_calc_y(Gadget *gad)
{
    uint y = gad->TopEdge + 1;
    if (gad->Height > FONT_HEIGHT + 3)
        y = gad->TopEdge + (gad->Height - FONT_HEIGHT) / 2;
    return (y);
}

static void
gadget_update_string(Gadget *gad, uint update_type)
{
    StringInfo *si = gad->SpecialInfo;
    (void) update_type;

    if (si != NULL) {
        uint len = strlen(si->Buffer);
        uint max = si->MaxChars;
        uint x = gad->LeftEdge + si->CLeft;
        uint y = gadget_string_calc_y(gad);
        uint padlen = max - len;
        if (padlen < 100)
            memset(si->Buffer + len, ' ', max - len);
        /* Render text of string to be edited and set cursor position */
        render_text_at(si->Buffer, max, x, y, 1, 0);
        si->Buffer[len] = '\0';
        if (gad == active_gadget)
            cursor_x = si->BufferPos;
    }
}

static void
gadget_draw_string(Gadget *gad)
{
    uint x = gad->LeftEdge;
    uint y = gad->TopEdge;
    struct IntuiText *it = gad->GadgetText;

    /*
     * The string box title should appear to the left of the string,
     * but only if not disabled.
     */
    for (it = gad->GadgetText; it != NULL; it = it->NextText) {
        uint rstart = 0;
        int  slen = strlen(it->IText);
        int  rpos = x - slen * FONT_WIDTH - 6;
        if (rpos < 0) {
            int rlen = x / FONT_WIDTH - 1;
            if (rlen <= 0)
                continue;
            rstart = slen - rlen;
            rpos = x - rlen * FONT_WIDTH - 6;
        }
        // XXX: Maybe this clipping intelligence should be built into
        //      render_text_at() so it can always trim to the screen borders.
        render_text_at(it->IText + rstart, 0, rpos, y + it->TopEdge,
                       it->FrontPen, it->BackPen);
    }
    gadget_update_string(gad, GADGET_STRING_UPDATE_ALL);

    /* Default is to draw border, turn off using GTTX_BORDER tag */
    if (gad->Flags & GFLG_GADGHBOX)
        gadget_draw_bounding_box(gad, BBFT_RIDGE, FALSE);
}

static void
gadget_draw_text(Gadget *gad)
{
    uint x = gad->LeftEdge;
    uint y = gad->TopEdge;
    struct IntuiText *it = gad->GadgetText;
    uint textlen_max = gad->Width / FONT_WIDTH;

    while (it != NULL) {
//      printf("rta %u %u %x %x\n",
//             x + it->LeftEdge, y + it->TopEdge, it->FrontPen, it->BackPen);
        render_text_at(it->IText, textlen_max,
                       x + it->LeftEdge, y + it->TopEdge,
                       it->FrontPen, it->BackPen);
        it = it->NextText;
    }

    if (gad->Flags & GFLG_GADGHBOX)
        gadget_draw_bounding_box(gad, BBFT_RIDGE, FALSE);
}

static void
gadget_notify(Gadget *gad, uint class, uint code, uint qual)
{
    IntuiMessage imsg;
    uint64_t usec = timer_tick_to_usec(timer_tick_get());
    memset(&imsg, 0, sizeof (imsg));
    imsg.Class = class;
    imsg.Code = code;
    imsg.Qualifier = qual;
    imsg.IAddress = gad;
    imsg.MouseX = mouse_x;
    imsg.MouseY = mouse_y;
    imsg.Seconds = usec / 1000000;
    imsg.Micros = usec % 1000000;
    GT_PutIMsg(&imsg);
}

static void
gadget_deactivate(Gadget *gad, uint code, uint qual)
{
    gad->Activation &= ~GACT_ACTIVEGADGET;
//  gadget_notify(gad, IDCMP_GADGETUP, code, qual);
    switch (gad->GadgetType) {
        case STRING_KIND:
        case INTEGER_KIND:
            cursor_visible = 0;
            break;
    }
    active_gadget = NULL;
}

static void
gadget_activate(Gadget *gad)
{
    if ((active_gadget != NULL) && (active_gadget != gad))
        gadget_deactivate(active_gadget, 0, 0);

    if (gad != NULL) {
        switch (gad->GadgetType) {
            case STRING_KIND:
            case INTEGER_KIND: {
                StringInfo *si = gad->SpecialInfo;
                cursor_x_start = gad->LeftEdge + ((si != NULL) ? si->CLeft : 0);
                cursor_y_start = gadget_string_calc_y(gad);
                cursor_x = (si != NULL) ? si->BufferPos : 0;
                cursor_y = 0;
                cursor_visible = 1;
                break;
            }
        }
        gad->Activation |= GACT_ACTIVEGADGET;
    }
    active_gadget = gad;
}

int
ActivateGadget(Gadget *gadget, Window *window, Requester *requester)
{
    (void) window;
    (void) requester;
    gadget_activate(gadget);
    return (0);
}

/*
 * RefreshGList
 * ------------
 * Draw or re-draw imagery for all gadgets in list
 */
void
RefreshGList(Gadget *gadgets, Window *window, Requester *requester, uint numGad)
{
    Gadget *gad = gadgets;
    (void) gadgets;
    (void) window;
    (void) requester;
    (void) numGad;
    while (numGad-- > 0) {
        if (gad == NULL)
            break;
        switch (gad->GadgetType) {
            case GENERIC_KIND:
                break;
            case BUTTON_KIND:
                gadget_draw_button(gad, 0);
                break;
            case CHECKBOX_KIND:
                break;
            case LISTVIEW_KIND:
                break;
            case MX_KIND:
                gadget_draw_mx(gad);
                break;
            case CYCLE_KIND:
                break;
            case PALETTE_KIND:
                break;
            case SCROLLER_KIND:
                break;
            case SLIDER_KIND:
                break;
            case STRING_KIND:
            case INTEGER_KIND:
                gadget_draw_string(gad);
                break;
            case TEXT_KIND:
            case NUMBER_KIND:
                gadget_draw_text(gad);
                break;
        }
        gad = gad->NextGadget;
    }
}

void
GT_RefreshWindow(Window *win, Requester *req)
{
    (void) win;
    (void) req;
}

uint                 imsg_count = 0;
static IntuiMessage *imsg_head  = NULL;  // Active messages
static IntuiMessage *imsg_tail  = NULL;  // Tail of active messages
static IntuiMessage *imsg_pool  = NULL;  // Available message buffers

/*
 * imsg_alloc() implements a small pool of spare message buffers
 * for quick allocation
 */
static IntuiMessage *
imsg_alloc(void)
{
    if (imsg_pool != NULL) {
        IntuiMessage *imsg = imsg_pool;
        imsg_pool = imsg_pool->SpecialLink;
        return (imsg);
    }

    /* No buffers in pool available */
    return (malloc(sizeof (IntuiMessage)));
}

static void
imsg_free(IntuiMessage *imsg)
{
    imsg->SpecialLink = imsg_pool;
    imsg_pool = imsg;
}

static void gadget_poll(void);

struct Message *
WaitPort(struct MsgPort *port)
{
    static uint64_t next_intuitick;
    (void) port;
    while (imsg_head == NULL) {
        gadget_poll();
        if ((imsg_head == NULL) && timer_tick_has_elapsed(next_intuitick)) {
            /* Send tick 10 times a second */
            next_intuitick = timer_tick_plus_msec(100);
            uint64_t usec = timer_tick_to_usec(timer_tick_get());
            IntuiMessage imsg;
            memset(&imsg, 0, sizeof (imsg));
            imsg.Class     = IDCMP_INTUITICKS;
            imsg.Code      = 0;
            imsg.Qualifier = 0;
            imsg.IAddress  = active_gadget;
            imsg.MouseX    = mouse_x;
            imsg.MouseY    = mouse_y;
            imsg.Seconds   = usec / 1000000;
            imsg.Micros    = usec % 1000000;
            GT_PutIMsg(&imsg);
            if (displaybeep)
                screen_beep_handle();
            break;
        }
    }

    return ((struct Message *) imsg_head);
}

IntuiMessage *
GT_GetIMsg(struct MsgPort *port)
{
    IntuiMessage *imsg = imsg_head;
    (void) port;
    gadget_poll();

    if (imsg != NULL) {
        imsg_count--;
        imsg_head = imsg_head->SpecialLink;
        if (imsg_head == NULL)
            imsg_tail = NULL;
//      printf("Iget");
    }
    return (imsg);
}

void
GT_ReplyIMsg(IntuiMessage *imsg)
{
    imsg_free(imsg);
}

/*
 * Not implemented yet:
 * IDCMP_MOUSEMOVE
 */


/*
 * GT_PutIMsg() is an internal gadget function used to send event
 *               messages back to the application.
 */
static void
GT_PutIMsg(IntuiMessage *imsg)
{
    IntuiMessage *new_imsg;

    if (imsg_count > 30)  // Limit queue length
        return;
    imsg_count++;

    new_imsg = imsg_alloc();
    if (new_imsg == NULL)
        return;

    memcpy(new_imsg, imsg, sizeof (*imsg));
    new_imsg->SpecialLink = NULL;
    if (imsg_tail == NULL) {
        /* No tail, thus no head */
        imsg_head = new_imsg;
    } else {
        imsg_tail->SpecialLink = new_imsg;
    }
    imsg_tail = new_imsg;
}

void
FreeGadgets(struct Gadget *gad)
{
    (void) gad;
}

/* ASCII input and output keystrokes */
#define KEY_CTRL_A           0x01  /* ^A Line begin */
#define KEY_CTRL_B           0x02  /* ^B Cursor left */
#define KEY_CTRL_C           0x03  /* ^C Abort */
#define KEY_CTRL_D           0x04  /* ^D Delete char to the right */
#define KEY_CTRL_E           0x05  /* ^E Line end */
#define KEY_CTRL_F           0x06  /* ^F Cursor right */
#define KEY_CTRL_G           0x07  /* ^G Keyboard beep / bell */
#define KEY_CTRL_H           0x08  /* ^H Terminal backspace character */
#define KEY_CTRL_I           0x09  /* ^I Tab */
#define KEY_CTRL_J           0x0a  /* ^J Newline */
#define KEY_CTRL_K           0x0b  /* ^K Erase to end of line */
#define KEY_CTRL_L           0x0c  /* ^L Redraw line */
#define KEY_CTRL_M           0x0d  /* ^M Carriage Return */
#define KEY_CTRL_N           0x0e  /* ^N Cursor down */
#define KEY_CTRL_O           0x0f  /* ^O Shift Tab */
#define KEY_CTRL_P           0x10  /* ^P Cursor up */
#define KEY_CTRL_R           0x12  /* ^R Redraw line */
#define KEY_CTRL_U           0x15  /* ^U Erase to start of line */
#define KEY_CTRL_V           0x16  /* ^V Take next input as literal */
#define KEY_CTRL_W           0x17  /* ^W Erase word */
#define KEY_CTRL_X           0x18  /* ^X Erase entire line */
#define KEY_CTRL_Y           0x19  /* ^Y Show history */
#define KEY_ESC              0x1b  /* Escape key */
#define KEY_SPACE            0x20  /* Space key */
#define KEY_DELETE           0x7f  /* ^? Backspace on some keyboards */
#define KEY_AMIGA_ESC        0x9b  /* Amiga key sequence */

#define KEY_LINE_BEGIN       KEY_CTRL_A
#define KEY_CURSOR_LEFT      KEY_CTRL_B
#define KEY_DEL_CHAR         KEY_CTRL_D
#define KEY_LINE_END         KEY_CTRL_E
#define KEY_CURSOR_RIGHT     KEY_CTRL_F
#define KEY_BACKSPACE        KEY_CTRL_H
#define KEY_TAB              KEY_CTRL_I
#define KEY_NL               KEY_CTRL_J
#define KEY_CLEAR_TO_END     KEY_CTRL_K
#define KEY_REDRAW1          KEY_CTRL_L
#define KEY_CR               KEY_CTRL_M
#define KEY_CURSOR_DOWN      KEY_CTRL_N
#define KEY_CURSOR_UP        KEY_CTRL_P
#define KEY_REDRAW2          KEY_CTRL_R
#define KEY_SHIFT_TAB        KEY_CTRL_O
#define KEY_CLEAR_TO_START   KEY_CTRL_U
#define KEY_DEL_WORD         KEY_CTRL_W
#define KEY_CLEAR            KEY_CTRL_X
#define KEY_HISTORY          KEY_CTRL_Y

static void
gadget_tabcycle_next(Gadget *gad, int direction)
{
    uint count = 0;
    Gadget *cgad;
    Gadget *gadhead;
    if (gad_context_head == NULL)
        return;
    gadhead = gad_context_head->gc_Gadget.NextGadget;
    if (gadhead == NULL)
        return;

    if (direction > 0) {
        /* Find next Gadget in tabcycle list */
        for (cgad = gad->NextGadget; cgad != gad; cgad = cgad->NextGadget) {
            if (cgad == NULL) {
                cgad = gadhead;
                if (cgad == gad)
                    break;
            }
            if (cgad->Flags & GFLG_TABCYCLE) {
                gadget_deactivate(gad, KEY_TAB, 0x8000);  // Tab
                gadget_activate(cgad);
                return;
            }
            if (count++ > 100) {
                printf("Bug1\n");
                break;
            }
        }
    } else {
        /* Find previous Gadget in tabcycle list */
        Gadget *prevgad = NULL;
        for (cgad = gad->NextGadget; cgad != gad; cgad = cgad->NextGadget) {
            if (cgad == NULL) {
                cgad = gadhead;
                if (cgad == gad)
                    break;
            }
            if (cgad->Flags & GFLG_TABCYCLE)
                prevgad = cgad;
            if (count++ > 100) {
                printf("Bug2\n");
                break;
            }
        }
        if (prevgad != NULL) {
            gadget_deactivate(gad, KEY_TAB, 0x8001);  // Shift-Tab
            gadget_activate(prevgad);
        }
    }
}

/* Input ESC key modes */
typedef enum {
    INPUT_MODE_NORMAL,  /* Normal user input */
    INPUT_MODE_ESC,     /* ESC key pressed */
    INPUT_MODE_BRACKET, /* ESC [ pressed */
    INPUT_MODE_1,       /* ESC [ 1 pressed (HOME key sequence) */
    INPUT_MODE_2,       /* ESC [ 2 pressed (INSERT key sequence) */
    INPUT_MODE_3,       /* ESC [ 3 pressed (DEL key sequence) */
    INPUT_MODE_1SEMI,   /* ESC [ 1 ; pressed (ctrl-cursor key) */
    INPUT_MODE_1SEMI2,  /* ESC [ 1 ; 2 pressed (shift-cursor key) */
    INPUT_MODE_1SEMI3,  /* ESC [ 1 ; 3 pressed (alt-cursor key) */
    INPUT_MODE_1SEMI5,  /* ESC [ 1 ; 5 pressed (ctrl-cursor key) */
    INPUT_MODE_LITERAL, /* Control-V pressed (next input is literal) */
} input_mode_t;

static void
gadget_string_edit(Gadget *gad, uint qual, uint ch)
{
    static input_mode_t input_mode = INPUT_MODE_NORMAL;
    StringInfo *si = gad->SpecialInfo;
    char       *input_buf;
    uint        input_pos;
    uint        len;
    uint        tmp;

    if (si == NULL)
        return;  // XXX: No StringInfo?
    input_buf = si->Buffer;
    input_pos = si->BufferPos;

    (void) qual;

    switch (input_mode) {
        default:
        case INPUT_MODE_NORMAL:
            break;
        case INPUT_MODE_ESC:
            if ((ch == '[') || (ch == 'O')) {
                input_mode = INPUT_MODE_BRACKET;
            } else {
                /* Unrecognized ESC sequence -- swallow both */
                input_mode = INPUT_MODE_NORMAL;
            }
            return;
        case INPUT_MODE_BRACKET:
            input_mode = INPUT_MODE_NORMAL;
            switch (ch) {
                case 'A':
                    ch = KEY_CURSOR_UP;
                    break;
                case 'B':
                    ch = KEY_CURSOR_DOWN;
                    break;
                case 'C':
                    ch = KEY_CURSOR_RIGHT;
                    break;
                case 'D':
                    ch = KEY_CURSOR_LEFT;
                    break;
                case 'F':
                    ch = KEY_LINE_END;
                    break;
                case 'H':
                    ch = KEY_LINE_BEGIN;
                    break;
                case 'M':
                    ch = KEY_CR;  /* Enter on numeric keypad */
                    break;
                case '1':
                    input_mode = INPUT_MODE_1;
                    return;
                case '2':
                    input_mode = INPUT_MODE_2;
                    return;
                case '3':
                    input_mode = INPUT_MODE_3;
                    return;
                default:
                    printf("\nUnknown 'ESC [ %c'\n", ch);
                    input_mode = INPUT_MODE_NORMAL;
                    goto redraw_prompt;
            }
            break;

        case INPUT_MODE_1:
            input_mode = INPUT_MODE_NORMAL;
            switch (ch) {
                case ';':
                    input_mode = INPUT_MODE_1SEMI;
                    return;
                case '~':
                    ch = KEY_LINE_BEGIN;
                    break;
                default:
                    printf("\nUnknown 'ESC [ 1 %c'\n", ch);
                    goto redraw_prompt;
            }
            break;

        case INPUT_MODE_1SEMI:
            switch (ch) {
                case '2':
                    input_mode = INPUT_MODE_1SEMI2;
                    break;
                case '3':
                    input_mode = INPUT_MODE_1SEMI3;
                    break;
                case '5':
                    input_mode = INPUT_MODE_1SEMI5;
                    break;
                default:
                    input_mode = INPUT_MODE_NORMAL;
                    printf("\nUnknown 'ESC [ 1 ; %c'\n", ch);
                    goto redraw_prompt;
            }
            return;

        case INPUT_MODE_1SEMI2:
        case INPUT_MODE_1SEMI3:
        case INPUT_MODE_1SEMI5:
            input_mode = INPUT_MODE_NORMAL;
            switch (ch) {
                case 'C':
                    ch = KEY_LINE_END;
                    break;
                case 'D':
                    ch = KEY_LINE_BEGIN;
                    break;
                default:
                    printf("\nUnknown 'ESC [ 1 ; 2|3|5 %c'\n", ch);
                    goto redraw_prompt;
            }
            break;

        case INPUT_MODE_2:
            if (ch != '~') {
                printf("\nUnknown 'ESC [ 1 %c'\n", ch);
                goto redraw_prompt;
            }
            /* Insert key */
            input_mode = INPUT_MODE_NORMAL;
            return;

        case INPUT_MODE_3:
            input_mode = INPUT_MODE_NORMAL;
            if (ch != '~') {
                printf("\nUnknown 'ESC [ 3 %c'\n", ch);
                goto redraw_prompt;
            }
            ch = KEY_DEL_CHAR;
            break;
        case INPUT_MODE_LITERAL:
            input_mode = INPUT_MODE_NORMAL;
            goto literal_input;
    }

    switch (ch & 0xff) {
        case KEY_REDRAW1:
        case KEY_REDRAW2:
            /* ^L or ^R redraws line */
redraw_prompt:
            gadget_draw_string(gad);
            break;
        case KEY_CR:
        case KEY_NL:
            /*
             * CR ends string gadget input and deactivates clicked gadget
             * I think it also notifies the application.
             */
            gadget_deactivate(gad, 0, 0);
            break;
        case KEY_BACKSPACE:
//      case KEY_BACKSPACE2:
            /* ^H deletes one character to the left */
            if (input_pos == 0)
                break;
            memmove(input_buf + input_pos - 1, input_buf + input_pos,
                    si->MaxChars + 1 - input_pos);
            input_pos--;
            gadget_update_string(gad, GADGET_STRING_UPDATE_ALL);
            break;
        case KEY_DELETE:
        case KEY_DEL_CHAR:
            if (input_buf[input_pos] == '\0')
                break;  /* Nothing more to delete at end of line */
            memmove(input_buf + input_pos, input_buf + input_pos + 1,
                    si->MaxChars - input_pos);
            gadget_update_string(gad, GADGET_STRING_UPDATE_ALL);
            break;
        case KEY_LINE_BEGIN:
            /* Go to the beginning of the input line (^A) */
            input_pos = 0;
            break;
        case KEY_LINE_END:
            /* Go to the end of the input line (^E) */
            input_pos += strlen(input_buf + input_pos);
            gadget_update_string(gad, GADGET_STRING_UPDATE_ALL);
            break;
        case KEY_CURSOR_LEFT:
            /* Move the cursor one position to the left (^B) */
            if (input_pos == 0)
                break;
            input_pos--;
            break;
        case KEY_CURSOR_RIGHT:
            /* Move the cursor one position to the right (^F) */
            if (input_pos >= si->MaxChars)
                break;
            if (input_buf[input_pos] == '\0')
                break;
            input_pos++;
            break;
        case KEY_CTRL_V:
            input_mode = INPUT_MODE_LITERAL;
            break;
        case KEY_ESC:
            /* ESC initiates an Escape sequence */
            input_mode = INPUT_MODE_ESC;
            break;
        case KEY_AMIGA_ESC:
            /* Amiga ESC initiates an Escape [ sequence */
            input_mode = INPUT_MODE_BRACKET;
            break;
        case KEY_CLEAR_TO_START:
            /* Delete all text to the left of the cursor */
            len = strlen(input_buf + input_pos);
            memmove(input_buf, input_buf + input_pos, len + 1);
            input_pos = 0;
            gadget_update_string(gad, GADGET_STRING_UPDATE_ALL);
            break;
        case KEY_CLEAR_TO_END:
            /* Delete all text from the cursor to end of line */
            input_buf[input_pos] = 0;
            gadget_update_string(gad, GADGET_STRING_UPDATE_ALL);
            break;
        case KEY_CLEAR:
            /* Delete all text */
            input_buf[0] = '\0';
            input_pos = 0;
            gadget_update_string(gad, GADGET_STRING_UPDATE_ALL);
            break;
        case KEY_DEL_WORD:
            /* Delete word */
            if (input_pos == 0)
                break;
            /* Skip whitespace */
            for (tmp = input_pos; tmp > 0; tmp--)
                if ((input_buf[tmp - 1] != KEY_SPACE) &&
                    (input_buf[tmp - 1] != KEY_TAB))
                    break;
            /* Find the start of the word */
            for (; tmp > 0; tmp--)
                if ((input_buf[tmp - 1] == KEY_SPACE) ||
                    (input_buf[tmp - 1] == KEY_TAB))
                    break;
            len = strlen(input_buf + input_pos);
            memmove(input_buf + tmp, input_buf + input_pos, len + 1);
            input_pos = tmp;
            gadget_update_string(gad, GADGET_STRING_UPDATE_ALL);
            break;
        case KEY_TAB:
            gadget_tabcycle_next(gad, 1);
            break;
//  (msg->Qualifier = 0x8001) for shift-tab
//  (msg->Qualifier = 0x8000) for tab
//  (msg->Code = 09) for tab
        case KEY_SHIFT_TAB:
            gadget_tabcycle_next(gad, -1);
            break;
        default:
            /* Regular input is inserted at current cursor position */
            if (((uint8_t) ch < 0x20) || ((uint8_t) ch >= 0x80))
                break;
literal_input:
            len = strlen(input_buf + input_pos) + 1;
            if (len + input_pos > si->MaxChars) {
                screen_displaybeep();
                break;  /* End of input buffer */
            }

            /* Push input following the cursor to the right */
            memmove(input_buf + input_pos + 1, input_buf + input_pos, len);
            input_buf[input_pos] = (uint8_t) ch;
            input_pos++;
            gadget_update_string(gad, GADGET_STRING_UPDATE_ALL);
            break;
    }
    si->BufferPos = input_pos;
    if (gad == active_gadget)
        cursor_x = input_pos;
    gadget_update_string(gad, GADGET_STRING_UPDATE_ALL);
}

static uint
gadget_keyboard_update_qual(uint8_t scancode, uint qual)
{
    uint mask = 0;
    switch (scancode & 0x7f) {
        case 0x60:  // Left shift
            mask = IEQUALIFIER_LSHIFT;
            break;
        case 0x61:  // Right shift
            mask = IEQUALIFIER_RSHIFT;
            break;
        case 0x62:  // Caps lock
            mask = IEQUALIFIER_CAPSLOCK;
            break;
        case 0x63:  // Control
            mask = IEQUALIFIER_CONTROL;
            break;
        case 0x64:  // Left Alt
            mask = IEQUALIFIER_LALT;
            break;
        case 0x65:  // Right Alt
            mask = IEQUALIFIER_RALT;
            break;
        case 0x66:  // Left Amiga
            mask = IEQUALIFIER_LCOMMAND;
            break;
        case 0x67:  // Right Amiga
            mask = IEQUALIFIER_RCOMMAND;
            break;
    }
    if (scancode & 0x80)
        qual &= ~mask;
    else
        qual |= mask;
    return (qual);
}

static void
gadget_handle_keyboard_input(int ch)
{
    static IntuiMessage imsg;
    static uint16_t qual;

    /* Update input qualifiers (Shift, Alt, Amiga keys, Control) */
    qual = gadget_keyboard_update_qual(ch >> 8, qual);

    /*
     * This would be where to handle some keys outside the application.
     * For example, the help key.
     */
#if 0
    if ((ch >> 8) == 0x5f) {  // Help key
        printf("Help");
        return;
    }
#endif

    if ((active_gadget != NULL) &&
        ((active_gadget->GadgetType == STRING_KIND) ||
         (active_gadget->GadgetType == INTEGER_KIND))) {
        /* String gadget implements editor */
        gadget_string_edit(active_gadget, qual, ch);
        return;
    }

    uint64_t usec = timer_tick_to_usec(timer_tick_get());
    imsg.Class     = IDCMP_RAWKEY;
    imsg.Code      = ch >> 8;
    imsg.Qualifier = qual;
    imsg.IAddress  = active_gadget;
    imsg.MouseX    = mouse_x;
    imsg.MouseY    = mouse_y;
    imsg.Seconds   = usec / 1000000;
    imsg.Micros    = usec % 1000000;
//  printf("%x:%x ", imsg.Qualifier, ch);
    GT_PutIMsg(&imsg);
}

static void
gadget_poll(void)
{
    main_poll();
    if (gui_wants_all_input) {
        int ch = input_rb_get();
        if (ch > 0) {
            /* Got keystroke */
            gadget_handle_keyboard_input(ch);
        }
    }
}

#define HOVER_AWAY    0  // Hover away from gadget while mouse button held
#define HOVER_ONTO    1  // Hover back onto gadget while mouse button held
#define HOVER_OVER    2  // Hover over gadget while mouse button held
#define HOVER_CLICK   3  // Mouse button clicked on gadget
#define HOVER_RELEASE 4  // Mouse button released on gadget previously clicked
#define HOVER_OFF     5  // Mouse button released while off gadget

static void
gadget_handle_click_hover(Gadget *gad, Gadget *oldgad, uint hover_type)
{
    int x = mouse_x;
    int y = mouse_y;
    static IntuiMessage imsg;
    imsg.Class = 0;  // Default to not sent

    (void) oldgad;
#if 0
    /* XXX: I don't know if this is necessary anymore */
    if ((oldgad != NULL) && (oldgad != gad) && (activegadget == oldgad))
        gadget_deactivate(oldgad, 0, 0);  // Deactivate previous gadget
#endif

    switch (hover_type) {
        case HOVER_AWAY:  // Hovered away from gadget while mouse button held
            switch (gad->GadgetType) {
                case BUTTON_KIND:
                    gadget_draw_button(gad, 0);
                    break;
                case MX_KIND: {
                    MxInfo *mx = gad->SpecialInfo;
                    if (mx != NULL)
                        mx->mx_seldisplay = mx->mx_selected;
                    gadget_update_mx(gad);  // update selection
                    break;
                }
            }
            break;
        case HOVER_ONTO:  // Hover back onto gadget while mouse button held
            switch (gad->GadgetType) {
                case BUTTON_KIND:
                    gadget_draw_button(gad, 1);
                    break;
                case STRING_KIND:
                case INTEGER_KIND:
                    gadget_update_string_mouse(gad);  // update cursor_x
                    break;
            }
            break;
        case HOVER_OVER:   // Hover over gadget while button is held pressed
            switch (gad->GadgetType) {
                case MX_KIND:
                    gadget_update_mx_mouse(gad);
                    break;
                case STRING_KIND:
                case INTEGER_KIND:
                    gadget_update_string_mouse(gad);  // update cursor_x
                    break;
            }
            break;
        case HOVER_CLICK:  // Mouse button clicked on gadget
            switch (gad->GadgetType) {
                case BUTTON_KIND:
                    gadget_draw_button(gad, 1);
                    break;
                case MX_KIND:
                    gadget_update_mx_mouse(gad);
                    break;
                case STRING_KIND:
                case INTEGER_KIND: {
                    gadget_activate(gad);
                    gui_wants_all_input = 1;
                    gadget_update_string_mouse(gad);  // update cursor_x
                    break;
                }
            }
            gadget_activate(gad);
            if (gad->Activation & GACT_IMMEDIATE)
                imsg.Class = IDCMP_GADGETDOWN;  // Not always sent
            break;
        case HOVER_RELEASE:  // Mouse button released on gadget prev. clicked
            switch (gad->GadgetType) {
                case BUTTON_KIND:
                    gadget_draw_button(gad, 0);
                    break;
                case STRING_KIND:
                case INTEGER_KIND:
                    // XXX: update cursor X position
                    //      If click in middle of string, move cursor to
                    //      that spot. Otherwise, move cursor to end of string
                    //
                    //      I think that the hover handling code above deals
                    //      with this, so no need tp dp anything here.
                    break;
                case MX_KIND: {  // Radio
                    /* Notify application */
                    MxInfo *mx = gad->SpecialInfo;
                    if (mx != NULL)
                        mx->mx_selected = mx->mx_seldisplay;

                    /* Now tell the application there was a gadgetdown */
                    gadget_notify(gad, IDCMP_GADGETDOWN, 0, 0);
                    break;
                }
            }
            // XXX: Should only send if GACT_RELVERIFY is enabled??
            imsg.Class = IDCMP_GADGETUP;
            break;
        case HOVER_OFF:  // Mouse button released while off gadget
//          printf("Off");
//          imsg.Class = IDCMP_GADGETOFF;
            break;
    }
    if (imsg.Class == 0)
        return;  // Don't notify application

    uint64_t usec = timer_tick_to_usec(timer_tick_get());
    // imsg.ExecMessage =   // Not yet supported
    // imsg.Class           // Set above
    imsg.Code      = 0;     // Not used for mouse clicks, I think.
    imsg.Qualifier = 0;     // Not used for mouse clicks, I think.
    imsg.IAddress  = gad;
    imsg.MouseX    = x;
    imsg.MouseY    = y;
    imsg.Seconds   = usec / 1000000;
    imsg.Micros    = usec % 1000000;
    // imsg.IDCMPWindow = NULL;  // Not yet supported
    // imsg.SpecialLink = NULL;  // Set by GT_PutIMsg()
    GT_PutIMsg(&imsg);
}

void
gadget_mouse_move(int x, int y)
{
    GadContext *gc;
    Gadget     *gad;

    /* The mouse is still over cur gadget, then just return */
    if (mouse_cur_gadget != NULL) {
        if ((mouse_cur_gadget->LeftEdge <= x) &&
            (mouse_cur_gadget->LeftEdge + mouse_cur_gadget->Width > x) &&
            (mouse_cur_gadget->TopEdge <= y) &&
            (mouse_cur_gadget->TopEdge + mouse_cur_gadget->Height > y)) {
            if (click_cur_gadget == mouse_cur_gadget) {
                /* Hovering on object while button is pressed */
                gadget_handle_click_hover(click_cur_gadget, NULL, HOVER_OVER);
            }
//          printf("s");
            return;
        }
    }

    /* Search for new hover gadget */
    gc = gad_context_head;
    while (gc != NULL) {
        gad = gc->gc_Gadget.NextGadget;
        while (gad != NULL) {
            if ((gad->LeftEdge <= x) && (gad->LeftEdge + gad->Width > x) &&
                (gad->TopEdge <= y) && (gad->TopEdge + gad->Height > y)) {
                mouse_cur_gadget = gad;
                if (click_cur_gadget == gad) {
                    /* Hovered over gadget while mouse depressed */
                    gadget_handle_click_hover(click_cur_gadget, NULL,
                                              HOVER_ONTO);
                }
//              printf("[%u]", mouse_cur_gadget->GadgetID);
                return;
            }
            gad = gad->NextGadget;
        }
        gc = gc->gc_next;
    }
    if ((click_cur_gadget != NULL) &&
        (click_cur_gadget == mouse_cur_gadget)) {
        /* Hovered away from gadget while mouse depressed */
        gadget_handle_click_hover(click_cur_gadget, NULL, HOVER_AWAY);
    }
    mouse_cur_gadget = NULL;  // Not hovering
}

void
gadget_mouse_button(uint button, uint button_down)
{
    if (button != MOUSE_BUTTON_LEFT)
        return;  // No support for menus at this point

    if (button_down == MOUSE_BUTTON_PRESS) {
        /* Button pressed */
        click_cur_gadget = mouse_cur_gadget;
        gadget_handle_click_hover(click_cur_gadget,
                                  active_gadget, HOVER_CLICK);
        active_gadget = mouse_cur_gadget;
    } else {
        /* Button released */
        if (click_cur_gadget == NULL)
            return;  // Background released

//      printf("BUp");
        if (mouse_cur_gadget != click_cur_gadget) {
//          printf("OffGad");
            gadget_handle_click_hover(click_cur_gadget, NULL, HOVER_OFF);
        } else {
            gadget_handle_click_hover(click_cur_gadget, NULL, HOVER_RELEASE);
        }
        click_cur_gadget = NULL;
    }
}

#if 0
int32_t
GT_GetGadgetAttrsA(Gadget *gad, Window *win,
                   Requester *req, CONST struct TagItem *taglist)
{
}
#endif

int32_t
GT_GetGadgetAttrs(Gadget *gad, Window *win, Requester *req, ...)
{
    va_list     ap;
    uint       *arg;
    uint        tag;
    uint        processed = 0;
    StringInfo *si = gad->SpecialInfo;

    (void) win;
    (void) req;

    va_start(ap, req);
    while ((tag = va_arg(ap, ULONG)) != TAG_DONE) {
        arg = (uint *) va_arg(ap, ULONG);
        switch (tag) {
            case GTST_String:
                if (((gad->GadgetType == STRING_KIND) ||
                     (gad->GadgetType == INTEGER_KIND)) &&
                    (si != NULL))  {
                    *(char **)arg = si->Buffer;
                    processed++;
                }
                break;
            case GTIN_Number:
            case GTNM_Number:  // Wrong tag, but support it anyway
                if (((gad->GadgetType == STRING_KIND) ||
                     (gad->GadgetType == INTEGER_KIND)) &&
                    (si != NULL))  {
                    uint value = 0;
                    sscanf(si->Buffer, "%u", &value);
                    *(uint *)arg = value;
                }
                break;
            case GTMX_Active: {
                MxInfo *mx = gad->SpecialInfo;
                if (mx != NULL) {
                    *arg = mx->mx_selected;
                    processed++;
                }
                break;
            }
        }
    }
    va_end(ap);
    return (processed);
}

void
GT_SetGadgetAttrs(Gadget *gad, Window *win, Requester *req, ...)
{
    va_list     ap;
    uint        arg;
    uint        tag;
    uint        refresh = 0;
    StringInfo *si = gad->SpecialInfo;

    (void) win;
    (void) req;

    va_start(ap, req);
    while ((tag = va_arg(ap, ULONG)) != TAG_DONE) {
        arg = va_arg(ap, ULONG);
        switch (tag) {
            case GA_Disabled:
                if (arg)
                    gad->Flags |= GFLG_DISABLED;
                else
                    gad->Flags &= ~GFLG_DISABLED;
                refresh = 1;
                break;
            case GTST_String:
                if ((si != NULL) & (si->Buffer != NULL)) {
                    char *str = (char *) arg;
                    uint len = strlen(str);
                    if (len > si->MaxChars)
                        len = si->MaxChars - 1;
                    strncpy(si->Buffer, str, len);
                    si->Buffer[len] = '\0';
                    si->NumChars = len;
                    si->DispPos = len;
                }
                refresh = 1;
                break;
            case GTIN_Number:
            case GTNM_Number:  // Wrong tag, but support it anyway
                if ((si != NULL) & (si->Buffer != NULL)) {
                    sprintf(si->Buffer, "%u", arg);
                    si->NumChars = strlen(si->Buffer);
                    si->DispPos = si->NumChars;
                }
                refresh = 1;
                break;
            case GTMX_Active: {
                MxInfo *mx = gad->SpecialInfo;
                if (mx != NULL) {
                    mx->mx_selected = arg;
                    mx->mx_seldisplay = arg;
                }
                refresh = 1;
                break;
            }
        }
    }
    va_end(ap);
    if (refresh) {
        switch (gad->GadgetType) {
            case MX_KIND:
                gadget_update_mx(gad);
                break;
            case STRING_KIND:
            case INTEGER_KIND:
                gadget_update_string(gad, GADGET_STRING_UPDATE_ALL);
                break;
            case BUTTON_KIND:
                gadget_draw_button(gad, 0);
                break;
        }
    }
}
