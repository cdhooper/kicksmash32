/*
 * MED commands specific to Amiga ROM.
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
#include <string.h>
#include "med_cmdline.h"
#include "timer.h"
#include "printf.h"
#include "serial.h"
#include "reset.h"
#include "util.h"
#include "autoconfig.h"
#include "db_disasm.h"
#include "amiga_chipset.h"
#include "vectors.h"
#include "cpu_control.h"

const char cmd_aconfig_help[] =
"aconfig [show]         - show current (unconfigured) device\n"
"aconfig auto           - perform automatic autoconfig\n"
"aconfig board [<addr>] - config current device to Zorro address\n"
"aconfig list           - show autoconfig space (free and allocated)\n"
"aconfig shutup         - tell device to shutup (go to next)\n";

const char cmd_cpu_help[] =
"cpu cacr [<val>]       - get / set CPU SR\n"
"cpu fault addr         - cause Address Error (alignment) fault\n"
"cpu fault aline        - cause A-Line instruction fault\n"
"cpu fault berr         - cause Bus Error\n"
"cpu fault chk          - cause CHK fault\n"
"cpu fault div0         - cause Divide By Zero fault\n"
"cpu fault fline        - cause F-Line instruction fault\n"
"cpu fault fmt          - cause Format Error (FPU)\n"
"cpu fault fpoe         - cause Floating Point Operand Error\n"
"cpu fault ill          - cause Illegal instruction fault\n"
"cpu fault priv         - cause Privilege Violation\n"
"cpu fault trap         - cause TRAP #7\n"
"cpu fault trapv        - cause TRAPV (trap on overflow)\n"
"cpu regs               - display interrupt registers\n"
"cpu type               - show CPU type";

const char cmd_dis_help[] =
"disas                          - disassemble from previous address\n"
"disas <addr> [<count>] [<syn>] - disassemble from <addr>\n"
"                                 <count> is the number of instructions\n"
"                                 <syn> is either mit or mot syntax";

const char cmd_reset_help[] =
"reset - reset Amiga\n";

/*
 * cmd_aconfig
 * -----------
 * Perform manual autoconfig of Zorro devices.
 */
rc_t
cmd_aconfig(int argc, char * const *argv)
{
    rc_t rc;
    if (argc < 2)
        return (autoconfig_show());
    if ((strcmp(argv[1], "?") == 0) || (strcmp(argv[1], "help") == 0)) {
        return (RC_USER_HELP);
    } else if (strcmp(argv[1], "auto") == 0) {
        uint count = 0;
        while ((rc = autoconfig_address(0)) == RC_SUCCESS)
            count++;
        if (rc == RC_NO_DATA) {
            if (count > 0)
                return (RC_SUCCESS);
            printf("No board detected\n");
        }
        return (rc);
    } else if ((strncmp(argv[1], "address", 4) == 0) ||
               (strncmp(argv[1], "board", 1) == 0)) {
        int pos;
        uint32_t addr;
        if (argc < 3) {
            /* No address specified -- do it automatically */
            rc = autoconfig_address(0);
            if (rc == RC_NO_DATA)
                printf("No board detected\n");
            return (rc);
        }
        pos = 0;
        if (((argv[2][0] == '-') || (argv[2][0] == '0')) &&
            (argv[2][1] == '\0')) {
            addr = 0;
        } else if ((sscanf(argv[2], "%x%n", &addr, &pos) != 1) ||
            (argv[2][pos] != '\0')) {
            printf("Invalid address %s\n", argv[2]);
            return (RC_USER_HELP);
        }
        rc = autoconfig_address(addr);
        if (rc == RC_NO_DATA)
            printf("No board detected\n");
        return (rc);
    } else if (strncmp(argv[1], "list", 4) == 0) {
        autoconfig_list();
    } else if (strncmp(argv[1], "show", 4) == 0) {
        return (autoconfig_show());
    } else if (strncmp(argv[1], "shutup", 4) == 0) {
        return (autoconfig_shutup());
    } else {
        printf("Unknown argument %s\n", argv[1]);
        return (RC_USER_HELP);
    }
    return (RC_SUCCESS);
}

/*
 * cmd_cpu
 * -------
 * Perform processor operations
 */
