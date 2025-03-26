/*
 * Intuition and Exec API.
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
#ifndef _INTUITION_H
#define _INTUITION_H

#include "exec_types.h"

#define TAG_DONE   (0UL)  // ends array of TagItems
#define TAG_END    (0UL)  // synonym for TAG_DONE
#define TAG_IGNORE (1UL)  // ignore this item, not end of array
#define TAG_MORE   (2UL)  // next is is pointer to another array of TagItems
#define TAG_SKIP   (3UL)  // skip this and the next ti_Data items
#define TAG_USER   ((uint32_t)(1UL<<31))

#define IDCMP_SIZEVERIFY        0x00000001
#define IDCMP_NEWSIZE           0x00000002
#define IDCMP_REFRESHWINDOW     0x00000004
#define IDCMP_MOUSEBUTTONS      0x00000008
#define IDCMP_MOUSEMOVE         0x00000010
#define IDCMP_GADGETDOWN        0x00000020
#define IDCMP_GADGETUP          0x00000040
#define IDCMP_REQSET            0x00000080
#define IDCMP_MENUPICK          0x00000100
#define IDCMP_CLOSEWINDOW       0x00000200
#define IDCMP_RAWKEY            0x00000400
#define IDCMP_REQVERIFY         0x00000800
#define IDCMP_REQCLEAR          0x00001000
#define IDCMP_MENUVERIFY        0x00002000
#define IDCMP_NEWPREFS          0x00004000
#define IDCMP_DISKINSERTED      0x00008000
#define IDCMP_DISKREMOVED       0x00010000
#define IDCMP_WBENCHMESSAGE     0x00020000
#define IDCMP_ACTIVEWINDOW      0x00040000
#define IDCMP_INACTIVEWINDOW    0x00080000
#define IDCMP_DELTAMOVE         0x00100000
#define IDCMP_VANILLAKEY        0x00200000
#define IDCMP_INTUITICKS        0x00400000
#define IDCMP_IDCMPUPDATE       0x00800000
#define IDCMP_MENUHELP          0x01000000
#define IDCMP_CHANGEWINDOW      0x02000000
#define IDCMP_GADGETHELP        0x04000000
#define IDCMP_EXTENDEDMOUSE     0x08000000

/* Screen attribute tags */
#define SA_Dummy             (TAG_USER + 32)
#define SA_Left              (SA_Dummy + 0x0001)
#define SA_Top               (SA_Dummy + 0x0002)
#define SA_Width             (SA_Dummy + 0x0003)
#define SA_Height            (SA_Dummy + 0x0004)
#define SA_Depth             (SA_Dummy + 0x0005)
#define SA_DetailPen         (SA_Dummy + 0x0006)
#define SA_BlockPen          (SA_Dummy + 0x0007)
#define SA_Title             (SA_Dummy + 0x0008)
#define SA_Colors            (SA_Dummy + 0x0009)
#define SA_ErrorCode         (SA_Dummy + 0x000A)
#define SA_Font              (SA_Dummy + 0x000B)
#define SA_SysFont           (SA_Dummy + 0x000C)
#define SA_Type              (SA_Dummy + 0x000D)
#define SA_BitMap            (SA_Dummy + 0x000E)
#define SA_PubName           (SA_Dummy + 0x000F)
#define SA_PubSig            (SA_Dummy + 0x0010)
#define SA_PubTask           (SA_Dummy + 0x0011)
#define SA_DisplayID         (SA_Dummy + 0x0012)
#define SA_DClip             (SA_Dummy + 0x0013)
#define SA_Overscan          (SA_Dummy + 0x0014)
#define SA_ShowTitle         (SA_Dummy + 0x0016)
#define SA_Behind            (SA_Dummy + 0x0017)
#define SA_Quiet             (SA_Dummy + 0x0018)
#define SA_AutoScroll        (SA_Dummy + 0x0019)
#define SA_Pens              (SA_Dummy + 0x001A)
#define SA_FullPalette       (SA_Dummy + 0x001B)
#define SA_ColorMapEntries   (SA_Dummy + 0x001C)
#define SA_Parent            (SA_Dummy + 0x001D)
#define SA_Draggable         (SA_Dummy + 0x001E)
#define SA_Exclusive         (SA_Dummy + 0x001F)
#define SA_SharePens         (SA_Dummy + 0x0020)
#define SA_BackFill          (SA_Dummy + 0x0021)
#define SA_Interleaved       (SA_Dummy + 0x0022)
#define SA_Colors32          (SA_Dummy + 0x0023)
#define SA_VideoControl      (SA_Dummy + 0x0024)
#define SA_FrontChild        (SA_Dummy + 0x0025)
#define SA_BackChild         (SA_Dummy + 0x0026)
#define SA_LikeWorkbench     (SA_Dummy + 0x0027)
#define SA_Reserved          (SA_Dummy + 0x0028)
#define SA_MinimizeISG       (SA_Dummy + 0x0029)
#define SA_OffScreenDragging (SA_Dummy + 0x002a)

