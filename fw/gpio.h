/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * Low level STM32 GPIO access.
 */

#ifndef _GPIO_H
#define _GPIO_H

#include <libopencm3/stm32/gpio.h>
#ifdef STM32F4
#include <libopencm3/stm32/f4/gpio.h>
#else
#include <libopencm3/stm32/f1/gpio.h>
#endif

#define USB_CC1_PORT        GPIOA
#define USB_CC1_PIN             GPIO8
#define USB_CC2_PORT        GPIOA
#define USB_CC2_PIN             GPIO10

#define SOCKET_OE_PORT      GPIOA
#define SOCKET_OE_PIN           GPIO0       // OE# Output Enable (Amiga)

#if BOARD_REV <= 3
#define FLASH_RB2_PORT      GPIOB
#define FLASH_RB2_PIN           GPIO0       // RB# Ready / Busy
#endif
#define FLASH_RP_PORT       GPIOB
#define FLASH_RP_PIN            GPIO1       // RP# Reset / Program
#define KBRST_PORT          GPIOB
#define KBRST_PIN               GPIO4
#define LED_POWER_PORT      GPIOB
#if BOARD_REV == 2
#define LED_POWER_PIN           GPIO9
#else
#define LED_POWER_PIN           GPIO8
#endif
#define FLASH_OEWE_PORT     GPIOB
#define FLASH_OEWE_PIN          GPIO9
#define FLASH_A18_PORT      GPIOB
#define FLASH_A18_PIN           GPIO10
#define FLASH_A19_PORT      GPIOB
#define FLASH_A19_PIN           GPIO11
#define FLASH_OE_PORT       GPIOB
#define FLASH_OE_PIN            GPIO13      // OE# Output Enable (Flash)
#define FLASH_WE_PORT       GPIOB
#define FLASH_WE_PIN            GPIO14      // WE# Write Enable
#define FLASH_RB_PORT       GPIOB
#define FLASH_RB_PIN            GPIO15      // RB# Ready / Busy

#define FLASH_D0_PORT       GPIOD   // PD0-PD15
#define FLASH_D16_PORT      GPIOE   // PE0-PE15
#define FLASH_D31_PORT      GPIOE   // PE0-PE15
#define FLASH_D31_PIN       GPIO15  // PE15
#define FLASH_A17_PORT      GPIOA   // PA5
#define FLASH_A17_PIN       GPIO5   // PA5
#define SOCKET_A0_PORT      GPIOC   // PC0-PC15
#define SOCKET_A13_PORT     GPIOA   // PA1-PA3
#define SOCKET_A16_PORT     GPIOA   // PA4-PA7
#define SOCKET_A16_PIN      GPIO4   // PA4
#define SOCKET_A18_PIN      GPIO6   // PA6
#define SOCKET_A19_PIN      GPIO7   // PA7

/* Caution: SOCKET_D31 was FLASH_CE */
#define SOCKET_D31_PORT     GPIOB
#define SOCKET_D31_PIN      GPIO12  // PB12

/* Values for gpio_setmode() */
#ifdef STM32F1
#define GPIO_SETMODE_INPUT_ANALOG        0x0  // Analog Input
#define GPIO_SETMODE_INPUT               0x4  // Floating input (reset state)
#define GPIO_SETMODE_INPUT_PULLUPDOWN    0x8  // Input with pull-up / pull-down
#define GPIO_SETMODE_OUTPUT_PPULL_10     0x1  // 10 MHz, Push-Pull
#define GPIO_SETMODE_OUTPUT_ODRAIN_10    0x5  // 10 MHz, Open-Drain
#define GPIO_SETMODE_OUTPUT_AF_PPULL_10  0x9  // 10 MHz, Alt func. Push-Pull
#define GPIO_SETMODE_OUTPUT_AF_ODRAIN_10 0xd  // 10 MHz, Alt func. Open-Drain
#define GPIO_SETMODE_OUTPUT_PPULL_2      0x2  // 2 MHz, Push-Pull
#define GPIO_SETMODE_OUTPUT_ODRAIN_2     0x6  // 2 MHz, Open-Drain
#define GPIO_SETMODE_OUTPUT_AF_PPULL_2   0xa  // 2 MHz, Alt func. Push-Pull
#define GPIO_SETMODE_OUTPUT_AF_ODRAIN_2  0xe  // 2 MHz, Alt func. Open-Drain
#define GPIO_SETMODE_OUTPUT_PPULL_50     0x3  // 50 MHz, Push-Pull
#define GPIO_SETMODE_OUTPUT_ODRAIN_50    0x7  // 50 MHz, Open-Drain
#define GPIO_SETMODE_OUTPUT_AF_PPULL_50  0xb  // 50 MHz, Alt func. Push-Pull
#define GPIO_SETMODE_OUTPUT_AF_ODRAIN_50 0xf  // 50 MHz, Alt func. Open-Drain
#endif

#define NUM_GPIO_BANKS 6

void gpio_setv(uint32_t GPIOx, uint16_t GPIO_Pins, int value);
void gpio_setmode(uint32_t GPIOx, uint16_t GPIO_Pins, uint value);
void gpio_init(void);
void gpio_show(int whichport, int pins);
void gpio_assign(int whichport, int pins, const char *assign);
uint gpio_name_match(const char **name, uint16_t pins[NUM_GPIO_BANKS]);
char *gpio_to_str(uint32_t port, uint16_t pin);

#endif /* _GPIO_H */

