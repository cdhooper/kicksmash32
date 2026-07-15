/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * STM32 IRQ handling code.
 */

#include "printf.h"
#include "board.h"
#include "main.h"
#include "uart.h"
#include "utils.h"
#include "led.h"
#include "irq.h"
#include "cmdline.h"
#include "mem_access.h"
#include "main.h"
#include "usb.h"
#include "readline.h"
#include <string.h>

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>


/*
 * STM32 exception and interrupt entry register stack frame.
 * The CPU automatically saves part of these registers, and the entry
 * code saves the rest. There is some cost in speed to saving all
 * registers, but having a full set of registers means it will be
 * easier to debug when things fail.
 */
typedef struct {
    /* Below is stacked by exception entry code */
    uint32_t sp;
    uint32_t r4;
    uint32_t r5;
    uint32_t r6;
    uint32_t r7;
    uint32_t r8;
    uint32_t r9;
    uint32_t r10;
    uint32_t r11;
    uint32_t lr_e;  // Exception return LR

    /* Below is stacked automatically by the CPU exception vector */
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;

    uint32_t lr;
    uint32_t pc;
    uint32_t psr;
} reg_frame_t;

/*
 * The following entry applies to both exceptions and interrupts.
 * It finishes the reg_frame_t stack frame which was started by the CPU
 * on exception or interrupt entry. All registers are saved.
 */
#define EXCEPTION_ENTRY \
    __asm__("MRS    r3, msp");            /* SP now */                    \
    __asm__("ADD    r3, r3, #0x20");      /* SP prior to interrupt */     \
    __asm__("STMFD  sp!, {r3-r11,lr}");   /* Save remaining registers */  \
    __asm__("MOV    %0, r3" : "=r" (sp))  /* Provide SP prior to except */

/*
 * The following exit code applies to both exceptions and interrupts.
 * It restores registers previously saved by the entry, setting the
 * pointer to the location of exception/interrupt entry.  The "POP {pc}"
 * instruction reloads the program counter from the LR value saved by
 * the processor. This value is usually one of the Cortex LR values on
 * exception. A load of this value causes the processor to either handle
 * a lower priority exception/interrupt or return to the instruction at
 * which the exception/interrupt occurred.
 */
#define EXCEPTION_EXIT \
    __asm__("LDMFD  sp!, {r3-r11}");      /* Ignore SP; restore R4-R11 */ \
    __asm__("POP    {pc}")                /* Return from exception */

/**
 * stm32_instruction_width() returns the size of the instruction
 *                           at the specified memory address.
 *
 * @param [in]  addr - Memory address.
 *
 * @return      0    - Address is invalid.
 * @return      2    - Two-byte instruction (Thumb).
 * @return      4    - Four-byte instruction (Thumb2).
 */
static uint
stm32_instruction_width(uint32_t addr)
{
    /* First range check PC */
    if ((addr & 1) != 0)
        return (0);  // Invalid

    if ((addr < 0x08000000) || (addr >= 0x08100000))
        return (0);  // Outside STM32 flash address range

    /*
     * Thumb2 has several 4-byte instructions.
     *     Example      Instruction
     *     ff f7 65 ff  bl 0x803c768 from PC=0x0803c89a
     *     ed f7 a5 ff  bl 0x8029f30 from PC=0x0803bfe2
     *     f1 f7 11 f9  bl 0x802d20c from PC=0x0803bfe6
     *     1d f0 ab fe  bl 0x8059db4 from PC=0x0803c05a
     *     bc fa 33 02  blx 0x6f81f0c from PC=8081638
     *     14 f0 01 0a  ands.w  r10, r4, #1
     *     4f f0 fe 08  mov.w r8, #254
     *     4f fa 88 f0  sxtb.w r0, r8
     *     bd e8 fc 87  ldmia.w sp!, {r2-r10,pc}
     *
     * If instruction word[0] >> 11 equals any of the following, then
     * it's a 4-byte instruction:
     *     11101 (0x1d)
     *     11110 (0x1e)
     *     11111 (0x1f)
     * Otherwise, it's assumed to be a 2-byte instruction.
     */
    switch (*ADDR16(addr) >> 11) {
        case 0x1d:  // 11101
        case 0x1e:  // 11110
        case 0x1f:  // 11111
            return (4);  // Thumb2 instruction
        default:
            return (2);  // Thumb instruction
    }
}

/**
 * fault_show_regs() displays additional fault status registers which are
 *                   present in the Cortex-M3 core.
 *
 * @param [in]  None.
 * @return      None.
 */
