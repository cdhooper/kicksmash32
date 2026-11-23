#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <string.h>
#include <dos/dos.h>
#include <dos/dostags.h>      // NP_Entry, etc
#include <dos/dosextens.h>    // struct Process
#include <utility/utility.h>  // UTILITYNAME

#include <exec/execbase.h>
#include <libraries/configvars.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <libraries/filehandler.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <clib/alib_protos.h>

#include <inline/exec.h>
#include <inline/dos.h>
#include <inline/macros.h>

#include <clib/debug_protos.h>
#include "printf.h"
#include "sm_msg.h"
#include "fs_vol.h"
#include "cpu_control.h"

#define ROM_VERSION 1

#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))

#define RawPutChar(___c) \
        LP1NR(0x204, RawPutChar , UBYTE, ___c, d0, , SysBase)

extern struct DosLibrary *DOSBase;
// extern struct ExecBase *SysBase;

// #define rom_end _end
void rom_end(void);  // XXX: Not the real end; _end doesn't seem to work
void rom_main(struct ExecBase *SysBase asm("a6"));
void rom_boot_init(struct ExecBase *SysBase asm("a6"));
void rom_handler_entry(void);
extern const char SmashRomName[];
extern const char SmashRomID[];

struct Task *my_CreateTask(CONST_STRPTR name,LONG pri, CONST APTR init_pc,
                           ULONG stack_size, void *userdata);

/*
 * Two resident modules:
 *   smashrom   (after DOS) starts smashfs, unless DOS already started it
 *              as the handler of a smashboot boot node.
 *   smashboot  (cold start, after expansion.library, before the boot
 *              menu and strap) adds a boot node for each volume which
 *              the host reports as bootable (hostsmash -m vol:bootpri=N).
 * The first one's rt_EndSkip points at the second, so that Kickstart's
 * ROM scan finds both.
 */
const struct Resident resident[2] = {
    {
        RTC_MATCHWORD,         // rt_MatchWord - word to match on (ILLEGAL)
        (void *) &resident[0], // rt_MatchTag - pointer to the above
        (void *) &resident[1], // rt_EndSkip - address to continue scan
        RTF_AFTERDOS,          // rt_Flags - various tag flags
        ROM_VERSION,           // rt_Version - release version number
        NT_UNKNOWN,            // rt_Type - type of module
        5,                     // rt_Pri - initialization priority
        (char *) "smashrom",   // rt_Name - pointer to node name
        (char *) SmashRomID,   // rt_IdString - pointer to identification
        rom_main               // rt_Init - pointer to init code
    },
    {
        RTC_MATCHWORD,
        (void *) &resident[1],
        rom_end,
        RTF_COLDSTART,
        ROM_VERSION,
        NT_UNKNOWN,
        -40,                   // After expansion; before bootmenu and strap
        (char *) "smashboot",
        (char *) SmashRomID,
        rom_boot_init
    },
};

/*
 * Handler "seglist" shared by all smashboot device nodes: a seglist
 * header (next segment) followed by a jump to rom_handler_entry().
 * DOS starts the handler from it, as with any C handler (dn_GlobalVec
 * -1). ss_claimed is set by whichever starts smashfs first: DOS (for a
 * boot node) or the smashrom module.
 */
typedef struct {
    BPTR    ss_next;     // Seglist next segment (none)
    UWORD   ss_jmp;      // JMP abs.l
    void   *ss_entry;    // rom_handler_entry
    UWORD   ss_claimed;  // smashfs has been started
} smashboot_stub_t;

#define SMASHBOOT_STUB(dn) ((smashboot_stub_t *) BADDR((dn)->dn_SegList))

/*
 * The fake ConfigDev's BootPoint (for strap), as in a boot block:
 * initialize dos.library, which then boots from the highest priority
 * boot node.
 */
__asm("    .text\n"
      "    .globl _smashboot_bootpoint\n"
      "    .globl _smashboot_bootpoint_end\n"
      "_smashboot_bootpoint:\n"
      "    lea     _smashboot_dosname(pc),a1\n"
      "    jsr     -96(a6)\n"        // FindResident
      "    tst.l   d0\n"
      "    beq.s   _smashboot_bp_fail\n"
      "    move.l  d0,a0\n"
      "    move.l  22(a0),a0\n"      // RT_INIT
      "    jsr     (a0)\n"
      "_smashboot_bp_fail:\n"
      "    rts\n"
      "_smashboot_dosname:\n"
      "    .asciz  \"dos.library\"\n"
      "    .even\n"
      "_smashboot_bootpoint_end:\n");
