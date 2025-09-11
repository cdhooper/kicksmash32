/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * Command implementations.
 */

#include "printf.h"
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include "board.h"
#include "main.h"
#include "cmdline.h"
#include "prom_access.h"
#include <stdbool.h>
#include "timer.h"
#include "uart.h"
#include "cmds.h"
#include "gpio.h"
#include "pcmds.h"
#include "adc.h"
#include "utils.h"
#include "usb.h"
#include "irq.h"
#include "kbrst.h"
#include "m29f160xt.h"
#include "msg.h"
#include "config.h"
#include "pin_tests.h"
#include "led.h"

#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/memorymap.h>
#define GPIOA_BASE GPIO_PORT_A_BASE
#define GPIOB_BASE GPIO_PORT_B_BASE
#define GPIOC_BASE GPIO_PORT_C_BASE
#define GPIOD_BASE GPIO_PORT_D_BASE
#define GPIOE_BASE GPIO_PORT_E_BASE

#ifndef SRAM_BASE
#define SRAM_BASE 0x20000000U
#endif

#define ROM_BANKS 8

#if defined(STM32F4)
#define FLASH_BASE FLASH_MEM_INTERFACE_BASE
#endif

const char cmd_cpu_help[] =
"cpu hardfault - cause CPU hard fault (bad address)\n"
"cpu regs      - show CPU registers";

const char cmd_gpio_help[] =
"gpio [name=value/mode/?] - display or set GPIOs";

const char cmd_prom_help[] =
"prom bank <cmd>         - show or set PROM bank for AmigaOS\n"
"prom cmd <cmd> [<addr>] - send a 32-bit command to both flash chips\n"
"prom id                 - report EEPROM chip vendor and id\n"
"prom erase chip|<addr>  - erase EEPROM chip or 128K sector; <len> optional\n"
"prom log [<count>]      - show log of Amiga address accesses\n"
"prom mode 0|1|2|3       - set EEPROM access mode (0=32, 1=16lo, 2=16hi)\n"
"prom name [<name>]      - set or show name of this board\n"
"prom read <addr> <len>  - read binary data from EEPROM (to terminal)\n"
"prom service            - enter Amiga/USB message service mode\n"
"prom temp               - show STM32 die temperature\n"
"prom write <addr> <len> - write binary data to EEPROM (from terminal)\n"
"prom test               - test pins (standalone board only)";

const char cmd_reset_help[] =
"reset              - reset CPU\n"
"reset amiga [hold] - reset Amiga using KBRST (hold leaves it in reset)\n"
"reset amiga long   - reset Amiga with a long reset (change ROM image)\n"
"reset dfu[rom]     - reset into DFU programming mode\n"
"reset prom         - reset ROM flash memory (forces Amiga reset as well)\n"
"reset usb          - reset and restart USB interface";

const char cmd_set_help[] =
"set bank [show|name|?]     - do various prom bank settings\n"
"set board_rev <num>        - set board revision\n"
"set flags <flags> [save]   - set config flags\n"
"set led <pct>              - set the Power LED brightness level\n"
"set mode <num>             - set prom mode (0=32, 1=16, 2=16hi, 3=auto)\n"
"set name <name>            - set Kicksmash board name\n"
"set sw_bank <num>          - set ROM switcher timeout bank\n"
"set sw_timeout <num>       - set ROM switcher timeout in seconds";

const char cmd_snoop_help[] =
"snoop        - capture and report ROM transactions\n"
"snoop addr   - hardware capture A0-A19\n"
"snoop lo     - hardware capture A0-A15 D0-D15\n"
"snoop hi     - hardware capture A0-A15 D16-D31";

const char cmd_usb_help[] =
"usb disable - reset and disable USB\n"
"usb regs    - display USB device registers\n"
"usb reset   - reset and restart USB device\n"
"usb stats   - USB statistics";

typedef struct {
    const char *const name;
    uintptr_t         addr;
} memmap_t;

