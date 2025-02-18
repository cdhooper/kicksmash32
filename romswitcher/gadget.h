/*
 * GadTools API and main gadget handling functions.
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
#ifndef _GADGET_H
#define _GADGET_H

#define ARROWIDCMP      (IDCMP_GADGETUP | IDCMP_GADGETDOWN |\
        IDCMP_INTUITICKS | IDCMP_MOUSEBUTTONS)

#define BUTTONIDCMP     (IDCMP_GADGETUP)
#define CHECKBOXIDCMP   (IDCMP_GADGETUP)
#define INTEGERIDCMP    (IDCMP_GADGETUP)
#define LISTVIEWIDCMP   (IDCMP_GADGETUP | IDCMP_GADGETDOWN |\
        IDCMP_MOUSEMOVE | ARROWIDCMP)

#define MXIDCMP         (IDCMP_GADGETDOWN)
#define NUMBERIDCMP     (0L)
#define CYCLEIDCMP      (IDCMP_GADGETUP)
#define PALETTEIDCMP    (IDCMP_GADGETUP)

/* Use ARROWIDCMP|SCROLLERIDCMP if your scrollers have arrows: */
#define SCROLLERIDCMP   (IDCMP_GADGETUP | IDCMP_GADGETDOWN | IDCMP_MOUSEMOVE)
#define SLIDERIDCMP     (IDCMP_GADGETUP | IDCMP_GADGETDOWN | IDCMP_MOUSEMOVE)
#define STRINGIDCMP     (IDCMP_GADGETUP)

/* Gadget types */
#define GENERIC_KIND    0   // 0x00
#define BUTTON_KIND     1   // 0x01
#define CHECKBOX_KIND   2   // 0x02
#define INTEGER_KIND    3   // 0x03
#define LISTVIEW_KIND   4   // 0x04
#define MX_KIND         5   // 0x05
#define NUMBER_KIND     6   // 0x06
#define CYCLE_KIND      7   // 0x07
#define PALETTE_KIND    8   // 0x08
#define SCROLLER_KIND   9   // 0x09
#define SLIDER_KIND     11  // 0x0b
#define STRING_KIND     12  // 0x0c
#define TEXT_KIND       13  // 0x0d