extern const UBYTE smashboot_bootpoint[];
extern const UBYTE smashboot_bootpoint_end[];

/*
 * call_a4() calls fn(arg) with a4 set to the specified globals pointer,
 * preserving the caller's a4. Returns fn()'s return value.
 */
__asm("    .text\n"
      "    .globl _call_a4\n"
      "_call_a4:\n"
      "    move.l  a4,-(sp)\n"
      "    move.l  8(sp),a4\n"       // globals
      "    move.l  16(sp),-(sp)\n"   // arg
      "    move.l  16(sp),a0\n"      // fn
      "    jsr     (a0)\n"
      "    addq.l  #4,sp\n"
      "    move.l  (sp)+,a4\n"
      "    rts\n");
int call_a4(void *globals, int (*fn)(void *arg), void *arg);

const char SmashRomID[]   = "smashrom "VERSION" ("BUILD_DATE")\r\n";

#define ADDR32(x)   ((volatile uint32_t *) ((uintptr_t)(x)))
typedef unsigned int uint;

void
dputs(const char *str)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    struct ExecBase *SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop

    while (*str != '\0')
        RawPutChar(*str++);
}

void
dputx(uint x)
{
    uint digit;
    char buf[32];
    char *ptr = buf + sizeof (buf) - 1;
    *ptr = '\0';
    for (digit = 0; digit < 8; digit++) {
        *(--ptr) = "0123456789abcdef"[x & 0xf];
        x >>= 4;
    }
    dputs(ptr);
}

uint8_t *copy_to_ram_ptr;
static void
sm_msg_copy_to_ram(void)
{
    uint copy_to_ram_start;
    uint copy_to_ram_end;
    __asm("lea _copy_to_ram_start,%0" : "=a" (copy_to_ram_start) ::);
    __asm("lea _copy_to_ram_end,%0" : "=a" (copy_to_ram_end) ::);

    uint len = copy_to_ram_end - copy_to_ram_start;
    printf("len=%x s=%x e=%x\n", len, copy_to_ram_start, copy_to_ram_end);
    copy_to_ram_ptr = AllocVec(len, MEMF_PUBLIC);
    if (copy_to_ram_ptr == NULL) {
        dputs("AllocMem fail 1\n");
        return;
    }
    memcpy(copy_to_ram_ptr, (void *) copy_to_ram_start, len);
    lcrc32_init();
    esend_cmd_core = (void *) (copy_to_ram_ptr +
                               (uintptr_t) send_cmd_core - copy_to_ram_start);
}

int
call_main(void)
{
    /* Globals are now available */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop

//  RawPutChar('y');
    DOSBase = (struct DosLibrary *) OpenLibrary(DOSNAME, 0);
    if (DOSBase == NULL) {
        dputs("NULL DOSBase\n");
        return (1);
    }

//  RawPutChar('_');
#undef DEBUG_ENTRY
#ifdef DEBUG_ENTRY
    *ADDR32(0x0777000c) = (uintptr_t) SysBase;
    *ADDR32(0x07770010) = (uintptr_t) DOSBase;
#endif

    /* Make smashfs debug output go to serial port */
    extern uint8_t flag_output;
    flag_output = 2;

    if (SysBase == NULL) {
        dputs("NULL SysBase\n");
    }
#if 0
    __asm("move.l a0,-(sp)");
    __asm("move.l a4,0x7770014");
    __asm("move.l %0,a0" :: "r" (&DOSBase));
    __asm("move.l a0,0x7770018");
    __asm("move.l (sp)+,a0");
#endif

    if (DOSBase == NULL) {
        dputs("NULL DOSBase 3\n");
        return (1);
    }

    if (DOSBase != NULL) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }

    /* Move KS communication code to RAM, as it needs to run from there */
    sm_msg_copy_to_ram();

    /* malloc/free() library Constructors / Destructors */
    void __ctor_stdlib_memory_init(void *);
    void __dtor_stdlib_memory_exit(void *);
    __ctor_stdlib_memory_init(SysBase);

    dputs(" call main\n");
    extern int main(int argc, char *argv[]);
    char *args[] = { NULL, "-dd" };

    /* Execute command line main() */
    int rc = main(ARRAY_SIZE(args), args);

    __dtor_stdlib_memory_exit(SysBase);
    FreeVec(copy_to_ram_ptr);
    return (rc);