rc_t
cmd_cpu(int argc, char * const *argv)
{
    if (argc < 1)
        return (RC_USER_HELP);

    if (strcmp(argv[1], "cacr") == 0) {
        if (argc < 3) {
            printf("%08x\n" , cpu_get_cacr());
        } else {
            int pos = 0;
            uint value;
            if ((sscanf(argv[2], "%x%n", &value, &pos) != 1) ||
                (argv[2][pos] != '\0')) {
                printf("Invalid cacr value %s\n", argv[2]);
                return (RC_BAD_PARAM);
            }
            cpu_set_cacr(value);
        }
    } else if (strcmp(argv[1], "fault") == 0) {
        if (argc < 3) {
            printf("cpu fault requires an argument\n");
            return (RC_USER_HELP);
        }
        if (strcmp(argv[2], "aline") == 0) {
            /* Any instruction whose opcode begins with A */
            __asm(".word 0xa000");
        } else if (strcmp(argv[2], "addr") == 0) {
            /* Branch to unaligned address */
            __asm("lea.l 0x1(pc),a0\n\t"
                  "jmp (a0)"::: "a0");
        } else if (strcmp(argv[2], "berr") == 0) {
            /* Gary will generate bus fault if address is not claimed by dev */
            *GARY_BTIMEOUT = 0xff;
            (void) *VADDR32(0x30000000);
            *GARY_BTIMEOUT = 0x7f;
        } else if (strcmp(argv[2], "chk") == 0) {
            /* CHK compares register in range 0..X */
            __asm("move.l #-1, d0\n\t"
                  "chk.l  #10, d0"::: "d0");
        } else if (strcmp(argv[2], "div0") == 0) {
            /* Divide by Zero */
            __asm("move.l #0, d0\n\t"
                  "divs.w #0, d0"::: "d0", "d1");
        } else if (strcmp(argv[2], "fline") == 0) {
            /* Any instruction whose opcode begins with F */
            __asm(".word 0xf000");
            __asm(".word 0x0000");
        } else if (strcmp(argv[2], "fmt") == 0) {
            /* Push an invalid FPU frame format and attempt to restore it */
            __asm("move.l #0xff000000, -(sp)");
            __asm("frestore (sp)+");
        } else if (strcmp(argv[2], "fpoe") == 0) {
            /* Any instruction whose opcode begins with F */
            __asm("fmove.l fp0,d0");
        } else if (strncmp(argv[2], "illegal", 3) == 0) {
            /* Illegal instruction */
            __asm("illegal");
        } else if (strcmp(argv[2], "priv") == 0) {
            /* Drop to user mode and issue STOP, which requires supervisor */
            __asm("move.w #0, sr");
            __asm("stop #0x2700");
        } else if (strcmp(argv[2], "trap") == 0) {
            /* CPU TRAP */
            __asm("trap #7");
        } else if (strcmp(argv[2], "trapv") == 0) {
            __asm("move.l #0x7fffffff, d0\n\t"
                  "addq.l #2, d0\n\t"
                  "trapv"::: "d0");
        } else {
            printf("Unknown argument cpu fault \"%s\"\n", argv[2]);
            return (RC_USER_HELP);
        }
    } else if (strncmp(argv[1], "regs", 3) == 0) {
        printf("Last interrupt:\n");
        irq_show_regs(0);
        printf("Last exception:\n");
        irq_show_regs(1);
    } else if (strncmp(argv[1], "type", 3) == 0) {
        printf("CPU %u\n", cpu_type);
    } else {
        printf("Unknown argument cpu \"%s\"\n", argv[1]);
        return (RC_USER_HELP);
    }
    return (RC_SUCCESS);
}

/*
 * cmd_dis
 * -------
 * Disassemble instructions at memory address
 */
rc_t
cmd_dis(int argc, char * const *argv)
{
    static db_addr_t next_addr;
    static bool      moto_syntax;
    static uint16_t  dis_count;
    uint             count;
    int              pos;
    char   const    *arg;
    if (dis_count == 0) {
        dis_count = 12;
        moto_syntax = 1;
    }
    if (argc > 4) {
        printf("Too many arguments\n");
        return (RC_USER_HELP);
    }
    if (argc > 1) {
        pos = 0;
        arg = argv[1];
        if ((sscanf(arg, "%x%n", &next_addr, &pos) != 1) ||
            (arg[pos] != '\0')) {
            printf("Invalid address %s\n", arg);
            return (RC_USER_HELP);
        }
    }
    if (argc > 2) {
        pos = 0;
        arg = argv[2];
        if ((sscanf(arg, "%u%n", &count, &pos) != 1) ||
            (arg[pos] != '\0')) {
            printf("Invalid count %s\n", arg);
            return (RC_USER_HELP);
        }
        if (count == 0)
            return (RC_SUCCESS);
        dis_count = count;
    }
    if (argc > 3) {
        arg = argv[3];
        if (strncmp(arg, "motorola", 3) == 0) {
            moto_syntax = 1;
        } else if (strcmp(arg, "mit") == 0) {
            moto_syntax = 0;
        } else {
            printf("Invalid syntax %s\n", arg);
            return (RC_USER_HELP);
        }
    }
    for (count = 0; count < dis_count; count++) {
        next_addr = db_disasm(next_addr, moto_syntax);
        if (next_addr == 0)
            return (RC_FAILURE);
    }
    return (RC_SUCCESS);
}