/* Window attribute tags */
#define WA_Dummy        (TAG_USER + 99) /* 0x80000063   */
#define WA_Left                 (WA_Dummy + 0x01)
#define WA_Top                  (WA_Dummy + 0x02)
#define WA_Width                (WA_Dummy + 0x03)
#define WA_Height               (WA_Dummy + 0x04)
#define WA_DetailPen            (WA_Dummy + 0x05)
#define WA_BlockPen             (WA_Dummy + 0x06)
#define WA_IDCMP                (WA_Dummy + 0x07)
#define WA_Flags                (WA_Dummy + 0x08)
#define WA_Gadgets              (WA_Dummy + 0x09)
#define WA_Checkmark            (WA_Dummy + 0x0A)
#define WA_Title                (WA_Dummy + 0x0B)
#define WA_ScreenTitle          (WA_Dummy + 0x0C)
#define WA_CustomScreen         (WA_Dummy + 0x0D)
#define WA_SuperBitMap          (WA_Dummy + 0x0E)
#define WA_MinWidth             (WA_Dummy + 0x0F)
#define WA_MinHeight            (WA_Dummy + 0x10)
#define WA_MaxWidth             (WA_Dummy + 0x11)
#define WA_MaxHeight            (WA_Dummy + 0x12)
#define WA_InnerWidth           (WA_Dummy + 0x13)
#define WA_InnerHeight          (WA_Dummy + 0x14)
#define WA_PubScreenName        (WA_Dummy + 0x15)
#define WA_PubScreen            (WA_Dummy + 0x16)
#define WA_PubScreenFallBack    (WA_Dummy + 0x17)
#define WA_WindowName           (WA_Dummy + 0x18)
#define WA_Colors               (WA_Dummy + 0x19)
#define WA_Zoom                 (WA_Dummy + 0x1A)
#define WA_MouseQueue           (WA_Dummy + 0x1B)
#define WA_BackFill             (WA_Dummy + 0x1C)
#define WA_RptQueue             (WA_Dummy + 0x1D)
#define WA_SizeGadget           (WA_Dummy + 0x1E)
#define WA_DragBar              (WA_Dummy + 0x1F)
#define WA_DepthGadget          (WA_Dummy + 0x20)
#define WA_CloseGadget          (WA_Dummy + 0x21)
#define WA_Backdrop             (WA_Dummy + 0x22)
#define WA_ReportMouse          (WA_Dummy + 0x23)
#define WA_NoCareRefresh        (WA_Dummy + 0x24)
#define WA_Borderless           (WA_Dummy + 0x25)
#define WA_Activate             (WA_Dummy + 0x26)
#define WA_RMBTrap              (WA_Dummy + 0x27)
#define WA_SimpleRefresh        (WA_Dummy + 0x29)
#define WA_SmartRefresh         (WA_Dummy + 0x2A)
#define WA_SizeBRight           (WA_Dummy + 0x2B)
#define WA_SizeBBottom          (WA_Dummy + 0x2C)
#define WA_AutoAdjust           (WA_Dummy + 0x2D)
#define WA_GimmeZeroZero        (WA_Dummy + 0x2E)
#define WA_MenuHelp             (WA_Dummy + 0x2F)
#define WA_NewLookMenus         (WA_Dummy + 0x30)
#define WA_AmigaKey             (WA_Dummy + 0x31)
#define WA_NotifyDepth          (WA_Dummy + 0x32)
#define WA_Pointer              (WA_Dummy + 0x34)
#define WA_BusyPointer          (WA_Dummy + 0x35)
#define WA_PointerDelay         (WA_Dummy + 0x36)
#define WA_TabletMessages       (WA_Dummy + 0x37)
#define WA_HelpGroup            (WA_Dummy + 0x38)
#define WA_HelpGroupWindow      (WA_Dummy + 0x39)
#define WA_Hidden               (WA_Dummy + 0x3c)
#define WA_PointerType          (WA_Dummy + 0x50)
#define WA_IconifyGadget        (WA_Dummy + 0x60)