#if 0
    if (DOSBase != NULL) {
        DOSBase = (struct DosLibrary *) OpenLibrary(DOSNAME, 34);
        Close(fh_i);
        Close(fh_o);
        Close(fh_e);
        CloseLibrary((struct Library *)DOSBase);
    }
#endif
}

int __regargs
rom_process_entry(void)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    struct ExecBase *SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop
    struct Process *proc;
    uint8_t *globals;
    int rc;

//  RawPutChar('e');
    proc = (struct Process *) FindTask(NULL);
    if (proc == NULL)
        return (0);

//  RawPutChar('n');
    globals = (uint8_t *) proc->pr_ExitData;
    if (globals == NULL)
        return (0);

//  RawPutChar('t');
    /* Got globals pointer */
#ifdef DEBUG_ENTRY
    *ADDR32(0x07770020) = (uintptr_t) globals;
#endif
    __asm("move.l %0,a4" :: "r" (globals));

    /* Globals are now available */
//  RawPutChar('r');
// __lib_init(SysBase);

    extern struct Library * __UtilityBase;
    __UtilityBase = OpenLibrary(UTILITYNAME, 0);

    rc = call_main();
    Forbid();
    return (rc);
}

/*
 * globals_alloc
 * -------------
 * Allocates and initializes a copy of the data and BSS segments for one
 * smashfs instance. Returns the value for a4 (-fbaserel), or NULL.
 */
static uint8_t *
globals_alloc(struct ExecBase *SysBase)
{
    uint8_t *globals;
    void    *data_start;
    uint     data_size;
    uint     bss_size;
    __asm("lea __sdata,%0"      : "=a" (data_start) ::);
    __asm("lea ___data_size,%0" : "=a" (data_size) ::);
    __asm("lea ___bss_size,%0"  : "=a" (bss_size) ::);

    if ((globals = AllocVec(data_size + bss_size, MEMF_PUBLIC)) == NULL) {
        dputs("AllocMem fail 2\n");
        return (NULL);
    }

    /* Must compile with -fbaserel so a4 will be used for globals */
    memcpy(globals, data_start, data_size);
    memset(globals + data_size, 0, bss_size);

    return (globals + 0x7ffe);  // offset gcc applies to a4-relative globals
}

/*
 * smashboot_dev
 * -------------
 * Returns the device node as a smashboot_dev_t if it was created by
 * smashboot_add(); otherwise NULL. Does not use globals.
 */
smashboot_dev_t *
smashboot_dev(struct DeviceNode *dn)
{
    smashboot_stub_t *st;

    if ((dn == NULL) || (dn->dn_SegList == 0))
        return (NULL);
    st = SMASHBOOT_STUB(dn);
    if ((st->ss_jmp != 0x4ef9) || (st->ss_entry != rom_handler_entry))
        return (NULL);
    return ((smashboot_dev_t *) dn);
}

/* fssm_Device: there is no such device; nothing should need to open it */
static const char smashboot_devname[] __attribute__((aligned(4))) =
    "\016smashfs.device";

static smashboot_stub_t *smashboot_stub;
static struct ConfigDev *smashboot_cd;

/*
 * smashboot_add
 * -------------
 * Called (from refresh_volume_list()) for each bootable volume before DOS
 * starts. Creates a device node for the volume and adds it to the
 * expansion MountList with AddBootNode(), so that DOS will boot from it
 * if it has the highest boot priority. AddBootNode() requires a
 * ConfigDev; the fake one has a DiagArea BootPoint which starts DOS.
 */
