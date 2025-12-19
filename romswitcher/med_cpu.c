/*
 * MED commands specific to Amiga CPU.
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
#include "printf.h"
#include "db_disasm.h"
#include "amiga_chipset.h"
#include "vectors.h"
#include "cpu_control.h"
#include "cpu_fault.h"
#include "med_cmds.h"

const char cmd_cpu_help[] =
"cpu fault <type>      - cause a CPU fault\n"
#ifndef AMIGAOS
"cpu regs              - display interrupt registers\n"
#endif
"cpu reg <reg> [<val>] - get / set CPU reg: cacr dtt* itt* pcr tc vbr\n"
"cpu spin <dev> [w]    - spin accessing one of ciaa, ciab, chipmem, or <addr>\n"
"cpu type              - show CPU type";

const char cmd_dis_help[] =
"disas                          - disassemble from previous address\n"
"disas <addr> [<count>] [<syn>] - disassemble from <addr>\n"
"                                 <count> is the number of instructions\n"
"                                 <syn> is either mit or mot syntax";

static void
read_spin(uint32_t addr, uint mode)
{
    uint count;
    switch (mode) {
        case 1:
            for (count = 300000; count > 0; count--)
                (void) *VADDR8(addr);
            break;
        case 2:
            for (count = 300000; count > 0; count--)
                (void) *VADDR16(addr);
            break;
        case 4:
            for (count = 300000; count > 0; count--)
                (void) *VADDR32(addr);
            break;
    }
}

static void
write_spin(uint32_t addr, uint mode)
{
    uint count;
    switch (mode) {
        case 1:
            for (count = 300000; count > 0; count--)
                *VADDR8(addr) = 0;
            break;
        case 2:
            for (count = 300000; count > 0; count--)
                *VADDR16(addr) = 0;
            break;
        case 4:
            for (count = 300000; count > 0; count--)
                *VADDR32(addr) = 0;
            break;
    }
}

/*
 * cmd_cpu
 * -------
 * Perform processor operations
 */