void
fault_show_regs(const void *sp)
{
    reg_frame_t pframe;
    const reg_frame_t *sf;

    if (sp != NULL) {
        sf = (const reg_frame_t *) sp - 1;
    } else {
        // XXX: make this point to the last fault frame, if present
        sf = &pframe;
        memset(&pframe, 0, sizeof (pframe));
        pframe.sp = (uint32_t) &pframe;
    }
    printf("R0=%08lx R3=%08lx R6=%08lx  R9=%08lx R12=%08lx PC=%08lx\n"
           "R1=%08lx R4=%08lx R7=%08lx R10=%08lx PSR=%08lx SP=%08lx\n"
           "R2=%08lx R5=%08lx R8=%08lx R11=%08lx LRE=%08lx LR=%08lx\n",
            sf->r0, sf->r3, sf->r6, sf->r9, sf->r12, sf->pc,
            sf->r1, sf->r4, sf->r7, sf->r10, sf->psr, sf->sp,
            sf->r2, sf->r5, sf->r8, sf->r11, sf->lr_e, sf->lr);
    printf("NVIC ICPR %08lx %08lx %08lx\n",
           NVIC_ICPR(0), NVIC_ICPR(1), NVIC_ICPR(2));
    printf("NVIC IABR %08lx %08lx %08lx\n",
           NVIC_IABR(0), NVIC_IABR(1), NVIC_IABR(2));
    if (SCB_ICSR != 0) {
        uint8_t vect = (uint8_t) SCB_ICSR;
        static const char * const exception_vects[] = {
            "Thread mode",        // 0
            "Reserved",           // 1
            "NMI",                // 2
            "Hard fault",         // 3
            "Memory mgmt fault",  // 4
            "Bus fault",          // 5
            "Usage fault",        // 6
            "Reserved",           // 7
            "Reserved",           // 8
            "Reserved",           // 9
            "Reserved",           // 10
            "SVCall",             // 11
            "Debug",              // 12
            "Reserved",           // 13
            "PendSV",             // 14
            "SysTick",            // 15
        };
        printf("SCB ICSR: %08lx  vect=0x%x", SCB_ICSR, vect);
        if (vect < 0x10)
            printf(":%s", exception_vects[vect]);
        printf("\n");
    }
    if (SCB_HFSR != 0)
        printf("SCB HFSR: %08lx\n", SCB_HFSR);
    if (SCB_SHCSR != 0)
        printf("SCB SHCSR: %08lx\n", SCB_SHCSR);
    if (SCB_CFSR != 0) {
        printf("SCB CFSR: %08lx\n", SCB_CFSR);
        if (SCB_BFAR != 0)
            printf("SCB BFAR: %08lx\n", SCB_BFAR);
        if (SCB_MMFAR != 0)
            printf("SCB MMFAR: %08lx\n", SCB_MMFAR);
    }
}

void
fault_hard(void)
{
    extern uint _data;
    /* Access 2 MB beyond SRAM start */
    (void) *(volatile uint32_t *)ADDR32(((uintptr_t) &_data) + 0x00200000);
}

static void
cmdline_loop(void)
{
    printf("New command line\n");
    while (1) {
        main_poll();
        cmdline();
    }
}

static void
cmdline_loop_uart(void)
{
    printf("Single-tasking command line\n");
    while (1) {
        cmdline();
    }
}

static uint
cmdline_or_reset(void)
{
    static uint8_t ran_before = 0;

    switch (ran_before++) {
        case 0:
            /* Attempt to resume */
            printf("Attempting resume\n");
            return (0);
        case 1:
            /* Try again with command line and background tasks */
            rl_initialize();
            return (1);
        case 2:
            /* Try again with only command line */
            rl_initialize();
            return (2);
        default:
            printf("Resetting...\n");
            uart_flush();
            scb_reset_system();
            break;
    }
    return (0);
}

/**
 * hard_fault_handler_impl() provides an implementation of a hardfault handler
 *                           with optional return, skipping the offending
 *                           instruction. This may be used by the memory access
 *                           routine to provide an error message at an invalid
 *                           memory access.
 *
 * @param [in]  sp - Register frame end address.
 *
 * @return      Modified Program Counter, skipping current instruction.
 * @return      None (infinite loop on fatal error).
 */