void
smashboot_add(const char *name, int bootpri)
{
    struct ExpansionBase *ExpansionBase;
    smashboot_dev_t      *sbd;
    struct DeviceNode    *dn;
    uint                  len;

    ExpansionBase = (struct ExpansionBase *)
                    OpenLibrary("expansion.library", 37);
    if (ExpansionBase == NULL)
        return;

    if (smashboot_cd == NULL) {
        uint             bplen = smashboot_bootpoint_end - smashboot_bootpoint;
        struct DiagArea *da;
        struct ConfigDev *cd;

        smashboot_stub = AllocMem(sizeof (*smashboot_stub),
                                  MEMF_PUBLIC | MEMF_CLEAR);
        da = AllocMem(sizeof (*da) + bplen, MEMF_PUBLIC | MEMF_CLEAR);
        cd = AllocConfigDev();
        if ((smashboot_stub == NULL) || (da == NULL) || (cd == NULL))
            goto add_fail;  // Small leak, but boot will likely fail anyway
        smashboot_stub->ss_jmp   = 0x4ef9;
        smashboot_stub->ss_entry = rom_handler_entry;

        da->da_Config    = DAC_CONFIGTIME;
        da->da_BootPoint = sizeof (*da);
        da->da_Size      = sizeof (*da) + bplen;
        memcpy(da + 1, smashboot_bootpoint, bplen);
        CacheClearU();

        cd->cd_BoardAddr = da;
        cd->cd_BoardSize = da->da_Size;
        cd->cd_Rom.er_Type = ERTF_DIAGVALID;
        *(struct DiagArea **) &cd->cd_Rom.er_Reserved0c = da;
        smashboot_cd = cd;
    }

    sbd = AllocMem(sizeof (*sbd), MEMF_PUBLIC | MEMF_CLEAR);
    if (sbd == NULL)
        goto add_fail;
    strcpy(sbd->sbd_hostname, name);

    /* DOS name: drop the trailing colon; others and spaces become '_' */
    for (len = 0; name[len] != '\0'; len++) {
        char ch = name[len];
        if ((ch == ':') && (name[len + 1] == '\0'))
            break;
        if ((ch == ':') || (ch == ' '))
            ch = '_';
        sbd->sbd_bname[len + 1] = ch;
    }
    sbd->sbd_bname[0] = len;

    /*
     * Startup message and environment, only so that the boot menu can
     * show the DOS type. No boot blocks: strap uses the BootPoint.
     */
    sbd->sbd_env[DE_TABLESIZE]    = DE_DOSTYPE;
    sbd->sbd_env[DE_SIZEBLOCK]    = 128;
    sbd->sbd_env[DE_NUMHEADS]     = 1;
    sbd->sbd_env[DE_SECSPERBLK]   = 1;
    sbd->sbd_env[DE_BLKSPERTRACK] = 1;
    sbd->sbd_env[DE_BOOTPRI]      = bootpri;
    sbd->sbd_env[DE_DOSTYPE]      = ID_SMASHBOOT_DOSTYPE;
    sbd->sbd_fssm.fssm_Device     = MKBADDR(smashboot_devname);
    sbd->sbd_fssm.fssm_Environ    = MKBADDR(sbd->sbd_env);

    dn = &sbd->sbd_dn;
    dn->dn_Startup   = MKBADDR(&sbd->sbd_fssm);
    dn->dn_Type      = DLT_DEVICE;
    dn->dn_StackSize = 8192;
    dn->dn_Priority  = -1;
    dn->dn_SegList   = MKBADDR(smashboot_stub);
    dn->dn_GlobalVec = -1;  // C handler: no BCPL global vector
    dn->dn_Name      = MKBADDR(sbd->sbd_bname);
    if (!AddBootNode(bootpri, ADNF_STARTPROC, dn, smashboot_cd))
        FreeMem(sbd, sizeof (*sbd));
add_fail:
    CloseLibrary((struct Library *) ExpansionBase);
}

/*
 * smashboot_scan
 * --------------
 * Asks the host for its volume list, and adds a boot node for each
 * bootable volume. Globals must be set up by the caller.
 */
static int
smashboot_scan(void *arg)
{
    extern struct Library *__UtilityBase;
    extern uint8_t flag_output;
    int refresh_volume_list(uint boot_scan);
    void __ctor_stdlib_memory_init(void *);
    void __dtor_stdlib_memory_exit(void *);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop
    flag_output = 2;  // Debug output (if any) goes to the serial port
    __UtilityBase = OpenLibrary(UTILITYNAME, 0);
    __ctor_stdlib_memory_init(SysBase);
    cpu_control_init();

    sm_msg_copy_to_ram();
    if (copy_to_ram_ptr != NULL) {
        refresh_volume_list(1);
        host_msg_exit();
        FreeVec(copy_to_ram_ptr);
    }
    __dtor_stdlib_memory_exit(SysBase);
    CloseLibrary(__UtilityBase);
    (void) arg;
    return (0);
}

/*
 * rom_boot_init
 * -------------
 * Entry for the smashboot ROM module (cold start, before DOS), called by
 * exec startup code. The a6 register is SysBase.
 */
void
rom_boot_init(struct ExecBase *SysBase asm("a6"))
{
    uint8_t *globals = globals_alloc(SysBase);

    if (globals == NULL)
        return;
    call_a4(globals, smashboot_scan, NULL);
    FreeVec(globals - 0x7ffe);
}