/* Window flags */
#define WFLG_SIZEGADGET     0x00000001  // include sizing system-gadget?
#define WFLG_DRAGBAR        0x00000002  // include dragging system-gadget?
#define WFLG_DEPTHGADGET    0x00000004  // include depth arrangement gadget?
#define WFLG_CLOSEGADGET    0x00000008  // include close-box system-gadget?
#define WFLG_SIZEBRIGHT     0x00000010  // size gadget uses right border
#define WFLG_SIZEBBOTTOM    0x00000020  // size gadget uses bottom border
#define WFLG_REFRESHBITS    0x000000C0
#define WFLG_SMART_REFRESH  0x00000000
#define WFLG_SIMPLE_REFRESH 0x00000040
#define WFLG_SUPER_BITMAP   0x00000080
#define WFLG_OTHER_REFRESH  0x000000C0
#define WFLG_BACKDROP       0x00000100  // this is a backdrop window
#define WFLG_REPORTMOUSE    0x00000200  // to hear about every mouse move
#define WFLG_GIMMEZEROZERO  0x00000400  // a GimmeZeroZero window
#define WFLG_BORDERLESS     0x00000800  // to get a Window sans border
#define WFLG_ACTIVATE       0x00001000  // when Window opens, it's Active
#define WFLG_RMBTRAP        0x00010000  // Catch RMB events for your own
#define WFLG_NOCAREREFRESH  0x00020000  // not to be bothered with REFRESH
#define WFLG_NW_EXTENDED    0x00040000  // extension data provided
#define WFLG_NEWLOOKMENUS   0x00200000  // window has NewLook menus
#define WFLG_WINDOWACTIVE   0x00002000  // this window is the active one
#define WFLG_INREQUEST      0x00004000  // this window is in request mode
#define WFLG_MENUSTATE      0x00008000  // Window is active with Menus on
#define WFLG_WINDOWREFRESH  0x01000000  // Window is currently refreshing
#define WFLG_WBENCHWINDOW   0x02000000  // WorkBench tool ONLY Window
#define WFLG_WINDOWTICKED   0x04000000  // only one timer tick at a time
#define WFLG_VISITOR        0x08000000  // visitor window
#define WFLG_ZOOMED         0x10000000  // identifies "zoom state"
#define WFLG_HASZOOM        0x20000000  // window has a zoom gadget
#define WFLG_HASICONIFY     0x40000000  // window has an iconification gadget

/* IntuiMessage event qualifiers */
#define IEQUALIFIER_LSHIFT              0x0001
#define IEQUALIFIER_RSHIFT              0x0002
#define IEQUALIFIER_CAPSLOCK            0x0004
#define IEQUALIFIER_CONTROL             0x0008
#define IEQUALIFIER_LALT                0x0010
#define IEQUALIFIER_RALT                0x0020
#define IEQUALIFIER_LCOMMAND            0x0040
#define IEQUALIFIER_RCOMMAND            0x0080
#define IEQUALIFIER_NUMERICPAD          0x0100
#define IEQUALIFIER_REPEAT              0x0200
#define IEQUALIFIER_INTERRUPT           0x0400
#define IEQUALIFIER_MULTIBROADCAST      0x0800
#define IEQUALIFIER_MIDBUTTON           0x1000
#define IEQUALIFIER_RBUTTON             0x2000
#define IEQUALIFIER_LEFTBUTTON          0x4000
#define IEQUALIFIER_RELATIVEMOUSE       0x8000

#define NTSC		                1
#define PAL                             4
#define NTSC_MONITOR_ID                 0x00011000
#define PAL_MONITOR_ID                  0x00021000
#define HIRES_KEY                       0x00008000
#define FS_NORMAL	                0
#define FPF_ROMFONT	                0x01
#define VTAG_BORDERSPRITE_SET           0x8000002f
#define CUSTOMSCREEN	                0x000F

#define RASSIZE(w,h) ((uint32_t)(h) * ((((uint32_t)(w) + 15) >> 3) & 0xFFFE))

typedef uint32_t Tag;

