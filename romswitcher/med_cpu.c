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

const char cmd_cpu_help[] =
"cpu fault <type>      - cause a CPU fault\n"
"cpu regs              - display interrupt registers\n"
"cpu reg <reg> [<val>] - get / set CPU reg: cacr dtt* itt* pcr tc vbr\n"
"cpu type              - show CPU type";

const char cmd_dis_help[] =
"disas                          - disassemble from previous address\n"
"disas <addr> [<count>] [<syn>] - disassemble from <addr>\n"
"                                 <count> is the number of instructions\n"
"                                 <syn> is either mit or mot syntax";

/*
 * cmd_cpu
 * -------
 * Perform processor operations
 */
rc_t
cmd_cpu(int argc, char * const *argv)
{
    if (argc < 2)
        return (RC_USER_HELP);

    if (strcmp(argv[1], "fault") == 0) {
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
        } else if (strcmp(argv[2], "fdiv") == 0) {
            /* Generate FPU Divide by Zero Error */
            __asm("fmove.l #0x0400, fpcr");
            __asm("fmove.l #0x0000, fpsr");
            __asm("fmove.l #42, fp0");   // FP0 = 42
            __asm("fmove.l #0, fp1");    // FP1 = 0
            __asm("fdiv.x fp1, fp0");
            (void) fpu_get_fpsr();
        } else if (strcmp(argv[2], "fpoe") == 0) {
            /* Generate FPCP Operand Error */
            __asm("fmove.l #0x2000, fpcr");
            __asm("fmove.l fp0,d0");

            __asm("move.l #0x00000000, -(sp)");
            __asm("frestore (sp)+");
        } else if (strcmp(argv[2], "fpuc") == 0) {
            /* Clear FPU fault state */
            __asm("move.l #0x00000000, -(sp)");
            __asm("frestore (sp)+");
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
            goto show_fault_valid;
        }
    } else if (strcmp(argv[1], "reg") == 0) {
        uint value = 0;
        if ((argc < 3) || (argc > 4)) {
show_reg_valid:
            printf("cpu reg cacr [<val>]  - get / set CPU CACR\n"
                   "cpu reg dtt0 [<val>]  - get / set CPU DTT0\n"
                   "cpu reg dtt1 [<val>]  - get / set CPU DTT1\n"
                   "cpu reg fpcr [<val>]  - get / set FPU FPCR\n"
                   "cpu reg fpsr [<val>]  - get / set FPU FPSR\n"
                   "cpu reg itt0 [<val>]  - get / set CPU ITT0\n"
                   "cpu reg itt1 [<val>]  - get / set CPU ITT1\n"
                   "cpu reg pcr [<val>]   - get / set CPU PCR\n"
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
        if (strcmp(argv[2], "cacr") == 0) {
            if (argc < 4)
                printf("%08x\n", cpu_get_cacr());
            else
                cpu_set_cacr(value);
        } else if (strcmp(argv[2], "dtt0") == 0) {
            if (argc < 4)
                printf("%08x\n", cpu_get_dtt0());
            else
                cpu_set_dtt0(value);
        } else if (strcmp(argv[2], "dtt1") == 0) {
            if (argc < 4)
                printf("%08x\n", cpu_get_dtt1());
            else
                cpu_set_dtt1(value);
        } else if (strcmp(argv[2], "fpcr") == 0) {
            if (argc < 4)
                printf("%08x\n", fpu_get_fpcr());
            else
                fpu_set_fpcr(value);
        } else if (strcmp(argv[2], "fpsr") == 0) {
            if (argc < 4)
                printf("%08x\n", fpu_get_fpsr());
            else
                fpu_set_fpsr(value);
        } else if (strcmp(argv[2], "itt0") == 0) {
            if (argc < 4)
                printf("%08x\n", cpu_get_itt0());
            else
                cpu_set_itt0(value);
        } else if (strcmp(argv[2], "itt1") == 0) {
            if (argc < 4)
                printf("%08x\n", cpu_get_itt1());
            else
                cpu_set_itt1(value);
        } else if (strcmp(argv[2], "pcr") == 0) {
            if (argc < 4)
                printf("%08x\n", cpu_get_pcr());
            else
                cpu_set_pcr(value);
        } else if (strcmp(argv[2], "tc") == 0) {
            if (argc < 4)
                printf("%08x\n", cpu_get_tc());
            else
                cpu_set_tc(value);
        } else if (strcmp(argv[2], "vbr") == 0) {
            if (argc < 4)
                printf("%08x\n", cpu_get_vbr());
            else
                cpu_set_vbr(value);
        } else {
            printf("Unknown argument cpu reg \"%s\"\n", argv[2]);
            goto show_reg_valid;
        }
    } else if (strcmp(argv[1], "regs") == 0) {
        printf("Last interrupt:\n  ");
        irq_show_regs(0);
        printf("Last exception:\n  ");
        irq_show_regs(1);
    } else if (strncmp(argv[1], "type", 3) == 0) {
        printf("CPU ");
        if (cpu_type == 68060) {
            uint pcr = cpu_get_pcr();
            uint rev = (pcr >> 8) & 0xff;
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
