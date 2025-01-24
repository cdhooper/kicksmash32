#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <string.h>
#include <dos/dos.h>
#include <dos/dostags.h>      // NP_Entry, etc
#include <dos/dosextens.h>    // struct Process
#include <utility/utility.h>  // UTILITYNAME

#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <clib/alib_protos.h>

#include <inline/exec.h>
#include <inline/dos.h>

#include <clib/debug_protos.h>
#include "printf.h"
#include "sm_msg.h"

#define VERSION 1

#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))

#define RawPutChar(___c) \
        LP1NR(0x204, RawPutChar , UBYTE, ___c, d0, , SysBase)

extern struct DosLibrary *DOSBase;
// extern struct ExecBase *SysBase;

// #define rom_end _end
void rom_end(void);  // XXX: Not the real end; _end doesn't seem to work
void rom_main(struct ExecBase *SysBase asm("a6"));
extern const char SmashRomName[];
extern const char SmashRomID[];

struct Task *my_CreateTask(CONST_STRPTR name,LONG pri, CONST APTR init_pc,
                           ULONG stack_size, void *userdata);

const struct Resident resident = {
    RTC_MATCHWORD,         // rt_MatchWord - word to match on (ILLEGAL)
    (void *) &resident,    // rt_MatchTag - pointer to the above
    rom_end,               // rt_EndSkip - address to continue scan
    RTF_AFTERDOS,          // rt_Flags - various tag flags
    VERSION,               // rt_Version - release version number
    NT_UNKNOWN,            // rt_Type - type of module
    5,                     // rt_Pri - initialization priority (before bootmenu)
    (char *) "smashrom",   // rt_Name - pointer to node name
    (char *) SmashRomID,   // rt_IdString - pointer to identification string
    rom_main               // rt_Init - pointer to init code
};

const char SmashRomID[]   = "smashrom 1.4 (23.01.2025)\r\n";

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
    copy_to_ram_ptr = AllocMem(len, MEMF_PUBLIC);
    if (copy_to_ram_ptr == NULL) {
        dputs("AllocMem fail 1\n");
        return;
    }
    memcpy(copy_to_ram_ptr, (void *) copy_to_ram_start, len);
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

    void *data_start;
    uint  data_size;
    uint  bss_size;
    __asm("lea __sdata,%0"      : "=a" (data_start) ::);
    __asm("lea ___data_size,%0" : "=a" (data_size) ::);
    __asm("lea ___bss_size,%0"  : "=a" (bss_size) ::);

    if ((globals = AllocVec(data_size + bss_size, MEMF_PUBLIC)) == NULL) {
        dputs("AllocMem fail 2\n");
        goto rom_main_end;
    }

    /* Must compile with -fbaserel so a4 will be used for globals */
    memcpy(globals, data_start, data_size);
    memset(globals + data_size, 0, bss_size);

    dputs("globals=");
    dputx((uintptr_t) globals);
    dputs(" data=");
    dputx((uintptr_t) data_size);
    dputs(" bss=");
    dputx((uintptr_t) bss_size);
    dputs("\n");
    void *bss_start;
    __asm("lea __bss_start,%0" : "=a" (bss_start) ::);
    dputs("romdata=");
    dputx((uintptr_t) data_start);
    dputs(" rombss=");
    dputx((uintptr_t) bss_start);
    dputs("\n");

// Change LD script to:
// ___data_size = SIZEOF(.data);
// ___bss_size = SIZEOF(.bss);
// ___total_data_size = SIZEOF(.data) + SIZEOF(.bss);

    globals += 0x7ffe;  // offset that gcc applies to a4-relative globals
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