typedef struct Message {
    struct  Node     mn_Node;
    struct  MsgPort *mn_ReplyPort;
    uint16_t         mn_Length;
} Message;

typedef struct IntuiText {
    uint8_t  FrontPen;                 // foreground pen number
    uint8_t  BackPen;                  // background pen number
    uint8_t  DrawMode;                 // the mode for rendering the text
    uint16_t LeftEdge;                 // relative start location for the text
    uint16_t TopEdge;                  // relative start location for the text
    const struct TextAttr *ITextFont;  // if NULL, you accept the default
    char    *IText;                    // pointer to null-terminated text
    struct IntuiText *NextText;        // pointer to another IntuiText to render
} IntuiText;

typedef struct IntuiMessage {
    struct Message ExecMessage;
    uint32_t Class;
    uint16_t Code;
    uint16_t Qualifier;
    APTR IAddress;
    int16_t MouseX;
    int16_t MouseY;
    uint32_t Seconds, Micros;
    struct Window *IDCMPWindow;
    struct IntuiMessage *SpecialLink;
} IntuiMessage;

typedef struct TagItem {
    Tag   ti_Tag;               // type of data
    uint32_t ti_Data;           // type-specific data
} TagItem;

typedef struct NewScreen {
    int16_t  LeftEdge;
    int16_t  TopEdge;
    int16_t  Width;             // screen width
    int16_t  Height;            // screen height
    int16_t  Depth;             // screen bitplanes
    uint8_t  DetailPen;
    uint8_t  BlockPen;
    uint16_t ViewModes;
    uint16_t Type;              // screen type
    struct TextAttr *Font;
    char  *DefaultTitle;
    struct Gadget *Gadgets;
    struct BitMap *CustomBitMap;
} NewScreen;

typedef struct Rectangle {
    int16_t MinX;
    int16_t MinY;
    int16_t MaxX;
    int16_t MaxY;
} Rectangle;

typedef struct SemaphoreRequest {
    struct MinNode sr_Link;
    struct Task    *sr_Waiter;
} SemaphoreRequest;

typedef struct SignalSemaphore {
    struct Node              ss_Link;
    int16_t                  ss_NestCount;
    struct MinList           ss_WaitQueue;
    struct SemaphoreRequest  ss_MultipleLink;
    struct Task             *ss_Owner;
    int16_t                  ss_QueueCount;
} SignalSemaphore;

typedef struct Layer_Info {
    struct Layer *top_layer;          // Frontmost layer
    void         *resPtr1;            // spare
    void         *resPtr2;            // spare
    struct ClipRect *FreeClipRects;   // Backing store
    struct Rectangle       bounds;    // flipping bounds
    struct SignalSemaphore Lock;      // Layer_Info lock
    struct MinList         gs_Head;   // all layers within this layer info
    int16_t       PrivateReserve3;    // Private
    void         *PrivateReserve4;    // Private
    uint16_t      Flags;
    int8_t        res_count;          // spare
    int8_t        LockLayersCount;    // # times LockLayers
    int8_t        PrivateReserve5;    // Private
    int8_t        UserClipRectsCount; // Private
    struct Hook  *BlankHook;          // LayerInfo backfill hook
    void         *resPtr5;            // Private
} Layer_Info;

typedef struct ViewPort
{
    struct   ViewPort *Next;
    struct   ColorMap *ColorMap;  // table of colors for this viewport
                                  // if NULL, MakeVPort assumes default values
    struct   CopList  *DspIns;    // used by MakeVPort()
    struct   CopList  *SprIns;    // used by sprite stuff
    struct   CopList  *ClrIns;    // used by sprite stuff
    struct   UCopList *UCopIns;   // User copper list
    int16_t  DWidth;
    int16_t  DHeight;
    int16_t  DxOffset;
    int16_t  DyOffset;
    uint16_t Modes;
    uint8_t  SpritePriorities;
    uint8_t  ExtendedModes;
    struct   RasInfo *RasInfo;
} ViewPort;

typedef uint8_t *PLANEPTR;

typedef struct BitMap
{
    uint16_t    BytesPerRow;
    uint16_t    Rows;
    uint8_t    Flags;
    uint8_t    Depth;
    uint16_t    pad;
    PLANEPTR Planes[8];
} BitMap;