/* Tags for GadTools functions */
#define GT_TagBase           (TAG_USER + 0x80000)
#define GT_ExtraSize         GT_Private0     // internal gadget use
#define GT_Private0          (GT_TagBase+3)  // (private)
#define GTCB_Checked         (GT_TagBase+4)  // State of checkbox
#define GTLV_Top             (GT_TagBase+5)  // Top visible one in listview
#define GTLV_Labels          (GT_TagBase+6)  // List to display in listview
#define GTLV_ReadOnly        (GT_TagBase+7)  // TRUE = listview is read-only
#define GTLV_ScrollWidth     (GT_TagBase+8)  // Scrollbar width
#define GTMX_Labels          (GT_TagBase+9)  // NULL-terminated array of labels
#define GTMX_Active          (GT_TagBase+10) // Active one in mx gadget
#define GTTX_Text            (GT_TagBase+11) // Text to display
#define GTTX_CopyText        (GT_TagBase+12) // Copy text label instead of ref
#define GTNM_Number          (GT_TagBase+13) // Number to display
#define GTCY_Labels          (GT_TagBase+14) // NULL-terminated array of labels
#define GTCY_Active          (GT_TagBase+15) // Active one in the cycle gad
#define GTPA_Depth           (GT_TagBase+16) // Number of bitplanes in palette
#define GTPA_Color           (GT_TagBase+17) // Palette color
#define GTPA_ColorOffset     (GT_TagBase+18) // First color to use in palette
#define GTPA_IndicatorWidth  (GT_TagBase+19) // Width of color indicator
#define GTPA_IndicatorHeight (GT_TagBase+20) // Height of color indicator
#define GTSC_Top             (GT_TagBase+21) // Top visible in scroller
#define GTSC_Total           (GT_TagBase+22) // Total in scroller area
#define GTSC_Visible         (GT_TagBase+23) // Number visible in scroller
#define GTSL_Min             (GT_TagBase+38) // Slider min value
#define GTSL_Max             (GT_TagBase+39) // Slider max value
#define GTSL_Level           (GT_TagBase+40) // Slider level
#define GTSL_MaxLevelLen     (GT_TagBase+41) // Max length of printed level
#define GTSL_LevelFormat     (GT_TagBase+42) // Format string for level
#define GTSL_LevelPlace      (GT_TagBase+43) // Where level should be placed
#define GTSL_DispFunc        (GT_TagBase+44) // Callback for number calculation
#define GTST_String          (GT_TagBase+45) // String gadget displayed string
#define GTST_MaxChars        (GT_TagBase+46) // Max length of string
#define GTIN_Number          (GT_TagBase+47) // Number in integer gadget
#define GTIN_MaxChars        (GT_TagBase+48) // Max number of digits
#define GTMN_TextAttr        (GT_TagBase+49) // MenuItem font TextAttr
#define GTMN_FrontPen        (GT_TagBase+50) // MenuItem text pen color
#define GTBB_Recessed        (GT_TagBase+51) // Make BevelBox recessed
#define GT_VisualInfo        (GT_TagBase+52) // Result of VisualInfo call
#define GTLV_ShowSelected    (GT_TagBase+53) // Show selected listview entry
#define GTLV_Selected        (GT_TagBase+54) // Set selected entry in list
#define GTTX_Border          (GT_TagBase+57) // Put a border around text gads
#define GTNM_Border          (GT_TagBase+58) // Put a border around number gads
#define GTSC_Arrows          (GT_TagBase+59) // Size of arrows for scrollbar
#define GTMN_Menu            (GT_TagBase+60) // LayoutMenuItems() menu pointer
#define GTMX_Spacing         (GT_TagBase+61) // Added to font height for pad
#define GTMN_FullMenu        (GT_TagBase+62) // CreateMenus() should validate
#define GTMN_SecondaryError  (GT_TagBase+63) // Get error report to ti_Data ptr
#define GT_Underscore        (GT_TagBase+64) // Symbol to underline label chars
#define GTST_EditHook        (GT_TagBase+55) // String EditHook
#define GTIN_EditHook        GTST_EditHook   // Same thing, different name,
#define GTMN_Checkmark       (GT_TagBase+65) // Checkmark img to use
#define GTMN_AmigaKey        (GT_TagBase+66) // Amiga-key img to use
#define GTMN_NewLookMenus    (GT_TagBase+67) // Use new style menu
#define GTCB_Scaled          (GT_TagBase+68) // Gadget imagry is scaled
#define GTMX_Scaled          (GT_TagBase+69) // Gadget imagry is scaled
#define GTPA_NumColors       (GT_TagBase+70) // Number of colors in palette
#define GTMX_TitlePlace      (GT_TagBase+71) // Where to put the title
#define GTTX_FrontPen        (GT_TagBase+72) // Text color in TEXT_KIND gad
#define GTTX_BackPen         (GT_TagBase+73) // Bgrnd color in TEXT_KIND gad
#define GTTX_Justification   (GT_TagBase+74) // See GTJ_#? constants
#define GTNM_FrontPen        (GT_TagBase+72) // Text color in NUMBER_KIND gad
#define GTNM_BackPen         (GT_TagBase+73) // Bgrnd color in NUMBER_KIND gad
#define GTNM_Justification   (GT_TagBase+74) // See GTJ_#? constants
#define GTNM_Format          (GT_TagBase+75) // Formatting string for number
#define GTNM_MaxNumberLen    (GT_TagBase+76) // Maximum length of number
#define GTBB_FrameType       (GT_TagBase+77) // defines what kind of boxes
#define GTLV_MakeVisible     (GT_TagBase+78) // Make this item visible
#define GTLV_ItemHeight      (GT_TagBase+79) // Height of an individual item
#define GTSL_MaxPixelLen     (GT_TagBase+80) // Max pixel size of level display
#define GTSL_Justification   (GT_TagBase+81) // how should level be displayed
#define GTPA_ColorTable      (GT_TagBase+82) // colors to use in palette
#define GTLV_CallBack        (GT_TagBase+83) // gen-purpose listview callback
#define GTLV_MaxPen          (GT_TagBase+84) // max pen number used by callback
#define GTTX_Clipped         (GT_TagBase+85) // make a TEXT_KIND clip text
#define GTNM_Clipped         (GT_TagBase+85) // make a NUMBER_KIND clip text
#define GTLV_Total           (GT_TagBase+92) // ListView total entries
#define GTLV_Visible         (GT_TagBase+93) // ListView visible entries

/* Bevel box frame types for GTBB_FrameType tag */
#define BBFT_BUTTON          1  // Standard button gadget box
#define BBFT_RIDGE           2  // Standard string gadget box
#define BBFT_ICONDROPBOX     3  // Standard icon drop box
#define BBFT_DISPLAY         6  // Standard display box (V47)
#define BBFT_CTXTFRAME       7  // Context frame with headline (V47)

