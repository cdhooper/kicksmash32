/*
 * Gadget rendering code test functions.
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
#include <string.h>
#include "util.h"
#include "printf.h"
#include "draw.h"
#include "intuition.h"
#include "gadget.h"
#include "screen.h"
#include "testgadget.h"

/* Only one of these should be defined for test at a time */
#undef TEST_EVENT_LOOP
#ifdef TEST_EVENT_LOOP

Gadget *gadgets;
Gadget *LastAdded;

static void
test_event_loop(void)
{
    static char bi_name[20];
    struct NewGadget ng;
    struct Screen *screen;
    struct Window *window;

    screen = OpenScreenTags(NULL,
                            SA_Depth,        4,
#if 0
                            SA_Font,         (ULONG) &font_attr,
                            SA_Type,         CUSTOMSCREEN,
                            SA_DisplayID,    monitor_id,
                            SA_Interleaved,  TRUE,
                            SA_Draggable,    FALSE,
                            SA_Quiet,        TRUE,
                            SA_Pens,         (ULONG) &pens,
                            SA_VideoControl, (ULONG) taglist,
#endif
                            TAG_DONE);

    window = OpenWindowTags(NULL,
                            WA_IDCMP,        (IDCMP_RAWKEY | BUTTONIDCMP |
                                              LISTVIEWIDCMP | STRINGIDCMP |
                                              MXIDCMP),
                            WA_CustomScreen, (ULONG) screen,
                            WA_Flags,        (WFLG_NOCAREREFRESH |
                                              WFLG_BORDERLESS |
                                              WFLG_ACTIVATE |
                                              WFLG_RMBTRAP),
//                          WA_ReportMouse,  TRUE,  // Generate IDCMP_MOUSEMOVE
                            TAG_DONE);
    gadgets = CreateContext(&LastAdded);

    ng.ng_Height     = 9;
    ng.ng_GadgetText = "S1";
    ng.ng_TopEdge    = 160;
    ng.ng_LeftEdge   = 24;
    ng.ng_Width      = 11 * FONT_WIDTH;
    ng.ng_GadgetID   = 2;
    strcpy(bi_name, "");
    LastAdded = CreateGadget(STRING_KIND, LastAdded, &ng,
                             GTST_MaxChars, 12,
                             GTST_String, (uint32_t) bi_name,
                             GA_Border, 6,
                             GA_TabCycle, TRUE,
                             TAG_DONE);
    Gadget *gad_s1 = LastAdded;

    ng.ng_Height     = 9;
    ng.ng_GadgetText = "S2";
    ng.ng_TopEdge    = 175;
    ng.ng_LeftEdge   = 24;
    ng.ng_Width      = 12 * FONT_WIDTH;
    ng.ng_GadgetID   = 3;
    strcpy(bi_name, "01234567890abcdef");
    LastAdded = CreateGadget(STRING_KIND, LastAdded, &ng,
                             GTST_MaxChars, 12,
                             GTST_String, (uint32_t) bi_name,
                             GA_Border, 6,
                             GA_TabCycle, TRUE,
                             TAG_DONE);
    Gadget *gad_s2 = LastAdded;

    ng.ng_GadgetText = "Text gadget with border";
    ng.ng_TopEdge    = 170;
    ng.ng_LeftEdge   = 150;
    ng.ng_Width      = 24 * 8;
    ng.ng_GadgetID   = 4;
    LastAdded = CreateGadget(TEXT_KIND, LastAdded, &ng,
                             GTTX_Border, TRUE, TAG_DONE);

    ng.ng_GadgetText = "Text gadget no border";
    ng.ng_TopEdge    = 190;
    ng.ng_LeftEdge   = 150;
    ng.ng_Width      = 24 * 8;
    ng.ng_GadgetID   = 5;
    LastAdded = CreateGadget(TEXT_KIND, LastAdded, &ng, TAG_DONE);

    /* Quit button */
    ng.ng_Height     = 12;
    ng.ng_TopEdge    = 160;
    ng.ng_LeftEdge   = 360;
    ng.ng_Width      = 88;
    ng.ng_GadgetText = "Quit";
    ng.ng_GadgetID   = 6;
    LastAdded = CreateGadget(BUTTON_KIND, LastAdded, &ng,
                             GA_DISABLED, 1,
                             GT_Underscore, '_',
                             TAG_DONE);
    /* Save button */
    ng.ng_Height     = 12;
    ng.ng_TopEdge    = 186;
    ng.ng_LeftEdge   = 360;
    ng.ng_Width      = 88;
    ng.ng_GadgetText = "S_ave";
    ng.ng_GadgetID   = 7;
    LastAdded = CreateGadget(BUTTON_KIND, LastAdded, &ng,
                             GA_DISABLED, 1,
                             GT_Underscore, '_',
                             TAG_DONE);

    /* MX Radio */
    char *sel_labels[] = { "1", "2", "Th", NULL };

    ng.ng_Height     = 9;
    ng.ng_TopEdge    = 150;
    ng.ng_LeftEdge   = 480;
    ng.ng_Width      = 50;
    ng.ng_GadgetText = "MX";
    ng.ng_GadgetID   = 8;
    LastAdded = CreateGadget(MX_KIND, LastAdded, &ng,
                             GTMX_Labels, (ULONG) sel_labels,
                             GTMX_Active, (UWORD) 1,
                             GTMX_Spacing, (UWORD) 2,
                             GTMX_Scaled, TRUE,
                             TAG_DONE);
    Gadget *mxgad = LastAdded;

    show_gadlist(gadgets);
    AddGList(window, gadgets, -1, -1, NULL);
    RefreshGList(gadgets, window, NULL, -1);
    GT_RefreshWindow(window, NULL);

    struct IntuiMessage *msg;

    uint count = 0;
    char s1_buffer[64];
    char s2_buffer[64];
    uint mx_last = 1;
    s1_buffer[0] = '\0';
    s2_buffer[0] = '\0';
    while (1) {
        WaitPort(window->UserPort);
        if ((msg = GT_GetIMsg(window->UserPort)) != NULL) {
//          if (msg->Class == IDCMP_INTUITICKS)
//              printf("[%x]", msg);
#if 1
            if (count++ > 5) {
                char *str = NULL;
                uint temp;
                GT_GetGadgetAttrs(gad_s1, NULL, NULL,
                                  GTST_String, (LONG) &str, TAG_DONE);
                if ((str != NULL) && (strcmp(s1_buffer, str) != 0)) {
                    strcpy(s1_buffer, str);
                    printf("S1=[%s]", str);
                }
                str = NULL;
                GT_GetGadgetAttrs(gad_s2, NULL, NULL,
                                  GTST_String, (LONG) &str, TAG_DONE);
                if ((str != NULL) && (strcmp(s2_buffer, str) != 0)) {
                    strcpy(s2_buffer, str);
                    printf("S2=[%s]", str);
                }
                GT_GetGadgetAttrs(mxgad, NULL, NULL,
                                  GTMX_Active, (LONG) &temp,
                                  TAG_DONE);
                if (mx_last != temp) {
                    mx_last = temp;
                    printf("MX=[%u]", temp);
                }
            }
#endif
            GT_ReplyIMsg(msg);
        }
// XXX: This loop should receive regular IDCMP_INTUITICKS
//      At what rate?
    }
}
#endif

void
test_gadget(void)
{
#ifdef TEST_EVENT_LOOP
    test_event_loop();
#endif
}
