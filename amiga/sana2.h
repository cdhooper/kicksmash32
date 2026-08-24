#ifndef SANA2_SANA2DEVICE_H
#define SANA2_SANA2DEVICE_H 1
/*
**	$Id: sana2.h,v 1.1 2003/08/06 15:15:40 obarthel Exp $
**	Includes Release 50.1
**
**	Structure definitions for SANA-II devices.
**
**	(C) Copyright 1991-2003 Amiga, Inc.
**	    All Rights Reserved
*/

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif

#ifndef EXEC_PORTS_H
#include <exec/ports.h>
#endif

#ifndef EXEC_IO_H
#include <exec/io.h>
#endif

#ifndef EXEC_TASKS_H
#include <exec/tasks.h>
#endif

#ifndef EXEC_ERRORS_H
#include <exec/errors.h>
#endif

#ifndef DEVICES_TIMER_H
#include <devices/timer.h>
#endif

#ifndef UTILITY_TAGITEM_H
#include <utility/tagitem.h>
#endif

#ifndef UTILITY_HOOKS_H
#include <utility/hooks.h>
#endif

/****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __GNUC__
   #ifdef __PPC__
    #pragma pack(2)
   #endif
#elif defined(__VBCC__)
   #pragma amiga-align
#endif

/****************************************************************************/

#define SANA2_MAX_ADDR_BITS  (128)
#define SANA2_MAX_ADDR_BYTES ((SANA2_MAX_ADDR_BITS+7)/8)

struct IOSana2Req
{
    struct IORequest ios2_Req;

    ULONG ios2_WireError;                     /* wire type specific error */
    ULONG ios2_PacketType;                    /* packet type */
    UBYTE ios2_SrcAddr[SANA2_MAX_ADDR_BYTES]; /* source address */
    UBYTE ios2_DstAddr[SANA2_MAX_ADDR_BYTES]; /* dest address */
    ULONG ios2_DataLength;                    /* length of packet data */
    APTR  ios2_Data;                          /* packet data */
    APTR  ios2_StatData;                      /* statistics data pointer */
    APTR  ios2_BufferManagement;              /* see SANA-II OpenDevice adoc */
};

/*
** Defines for the io_Flags field
*/
#define SANA2IOB_RAW   (7)         /* raw packet IO requested */
#define SANA2IOB_BCAST (6)         /* broadcast packet (received) */
#define SANA2IOB_MCAST (5)         /* multicast packet (received) */
#define SANA2IOB_QUICK (IOB_QUICK) /* quick IO requested (0) */

#define SANA2IOF_RAW   (1<<SANA2IOB_RAW)
#define SANA2IOF_BCAST (1<<SANA2IOB_BCAST)
#define SANA2IOF_MCAST (1<<SANA2IOB_MCAST)
#define SANA2IOF_QUICK (IOF_QUICK)

/*
** Defines for OpenDevice() flags
*/
#define SANA2OPB_MINE (0) /* exclusive access requested */
#define SANA2OPB_PROM (1) /* promiscuous mode requested */

#define SANA2OPF_MINE (1<<SANA2OPB_MINE)
#define SANA2OPF_PROM (1<<SANA2OPB_PROM)

/*
** Defines for OpenDevice() tags
*/
#define S2_Dummy             (TAG_USER + 0xB0000)

#define S2_CopyToBuff        (S2_Dummy +  1)
#define S2_CopyFromBuff      (S2_Dummy +  2)
#define S2_PacketFilter      (S2_Dummy +  3)
#define S2_CopyToBuff16      (S2_Dummy +  4)
#define S2_CopyFromBuff16    (S2_Dummy +  5)
#define S2_CopyToBuff32      (S2_Dummy +  6)
#define S2_CopyFromBuff32    (S2_Dummy +  7)
#define S2_DMACopyToBuff32   (S2_Dummy +  8)
#define S2_DMACopyFromBuff32 (S2_Dummy +  9)
#define S2_DMACopyToBuff64   (S2_Dummy + 10)
#define S2_DMACopyFromBuff64 (S2_Dummy + 11)
#define S2_Log               (S2_Dummy + 12)