/* Gadget class attributes */
#define GA_Dummy             (TAG_USER+0x30000)
#define GA_Left              (GA_Dummy+1)    // Gadget relative to left edge
#define GA_RelRight          (GA_Dummy+2)    // Gadget relative to right edge
#define GA_Top               (GA_Dummy+3)    // Gadget relative to top edge
#define GA_RelBottom         (GA_Dummy+4)    // Gadget relative to bottom edge
#define GA_Width             (GA_Dummy+5)    // Width
#define GA_RelWidth          (GA_Dummy+6)    // Width relative to window size
#define GA_Height            (GA_Dummy+7)    // Height
#define GA_RelHeight         (GA_Dummy+8)    // Height relative to window size
#define GA_Text              (GA_Dummy+9)    // Gadget text/intuitext/image
#define GA_Image             (GA_Dummy+10)   // Gadget imagry is image
#define GA_Border            (GA_Dummy+11)   // Gadget imagry is border
#define GA_SelectRender      (GA_Dummy+12)   // Selected gadget imagry
#define GA_Highlight         (GA_Dummy+13)   // GFLG_GADGH NONE BOX COMP IMAGE
#define GA_Disabled          (GA_Dummy+14)   // Disabled if TRUE
#define GA_GZZGadget         (GA_Dummy+15)   // WFLG_GIMMEZEROZERO
#define GA_ID                (GA_Dummy+16)   // ID assigned by application
#define GA_UserData          (GA_Dummy+17)   // Application-specific data
#define GA_SpecialInfo       (GA_Dummy+18)   // Gadget-specific data
#define GA_Selected          (GA_Dummy+19)   // Gadget selected
#define GA_EndGadget         (GA_Dummy+20)   // Gadget selection ends requester
#define GA_Immediate         (GA_Dummy+21)   // Notify app when gadget activated
#define GA_RelVerify         (GA_Dummy+22)   // Report over gadget when released
#define GA_FollowMouse       (GA_Dummy+23)   // Report mouse while activated
#define GA_RightBorder       (GA_Dummy+24)   // Gadget In right border
#define GA_LeftBorder        (GA_Dummy+25)   // Gadget In left border
#define GA_TopBorder         (GA_Dummy+26)   // Gadget in top border
#define GA_BottomBorder      (GA_Dummy+27)   // Gadget in bottom border
#define GA_ToggleSelect      (GA_Dummy+28)   // Gadget is toggle-selected
#define GA_SysGadget         (GA_Dummy+29)   // System gadget
#define GA_SysGType          (GA_Dummy+30)   // System gadget type
#define GA_Previous          (GA_Dummy+31)   // Link to previous gadget
#define GA_Next              (GA_Dummy+32)   // Link to next gadget
#define GA_DrawInfo          (GA_Dummy+33)   // GA _Text _IntuiText _LabelImage
#define GA_LabelImage        (GA_Dummy+35)   // Label is image object
#define GA_TabCycle          (GA_Dummy+36)   // Part of tab key cycle
#define GA_GadgetHelp        (GA_Dummy+37)   // Help key causes IDCMP_GADGETHELP
#define GA_Bounds            (GA_Dummy+38)   // Extended gadget's bounds
#define GA_RelSpecial        (GA_Dummy+39)   // Special relativity
#define GA_TextAttr          (GA_Dummy+40)   // Text font
#define GA_ReadOnly          (GA_Dummy+41)   // Not selectable
#define GA_Underscore        (GA_Dummy+42)   // Underscore char kbd shortcut
#define GA_ActivateKey       (GA_Dummy+43)   // Shortcut activation key
#define GA_BackFill          (GA_Dummy+44)   // Backfill pattern hook

