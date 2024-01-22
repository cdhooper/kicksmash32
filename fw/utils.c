/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * Generic POSIX function emulation.
 */

#include "printf.h"
#include "utils.h"
#include <stdbool.h>
#include "board.h"
#include "main.h"
#include "clock.h"

#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/dbgmcu.h>
#define HCLK_FREQ clock_get_hclk()
#define APB1_FREQ clock_get_apb1()
#define APB2_FREQ clock_get_apb2()
#define DBGMCU_DEVID (DBGMCU_IDCODE & DBGMCU_IDCODE_DEV_ID_MASK)
#define DBGMCU_REVID ((DBGMCU_IDCODE & DBGMCU_IDCODE_REV_ID_MASK) >> 16)
#if defined(STM32F4)
#define FLASH_BASE 0x08000000U
#endif

static void SystemInit_post(void) { }


#ifdef STM32F103xE
#define COMPILE_CPU "STM32F103xE"
#elif defined(STM32F105xC)
#define COMPILE_CPU "STM32F105xC"
#elif defined(STM32F105xE)
#define COMPILE_CPU "STM32F105xE"
#elif defined(STM32F107xC)
#define COMPILE_CPU "STM32F107xC"
#elif defined(STM32F100xB)
#define COMPILE_CPU "STM32F100xB"
#elif defined(STM32F100xE)
#define COMPILE_CPU "STM32F100xE"
#elif defined(STM32F1) || defined(STM32F1XX)
#define COMPILE_CPU "STM32F1"

#elif defined(STM32F407xx)
#define COMPILE_CPU "STM32F407"
#elif defined(STM32F4) || defined(STM32F4XX)
#define COMPILE_CPU "STM32F4"
#else
#define COMPILE_CPU "unknown"
#endif

/* Force a variable into the custom "persist" section */
#define SRAM_PERSIST __attribute__((section(".persist,\"aw\",%nobits@")))

/*
 * The System memory base appears at different addresses on different
 * STM32 processors.
 */
#if defined(STM32F103xE)
#define SYSTEM_MEMORY_BASE 0x1ffff000  // STM32F103
#elif defined(STM32F105xE) || defined(STM32F107xC)
#define SYSTEM_MEMORY_BASE 0x1fffb000  // STM32F105/STM32F107
#else
#define SYSTEM_MEMORY_BASE 0x1fff0000  // STM32F4xx
#endif

#define RESET_TO_BOOTLOADER_MAGIC 0xd0df00ba

SRAM_PERSIST static uint32_t system_reset_to_dfu_magic;

/* Force exceptions/interrupts to use new VTOR */
static void dmb(void)
{
    __asm__ volatile("dmb");
}

void
reset_check(void)
{
    if (system_reset_to_dfu_magic == RESET_TO_BOOTLOADER_MAGIC) {
        system_reset_to_dfu_magic = 1;

        uint32_t  addr = SYSTEM_MEMORY_BASE;
        uint32_t *base = (uint32_t *)(uintptr_t) addr;

        /* Set the vector table pointer */
        SCB_VTOR = addr;
        dmb();
        /* Set the stack pointer */
        __asm__("MSR msp, %0" : : "r" (ADDR32(base)[0]));   // SP = base[0]
        /* Set the program counter (jump) */
        __asm__("BX %0\n\t" : : "r" (ADDR32(base)[1]));     // PC = base[1]
    } else {
        system_reset_to_dfu_magic = 2;

        /*
         * STM32F107/STM32F407 ROM DFU does not correctly re-initialize
         * VTOR following exit from DFU mode, it must be done here.
         */
        SCB_VTOR = FLASH_BASE;  // Set the vector table pointer
        dmb();

        /* Continue normal HAL SystemInit() */
        SystemInit_post();
    }
}

void
reset_cpu(void)
{
    scb_reset_system();
}

void
reset_dfu(void)
{
    system_reset_to_dfu_magic = RESET_TO_BOOTLOADER_MAGIC;
    scb_reset_system();
}

void
show_reset_reason(void)
{
    uint32_t reg = RCC_CSR;
    RCC_CSR = RCC_CSR_RMVF;  // Clear reset flags

    if (reg & RCC_CSR_LPWRRSTF)
        printf("    %s\n", "Low-power reset");
    if (reg & RCC_CSR_WWDGRSTF)
        printf("    %s\n", "Window Watchdog reset");
    if (reg & RCC_CSR_IWDGRSTF)
        printf("    %s\n", "Independent Watchdog reset");
    if (reg & RCC_CSR_PORRSTF)
        printf("    %s\n", "Power-on reset");
    else if (reg & RCC_CSR_SFTRSTF)
        printf("    %s\n", "Software reset");
    else if (reg & RCC_CSR_PINRSTF)
        printf("    %s\n", "NRST pin reset");
}