/*
 * rom_handler_main
 * ----------------
 * smashfs main() for a process started by DOS as a boot node's handler.
 */
static int
rom_handler_main(void *pkt)
{
    extern struct Library * __UtilityBase;
    extern struct DosPacket *rom_startup_pkt;

    /* The global SysBase in the new globals is not yet set */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop
    __UtilityBase = OpenLibrary(UTILITYNAME, 0);
    rom_startup_pkt = pkt;  // main() replies to it
    return (call_main());
}

/*
 * rom_handler_entry
 * -----------------
 * Entry (from the smashboot stub) of a handler process which DOS started
 * for a smashboot device node. The first such process becomes smashfs,
 * serving all volumes. If smashfs is already running, it will take over
 * the device node, so this one just fails the startup packet.
 */
void
rom_handler_entry(void)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    struct ExecBase *SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop
    struct Process    *proc = (struct Process *) FindTask(NULL);
    struct MsgPort    *port;
    struct DosPacket  *pkt;
    struct DeviceNode *dn;
    smashboot_stub_t  *st;
    uint8_t           *globals;
    UWORD              claimed;

    WaitPort(&proc->pr_MsgPort);
    pkt = (struct DosPacket *)
          GetMsg(&proc->pr_MsgPort)->mn_Node.ln_Name;
    dn = (struct DeviceNode *) BADDR(pkt->dp_Arg3);
    st = SMASHBOOT_STUB(dn);

    Forbid();
    claimed = st->ss_claimed;
    st->ss_claimed = 1;
    Permit();

    if (claimed == 0) {
        globals = globals_alloc(SysBase);
        if (globals != NULL) {
            call_a4(globals, rom_handler_main, pkt);
            return;
        }
        st->ss_claimed = 0;
    }

    /* Reply to the startup packet: fail */
    pkt->dp_Res1 = DOSFALSE;
    pkt->dp_Res2 = ERROR_OBJECT_IN_USE;
    port = pkt->dp_Port;
    pkt->dp_Port = &proc->pr_MsgPort;
    PutMsg(port, pkt->dp_Link);
}

/*
 * smashboot_claimed
 * -----------------
 * Returns non-zero if smashfs was already started by DOS as the handler
 * of a smashboot device node. Otherwise claims the handler stub (if
 * any), so that a later DOS start of the handler won't run a second
 * smashfs. Does not use globals.
 */
static UWORD
smashboot_claimed(struct ExecBase *SysBase)
{
    struct ExpansionBase *ExpansionBase;
    struct BootNode      *bn;
    UWORD                 claimed = 0;

    ExpansionBase = (struct ExpansionBase *)
                    OpenLibrary("expansion.library", 37);
    if (ExpansionBase == NULL)
        return (0);
    Forbid();
    for (bn = (struct BootNode *) ExpansionBase->MountList.lh_Head;
         bn->bn_Node.ln_Succ != NULL;
         bn = (struct BootNode *) bn->bn_Node.ln_Succ) {
        struct DeviceNode *dn = bn->bn_DeviceNode;
        if (smashboot_dev(dn) != NULL) {
            smashboot_stub_t *st = SMASHBOOT_STUB(dn);
            claimed = st->ss_claimed;
            st->ss_claimed = 1;
            break;
        }
    }
    Permit();
    CloseLibrary((struct Library *) ExpansionBase);
    return (claimed);
}

int
rom_task_entry(void)
{
    uint8_t *globals;
    struct Task *task;
    struct Process *child;
    struct DosLibrary *DOSBase;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    struct ExecBase *SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop
    __asm("movem.l d0-d7/a0-a6,-(sp)");
//  RawPutChar('e');

    task = FindTask(NULL);
    globals = task->tc_UserData;
    __asm("move.l %0,a4" :: "r" (globals));   // Point a4 at globals

    DOSBase = (struct DosLibrary *) OpenLibrary(DOSNAME, 34);
    if (DOSBase == NULL) {
        dputs("NULL DOSBase 2\n");
        goto task_entry_fail;
    }

    /*
     * For an unknown reason, if the CreateNewProcTags() is called from
     * rom_main(), then the caller will crash.
     * XXX: Maybe it won't now that the malloc() bug is fixed.
     */
    child = CreateNewProcTags(NP_Entry, (ULONG) rom_process_entry,
                              NP_Name, (ULONG) "smashfs",
                              NP_StackSize, 8192,
                              NP_Priority, -1,
                              NP_CloseInput, FALSE,
                              NP_CloseOutput, FALSE,
                              NP_CopyVars, FALSE,
                              NP_ExitData, (ULONG) globals,
//                            NP_Path, 0,
//                            NP_Input, 0,      // Default is to open NIL:
//                            NP_Output, 0,     // Default is to open NIL:
//                            NP_Error, 0,      // Default is NULL
//                            NP_WindowPtr, 0,  // Workbench window (task=NULL)
//                            NP_HomeDir, 0,    // Default is NULL from task
//                            NP_CurrentDir, 0,  // Default is NULL from task
//                            NP_FreeSeglist, FALSE, // Only valid if NP_Seglist
//                            NP_ConsoleTask, msgport,
//                            NP_Cli, TRUE,  // Parent env gets copied
                              TAG_END);

    CloseLibrary((struct Library *)DOSBase);
    if (child == NULL)
        dputs("Failed to start smashfs process\n");

task_entry_fail:
    __asm("movem.l (sp)+,d0-d7/a0-a6");
    return (0);
}