/*
** Filled in by 'S2_DEVICEQUERY'
*/
struct Sana2DeviceQuery
{
    /*
    ** Standard information
    */
    ULONG   SizeAvailable;  /* bytes available */
    ULONG   SizeSupplied;   /* bytes supplied */
    ULONG   DevQueryFormat; /* this is type 0 */
    ULONG   DeviceLevel;    /* this document is level 0 */
    /*
    ** Common information
    */
    UWORD   AddrFieldSize;  /* address size in bits */
    ULONG   MTU;            /* maximum packet data size */
    ULONG   BPS;            /* line rate (bits/sec) */
    ULONG   HardwareType;   /* what the wire is (see below) */
    ULONG   RawMTU;         /* maximum raw packet data size */
    /*
    ** Format specific information
    */
};

/*
** Defined Hardware types
**
**  If your hardware type is not listed below contact Amiga to get
**  a new type number added for your hardware.
*/
#define S2WireType_Ethernet    1
#define S2WireType_IEEE802     6
#define S2WireType_Arcnet      7
#define S2WireType_LocalTalk  11
#define S2WireType_DyLAN      12

#define S2WireType_AmokNet   200 /* Amiga Floppy Port hardware */

#define S2WireType_Liana     202 /* Village Tronic parallel port hw */

#define S2WireType_PPP       253
#define S2WireType_SLIP      254
#define S2WireType_CSLIP     255 /* Compressed SLIP */

#define S2WireType_PLIP      420 /* SLIP over a parallel port */

/*
** Filled in by 'S2_GETTYPESTATS'
*/
struct Sana2PacketTypeStats
{
    ULONG PacketsSent;     /* transmitted count */
    ULONG PacketsReceived; /* received count */
    ULONG BytesSent;       /* bytes transmitted count */
    ULONG BytesReceived;   /* bytes received count */
    ULONG PacketsDropped;  /* packets dropped count */
};

/*
** Filled in by 'S2_GETSPECIALSTATS'
*/
struct Sana2SpecialStatRecord
{
    ULONG  Type;   /* statistic identifier */
    ULONG  Count;  /* the statistic */
    STRPTR String; /* statistic name */
};

struct Sana2SpecialStatHeader
{
    ULONG RecordCountMax;      /* room available */
    ULONG RecordCountSupplied; /* number supplied */
    /* struct Sana2SpecialStatRecord[RecordCountMax]; */
};

/*
** Filled in by 'S2_GETGLOBALSTATS'
*/
struct Sana2DeviceStats
{
    ULONG          PacketsReceived;      /* received count */
    ULONG          PacketsSent;          /* transmitted count */
    ULONG          BadData;              /* bad packets received */
    ULONG          Overruns;             /* hardware miss count */
    ULONG          Unused;               /* Unused field */
    ULONG          UnknownTypesReceived; /* orphan count */
    ULONG          Reconfigurations;     /* network reconfigurations */
    struct timeval LastStart;            /* time of last online */
};

/*
** A 64 bit big endian integer in ISO 'C' terms.
*/
typedef struct
{
    ULONG s2q_High;
    ULONG s2q_Low;
} S2QUAD;

/*
** Revised version, filled in by 'S2_GETEXTENDEDGLOBALSTATS'
*/
struct Sana2ExtDeviceStats
{
    ULONG          s2xds_Length;
    ULONG          s2xds_Actual;

    S2QUAD         s2xds_PacketsReceived;
    S2QUAD         s2xds_PacketsSent;
    S2QUAD         s2xds_BadData;
    S2QUAD         s2xds_Overruns;
    S2QUAD         s2xds_UnknownTypesReceived;
    S2QUAD         s2xds_Reconfigurations;
    struct timeval s2xds_LastStart;

    struct timeval s2xds_LastConnected;
    struct timeval s2xds_LastDisconnected;
    struct timeval s2xds_TimeConnected;
};

/*
** Used by 'S2_CONNECT' and 'S2_DISCONNECT'
*/
struct Sana2Connection
{
   ULONG          s2c_Size;
   struct MinList s2c_Options;
   struct Hook    s2c_ErrorHook;
   struct Hook    s2c_ConnectHook;
   struct Hook    s2c_DisconnectHook;
   STRPTR         s2c_Login;
   STRPTR         s2c_Password;
};

struct Sana2ConnectionOption
{
   struct MinNode s2co_MinNode;
   STRPTR         s2co_Name;
   STRPTR         s2co_Value;
};

