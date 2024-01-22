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
#include "version.h"

static void
reset_everything(void)
{
    RCC_APB1ENR  = 0;  // Disable all peripheral clocks
    RCC_APB1RSTR = 0xffffffff;  // Reset APB1
    RCC_APB2RSTR = 0xffffffff;  // Reset APB2
    RCC_APB1RSTR = 0x00000000;  // Release APB1 reset
    RCC_APB2RSTR = 0x00000000;  // Release APB2 reset
}


int
main(void)
{
    reset_check();
    reset_everything();
    clock_init();
    timer_init();
//  timer_delay_msec(500);  // Just for development purposes
    gpio_init();
    led_init();
    uart_init();

    printf("\r\nKicksmash 32 %s\n", version_str);
    identify_cpu();
    show_reset_reason();
    check_board_standalone();

    rl_initialize();  // Enable command editing and history
    using_history();

    adc_init();
    ee_init();

    if (board_is_standalone)
        printf("Standalone\n");
    else
        printf("in Amiga\n");

    usb_startup();
    led_power(1);

    while (1) {
        usb_poll();
        adc_poll(true, false);
        ee_poll();
        cmdline();
        kbrst_poll();
    }

    return (0);
}
