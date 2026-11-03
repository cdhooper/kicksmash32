/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * main routine.
 */

#include "printf.h"
#include <stdint.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include "board.h"
#include "main.h"
#include "clock.h"
#include "uart.h"
#include "usb.h"
#include "m29f160xt.h"
#include "led.h"
#include "gpio.h"
#include "adc.h"
#include "cmdline.h"
#include "readline.h"
#include "timer.h"
#include "utils.h"
#include "kbrst.h"
#include "pin_tests.h"
#include "persist.h"
#include "config.h"
#include "msg.h"
#include "version.h"

/* Linker script symbol: value is the firmware image size in flash */
extern uint8_t _fw_size;

/* Firmware identification, located by scanning flash (see main.h) */
const fw_info_t fw_info = {
    .fi_magic       = FW_INFO_MAGIC,
    .fi_struct_size = sizeof (fw_info_t),
    .fi_self        = &fw_info,
    .fi_version_str = &version_str,
    .fi_fw_size     = (uintptr_t) &_fw_size,
};

static void
reset_periphs(void)
{
    RCC_APB1ENR  = 0;  // Disable all peripheral clocks
    RCC_APB1RSTR = 0xffffffff;  // Reset APB1
    RCC_APB2RSTR = 0xffffffff;  // Reset APB2
    RCC_APB1RSTR = 0x00000000;  // Release APB1 reset
    RCC_APB2RSTR = 0x00000000;  // Release APB2 reset
}

void
main_poll(void)
{
    usb_poll();
    adc_poll(true, false);
    ee_poll();
    kbrst_poll();
    config_poll();
    msg_poll();
    led_poll();
}

int
main(void)
{
    uint persist_good = (persist.sp_magic == SP_MAGIC);
    reset_periphs();
    persist_init();
    reset_check();
    clock_init();
    timer_init();
//  timer_delay_msec(500);  // Just for development purposes
    gpio_init();
    led_init();
    uart_init();

    printf("\r\nKicksmash 32 %s\n", version_str);

    identify_cpu();
    show_reset_reason();
    config_read();
    persist.sp_board_rev = config.board_rev;
    led_set_brightness(config.led_level);
    usb_startup();
    check_board_standalone();
    if (persist_good) {
        ee_set_bank(persist.sp_bank_current);
    } else {
        ee_update_bank_at_poweron();
    }

    adc_init();
    ee_init();
    msg_init();

    if (board_is_standalone) {
        printf("Standalone\n");
    } else {
        printf("in Amiga\n");
    }

    rl_initialize();  // Enable command editing and history
    using_history();

    while (1) {
        main_poll();
        cmdline();
    }

    return (0);
}