static void __attribute__((noinline))
hard_fault_handler_impl(void *sp)
{
    reg_frame_t *frame;
    uint         width;
    uint         mode;

    mem_fault_count++;
    if (!mem_fault_ok || ((mem_fault_count >> 16) != 0)) {
        puts("Hard fault");
        fault_show_regs(sp);
        led_alert(1);
        if ((mode = cmdline_or_reset()) != 0) {
            frame = (reg_frame_t *) sp - 1;

            if (mode == 1)
                frame->pc = (uintptr_t) cmdline_loop;
            else
                frame->pc = (uintptr_t) cmdline_loop_uart;
            return;  // Return to local command handler
        }
    }

    /*
     * If this is an imprecise bus fault, the processor has likely
     * already advanced beyond the faulting instruction.  Attempt to
     * resume at the current instruction unless there have been too
     * many access faults already.
     */
    if ((SCB_CFSR & SCB_CFSR_IMPRECISERR) &&
         ((mem_fault_count >> 8) == 0)) {
        return;
    }

    /* Skip the faulting instruction */
    frame = (reg_frame_t *) sp - 1;
    width = stm32_instruction_width(frame->pc);
    if (width != 0) {
        frame->pc += width;
        return;
    }
}

/**
 * hard_fault_handler() processor Hard Fault vector entry point.
 *
 * @param [in]  None.
 * @return      Modified Program Counter.
 */
void __attribute__((naked))
hard_fault_handler(void)
{
    void *sp;

    EXCEPTION_ENTRY;  // Save processor registers
    hard_fault_handler_impl(sp);
    EXCEPTION_EXIT;   // Restore processor registers
}

static void __attribute__((noinline))
nmi_handler_impl(const void *sp)
{
    puts("NMI");
    fault_show_regs(sp);
    led_alert(1);
    cmdline_loop();
}

void __attribute__((naked))
nmi_handler(void)
{
    void *sp;

    EXCEPTION_ENTRY;  // Save processor registers
    nmi_handler_impl(sp);
    EXCEPTION_EXIT;   // Restore processor registers
}

static void __attribute__((noinline))
bus_fault_handler_impl(const void *sp)
{
    puts("Bus fault");
    fault_show_regs(sp);
    led_alert(1);
    cmdline_loop();
}

void __attribute__((naked))
bus_fault_handler(void)
{
    void          *sp;

    EXCEPTION_ENTRY;  // Save processor registers
    bus_fault_handler_impl(sp);
    EXCEPTION_EXIT;   // Restore processor registers
}

static void __attribute__((noinline))
mem_manage_handler_impl(const void *sp)
{
    puts("Memory management exception");
    fault_show_regs(sp);
    led_alert(1);
    cmdline_loop();
}

void __attribute__((naked))
mem_manage_handler(void)
{
    void          *sp;

    EXCEPTION_ENTRY;  // Save processor registers
    mem_manage_handler_impl(sp);
    EXCEPTION_EXIT;   // Restore processor registers
}

static void __attribute__((noinline))
usage_fault_handler_impl(const void *sp)
{
    puts("Usage fault");
    fault_show_regs(sp);
    led_alert(1);
    cmdline_loop();
}

void __attribute__((naked))
usage_fault_handler(void)
{
    void          *sp;

    EXCEPTION_ENTRY;  // Save processor registers
    usage_fault_handler_impl(sp);
    EXCEPTION_EXIT;   // Restore processor registers
}

static void __attribute__((noinline))
unknown_handler_impl(const void *sp)
{
    puts("Unknown interrupt/fault");
    fault_show_regs(sp);
    led_alert(1);
    cmdline_loop();
}

/*
 * unknown_handler
 * ---------------
 * Catches all STM32 interrupts and exceptions not handled by explicit
 * handler functions in this code.
 *
 * Example, trigger window watchdog interrupt:
 *    CMD> cb 0xe000e100 1 ; cb 0xe000e200 1
 *
 *    Unknown fault
 *    R0=e000e200 R3=2000fdf9 R6=20000f94  R9=2000fdf8 R12=00000000 PC=08001678
 *    R1=00000001 R4=00000000 R7=20000f98 R10=e000e200 PSR=21000200 SP=2000fd88
 *    R2=00000001 R5=00000000 R8=00000000 R11=00000000 LRE=fffffff9 LR=08002d51
 *    SCB ICSR: 00000810  vect=0x10
 */
void unknown_handler(void);
void __attribute__((naked))
unknown_handler(void)
{
    void *sp;

    EXCEPTION_ENTRY;  // Save processor registers
    unknown_handler_impl(sp);
    EXCEPTION_EXIT;   // Restore processor registers
}