#define GA_LEFT              GA_Left
#define GA_RELRIGHT          GA_RelRight
#define GA_TOP               GA_Top
#define GA_RELBOTTOM         GA_RelBottom
#define GA_WIDTH             GA_Width
#define GA_RELWIDTH          GA_RelWidth
#define GA_HEIGHT            GA_Height
#define GA_RELHEIGHT         GA_RelHeight
#define GA_TEXT              GA_Text
#define GA_IMAGE             GA_Image
#define GA_BORDER            GA_Border
#define GA_SELECTRENDER      GA_SelectRender
#define GA_HIGHLIGHT         GA_Highlight
#define GA_DISABLED          GA_Disabled
#define GA_GZZGADGET         GA_GZZGadget
#define GA_USERDATA          GA_UserData
#define GA_SPECIALINFO       GA_SpecialInfo
#define GA_SELECTED          GA_Selected
#define GA_ENDGADGET         GA_EndGadget
#define GA_IMMEDIATE         GA_Immediate
#define GA_RELVERIFY         GA_RelVerify
#define GA_FOLLOWMOUSE       GA_FollowMouse
#define GA_RIGHTBORDER       GA_RightBorder
#define GA_LEFTBORDER        GA_LeftBorder
#define GA_TOPBORDER         GA_TopBorder
#define GA_BOTTOMBORDER      GA_BottomBorder
#define GA_TOGGLESELECT      GA_ToggleSelect
#define GA_SYSGADGET         GA_SysGadget
#define GA_SYSGTYPE          GA_SysGType
#define GA_PREVIOUS          GA_Previous
#define GA_NEXT              GA_Next
#define GA_DRAWINFO          GA_DrawInfo
#define GA_INTUITEXT         GA_IntuiText
#define GA_LABELIMAGE        GA_LabelImage

#define STRINGA_Dummy          (TAG_USER +0x32000)
#define STRINGA_Justification  (STRINGA_Dummy+0x0010)  // GACT_STRING*

/* Gadget.Flags */
#define GFLG_GADGHIGHBITS 0x0003
#define GFLG_GADGHCOMP    0x0000  // Complement select box
#define GFLG_GADGHBOX     0x0001  // Draw box around image
#define GFLG_GADGHIMAGE   0x0002  // Blast in this alternate image
#define GFLG_GADGHNONE    0x0003  // don't highlight
#define GFLG_GADGIMAGE    0x0004  // GadgetRender or SelectRender has image
#define GFLG_RELBOTTOM    0x0008  // vert. pos. is relative to bottom edge
#define GFLG_RELRIGHT     0x0010  // horiz. pos. is relative to right edge
#define GFLG_RELWIDTH     0x0020  // width relative to req/window
#define GFLG_RELHEIGHT    0x0040  // height relative to req/window
#define GFLG_RELSPECIAL   0x4000  // custom gadget has special relativity.
#define GFLG_SELECTED     0x0080
#define GFLG_DISABLED     0x0100
#define GFLG_LABELMASK    0x3000
#define GFLG_LABELITEXT   0x0000  // GadgetText points to IntuiText
#define GFLG_LABELSTRING  0x1000  // GadgetText points to (STRPTR)
#define GFLG_LABELIMAGE   0x2000  // GadgetText points to Image (object)
#define GFLG_TABCYCLE     0x0200  // Participates in Tab cycling activation
#define GFLG_STRINGEXTEND 0x0400  // this Gadget has StringExtend
#define GFLG_IMAGEDISABLE 0x0800  // Gadget's image can do disabled rendering
#define GFLG_EXTENDED     0x8000  // Gadget is extended
#define GACT_RELVERIFY    0x0001
#define GACT_IMMEDIATE    0x0002
#define GACT_ENDGADGET    0x0004
#define GACT_FOLLOWMOUSE  0x0008
#define GACT_RIGHTBORDER  0x0010
#define GACT_LEFTBORDER   0x0020
#define GACT_TOPBORDER    0x0040
#define GACT_BOTTOMBORDER 0x0080
#define GACT_TOGGLESELECT 0x0100  // toggle-select mode
#define GACT_BOOLEXTEND   0x2000  // this Gadget has a BoolInfo
#define GACT_STRINGLEFT   0x0000  // this has value zero
#define GACT_STRINGCENTER 0x0200
#define GACT_STRINGRIGHT  0x0400
#define GACT_LONGINT      0x0800  // this String Gadget is for Long Ints
#define GACT_ALTKEYMAP    0x1000  // this String has an alternate keymap
#define GACT_STRINGEXTEND 0x2000  // this String Gadget has StringExtend
#define GACT_ACTIVEGADGET 0x4000  // this gadget is "active"

typedef struct Gadget {
    struct Gadget *NextGadget;    // next gadget in the list
    int16_t  LeftEdge;            // "hit box" of gadget (x)
    int16_t  TopEdge;             // "hit box" of gadget (y)
    int16_t  Width;               // "hit box" of gadget (w)
    int16_t  Height;              // "hit box" of gadget (h)
    uint16_t Flags;               // gadget flags: GFLG_*
    uint16_t Activation;          // activation flags: GACT_*
    uint16_t GadgetType;          // see below for defines
    APTR     GadgetRender;
    APTR     SelectRender;
    struct IntuiText *GadgetText; // text for this gadget
    int32_t  MutualExclude;       // obsolete
    APTR     SpecialInfo;
    uint16_t GadgetID;            // user-definable ID field
    APTR     UserData;            // ptr to User data (ignored by Intuition)
} Gadget;