struct Sana2ConnectionMessage
{
   ULONG                    s2cm_Size;
   struct Sana2Connection * s2cm_Connection;
   struct IORequest *       s2cm_Request[2];
   LONG                     s2cm_RequestType;
};

/*
** Used by 'S2_SAMPLE_THROUGHPUT'
*/
struct Sana2ThroughputStats
{
   ULONG          s2ts_Length;
   ULONG          s2ts_Actual;

   struct Task *  s2ts_NotifyTask;
   ULONG          s2ts_NotifyMask;

   struct timeval s2ts_StartTime;
   struct timeval s2ts_EndTime;
   S2QUAD         s2ts_BytesSent;
   S2QUAD         s2ts_BytesReceived;
   S2QUAD         s2ts_Updates;
};

/*
** Used by the 'S2_Log' callback hook
*/
struct S2LogMessage
{
   LONG   s2lm_Size;
   ULONG  s2lm_Priority;
   STRPTR s2lm_Name;
   STRPTR s2lm_Message;
};

/*
** Log priority levels used by the 'S2_Log' callback hook
*/
#define S2LOG_Emergency   0 /* A panic condition */
#define S2LOG_Alert       1 /* A condition that should be corrected
                               immediately */
#define S2LOG_Critical    2 /* Critical conditions */
#define S2LOG_Error       3 /* A plain error */
#define S2LOG_Warning     4 /* A warning message */
#define S2LOG_Notice      5 /* Conditions that are not error conditions,
                               but should possibly be handled specially */
#define S2LOG_Information 6 /* An informational message */
#define S2LOG_Debug       7 /* Generated only in debugging mode */

/*
** Device Commands
*/
#define S2_START               (CMD_NONSTD)  /* First old style command */

#define S2_DEVICEQUERY         (S2_START+ 0)
#define S2_GETSTATIONADDRESS   (S2_START+ 1)
#define S2_CONFIGINTERFACE     (S2_START+ 2)
#define S2_ADDMULTICASTADDRESS (S2_START+ 5)
#define S2_DELMULTICASTADDRESS (S2_START+ 6)
#define S2_MULTICAST           (S2_START+ 7)
#define S2_BROADCAST           (S2_START+ 8)
#define S2_TRACKTYPE           (S2_START+ 9)
#define S2_UNTRACKTYPE         (S2_START+10)
#define S2_GETTYPESTATS        (S2_START+11)
#define S2_GETSPECIALSTATS     (S2_START+12)
#define S2_GETGLOBALSTATS      (S2_START+13)
#define S2_ONEVENT             (S2_START+14)
#define S2_READORPHAN          (S2_START+15)
#define S2_ONLINE              (S2_START+16)
#define S2_OFFLINE             (S2_START+17)

#define S2_END                 (S2_START+18) /* Last old style command */

/*
** New style commands introduced in 1997 and beyond
*/
#define S2_ADDMULTICASTADDRESSES  0xC000
#define S2_DELMULTICASTADDRESSES  0xC001
#define S2_GETPEERADDRESS         0xC002
#define S2_GETDNSADDRESS          0xC003
#define S2_GETEXTENDEDGLOBALSTATS 0xC004
#define S2_CONNECT                0xC005
#define S2_DISCONNECT             0xC006
#define S2_SAMPLE_THROUGHPUT      0xC007
#define S2_SANA2HOOK              0xC008

/*
** Defined errors for io_Error (see also <exec/errors.h>)
*/
#define S2ERR_NO_ERROR       0 /* peachy-keen */
#define S2ERR_NO_RESOURCES   1 /* resource allocation failure  */
#define S2ERR_BAD_ARGUMENT   3 /* garbage somewhere */
#define S2ERR_BAD_STATE      4 /* inappropriate state */
#define S2ERR_BAD_ADDRESS    5 /* who? */
#define S2ERR_MTU_EXCEEDED   6 /* too much to chew */
#define S2ERR_NOT_SUPPORTED  8 /* hardware can't support cmd */
#define S2ERR_SOFTWARE       9 /* software error detected */
#define S2ERR_OUTOFSERVICE  10 /* driver is OFFLINE */
#define S2ERR_TX_FAILURE    11 /* Transmission attempt failed */

