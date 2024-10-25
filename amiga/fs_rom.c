#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <string.h>
#include <dos/dos.h>
#include <dos/dostags.h>    // NP_Entry, etc
#include <dos/dosextens.h>  // struct Process

#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>

#include <inline/exec.h>
#include <inline/dos.h>

#include <clib/debug_protos.h>
#include "printf.h"

#define VERSION 1

#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))

#define RawPutChar(___c) \
        LP1NR(0x204, RawPutChar , UBYTE, ___c, d0, , SysBase)

extern struct DosLibrary *DOSBase;
extern struct ExecBase *SysBase;

extern void nop_func(void);
extern void rom_end(void);
extern int rom_main(int arg);
extern const char SmashRomName[];
extern const char SmashRomID[];

const struct Resident resident = {
    RTC_MATCHWORD,         // rt_MatchWord - word to match on (ILLEGAL)
    (void *) &resident,    // rt_MatchTag - pointer to the above
    rom_end,               // rt_EndSkip - address to continue scan
    RTF_AFTERDOS,          // rt_Flags - various tag flags
    VERSION,               // rt_Version - release version number
    NT_UNKNOWN,            // rt_Type - type of module
    5,                     // rt_Pri - initialization priority (before bootmenu)
    (char *) SmashRomName, // rt_Name - pointer to node name
    (char *) SmashRomID,   // rt_IdString - pointer to identification string
    rom_main               // rt_Init - pointer to init code
};
int bss_value;
int data_value = 0x98765432; // 0xff894c  and 0x705a168 (base 705a160)

const char SmashRomName[] = "smashrom";
const char SmashRomID[]   = "smashrom 1.1 (10.18.2024)\r\n";

#define ADDR32(x)   ((volatile uint32_t *) ((uintptr_t)(x)))
typedef unsigned int uint;

#if 0
static void
dump_regs0(void)
{
    // a0-a7 are in 7770080 to 777009f
    // d0-d7 are in 77700a0 to 77700bf
    __asm("movem.l d0-d1/a0-a1,-(sp)");
    __asm("move.l  a0,-(sp)");
    __asm("move.l  #0x077700c0,a0");
    __asm("movem.l d0-d7,-(a0)");
    __asm("movem.l a1-a7,-(a0)");
    __asm("move.l  (sp)+,d0");
    __asm("move.l  d0,-(a0)");
    __asm("movem.l (sp)+,d0-d1/a0-a1");
}
#endif

#if 0
static void
dump_regs1(void)
{
    // a0-a7 are in 7770000 to 777001f
    // d0-d7 are in 7770020 to 777003f
    __asm("movem.l d0-d1/a0-a1,-(sp)");
    __asm("move.l  a0,-(sp)");
    __asm("move.l  #0x07770040,a0");
    __asm("movem.l d0-d7,-(a0)");
    __asm("movem.l a1-a7,-(a0)");
    __asm("move.l  (sp)+,d0");
    __asm("move.l  d0,-(a0)");
    __asm("movem.l (sp)+,d0-d1/a0-a1");
}
#endif

static void
dputs(const char *str)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    struct ExecBase *SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop

    while (*str != '\0')
        RawPutChar(*str++);
}

int
call_main(void)
{
    /* Globals are now available */
    void *ptr;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop

#ifdef DEBUG_ENTRY
    *ADDR32(0x07770000) = bss_value;
    *ADDR32(0x07770004) = data_value;
    *ADDR32(0x07770008) = (uintptr_t) &data_value;
    *ADDR32(0x0777000c) = (uintptr_t) SysBase;
#endif

    RawPutChar('y');
    DOSBase = (struct DosLibrary *) OpenLibrary("dos.library", 34);

    RawPutChar('_');

    /* Make smashfs debug output go to serial port */
    extern uint8_t flag_output;
    flag_output = 2;

    Delay(600);
    dputs("Child\n");
    BPTR fh_i = 0;
    BPTR fh_o = 0;
    BPTR fh_e = 0;
    ptr = SysBase;
    if (ptr == NULL) {
        dputs("NULL SysBase\n");
    }
    if (DOSBase != NULL) {
        dputs("Call open\n");
        fh_i = Open("Con:", MODE_READWRITE);
        dputs("Call DupLock 1\n");
        fh_o = DupLock(fh_i);
        dputs("Call DupLock 2\n");
        fh_e = DupLock(fh_i);
        Delay(50);
        RawPutChar('I');
        SelectInput(fh_i);
        Delay(50);
        RawPutChar('O');
        SelectOutput(fh_o);
        Delay(50);
        RawPutChar('E');
        SelectError(fh_e);
        Delay(50);
    } else {
        dputs("NULL DOSBase\n");
    }

    if (DOSBase != NULL) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }

    dputs(" call main\n");
    extern int main(int argc, char *argv[]);
    char *args[] = { NULL, "-dd" };

    return (main(ARRAY_SIZE(args), args));