rc_t
cmd_cpu(int argc, char * const *argv)
{
#ifdef AMIGA
    uint invalid = 0;
#endif
    if (argc < 2)
        return (RC_USER_HELP);

    if (strcmp(argv[1], "fault") == 0) {
#ifdef AMIGA
        if ((argc < 3) || (argc > 4)) {
show_fault_valid:
            printf("cpu fault addr  - cause Address Error (alignment) fault\n"
                   "cpu fault aline - cause A-Line instruction fault\n"
                   "cpu fault berr  - cause Bus Error\n"
                   "cpu fault chk   - cause CHK fault\n"
                   "cpu fault div0  - cause Divide By Zero fault\n"
                   "cpu fault fdiv  - cause FPU Divide by Zero fault\n"
                   "cpu fault fline - cause F-Line instruction fault\n"
                   "cpu fault fmt   - cause Format Error (FPU)\n"
                   "cpu fault fpoe  - cause Floating Point Operand Error\n"
                   "cpu fault fpuc  - clear FPU fault state\n"
                   "cpu fault ill   - cause Illegal instruction fault\n"
                   "cpu fault priv  - cause Privilege Violation\n"
                   "cpu fault trap  - cause TRAP #7\n"
                   "cpu fault trapv - cause TRAPV (trap on overflow)\n");
            return (RC_BAD_PARAM);
        }
        SUPERVISOR_STATE_ENTER();

        if (strcmp(argv[2], "aline") == 0) {
            /* Any instruction whose opcode begins with A */
            CPU_FAULT_ALINE();
        } else if (strcmp(argv[2], "addr") == 0) {
            /* Branch to unaligned address */
            CPU_FAULT_ADDR();
        } else if (strcmp(argv[2], "berr") == 0) {
            /* Gary will generate bus fault if address is not claimed by dev */
            *GARY_BTIMEOUT = 0xff;
            (void) *VADDR32(0x30000000);
            *GARY_BTIMEOUT = 0x7f;
        } else if (strcmp(argv[2], "chk") == 0) {
            /* CHK compares register in range 0..X */
            CPU_FAULT_CHK();
        } else if (strcmp(argv[2], "div0") == 0) {
            /* Divide by Zero */
            CPU_FAULT_DIV0();
        } else if (strcmp(argv[2], "fline") == 0) {
            /* Any instruction whose opcode begins with F */
            CPU_FAULT_FLINE();
        } else if (strcmp(argv[2], "fmt") == 0) {
            /* Push an invalid FPU frame format and attempt to restore it */
            CPU_FAULT_FMT();
        } else if (strcmp(argv[2], "fdiv") == 0) {
            /* Generate FPU Divide by Zero Error */
            CPU_FAULT_FDIV();
            (void) fpu_get_fpsr();
        } else if (strcmp(argv[2], "fpoe") == 0) {
            /* Generate FPCP Operand Error */
            CPU_FAULT_FPCP();
        } else if (strcmp(argv[2], "fpuc") == 0) {
            /* Clear FPU fault state */
            CPU_FAULT_FPUC();
        } else if (strncmp(argv[2], "illegal", 3) == 0) {
            /* Illegal instruction */
            CPU_FAULT_ILL_INST();
        } else if (strcmp(argv[2], "priv") == 0) {
            /* Drop to user mode and issue STOP, which requires supervisor */
            CPU_FAULT_PRIV();
        } else if (strcmp(argv[2], "trap") == 0) {
            /* CPU TRAP */
            CPU_FAULT_TRAP();
        } else if (strcmp(argv[2], "trapv") == 0) {
            CPU_FAULT_TRAPV();
        } else {
            invalid = 1;
        }
        SUPERVISOR_STATE_EXIT();

        if (invalid) {
            printf("Unknown argument cpu fault \"%s\"\n", argv[2]);
            goto show_fault_valid;
        }
#endif
#ifdef AMIGA
    } else if (strcmp(argv[1], "reg") == 0) {
        uint value = 0;
        if ((argc < 3) || (argc > 4)) {
show_reg_valid:
            printf("cpu reg cacr [<val>]  - get / set CPU CACR\n");
            if (cpu_type > 68030) {
                printf("cpu reg dtt0 [<val>]  - get / set CPU DTT0\n"
                       "cpu reg dtt1 [<val>]  - get / set CPU DTT1\n");
            }
            printf("cpu reg fpcr [<val>]  - get / set FPU FPCR\n"
                   "cpu reg fpsr [<val>]  - get / set FPU FPSR\n");
            if (cpu_type > 68030) {
                printf("cpu reg itt0 [<val>]  - get / set CPU ITT0\n"
                       "cpu reg itt1 [<val>]  - get / set CPU ITT1\n");
            }
            printf("cpu reg pcr [<val>]   - get / set CPU PCR\n");
            if (cpu_type == 68030) {
                printf("cpu reg tt0 [<val>]   - get / set CPU TT0\n"
                       "cpu reg tt1 [<val>]   - get / set CPU TT1\n");
            }
            printf("cpu reg sr [<val>]    - get / set CPU SR\n"
                   "cpu reg tc [<val>]    - get / set CPU MMU TC\n"
                   "cpu reg vbr [<val>]   - get / set CPU VBR\n");
            return (RC_BAD_PARAM);
        }
        if (argc == 4) {
            int pos = 0;
            if ((sscanf(argv[3], "%x%n", &value, &pos) != 1) ||
                (argv[3][pos] != '\0')) {
                printf("Invalid register value %s\n", argv[3]);
                return (RC_BAD_PARAM);
            }
        }
        SUPERVISOR_STATE_ENTER();
        if (strcmp(argv[2], "cacr") == 0) {
            if (argc < 4)
                value = cpu_get_cacr();
            else
                cpu_set_cacr(value);
        } else if (strcmp(argv[2], "dtt0") == 0) {
            if (argc < 4)
                value = cpu_get_dtt0();
            else
                cpu_set_dtt0(value);
        } else if (strcmp(argv[2], "dtt1") == 0) {
            if (argc < 4)
                value = cpu_get_dtt1();
            else
                cpu_set_dtt1(value);
        } else if (strcmp(argv[2], "fpcr") == 0) {
            if (argc < 4)
                value = fpu_get_fpcr();
            else
                fpu_set_fpcr(value);
        } else if (strcmp(argv[2], "fpsr") == 0) {
            if (argc < 4)
                value = fpu_get_fpsr();
            else
                fpu_set_fpsr(value);
        } else if (strcmp(argv[2], "itt0") == 0) {
            if (argc < 4)
                value = cpu_get_itt0();
            else
                cpu_set_itt0(value);
        } else if (strcmp(argv[2], "itt1") == 0) {
            if (argc < 4)
                value = cpu_get_itt1();
            else
                cpu_set_itt1(value);
        } else if (strcmp(argv[2], "pcr") == 0) {
            if (argc < 4)
                value = cpu_get_pcr();
            else
                cpu_set_pcr(value);
        } else if (strcmp(argv[2], "tc") == 0) {
            if (argc < 4)
                value = cpu_get_tc();
            else
                cpu_set_tc(value);
        } else if (strcmp(argv[2], "sr") == 0) {
            if (argc < 4)
                value = cpu_get_sr();
            else
                cpu_set_sr(value);
        } else if (strcmp(argv[2], "tt0") == 0) {
            if (argc < 4)
                value = cpu_get_tt0();
            else
                cpu_set_tt0(value);
        } else if (strcmp(argv[2], "tt1") == 0) {
            if (argc < 4)
                value = cpu_get_tt1();
            else
                cpu_set_tt1(value);
        } else if (strcmp(argv[2], "vbr") == 0) {
            if (argc < 4)
                value = cpu_get_vbr();
            else
                cpu_set_vbr(value);
        } else {
            invalid = 1;
        }
        SUPERVISOR_STATE_EXIT();

        if (invalid) {
            printf("Unknown argument cpu reg \"%s\"\n", argv[2]);
            goto show_reg_valid;
        } else {
            if (argc < 4)
                printf("%08x\n", value);
        }
#endif
#if defined(AMIGA) && !defined(AMIGAOS)
    } else if (strcmp(argv[1], "regs") == 0) {
        printf("Last interrupt:\n  ");
        irq_show_regs(0);
        printf("Last exception:\n  ");
        irq_show_regs(1);
#endif
    } else if (strncmp(argv[1], "spin", 4) == 0) {
        const char *arg = argv[2];
        uint read_op = 1;
        uint mode = 1;
        switch (argv[1][4]) {
            case '\0':
            case 'b':
                mode = 1;
                break;
            case 'w':
                mode = 2;
                break;
            case 'l':
                mode = 4;
                break;
            default:
                printf("Unknown mode %s for spin\n", argv[1] + 4);
                return (RC_BAD_PARAM);
                break;
        }

        if ((argc < 3) || (argc > 4))
            return (RC_USER_HELP);
        if ((argc == 4) && (argv[3][0] == 'w'))
            read_op = 0;  // Write mode

#ifdef AMIGA
        if (strcmp(arg, "chipmem") == 0) {
            if (read_op)
                read_spin(0x1010, mode);
            else
                write_spin(0x1010, mode);
        } else if (strcmp(arg, "ciaa") == 0) {
            if (read_op)
                read_spin(CIA_A_BASE, mode);
            else
                write_spin(CIA_A_BASE, mode);
        } else if (strcmp(arg, "ciab") == 0) {
            if (read_op)
                read_spin(CIA_B_BASE, mode);
            else
                write_spin(CIA_B_BASE, mode);
#else
        if (0) {
#endif
        } else {
            uint32_t addr;
            int pos = 0;
            if ((sscanf(arg, "%x%n", &addr, &pos) != 1) || (arg[pos] != '\0')) {
                printf("Invalid address %s\n", arg);
                return (RC_USER_HELP);
            }
            if (read_op)
                read_spin(addr, mode);
            else
                write_spin(addr, mode);
        }
        return (RC_SUCCESS);
    } else if (strncmp(argv[1], "type", 3) == 0) {
        printf("CPU ");
#ifdef AMIGA
        if (cpu_type == 68060) {
            uint pcr;
            uint rev;
            SUPERVISOR_STATE_ENTER();
            pcr = cpu_get_pcr();
            SUPERVISOR_STATE_EXIT();
            rev = (pcr >> 8) & 0xff;
            switch (pcr >> 16) {
                default:
                case 0x0430:    // Full 68030
                    printf("68060");
                    break;
                case 0x0431:  // MC68LC060 (no FPU) or MC68EC060 (no MMU or FPU)
                    printf("680LC60 or 68EC060");
                    break;
            }
            printf(" Rev%u\n", rev);
        } else {
            printf("%u\n", cpu_type);
        }
#else
        printf("unknown\n");
#endif
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
#ifdef _DCC
    return (RC_SUCCESS);
#else
    static db_addr_t next_addr;
    static int       moto_syntax;
    static uint16_t  dis_count;
    uint             count;
    uint             value;
    uint             mode = 4;
    int              pos;
    char   const    *arg;
    char   const    *cmd;
    if (dis_count == 0) {
        dis_count = 12;
        moto_syntax = 1;
    }
    cmd = skip(argv[0], "disas");
    while (*cmd != '\0') {
        switch (*cmd) {
            case 'b':
                mode = 1;
                break;
            case 'w':
                mode = 2;
                break;
            case 'l':
                mode = 4;
                break;
            case 'q':
                mode = 8;
                break;
            default:
#if defined(__x86_64__) || defined(__i386__)
                printf("disas[bwlq] <addr> <count>\n");
#endif
                return (RC_USER_HELP);
        }
        cmd++;
    }
#ifdef AMIGA
    (void) mode;
#endif

    if (argc > 4) {
        printf("Too many arguments\n");
        return (RC_USER_HELP);
    }
    if (argc > 1) {
        pos = 0;
        arg = argv[1];
        if ((sscanf(arg, "%x%n", &value, &pos) != 1) ||
            (arg[pos] != '\0')) {
            printf("Invalid address %s\n", arg);
            return (RC_USER_HELP);
        }
        next_addr = value;
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
#if defined(__x86_64__) || defined(__i386__)
        if (mode > 4)
            next_addr = db_disasm_64(next_addr, moto_syntax);
        else
#endif
        next_addr = db_disasm(next_addr, moto_syntax);
        if (next_addr == 0)
            return (RC_FAILURE);
    }
    return (RC_SUCCESS);
#endif
}