typedef struct AreaInfo {
    int16_t *VctrTbl;   // ptr to start of vector table
    int16_t *VctrPtr;   // ptr to current vertex
    int8_t  *FlagTbl;   // ptr to start of vector flag table
    int8_t  *FlagPtr;   // ptrs to areafill flags
    int16_t  Count;     // number of vertices in list
    int16_t  MaxCount;  // AreaMove/Draw will not allow Count>MaxCount
    int16_t  FirstX;    // first point for this polygon
    int16_t  FirstY;    // first point for this polygon
} AreaInfo;

struct TmpRas
{
    int8_t  *RasPtr;
    int32_t  Size;
};

typedef struct RastPort {
    struct Layer    *Layer;
    struct BitMap   *BitMap;
    uint16_t        *AreaPtrn;    // ptr to areafill pattern
    struct TmpRas   *TmpRas;
    struct AreaInfo *AreaInfo;
    struct GelsInfo *GelsInfo;
    uint8_t          Mask;        // write mask for this raster SetWriteMask()
    int8_t           FgPen;       // foreground pen for this raster  SetAPen()
    int8_t           BgPen;       // background pen                  SetBPen()
    int8_t           AOlPen;      // areafill outline pen      SetOutLinePen()
    int8_t           DrawMode;    // drawing mode for fill/line/text SetDrMd()
    int8_t           AreaPtSz;    // 2^n words for areafill pattern
    int8_t           linpatcnt;   // current line drawing pattern preshift
    int8_t           dummy;
    uint16_t         Flags;       // miscellaneous control bits
    uint16_t         LinePtrn;    // 16 bits for textured lines
    int16_t          cp_x;        // current pen position
    int16_t          cp_y;        // current pen position
    uint8_t          minterms[8];
    int16_t          PenWidth;
    int16_t          PenHeight;
    struct TextFont *Font;        // current font address
    uint8_t          AlgoStyle;   // the algorithmically generated style
    uint8_t          TxFlags;     // text specific flags
    uint16_t         TxHeight;    // text height
    uint16_t         TxWidth;     // text nominal width
    uint16_t         TxBaseline;  // text baseline
    int16_t          TxSpacing;   // text spacing (per character)
    APTR            *RP_User;
} RastPort;

typedef struct Screen {
    struct Screen *NextScreen;     // linked list of screens
    struct Window *FirstWindow;    // linked list Screen's Windows
    int16_t LeftEdge, TopEdge;     // parameters of the screen
    int16_t Width, Height;         // parameters of the screen

    int16_t MouseY;                // position relative to upper-left
    int16_t MouseX;                // position relative to upper-left

    uint16_t Flags;

    STRPTR Title;                  // null-terminated Title text
    STRPTR DefaultTitle;           // for Windows without ScreenTitle
    int8_t BarHeight, BarVBorder, BarHBorder, MenuVBorder, MenuHBorder;
    int8_t WBorTop, WBorLeft, WBorRight, WBorBottom;
    struct TextAttr *Font;         // default font
    struct ViewPort ViewPort;
    RastPort RastPort;             // describing Screen rendering
    struct BitMap BitMap;          // don't use
    struct Layer_Info LayerInfo;   // each screen gets a LayerInfo
    struct Gadget *FirstGadget;
    uint8_t DetailPen;             // for bar/border/gadget rendering
    uint8_t BlockPen;              // for bar/border/gadget rendering
    uint16_t SaveColor0;
    struct Layer *BarLayer;        // for screen and menu bars
    uint8_t *ExtData;
    uint8_t *UserData;             // User data extension
} Screen;