/*
** From <exec/errors.h>
**
**  IOERR_OPENFAIL   (-1) * device/unit failed to open *
**  IOERR_ABORTED    (-2) * request terminated early [after AbortIO()] *
**  IOERR_NOCMD      (-3) * command not supported by device *
**  IOERR_BADLENGTH  (-4) * not a valid length (usually IO_LENGTH) *
**  IOERR_BADADDRESS (-5) * invalid address (misaligned or bad range) *
**  IOERR_UNITBUSY   (-6) * device opens ok, but requested unit is busy *
**  IOERR_SELFTEST   (-7) * hardware failed self-test *
*/
#define IOERR_OPENED (-8)

/*
** Defined errors for ios2_WireError
*/
#define S2WERR_GENERIC_ERROR      0 /* no specific info available */
#define S2WERR_NOT_CONFIGURED     1 /* unit not configured */
#define S2WERR_UNIT_ONLINE        2 /* unit is currently online */
#define S2WERR_UNIT_OFFLINE       3 /* unit is currently offline */
#define S2WERR_ALREADY_TRACKED    4 /* protocol already tracked */
#define S2WERR_NOT_TRACKED        5 /* protocol not tracked */
#define S2WERR_BUFF_ERROR         6 /* buff mgt func returned error */
#define S2WERR_SRC_ADDRESS        7 /* source address problem */
#define S2WERR_DST_ADDRESS        8 /* destination address problem */
#define S2WERR_BAD_BROADCAST      9 /* broadcast address problem */
#define S2WERR_BAD_MULTICAST     10 /* multicast address problem */
#define S2WERR_MULTICAST_FULL    11 /* multicast address list full */
#define S2WERR_BAD_EVENT         12 /* unsupported event class */
#define S2WERR_BAD_STATDATA      13 /* statdata failed sanity check */
/*** THERE IS NO WIRE ERROR CODE 14 ***/
#define S2WERR_IS_CONFIGURED     15 /* attempt to config twice */
#define S2WERR_NULL_POINTER      16 /* null pointer detected */
#define S2WERR_TOO_MANY_RETRIES  17 /* tx failed - too many retries */
#define S2WERR_RCVREL_HDW_ERR    18 /* Driver fixable HW error */
#define S2WERR_UNIT_DISCONNECTED 19 /* unit is currently not connected */
#define S2WERR_UNIT_CONNECTED    20 /* unit is currently connected */
#define S2WERR_INVALID_OPTION    21 /* invalid option rejected */
#define S2WERR_MISSING_OPTION    22 /* a mandatory option is missing */
#define S2WERR_AUTHENTICATION_FAILED 23 /* could not log in */
/*
** For our dsylexic friends
*/
#define S2WERR_TOO_MANY_RETIRES S2WERR_TOO_MANY_RETRIES

/*
** Defined events
*/
#define S2EVENT_ERROR         (1UL<< 0) /* error catch all */
#define S2EVENT_TX            (1UL<< 1) /* transmitter error catch all  */
#define S2EVENT_RX            (1UL<< 2) /* receiver error catch all */
#define S2EVENT_ONLINE        (1UL<< 3) /* unit is in service */
#define S2EVENT_OFFLINE       (1UL<< 4) /* unit is not in service */
#define S2EVENT_BUFF          (1UL<< 5) /* buff mgt function error */
#define S2EVENT_HARDWARE      (1UL<< 6) /* hardware error catch all */
#define S2EVENT_SOFTWARE      (1UL<< 7) /* software error catch all */
#define S2EVENT_CONFIGCHANGED (1UL<< 8) /* driver configuration changed */
#define S2EVENT_CONNECT       (1UL<< 9) /* driver has opened session */
#define S2EVENT_DISCONNECT    (1UL<<10) /* driver has closed session */

 
struct Sana2Hook
{
	struct Hook s2h_Hook;
	Tag*   s2h_Methods;
};

/****************************************************************************/

#ifdef __GNUC__
   #ifdef __PPC__
    #pragma pack()
   #endif
#elif defined(__VBCC__)
   #pragma default-align
#endif

#ifdef __cplusplus
}
#endif

/****************************************************************************/

#endif  /* SANA2_SANA2DEVICE_H */
