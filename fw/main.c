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
#include "config.h"
#include "msg.h"
#include "version.h"

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

extern uint _binary_objs_usbdfu_bin_start;
extern uint _binary_objs_usbdfu_bin_end;
extern uint _binary_objs_usbdfu_bin_size;
int
main(void)
{
    reset_periphs();
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
    led_set_brightness(config.led_level);
    usb_startup();
    check_board_standalone();
    ee_update_bank_at_poweron();

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