void wwdg_isr(void) __attribute__((alias("unknown_handler")));
void pvd_isr(void) __attribute__((alias("unknown_handler")));
void tamper_isr(void) __attribute__((alias("unknown_handler")));
void rtc_isr(void) __attribute__((alias("unknown_handler")));
void flash_isr(void) __attribute__((alias("unknown_handler")));
void rcc_isr(void) __attribute__((alias("unknown_handler")));
void exti0_isr(void) __attribute__((alias("unknown_handler")));
void exti1_isr(void) __attribute__((alias("unknown_handler")));
void exti2_isr(void) __attribute__((alias("unknown_handler")));
void exti3_isr(void) __attribute__((alias("unknown_handler")));
void exti4_isr(void) __attribute__((alias("unknown_handler")));
void dma1_channel1_isr(void) __attribute__((alias("unknown_handler")));
void dma1_channel2_isr(void) __attribute__((alias("unknown_handler")));
void dma1_channel3_isr(void) __attribute__((alias("unknown_handler")));
void dma1_channel4_isr(void) __attribute__((alias("unknown_handler")));
void dma1_channel5_isr(void) __attribute__((alias("unknown_handler")));
void dma1_channel6_isr(void) __attribute__((alias("unknown_handler")));
void dma1_channel7_isr(void) __attribute__((alias("unknown_handler")));
void adc1_2_isr(void) __attribute__((alias("unknown_handler")));
void usb_hp_can_tx_isr(void) __attribute__((alias("unknown_handler")));
void usb_lp_can_rx0_isr(void) __attribute__((alias("unknown_handler")));
void can_rx1_isr(void) __attribute__((alias("unknown_handler")));
void can_sce_isr(void) __attribute__((alias("unknown_handler")));
void exti9_5_isr(void) __attribute__((alias("unknown_handler")));
void tim1_brk_isr(void) __attribute__((alias("unknown_handler")));
void tim1_up_isr(void) __attribute__((alias("unknown_handler")));
void tim1_trg_com_isr(void) __attribute__((alias("unknown_handler")));
void tim1_cc_isr(void) __attribute__((alias("unknown_handler")));
// void tim2_isr(void) __attribute__((alias("unknown_handler")));
// void tim3_isr(void) __attribute__((alias("unknown_handler")));
void tim4_isr(void) __attribute__((alias("unknown_handler")));
void i2c1_ev_isr(void) __attribute__((alias("unknown_handler")));
void i2c1_er_isr(void) __attribute__((alias("unknown_handler")));
void i2c2_ev_isr(void) __attribute__((alias("unknown_handler")));
void i2c2_er_isr(void) __attribute__((alias("unknown_handler")));
void spi1_isr(void) __attribute__((alias("unknown_handler")));
void spi2_isr(void) __attribute__((alias("unknown_handler")));
// void usart1_isr(void) __attribute__((alias("unknown_handler")));
void usart2_isr(void) __attribute__((alias("unknown_handler")));
void usart3_isr(void) __attribute__((alias("unknown_handler")));
void exti15_10_isr(void) __attribute__((alias("unknown_handler")));
void rtc_alarm_isr(void) __attribute__((alias("unknown_handler")));
void usb_wakeup_isr(void) __attribute__((alias("unknown_handler")));
void tim8_brk_isr(void) __attribute__((alias("unknown_handler")));
void tim8_up_isr(void) __attribute__((alias("unknown_handler")));
void tim8_trg_com_isr(void) __attribute__((alias("unknown_handler")));
void tim8_cc_isr(void) __attribute__((alias("unknown_handler")));
void adc3_isr(void) __attribute__((alias("unknown_handler")));
void fsmc_isr(void) __attribute__((alias("unknown_handler")));
void sdio_isr(void) __attribute__((alias("unknown_handler")));
// void tim5_isr(void) __attribute__((alias("unknown_handler")));
void spi3_isr(void) __attribute__((alias("unknown_handler")));
void uart4_isr(void) __attribute__((alias("unknown_handler")));
void uart5_isr(void) __attribute__((alias("unknown_handler")));
void tim6_isr(void) __attribute__((alias("unknown_handler")));
void tim7_isr(void) __attribute__((alias("unknown_handler")));
void dma2_channel1_isr(void) __attribute__((alias("unknown_handler")));
void dma2_channel2_isr(void) __attribute__((alias("unknown_handler")));
void dma2_channel3_isr(void) __attribute__((alias("unknown_handler")));
void dma2_channel4_5_isr(void) __attribute__((alias("unknown_handler")));
void dma2_channel5_isr(void) __attribute__((alias("unknown_handler")));
void eth_isr(void) __attribute__((alias("unknown_handler")));
void eth_wkup_isr(void) __attribute__((alias("unknown_handler")));
void can2_tx_isr(void) __attribute__((alias("unknown_handler")));
void can2_rx0_isr(void) __attribute__((alias("unknown_handler")));
void can2_rx1_isr(void) __attribute__((alias("unknown_handler")));
void can2_sce_isr(void) __attribute__((alias("unknown_handler")));
// void otg_fs_isr(void) __attribute__((alias("unknown_handler")));