static uint
time_check(const char *text, uint diff, uint min, uint max)
{
    int errs = 0;
    if ((min <= diff) && (max >= diff)) {
        printf("PASS: ");
    } else {
        printf("FAIL: ");
        errs++;
    }
    printf("%-24s %u usec\n", text, diff);
    return (errs);
}

static rc_t
timer_test(void)
{
    uint64_t start;
    uint64_t diff;
    uint     errs = 0;

    start = timer_tick_get();
    timer_delay_ticks(0);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_ticks(0)", (uint)diff, 0, 500);

    start = timer_tick_get();
    timer_delay_ticks(100);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_ticks(100)", (uint)diff, 100, 500);

    start = timer_tick_get();
    timer_delay_usec(1);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_usec(1)", (uint)diff, 1, 500);

    start = timer_tick_get();
    timer_delay_usec(10);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_usec(10)", (uint)diff, 10, 410);

    start = timer_tick_get();
    timer_delay_usec(1000);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_usec(1000)", (uint)diff, 1000, 1500);

    start = timer_tick_get();
    timer_delay_msec(1);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_msec(1)", (uint)diff, 1000, 1500);

    start = timer_tick_get();
    timer_delay_msec(10);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_msec(10)", (uint)diff, 10000, 10500);

    start = timer_tick_get();
    timer_delay_msec(1000);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_msec(1000)", (uint)diff, 1000000, 1000500);

    // XXX: Replace one second above with RTC tick?

    if (errs > 0)
        return (RC_FAILURE);
    return (RC_SUCCESS);
}

static rc_t
timer_watch(void)
{
    bool_t   fail = FALSE;
    uint64_t last = timer_tick_get();
    uint64_t now;

    while (1) {
        now = timer_tick_get();
        if (last >= now) {
            printf("\nLast=%llx now=%llx Current=%012llx",
                   (long long) last, (long long) now, timer_tick_get());
        } else {
            if ((last >> 32) != (now >> 32))
                putchar('.');
            last = now;
        }
        if (input_break_pending()) {
            printf("^C\n");
            break;
        }
    }
    return ((fail == TRUE) ? RC_FAILURE : RC_SUCCESS);
}

rc_t
cmd_reset(int argc, char * const *argv)
{
    if ((argc < 2) ||
        ((strcmp(argv[1], "amiga") == 0) || (strcmp(argv[1], "cpu") == 0))) {
        printf("Resetting...\n");
        timer_delay_msec(1);
        reset_cpu();
    } else {
        printf("Unknown argument %s\n", argv[1]);
        return (RC_USER_HELP);
    }
    return (RC_SUCCESS);
}

rc_t
cmd_time(int argc, char * const *argv)
{
    rc_t rc;

    if (argc <= 1)
        return (RC_USER_HELP);

    if (strncmp(argv[1], "cmd", 1) == 0) {
        uint64_t time_start;
        uint64_t time_diff;

        if (argc <= 2) {
            printf("error: time cmd requires command to execute\n");
            return (RC_USER_HELP);
        }
        time_start = timer_tick_get();
        rc = cmd_exec_argv(argc - 2, argv + 2);
        time_diff = timer_tick_get() - time_start;
        printf("%lld us\n", timer_tick_to_usec(time_diff));
        if (rc == RC_USER_HELP)
            rc = RC_FAILURE;
    } else if (strncmp(argv[1], "now", 1) == 0) {
        uint64_t now = timer_tick_get();
        printf("tick=0x%llx uptime=%lld usec\n", now, timer_tick_to_usec(now));
        rc = RC_SUCCESS;
    } else if (strncmp(argv[1], "watch", 1) == 0) {
        rc = timer_watch();
    } else if (strncmp(argv[1], "test", 1) == 0) {
        rc = timer_test();
    } else {
        printf("Unknown argument %s\n", argv[1]);
        return (RC_USER_HELP);
    }
    return (rc);
}