static const memmap_t memmap[] = {
    { "ADC1",   ADC1_BASE },
    { "AHB",    PERIPH_BASE_AHB },
    { "APB1",   PERIPH_BASE_APB1 },
    { "APB2",   PERIPH_BASE_APB2 },
#ifdef STM32F1
    { "AFIO",   AFIO_BASE },
    { "BKP",    BACKUP_REGS_BASE },
#endif
#if defined(STM32F103xE)
    { "BKP",    RTC_BKP_BASE },
#endif
    { "DAC",    DAC_BASE },
    { "DMA1",   DMA1_BASE },
    { "DMA2",   DMA2_BASE },
    { "EXTI",   EXTI_BASE },
    { "FLASH",  FLASH_BASE },
    { "FPEC",   FLASH_MEM_INTERFACE_BASE },
    { "GPIOA",  GPIOA_BASE },
    { "GPIOB",  GPIOB_BASE },
    { "GPIOC",  GPIOC_BASE },
    { "GPIOD",  GPIOD_BASE },
    { "GPIOE",  GPIOE_BASE },
    { "IWDG",   IWDG_BASE },
    { "PWR",    POWER_CONTROL_BASE },
    { "RCC",    RCC_BASE },
    { "RTC",    RTC_BASE },
    { "SCB",    SCB_BASE },
    { "SRAM",   SRAM_BASE },
    { "TIM1",   TIM1_BASE },
    { "TIM2",   TIM2_BASE },
    { "TIM3",   TIM3_BASE },
    { "TIM4",   TIM4_BASE },
    { "TIM5",   TIM5_BASE },
    { "TIM8",   TIM8_BASE },
    { "USART1", USART1_BASE },
    { "USART3", USART3_BASE },
    { "USB",    USB_PERIPH_BASE },
    { "WWDG",   WWDG_BASE },
};

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
    uint     count;

    start = timer_tick_get();
    for (count = 1000; count > 0; count--)
        if (timer_tick_get() != start)
            break;
    if (count == 0) {
        printf("Timer tick is not advancing\n");
        return (RC_FAILURE);
    }

    start = timer_tick_get();
    timer_delay_ticks(0);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_ticks(0)", (uint)diff, 0, 5);

    start = timer_tick_get();
    timer_delay_ticks(100);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_ticks(100)", (uint)diff, 2, 5);

    start = timer_tick_get();
    timer_delay_usec(1);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_usec(1)", (uint)diff, 1, 5);

    start = timer_tick_get();
    timer_delay_usec(10);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_usec(10)", (uint)diff, 10, 15);

    start = timer_tick_get();
    timer_delay_usec(1000);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_usec(1000)", (uint)diff, 1000, 1005);

    start = timer_tick_get();
    timer_delay_msec(1);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_msec(1)", (uint)diff, 1000, 1005);

    start = timer_tick_get();
    timer_delay_msec(10);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_msec(10)", (uint)diff, 10000, 10007);

    start = timer_tick_get();
    timer_delay_msec(1000);
    diff = timer_tick_to_usec(timer_tick_get() - start);
    errs += time_check("timer_delay_msec(1000)", (uint)diff, 1000000, 1000007);

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
cmd_time(int argc, char * const *argv)
{
    rc_t rc;

    if (argc <= 1)
        return (RC_USER_HELP);

    if (strncmp(argv[1], "amiga", 1) == 0) {
        extern uint64_t amiga_time;

        if (amiga_time == 0) {
            printf("Amiga time not set. First use \"hostsmash -c set\""
                   " or \"smash -c set\"\n");
            return (RC_NO_DATA);
        }
        uint64_t now    = timer_tick_get();
        uint64_t usec   = timer_tick_to_usec(now);
        uint64_t both   = usec + amiga_time;
        uint32_t t_usec = both % 1000000;
        uint32_t t_sec  = both / 1000000;
        printf("Amiga seconds %lu.%06lu\n", t_sec, t_usec);
        rc = RC_SUCCESS;
    } else if (strncmp(argv[1], "cmd", 1) == 0) {
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

static rc_t
cmd_prom_temp(int argc, char * const *argv)
{
    adc_show_sensors();
    return (RC_SUCCESS);
}

static void
merge_args(char *buf, uint buflen, uint argc, char * const *argv)
{
    uint arg;
    uint len;

    buflen--;  // Save space for ending '\0'
    for (arg = 0; arg < argc; arg++) {
        if (buflen == 0)
            break;
        if (arg != 0) {
            *(buf++) = ' ';
            buflen--;
            if (buflen == 0)
                break;
        }
        len = strlen(argv[arg]);
        if (len > buflen)
            len = buflen;
        memcpy(buf, argv[arg], len);
        buf    += len;
        buflen -= len;
        if (buflen == 0)
            break;
    }
    *buf = '\0';
}

static int
cmd_prom_bank(int argc, char * const *argv)
{
    uint8_t  bank_start = 0;
    uint8_t  bank_end   = 0;
    uint     flag_bank_merge       = 0;
    uint     flag_bank_unmerge     = 0;
    uint     flag_bank_longreset   = 0;
    uint     flag_set_current_bank = 0;
    uint     flag_set_reset_bank   = 0;
    uint     flag_set_poweron_bank = 0;
    uint     flag_set_bank_name    = 0;
    uint     bank;
    uint     rc;
    char     name[32];

    if (argc < 2) {
        printf("prom bank requires an argument\n");
        printf("One of: ?, show, name, current, longreset, nextreset, "
               "poweron, merge, unmerge\n");
        return (1);
    }
    if (strcmp(argv[1], "?") == 0) {
        printf("  show                       Display all ROM bank information\n"
               "  merge <start> <end>        Merge banks for larger ROMs\n"
               "  unmerge <start> <end>      Unmerge banks\n"
               "  name <bank> <text>         Set bank name (description)\n"
               "  longreset <bank> [<bank>]  Banks to sequence at long reset\n"
               "  poweron <bank>             Default bank at poweron\n"
               "  current <bank>             Force new bank immediately\n"
               "  nextreset <bank>           Force new bank at next reset\n");
    } else if (strcmp(argv[1], "show") == 0) {
        config_bank_show();
        return (0);
    } else if ((strncmp(argv[1], "current", 3) == 0) ||
               (strcmp(argv[1], "set") == 0)) {
        flag_set_current_bank++;
    } else if (strncmp(argv[1], "poweron", 5) == 0) {
        flag_set_poweron_bank++;
    } else if (strncmp(argv[1], "longreset", 4) == 0) {
        flag_bank_longreset++;
    } else if (strncmp(argv[1], "nextreset", 4) == 0) {
        flag_set_reset_bank++;
    } else if (strcmp(argv[1], "merge") == 0) {
        flag_bank_merge++;
    } else if (strcmp(argv[1], "unmerge") == 0) {
        flag_bank_unmerge++;
    } else if (strncmp(argv[1], "name", 3) == 0) {
        flag_set_bank_name++;
    } else {
        printf("Unknown argument prom bank '%s'\n", argv[1]);
        return (1);
    }
    if (flag_set_bank_name) {
        if (argc < 4) {
            printf("prom bank %s requires a <bank> number and "
                   "\"name text\" or - to delete\n", argv[1]);
            return (1);
        }
        bank = atoi(argv[2]);
        if (bank >= ROM_BANKS) {
            printf("Bank %u is invalid (maximum bank is %u)\n",
                   bank, ROM_BANKS - 1);
            return (1);
        }
        merge_args(name, sizeof (name), argc - 3, argv + 3);
        if ((name[0] == '-') && (name[1] == '\0'))
            return (config_set_bank_name(bank, ""));
        else
            return (config_set_bank_name(bank, name));
    }
    if (flag_bank_longreset) {
        uint8_t     banks[ROM_BANKS];
        uint        cur;

        rc = 0;
        for (cur = 0; cur < ROM_BANKS; cur++) {
            if (cur + 2 < (uint) argc) {
                bank = atoi(argv[cur + 2]);
                if (bank >= ROM_BANKS) {
                    printf("Bank %u is invalid (maximum bank is %u)\n",
                           bank, ROM_BANKS - 1);
                    rc++;
                    continue;
                }
                banks[cur] = bank;
            } else {
                banks[cur] = 0xff;
            }
        }
        if (rc != 0)
            return (rc);
        return (config_set_bank_longreset(banks));
    }
    if (flag_set_current_bank || flag_set_poweron_bank || flag_set_reset_bank) {
        if (argc != 3) {
            printf("prom bank %s requires a <bank> number to set\n", argv[1]);
            return (1);
        }

        bank = atoi(argv[2]);
        if (bank >= ROM_BANKS) {
            printf("Bank %u is invalid (maximum bank is %u)\n",
                   bank, ROM_BANKS - 1);
            return (1);
        }

        return (config_set_bank(bank, flag_set_current_bank,
                                flag_set_poweron_bank, flag_set_reset_bank));
    }
    if (flag_bank_merge || flag_bank_unmerge) {
        uint        count;
        if (argc != 4) {
            printf("prom bank %s requires <start> and <end> bank numbers "
                   "(range)\n", argv[1]);
            return (1);
        }
        bank_start = atoi(argv[2]);
        bank_end   = atoi(argv[3]);
        count = bank_end - bank_start + 1;
        if (bank_start > bank_end) {
            printf("bank %u is not less than end %u\n", bank_start, bank_end);
            return (1);
        }
        if (bank_end >= ROM_BANKS) {
            printf("Bank %u is invalid (maximum bank is %u)\n",
                   bank_end, ROM_BANKS - 1);
            return (1);
        }
        if ((count != 1) && (count != 2) && (count != 4) && (count != 8)) {
            printf("Bank sizes must be a power of 2 (1, 2, 4, or 8 banks)\n");
            return (1);
        }
        if ((count == 2) && (bank_start & 1)) {
            printf("Two-bank ranges must start with an even bank number "
                   "(0, 2, 4, or 6)\n");
            return (1);
        }
        if ((count == 4) && (bank_start != 0) && (bank_start != 4)) {
            printf("Four-bank ranges must start with either bank 0 or "
                   "bank 4\n");
            return (1);
        }
        if ((count == 8) && (bank_start != 0)) {
            printf("Eight-bank ranges must start with bank 0\n");
            return (1);
        }

        return (config_set_bank_merge(bank_start, bank_end, flag_bank_unmerge));
    }
    return (0);
}

rc_t
cmd_prom(int argc, char * const *argv)
{
    enum {
        OP_NONE,
        OP_READ,
        OP_SERVICE,
        OP_WRITE,
        OP_ERASE_CHIP,
        OP_ERASE_SECTOR,
    } op_mode = OP_NONE;
    rc_t        rc;
    const char *arg = argv[0];
    char       *this_cmd = "prom";
    char       *temp_cmd;
    uint32_t    addr = 0;
    uint32_t    len = 0;

    if (strcmp(arg, "set") == 0)
        this_cmd = "set";
    temp_cmd = this_cmd;
    while (*arg != '\0') {
        if (*arg != *temp_cmd)
            break;
        arg++;
        temp_cmd++;
    }
    if (*arg == '\0') {
        argv++;
        argc--;
        if (argc < 1) {
            printf("error: prom command requires operation to perform\n");
            return (RC_USER_HELP);
        }
        arg = argv[0];
    }
    if (strcmp("bank", arg) == 0) {
        return (cmd_prom_bank(argc, argv));
    } else if (strcmp("cmd", arg) == 0) {
        uint32_t cmd;
        if ((argc < 2) || (argc > 3)) {
            printf("error: prom cmd <cmd> [<addr>]\n");
            return (RC_USER_HELP);
        }
        rc = parse_value(argv[1], (uint8_t *) &cmd, 4);
        if (rc != RC_SUCCESS)
            return (rc);

        if (argc == 3) {
            rc = parse_value(argv[2], (uint8_t *) &addr, 4);
            if (rc != RC_SUCCESS)
                return (rc);
        } else {
            addr = 0x00555;  // Default address for commands
        }

        prom_cmd(addr, cmd);
        return (RC_SUCCESS);
    } else if (strncmp(arg, "erase", 2) == 0) {
        if (argc < 2) {
            printf("error: prom erase requires either chip or "
                   "<addr> argument\n");
            return (RC_USER_HELP);
        }
        if (strcmp(argv[1], "chip") == 0) {
            op_mode = OP_ERASE_CHIP;
            argc--;
            argv++;
        } else {
            op_mode = OP_ERASE_SECTOR;
        }
    } else if (strcmp("id", arg) == 0) {
        return (prom_id());
    } else if (strcmp("log", arg) == 0) {
        uint max = 0;
        if (argc > 1) {
            if (strncmp(argv[1], "stats", 4) == 0) {
                max = 0x999;
            } else if (strncmp(argv[1], "full", 4) == 0) {
                max = (uint) -1;
            } else {
                rc = parse_value(argv[1], (uint8_t *) &max, 2);
                if (rc != RC_SUCCESS)
                    return (rc);
            }
        }
        if (max == 0)
            max = 10;
        return (address_log_replay(max));
    } else if (strcmp("mode", arg) == 0) {
        if ((argc > 1) && (argv[1][0] >= '0') && (argv[1][0] <= '4')) {
            uint mode = argv[1][0] - '0';
            prom_mode(mode);
        } else {
            prom_show_mode();
            printf("\n");
        }
        return (RC_SUCCESS);
    } else if (strcmp("name", arg) == 0) {
        if (argc <= 1) {
            config_name(NULL);
        } else {
            char name[32];
            merge_args(name, sizeof (name), argc - 1, argv + 1);
            config_name(name);
        }
        return (RC_SUCCESS);
    } else if (strcmp("read", arg) == 0) {
        op_mode = OP_READ;
    } else if (strcmp("service", arg) == 0) {
        op_mode = OP_SERVICE;
    } else if (strcmp("temp", arg) == 0) {
        return (cmd_prom_temp(argc - 1, argv + 1));
    } else if (strcmp("write", arg) == 0) {
        op_mode = OP_WRITE;
    } else if (strcmp("test", arg) == 0) {
        rc = pin_tests(1);
        if (rc == 0)
            rc = prom_test();
        if (rc != 0)
            led_alert(1);
        return (rc);
    } else {
        printf("error: unknown prom operation %s\n", arg);
        return (RC_USER_HELP);
    }

    if (argc > 1) {
        rc = parse_value(argv[1], (uint8_t *) &addr, 4);
        if (rc != RC_SUCCESS)
            return (rc);
    }

    if (argc > 2) {
        rc = parse_value(argv[2], (uint8_t *) &len, 4);
        if (rc != RC_SUCCESS)
            return (rc);
    }

    switch (op_mode) {
        case OP_READ:
            if (argc != 3) {
                printf("error: prom %s requires <addr> and <len>\n", arg);
                return (RC_USER_HELP);
            }
            rc = prom_read_binary(addr, len);
            break;
        case OP_WRITE:
            if (argc != 3) {
                printf("error: prom %s requires <addr> and <len>\n", arg);
                return (RC_USER_HELP);
            }
            rc = prom_write_binary(addr, len);
            break;
        case OP_ERASE_CHIP:
            printf("Chip erase\n");
            if (argc != 1) {
                printf("error: prom erase chip does not have arguments\n");
                return (RC_USER_HELP);
            }
            rc = prom_erase(ERASE_MODE_CHIP, 0, 0);
            break;
        case OP_ERASE_SECTOR:
            printf("Sector erase %lx", addr);
            if (len > 0)
                printf(" len %lx", len);
            printf("\n");
            if ((argc < 2) || (argc > 3)) {
                printf("error: prom erase sector requires <addr> and "
                       "allows optional <len>\n");
                return (RC_USER_HELP);
            }
            rc = prom_erase(ERASE_MODE_SECTOR, addr, len);
            break;
        case OP_SERVICE:
            msg_usb_service();
            return (RC_SUCCESS);
        default:
            printf("BUG: op_mode\n");
            return (RC_FAILURE);
    }

    if (rc != 0)
        printf("FAILURE %d\n", rc);
    return (rc);
}

rc_t
cmd_map(int argc, char * const *argv)
{
    uint third = (ARRAY_SIZE(memmap) + 2) / 3;
    uint ent;
    for (ent = 0; ent < third; ent++) {
        printf("    %-6s %08x", memmap[ent].name, memmap[ent].addr);
        if (ent + third < ARRAY_SIZE(memmap))
            printf("    %-6s %08x",
                   memmap[ent + third].name, memmap[ent + third].addr);
        if (ent + third * 2 < ARRAY_SIZE(memmap))
            printf("    %-6s %08x",
                   memmap[ent + third * 2].name, memmap[ent + third * 2].addr);
        printf("\n");
    }
    return (RC_SUCCESS);
}

static void
shutdown_all(void)
{
    uart_flush();
    usb_shutdown();
    usb_signal_reset_to_host(1);
    timer_delay_msec(30);
    msg_shutdown();
    adc_shutdown();
    timer_shutdown();
    /* Put all peripherals in reset */
    /* Clear NVIC? */
}

rc_t
cmd_reset(int argc, char * const *argv)
{
    if (argc < 2) {
        printf("Resetting...\n");
        shutdown_all();
        reset_cpu();
        return (RC_FAILURE);
    } else if (strncmp(argv[1], "dfu", 3) == 0) {
        uint isrom = (argv[1][3] == 'r');
        printf("Resetting to DFU%s...\n", isrom ? " in ROM" : "");
        shutdown_all();
        reset_dfu(isrom);
        return (RC_SUCCESS);
    } else if (strcmp(argv[1], "usb") == 0) {
        timer_delay_msec(1);
        usb_shutdown();
        usb_signal_reset_to_host(1);
        usb_startup();
        return (RC_SUCCESS);
    } else if (strcmp(argv[1], "amiga") == 0) {
        uint hold = 0;
        uint longreset = 0;
        while (argc > 2) {
            if (strcmp(argv[2], "hold") == 0)
                hold = 1;
            else if (strcmp(argv[2], "long") == 0)
                longreset = 1;
            else
                printf("Invalid reset amiga \"%s\"\n", argv[2]);
            argc--;
            argv++;
        }
        kbrst_amiga(hold, longreset);
        if (hold)
            printf("Holding Amiga in reset\n");
        else
            printf("Resetting Amiga\n");
        return (RC_SUCCESS);
    } else if (strcmp(argv[1], "prom") == 0) {
        printf("Resetting Amiga and flash ROM\n");
        kbrst_amiga(0, 0);
        timer_delay_msec(200);
        amiga_not_in_reset = 0;
        ee_enable();
        ee_read_mode();
        ee_set_bank(config.bi.bi_bank_current);
        ee_disable();
        return (RC_SUCCESS);
    } else {
        printf("Unknown argument %s\n", argv[1]);
        return (RC_USER_HELP);
    }
}

rc_t
cmd_cpu(int argc, char * const *argv)
{
    if (argc < 2)
        return (RC_USER_HELP);
    if (strncmp(argv[1], "regs", 1) == 0) {
        fault_show_regs(NULL);
    } else if (strncmp(argv[1], "hardfault", 2) == 0) {
        fault_hard();
    } else {
        printf("Unknown argument %s\n", argv[1]);
        return (RC_USER_HELP);
    }
    return (RC_SUCCESS);
}

rc_t
cmd_usb(int argc, char * const *argv)
{
    if (argc < 2)
        return (RC_USER_HELP);
    if (strncmp(argv[1], "disable", 1) == 0) {
        timer_delay_msec(1);
        usb_shutdown();
        usb_signal_reset_to_host(0);
        return (RC_SUCCESS);
    } else if (strncmp(argv[1], "regs", 3) == 0) {
        usb_show_regs();
    } else if (strcmp(argv[1], "reset") == 0) {
        timer_delay_msec(1);
        usb_shutdown();
        usb_signal_reset_to_host(1);
        usb_startup();
        return (RC_SUCCESS);
    } else if (strncmp(argv[1], "stat", 2) == 0) {
        usb_show_stats();
    } else {
        printf("Unknown argument %s\n", argv[1]);
        return (RC_USER_HELP);
    }
    return (RC_SUCCESS);
}

rc_t
cmd_gpio(int argc, char * const *argv)
{
    int arg;
    if (argc < 2) {
        gpio_show(-1, 0xffff);
        return (RC_SUCCESS);
    }

    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        int port = -1;
        uint16_t pins[NUM_GPIO_BANKS];
        const char *assign = NULL;

        memset(pins, 0, sizeof (pins));

        if (gpio_name_match(&ptr, pins) != 0) {
            if ((*ptr == 'p') || (*ptr == 'P'))
                ptr++;

            /* Find port, if specified */
            if ((*ptr >= 'a') && (*ptr <= 'f'))
                port = *(ptr++) - 'a';
            else if ((*ptr >= 'A') && (*ptr <= 'F'))
                port = *(ptr++) - 'A';

            /* Find pin, if specified */
            if ((*ptr >= '0') && (*ptr <= '9')) {
                uint pin = *(ptr++) - '0';
                if ((*ptr >= '0') && (*ptr <= '9'))
                    pin = pin * 10 + *(ptr++) - '0';
                if (pin > 15) {
                    printf("Invalid argument %s\n", argv[arg]);
                    return (RC_BAD_PARAM);
                }
                pins[port] = BIT(pin);
            } else if (*ptr == '*') {
                ptr++;
                pins[port] = 0xffff;
            }
        }

        if (*ptr == '=') {
            assign = ptr + 1;
            ptr = "";
            if (port == -1) {
                uint tport;
                for (tport = 0; tport < NUM_GPIO_BANKS; tport++)
                    if (pins[tport] != 0)
                        break;
                if (tport == NUM_GPIO_BANKS) {
                    printf("You must specify the GPIO to assign: %s\n",
                           argv[arg]);
                    return (RC_BAD_PARAM);
                }
            }
        }
        if (*ptr != '\0') {
            printf("Invalid argument %s\n", argv[arg]);
            return (RC_BAD_PARAM);
        }

        if (assign != NULL) {
            if (port == -1) {
                for (port = 0; port < NUM_GPIO_BANKS; port++) {
                    if (pins[port] != 0) {
                        gpio_assign(port, pins[port], assign);
                    }
                }
            } else {
                gpio_assign(port, pins[port], assign);
            }
        } else {
            if (port == -1) {
                for (port = 0; port < NUM_GPIO_BANKS; port++)
                    gpio_show(port, pins[port]);
            } else {
                gpio_show(port, pins[port]);
            }
        }
    }

    return (RC_SUCCESS);
}


static const char *const config_flag_bits[] = {
    "OEWE_PIN_SHOW", "", "", "",
        "", "", "", "",
    "", "", "", "",
        "", "", "", "",
    "", "", "", "",
        "", "", "", "",
    "", "", "", "",
        "", "", "", "",
};

static void
decode_bits(const char *const *bits, uint32_t flags)
{
    uint bit;
    uint printed = 0;

    for (bit = 0; bit < 32; bit++) {
        if (flags & BIT(bit)) {
            if (printed++)
                printf(", ");
            if (bits[bit][0] == '\0') {
                printf("bit%u", bit);
            } else {
                printf("%s", bits[bit]);
            }
        }
    }
}

static uint
match_bits(const char *const *bits, const char *name)
{
    uint bit;
    for (bit = 0; bit < 32; bit++) {
        if (strcasecmp(name, bits[bit]) == 0)
            return (bit);
    }
    return (bit);
}

#define CFOFF(x) offsetof(config_t, x), sizeof (config.x)

#define MODE_DEC          0       // Show value in decimal
#define MODE_HEX          BIT(0)  // Show value in hexadecimal
#define MODE_STRING       BIT(1)  // Show string
#define MODE_BIT_FLAGS    BIT(2)  // Decode debug flags
#define MODE_FAN_AUTO     BIT(3)  // Interpret BIT(7) as "auto"; 0 = decimal
#define MODE_SIGNED       BIT(4)  // Decimal value is signed
#define MODE_SW_TIME      BIT(5)  // Special encoded seconds / minutes
typedef struct {
    const char *cs_name;
    const char *cs_desc;
    uint16_t    cs_offset;  // Offset into config structure
    uint8_t     cs_size;    // Size of config value in bytes
    uint8_t     cs_mode;    // Mode bits for display (1=hex)
} config_set_t;
static const config_set_t config_set[] = {
    { "board_rev",      "Board Revision",
      CFOFF(board_rev), MODE_DEC },
    { "flags",          "",
      CFOFF(flags), MODE_HEX | MODE_BIT_FLAGS },
    { "led",            "LED",
      CFOFF(led_level), MODE_DEC },
    { "mode",           "Prom mode ",
      CFOFF(led_level), MODE_DEC },
    { "name",          "Board name",
      CFOFF(name), MODE_STRING },
    { "sw_bank",       "ROM Switcher bank",
      offsetof(config_t, nv_mem[1]), 1, MODE_DEC },
    { "sw_timeout",    "ROM Switcher timeout",
      offsetof(config_t, nv_mem[0]), 1, MODE_SW_TIME },
};

rc_t
cmd_set(int argc, char * const *argv)
{
    if (argc <= 1) {
        uint pos;
        for (pos = 0; pos < ARRAY_SIZE(config_set); pos++) {
            char buf[32];
            const config_set_t *c = &config_set[pos];
            uint cs_mode = c->cs_mode;
            uint value = 0;
            void *src = (void *) ((uintptr_t) &config + c->cs_offset);
            if (cs_mode & MODE_STRING) {
                /* String */
                sprintf(buf, "%s \"%.*s\"",
                        c->cs_name, c->cs_size, (char *)src);
            } else if (cs_mode & MODE_HEX) {
                /* Hexadecimal */
                memcpy(&value, src, c->cs_size);
                sprintf(buf, "%s %0*x", c->cs_name, c->cs_size * 2, value);
            } else {
                /* Decimal : MODE_DEC */
                memcpy(&value, src, c->cs_size);
                if ((cs_mode & MODE_FAN_AUTO) && (value & BIT(7))) {
                    sprintf(buf, "%s %s", c->cs_name, "auto");
                } else if (cs_mode & MODE_SW_TIME) {
                    if (value & BIT(7))
                        value = (value & 0x7f) * 60;  // minutes
                    sprintf(buf, "%s %u", c->cs_name, value);
                } else if (cs_mode & MODE_SIGNED) {
                    if (c->cs_size == 1)
                        value = (int8_t) value;
                    else if (c->cs_size == 2)
                        value = (int16_t) value;
                    sprintf(buf, "%s %d", c->cs_name, value);
                } else {
                    sprintf(buf, "%s %u", c->cs_name, value);
                }
            }
            printf("%s%*s%s", buf, 24 - strlen(buf), "", c->cs_desc);
            if (cs_mode & MODE_BIT_FLAGS) {
                if (strncmp(c->cs_name, "flags", 4) == 0) {
                    /* Decode config flags */
                    if (value == 0)
                        printf("Config flags");
                    decode_bits(config_flag_bits, value);
                }
            }
            if (strcmp(c->cs_name, "led") == 0) {
                printf(" %u%%%s",
                       config.led_level, led_alert_state ? " ALERT" : "");
            } else if (strcmp(c->cs_name, "mode") == 0) {
                prom_show_mode();
            }
            printf("\n");
        }
        return (RC_SUCCESS);
    }

    if (strcmp(argv[1], "bank") == 0) {
        return (cmd_prom(argc - 1, argv + 1));
    } else if (strcmp(argv[1], "board_rev") == 0) {
        if (argc <= 2) {
            printf("%s %u\n", argv[0], config.board_rev);
            return (RC_SUCCESS);
        }
        if (argc != 3) {
            printf("set %s requires an argument\n", argv[1]);
            return (RC_FAILURE);
        }
        uint rev = atoi(argv[2]);
        if (rev > 0xff) {
            printf("FAIL: Valid range is 0 to 255\n");
            return (RC_FAILURE);
        }
        if (config.board_rev != rev) {
            config.board_rev = rev;
            config_updated();
        }
    } else if ((strcmp(argv[1], "help") == 0) ||
               (strcmp(argv[1], "?") == 0)) {
        return (RC_USER_HELP);
    } else if (strncmp(argv[1], "flags", 4) == 0) {
        int      arg;
        int      pos = 0;
        int      add_sub = 0;
        uint     bit;
        uint     do_save = 0;
        uint     did_set = 0;
        uint32_t value;
        uint32_t nvalue = 0;
        if (argc <= 2) {
            printf("Config flags are a combination of bits: "
                   "specify all bit numbers or names\n");
            for (bit = 0; bit < 32; bit++)
                if (config_flag_bits[bit][0] != '\0')
                    printf(" %c %2u  %s\n",
                           config.flags & BIT(bit) ? '*' : ' ',
                           bit, config_flag_bits[bit]);
            printf("Current config %08lx  ", config.flags);
            decode_bits(config_flag_bits, config.flags);
            printf("\n");
            return (RC_SUCCESS);
        }
        for (arg = 2; arg < argc; arg++) {
            char *flagname = argv[arg];
            if (*flagname == '+') {
                flagname++;
                add_sub = 1;
            } else if (*flagname == '-') {
                flagname++;
                add_sub = -1;
            }
            if (*flagname == '\0')
                continue;
            if (strcasecmp(flagname, "save") == 0) {
                do_save = 1;
                continue;
            } else if ((bit = match_bits(config_flag_bits, flagname)) < 32) {
                if (did_set == 0)
                    nvalue = 0;
                nvalue |= BIT(bit);
                did_set = 1;
            } else {
                if ((sscanf(flagname, "%x%n", &value, &pos) != 1) ||
                    (flagname[pos] != '\0')) {
                    printf("Invalid argument: %s\n", flagname);
                    return (RC_USER_HELP);
                }
                if ((pos >= 4) || (value >= 32))
                    nvalue = value;
                else
                    nvalue |= BIT(value);
                did_set = 1;
            }
        }
        if (add_sub > 0)
            nvalue = config.flags | nvalue;
        else if (add_sub < 0)
            nvalue = config.flags & ~nvalue;

        if (config.flags != nvalue) {
            config.flags = nvalue;
            printf("Config flags %08lx ", nvalue);
            decode_bits(config_flag_bits, nvalue);
            printf("\n");
        }
        if (do_save)
            config_updated();
    } else if (strcmp(argv[1], "led") == 0) {
        if (argc <= 2) {
            printf("led %u %%", config.led_level);
            if (led_alert_state)
                printf(" alert");
            printf("\n");
            return (RC_SUCCESS);
        }
        if (argc != 3) {
            printf("set led requires a percentage\n");
            return (RC_FAILURE);
        }
        if (strcmp(argv[2], "alert") == 0) {
            led_alert(1);
            return (RC_SUCCESS);
        } else if (strcmp(argv[2], "normal") == 0) {
            led_alert(0);
            return (RC_SUCCESS);
        }
        uint percent = atoi(argv[2]);
        if (percent > 100) {
            printf("FAIL: Percent range is 0 to 100\n");
            return (RC_FAILURE);
        }
        led_set_brightness(percent);
        config_set_led(percent);
    } else if (strcmp(argv[1], "mode") == 0) {
        return (cmd_prom(argc - 1, argv + 1));
    } else if (strcmp(argv[1], "name") == 0) {
        if (argc <= 2) {
            config_name(NULL);
        } else {
            char name[32];
            merge_args(name, sizeof (name), argc - 2, argv + 2);
            config_name(name);
        }
        return (RC_SUCCESS);
    } else if (strncmp(argv[1], "sw_bank", 4) == 0) {
        if (argc <= 2) {
            printf("%s %u\n", argv[1], config.nv_mem[1]);
        } else {
            uint bank = atoi(argv[2]);
            if (bank > 7) {
                printf("FAIL: Bank range is 0 to 7\n");
                return (RC_FAILURE);
            }
            if (config.nv_mem[1] != bank) {
                config.nv_mem[1] = bank;
                config_updated();
            }
        }
        return (RC_SUCCESS);
    } else if (strncmp(argv[1], "sw_timeout", 4) == 0) {
        uint value;
        if (argc <= 2) {
            value = config.nv_mem[0];
            if (value & BIT(7))
                value = (value & 0x7f) * 60;  // minutes
            printf("%s %u\n", argv[1], value);
        } else {
            uint svalue = atoi(argv[2]);
            value = svalue;
            if (value > 127) {
                value /= 60;
                if (value > 127)
                    value = 127;
                value |= BIT(7);
            }
            if (config.nv_mem[0] != value) {
                config.nv_mem[0] = value;
                config_updated();
                if (value & BIT(7))
                    value = (value & 0x7f) * 60;  // minutes
                if (value != svalue) {
                    printf("sw_timeout %u rounded to %u seconds\n",
                           svalue, value);
                }
            }
        }
        return (RC_SUCCESS);
    } else {
        printf("set \"%s\" unknown argument\n", argv[1]);
        return (RC_USER_HELP);
    }

    return (RC_SUCCESS);
}


rc_t
cmd_snoop(int argc, char * const *argv)
{
    uint mode = CAPTURE_SW;
    if (argc > 1) {
        if (strcmp(argv[1], "addr") == 0) {
            mode = CAPTURE_ADDR;
        } else if (strncmp(argv[1], "low", 2) == 0) {
            mode = CAPTURE_DATA_LO;
        } else if (strncmp(argv[1], "high", 2) == 0) {
            mode = CAPTURE_DATA_HI;
        } else {
            printf("snoop \"%s\" unknown argument\n", argv[1]);
            return (RC_USER_HELP);
        }
    }
    bus_snoop(mode);

    return (RC_SUCCESS);
}