void
identify_cpu(void)
{
    const char *runtime_cpu;
    switch (SCB_CPUID) {
        case 0x410fc241:
            runtime_cpu = "STM32F4";
            break;
        case 0x411fc231:
            runtime_cpu = "STM32F1";
            break;
        case 0x412fc230:
            runtime_cpu = "STM32F2/STM32L";
            break;
        case 0x412fc231:
            runtime_cpu = "GD32F1";
            break;
        default:
            runtime_cpu = "?";
            break;
    }
    printf("    CPUID=%08lx Dev=%04lx Rev=%04lx (compile: %s BOARD=%d)\n",
           SCB_CPUID, DBGMCU_DEVID, DBGMCU_REVID, COMPILE_CPU, BOARD_REV);
    printf("    Hardware: %s", runtime_cpu);
    if (DBGMCU_DEVID != 0) {
        const char *core_type;
        const char *core_rev = "?";
        switch (DBGMCU_DEVID) {
            case 0x0410:
                core_type = "Medium-density";
                switch (DBGMCU_REVID) {
                    case 0x0000:
                        core_rev = "A";
                        break;
                    case 0x2000:
                        core_rev = "B";
                        break;
                    case 0x2001:
                        core_rev = "Z";
                        break;
                    case 0x2003:
                        core_rev = "1, 2, 3, X or Y";
                        break;
                    default:
                        break;
                }
                break;
            case 0x0411:  // STM32F4
                core_type = "STM32F407-Disco";
                break;
            case 0x0412:
                core_type = "Low-density";
                switch (DBGMCU_REVID) {
                    case 0x1000:
                        core_rev = "A";
                        break;
                    default:
                        break;
                }
                break;
            case 0x0413:  // STM32F405/STM32F407 and STM32F415/STM32F417
                core_type = "STM32F405/07";
                switch (DBGMCU_REVID) {
                    case 0x1000:
                        core_rev = "A";
                        break;
                    case 0x1001:
                        core_rev = "Z";
                        break;
                    case 0x1003:
                        core_rev = "1";
                        break;
                    case 0x1007:
                        core_rev = "2";
                        break;
                    case 0x100F:
                        core_rev = "Y/4";
                        break;
                    case 0x101F:
                        core_rev = "5/6";
                        break;
                    default:
                        break;
                }
                break;
            case 0x0414:
                core_type = "High-density";
                switch (DBGMCU_REVID) {
                    case 0x1000:
                        core_rev = "A or 1";
                        break;
                    case 0x1001:
                        core_rev = "Z";
                        break;
                    case 0x1003:
                        core_rev = "1, 2, 3, X or Y";
                        break;
                    default:
                        break;
                }
                break;
            case 0x0418:
                core_type = "Connectivity";
                switch (DBGMCU_REVID) {
                    case 0x1000:
                        core_rev = "A";
                        break;
                    case 0x1001:
                        core_rev = "Z";
                        break;
                    default:
                        break;
                }
                break;
            case 0x0419:  // STM32F42xxx and STM32F43xxx
                core_type = "STM32F42/F43";
                switch (DBGMCU_REVID) {
                    case 0x1000:
                        core_rev = "A";
                        break;
                    case 0x1003:
                        core_rev = "Y";
                        break;
                    case 0x1007:
                        core_rev = "1";
                        break;
                    case 0x2001:
                        core_rev = "3";
                        break;
                    case 0x2003:
                        core_rev = "5/B";
                        break;
                    default:
                        break;
                }
                break;
            case 0x0430:
                core_type = "XL-density";
                switch (DBGMCU_REVID) {
                    case 0x1000:
                        core_rev = "A or 1";
                        break;
                    default:
                        break;
                }
                break;
            default:
                core_type = "Unknown-density";
                break;
        }
        printf("    %s revision %s\n", core_type, core_rev);
    }
    printf("    HCLK=%ld MHz  APB1=%ld MHz  APB2=%ld MHz\n",
           HCLK_FREQ / 1000000, APB1_FREQ / 1000000, APB2_FREQ / 1000000);

#if 0
#ifdef STM32F1XX
    printf("    FlashSize=%u KB FlashPage=%u KB\n", *ADDR16(FLASHSIZE_BASE),
            FLASH_PAGE_SIZE / 1024);
#else
    printf("    FlashSize=%u KB\n", *ADDR16(FLASHSIZE_BASE));
#endif
#endif
}

/* Deal with annoying newlib warnings */
void _close(void);
void _close(void) { }
void _close_r(void) __attribute__((alias("_close")));
void _lseek(void)   __attribute__((alias("_close")));
void _read(void)    __attribute__((alias("_close")));
void _write(void)   __attribute__((alias("_close")));