typedef struct Window {
    struct Window *NextWindow;      // for the linked list in a screen
    int16_t LeftEdge;               // screen dimensions of window
    int16_t TopEdge;                // screen dimensions of window
    int16_t Width;                  // screen dimensions of window
    int16_t Height;                 // screen dimensions of window
    int16_t MouseY;                 // relative to upper-left of window
    int16_t MouseX;                 // relative to upper-left of window
    int16_t MinWidth;               // minimum sizes
    int16_t MinHeight;              // minimum sizes
    uint16_t MaxWidth;              // maximum sizes
    uint16_t MaxHeight;             // maximum sizes
    uint32_t Flags;                 // see below for defines
    struct Menu *MenuStrip;         // the strip of Menu headers
    STRPTR Title;                   // the title text for this window
    struct Requester *FirstRequest; // all active Requesters
    struct Requester *DMRequest;    // double-click Requester
    int16_t ReqCount;               // count of reqs blocking Window
    Screen   *WScreen;              // this Window's Screen
    RastPort *RPort;                // this Window's very own RastPort
    uint8_t   BorderLeft;
    uint8_t   BorderTop;
    uint8_t   BorderRight;
    uint8_t   BorderBottom;
    RastPort *BorderRPort;
    struct Gadget *FirstGadget;
    struct Window *Parent;          // for opening / closing window
    struct Window *Descendant;
    uint16_t *Pointer;              // sprite data
    int8_t PtrHeight;               // sprite height - padding
    int8_t PtrWidth;                // sprite width (<= 16)
    int8_t XOffset, YOffset;        // sprite offsets
    uint32_t IDCMPFlags;
    struct MsgPort *UserPort;
    struct MsgPort *WindowPort;
    struct IntuiMessage *MessageKey;
    uint8_t DetailPen;                // for bar/border/gadget rendering
    uint8_t BlockPen;                 // for bar/border/gadget rendering
    struct Image *CheckMark;
    STRPTR ScreenTitle;               // Screen when active
    int16_t GZZMouseX;
    int16_t GZZMouseY;
    int16_t GZZWidth;
    int16_t GZZHeight;

    uint8_t         *ExtData;
    int8_t          *UserData;        // User data extension
    struct Layer    *WLayer;
    struct TextFont *IFont;
    uint32_t         MoreFlags;
} Window;

typedef struct NewWindow {
    int16_t LeftEdge;              // screen dimensions of window
    int16_t TopEdge;               // screen dimensions of window
    int16_t Width;                 // screen dimensions of window
    int16_t Height;                // screen dimensions of window
    uint8_t DetailPen;          // for bar/border/gadget rendering
    uint8_t BlockPen;           // for bar/border/gadget rendering
    uint32_t IDCMPFlags;           // User-selected IDCMP flags
    uint32_t Flags;                // see Window struct for defines
    struct Gadget *FirstGadget;
    struct Image *CheckMark;
    STRPTR Title;               // the title text for this window
    struct Screen *Screen;
    struct BitMap *BitMap;
    int16_t MinWidth;              // minimums
    int16_t MinHeight;             // minimums
    uint16_t MaxWidth;             // maximums
    uint16_t MaxHeight;            // maximums
    uint16_t Type;
} NewWindow;

typedef struct TextAttr {
    char     *ta_Name;          // name of the font
    uint16_t  ta_YSize;         // height of the font
    uint8_t   ta_Style;         // intrinsic font style
    uint8_t   ta_Flags;         // font preferences and flags
} TextAttr;

typedef struct Requester {
    struct Requester *OlderRequest;
    int16_t LeftEdge;           // dimensions of the entire box
    int16_t TopEdge;            // dimensions of the entire box
    int16_t Width;              // dimensions of the entire box
    int16_t Height;             // dimensions of the entire box
    int16_t RelLeft;            // for Pointer relativity offsets
    int16_t RelTop;             // for Pointer relativity offsets
    struct Gadget *ReqGadget;   // pointer to a list of Gadgets
    struct Border *ReqBorder;   // the box's border
    struct IntuiText *ReqText;  // the box's text
    uint16_t Flags;             // see definitions below
    uint8_t BackFill;           // pen number for back-plane fill
    struct Layer *ReqLayer;     // Layer in place of clip rect
    uint8_t ReqPad1[32];
    struct BitMap *ImageBMap;   // the BitMap of PREDRAWN imagery
    Window *RWindow;            // added.  points back to Window
    struct Image  *ReqImage;    // new for V36: drawn if USEREQIMAGE set
    uint8_t ReqPad2[32];
} Requester;

struct GfxBase {
    uint16_t  DisplayFlags;           // NTSC PAL GENLOC etc
};

extern Screen sscreen;
// extern struct Window window;

Screen *OpenScreenTagList(struct NewScreen *ns, struct TagItem *taglist);
Screen *OpenScreenTags(struct NewScreen *ns, ULONG taglist, ...);
int     CloseScreen(struct Screen *screen);
Window *OpenWindowTags(const struct NewWindow *newWindow,
                       uint32_t tag1Type, ... );
void    CloseWindow(struct Window *window);
void   *GetVisualInfoA(struct Screen *screen, const struct TagItem *taglist);
void    FreeVisualInfo(void *vi);
void    ColdReboot(void);

#endif /* _INTUITION_H */