/* Gadtools internal root context Gadget */
typedef struct GadContext
{
    struct Gadget      gc_Gadget; // The actual gadget
    struct GadContext *gc_next;   // Next context gadget
    uint32_t           gc_flags;  // Context flags
} GadContext;

struct NewGadget
{
    int16_t         ng_LeftEdge;    // gadget position
    int16_t         ng_TopEdge;     // gadget position
    int16_t         ng_Width;       // gadget size
    int16_t         ng_Height;      // gadget size
    const char     *ng_GadgetText;  // gadget label (ref GA_DrawInfo for type)
    const TextAttr *ng_TextAttr;    // desired font for gadget label
    uint16_t        ng_GadgetID;    // gadget ID
    uint32_t        ng_Flags;       // see below
    APTR            ng_VisualInfo;  // Set to retval of GetVisualInfo()
    APTR            ng_UserData;    // gadget UserData
};

typedef struct StringExtend {
    struct TextFont *Font;           // must be an open Font (not TextAttr)
    uint8_t          Pens[2];        // color of text/background
    uint8_t          ActivePens[2];  // colors when gadget is active
    uint32_t         InitialModes;   // initial mode flags, below
    struct Hook     *EditHook;       // if non-NULL, must supply WorkBuffer
    char *           WorkBuffer;     // must be as large as StringInfo.Buffer
    uint32_t         Reserved[4];    // set to 0
} StringExtend;

/* StringInfo pointed to by Gadget.SpecialInfo */
typedef struct StringInfo {
    char   *Buffer;        // Buffer containing the start and final string
    char   *UndoBuffer;    // Optional buffer for undoing current entry
    int16_t BufferPos;     // Character position in Buffer
    int16_t MaxChars;      // Max number of chars in Buffer (including NULL)
    int16_t DispPos;       // buffer position of first displayed character
    int16_t UndoPos;       // Character position in the undo buffer
    int16_t NumChars;      // Number of characters currently in Buffer
    int16_t DispCount;     // Number of whole characters visible in Container
    int16_t CLeft;         // Left offset of the container
    int16_t CTop;          // Top offset of the container
    struct StringExtend *Extension;
    int32_t LongInt;
    struct KeyMap *AltKeyMap;
} StringInfo;

/* MxInfo pointed to by Gadget.SpecialInfo */
typedef struct MxInfo {
    uint8_t  mx_selected;   // Selected position (0...choices)
    uint8_t  mx_seldisplay; // Selected position to show (sync on mouse release)
    uint8_t  mx_scaled;     // Boolean whether object is scaled
    uint8_t  mx_max_len;    // Maximum select title length
    uint8_t  mx_unused;     // Unused
    uint16_t mx_count;      // Count of choices
    int16_t  mx_spacing;    // Pixels between FONT_HEIGHT choices
    uint16_t mx_sel_height; // Individual selection height
    uint16_t mx_sel_width;  // Individual selection width
    char   **mx_labels;     // Choice labels
} MxInfo;


void test_gadget(void);
void gadget_mouse_move(int x, int y);
void gadget_mouse_button(uint button, uint button_down);
void show_gadlist(Gadget *gad_list);

/* Amiga Exec API */
struct Message *WaitPort(struct MsgPort *port);

/* Amiga GadTools API */
struct Gadget *CreateContext(Gadget **gad_list);
IntuiMessage *GT_GetIMsg(struct MsgPort *port);
void GT_ReplyIMsg(IntuiMessage *imsg);
int32_t GT_GetGadgetAttrs(Gadget *gad, Window *win, Requester *req, ... );
void GT_SetGadgetAttrs(Gadget *gad, Window *win, Requester *req, ... );
void GT_RefreshWindow(Window *win, Requester *req);
struct Gadget *CreateGadget(uint32_t kind, Gadget *gad,
                            struct NewGadget *ng, ...);
void FreeGadgets(struct Gadget *gad);
int ActivateGadget(Gadget *gadget, Window *window, Requester *requester);
uint16_t AddGList(Window *window, Gadget *gadget, uint position, int numGad,
                  Requester *requester);
void RefreshGList(Gadget *gadgets, Window *window,
                  Requester *requester, uint numGad);
void DrawBevelBox(RastPort *rp, long left, long top, long width, long height,
                  Tag tag1, ...);

#endif /* _GADGET_H */