#if 0
    if (DOSBase != NULL) {
        DOSBase = (struct DosLibrary *) OpenLibrary("dos.library", 34);
        Close(fh_i);
        Close(fh_o);
        Close(fh_e);
        CloseLibrary((struct Library *)DOSBase);
    }
#endif
}

int __regargs
smashfs_entry(void)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    struct ExecBase *SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop
    struct Process *proc;
//  register uint8_t *glob asm("a4");
    uint8_t *globals;
    int rc;

    RawPutChar('e');
    proc = (struct Process *) FindTask(NULL);
    if (proc == NULL)
        return (0);

    RawPutChar('n');
    globals = (uint8_t *) proc->pr_ExitData;
    if (globals == NULL)
        return (0);

    RawPutChar('t');
    /* Got globals pointer */
#ifdef DEBUG_ENTRY
    *ADDR32(0x07770070) = (uintptr_t) globals;
#endif
//  globals += 0x7ffe;  // offset that gcc applies to a4-relative globals
    __asm("move.l %0,a4" :: "r" (globals));

    /* Globals are now available */
    RawPutChar('r');
// __lib_init(SysBase);

    extern struct Library * __UtilityBase;
    __UtilityBase = OpenLibrary("utility.library",0);

    rc = call_main();
    Forbid();
    return (rc);
}

// a6 has sysbase
int
rom_main(int arg)
{
    char *smashfs_name;
    struct ExecBase *SysBase;
    extern struct DosLibrary *DOSBase;
    uint8_t *globals;
    void *data_ptr;
    uint  data_size;
//  dump_regs0();

//  __asm("move.l a6,_SysBase");  // SysBase provided in a6

//  __asm("move.l a6,%0" : "=a" (SysBase) ::);  // SysBase provided in a6
//  __asm("move.l a6,d0");
//  __asm("move.l d0,0x4");

// XXX: with -fbaserel, a4 is used for globals

    // Call RawPutChar directly
    __asm("moveq #100,d0");
    __asm("jsr -516(a6)");
    __asm("moveq #101,d0");
    __asm("jsr -516(a6)");

    /* Get local SysBase */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop

    RawPutChar('b');

//  __asm("movem.l d0-d7/a0-a6,-(sp)");
    // a6 should be SysBase
//  dump_regs1();
    (void) arg;

    /* Open local DOSBase (no globals access yet) */
//  DOSBase = (struct DosLibrary *) OpenLibrary("dos.library", 34);
//  DOSBase = (struct DosLibrary *) FindResident("dos.library");

    RawPutChar('u');
#ifdef DEBUG_ENTRY
    *ADDR32(0x07770044) = (uintptr_t) &arg;
    *ADDR32(0x07770048) = (uintptr_t) rom_main;
    *ADDR32(0x07770054) = (uintptr_t) SysBase;
#endif

    RawPutChar('g');

    __asm("lea __sdata,%0" : "=a" (data_ptr) ::);
    __asm("lea ___data_size,%0" : "=a" (data_size) ::);
#ifdef DEBUG_ENTRY
    *ADDR32(0x07770058) = (uintptr_t) data_ptr;
    *ADDR32(0x0777005c) = (uintptr_t) data_size;
#endif
    if ((globals = AllocMem(data_size, MEMF_PUBLIC)) == NULL) {
        dputs("AllocMem fail\n");
    } else {
        memcpy(globals, data_ptr, data_size);
        globals += 0x7ffe;  // offset that gcc applies to a4-relative globals
        __asm("move.l %0,a4" :: "r" (globals));

        /* Globals are now available */

        DOSBase = (struct DosLibrary *) OpenLibrary("dos.library", 34);
        if (DOSBase == NULL) {
            dputs("NULL DOSBase\n");
        } else {
            struct Process *child;
            smashfs_name = AllocMem(16, MEMF_PUBLIC);
            strcpy(smashfs_name, "smashfs task");

            child = CreateNewProcTags(NP_Entry, (ULONG) smashfs_entry,
                                      NP_Name, (ULONG) smashfs_name,
                                      NP_StackSize, 16384,
                                      NP_Priority, 10,
                                      NP_ExitData, (ULONG) globals,
                                      TAG_END);
            CloseLibrary((struct Library *)DOSBase);
            DOSBase = NULL;
            if (child == NULL)
                dputs("Failed to start smashfs process\n");
        }
    }
    RawPutChar('.');

#ifdef DEBUG_ENTRY
    *ADDR32(0x07770068) = (uintptr_t) child;
    *ADDR32(0x0777006c) = (uintptr_t) DOSBase;
#endif

//  __asm("movem.l (sp)+,d0-d7/a0-a6");
    return (1);
}

void
exit(int arg)
{
    (void) arg;
    dputs("Exit\n");
    while (1)
        ;
}