/*
 * rom_main
 * --------
 * Entry for ROM module, called by exec startup code.
 * The a6 register is SysBase
 */
void
rom_main(struct ExecBase *SysBase asm("a6"))
{
//  struct ExecBase *SysBase;
    uint8_t *globals;

//  __asm("movem.l d0-d7/a0-a6,-(sp)");

    /* Get local SysBase */
//  __asm("move.l a6,%0" : "=a" (SysBase) ::);  // SysBase provided in a6

#ifdef DEBUG_ENTRY
    *ADDR32(0x07770040) = (uintptr_t) rom_main;
    *ADDR32(0x07770044) = (uintptr_t) SysBase;
#endif

    if (smashboot_claimed(SysBase))
        goto rom_main_end;  // DOS started smashfs for a boot node

    if ((globals = globals_alloc(SysBase)) == NULL)
        goto rom_main_end;
    __asm("move.l %0,a4" :: "r" (globals));


    /* Globals are now available */

#if 0
    __asm("movem.l d0-d7/a0-a6,-(sp)");
    /* Local DOSBase */
    struct DosLibrary *DOSBase = (struct DosLibrary *) OpenLibrary(DOSNAME, 34);
    if (DOSBase == NULL) {
        dputs("NULL DOSBase 4\n");
        goto rom_main_end;
    }
    /*
     * XXX: I don't understand how this call is trashing the ROM caller of
     *      my function. Stack padding didn't help. Saving all registers
     *      didn't help. If I instead create a task and have that task call
     *      CreateNewProcTags(), that works.
     */
    struct Process *child;
    child = CreateNewProcTags(NP_Entry, (ULONG) rom_process_entry,
                              NP_Name, (ULONG) "smashfs",
                              NP_StackSize, 8192,
                              NP_Priority, -1,
                              NP_CloseInput, FALSE,
                              NP_CloseOutput, FALSE,
                              NP_CopyVars, FALSE,
                              NP_ExitData, (ULONG) globals,
//                            NP_Path, 0,
//                            NP_Input, 0,      // Default is to open NIL:
//                            NP_Output, 0,     // Default is to open NIL:
//                            NP_Error, 0,      // Default is NULL
//                            NP_WindowPtr, 0,  // Workbench window (task=NULL)
//                            NP_HomeDir, 0,    // Default is NULL from task
//                            NP_CurrentDir, 0,  // Default is NULL from task
//                            NP_FreeSeglist, FALSE, // Only valid if NP_Seglist
//                            NP_ConsoleTask, msgport,
//                            NP_Cli, TRUE,  // Parent env gets copied
                              TAG_END);
    CloseLibrary((struct Library *)DOSBase);
    if (child == NULL)
        dputs("Failed to start smashfs process\n");
    __asm("movem.l (sp)+,d0-d7/a0-a6");
#else
    struct Task *task;
    task = my_CreateTask("smashfs", 10, rom_task_entry, 4096, globals);
    dputs("task=");
    dputx((uintptr_t) task);
    dputs("\n");
    if (task == NULL)
        dputs("Failed to start smashfs task\n");
#endif
//  RawPutChar('^');

rom_main_end: ;  // semicolon needed for gcc 6.5
//  __asm("" ::: "d2", "d3", "d4", "d5", "d6", "d7");
//  __asm("movem.l (sp)+,d0-d7/a0-a6");
}

#if 0
void
exit(int arg)
{
    (void) arg;
    dputs("Exit\n");
    while (1)
        ;
}
#endif
