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
#define USB_VBUS_PORT       GPIOA
#define USB_VBUS_PIN            GPIO9

#define USER_BUTTON_PORT    GPIOA
#define USER_BUTTON_PIN         GPIO0       // User button

#define ENABLE_V5_PORT      GPIOB
#define ENABLE_V5_PIN           GPIO0
#define KBRST_PORT          GPIOB
#define KBRST_PIN               GPIO4
#define BOOT0_PORT          GPIOB
#define BOOT0_PIN               GPIO5
#define LED_POWER_PORT      GPIOB
#define LED_POWER_PIN           GPIO8
#define LED_STATUS_PORT     GPIOB
#define LED_STATUS_PIN          GPIO9
#define SOCKET_A18B_PORT    GPIOB
#define SOCKET_A18B_PIN         GPIO10
#define SOCKET_A19B_PORT    GPIOB
#define SOCKET_A19B_PIN         GPIO11
#define SOCKET_OE_PORT      GPIOB
#define SOCKET_OE_PIN           GPIO13      // OE# Output Enable
#define SOCKET_CE_PORT      GPIOB
#define SOCKET_CE_PIN           GPIO14      // CE# Chip Enable
#define SOCKET_BYTE_PORT    GPIOB
#define SOCKET_BYTE_PIN         GPIO15      // BYTE pin

#define SOCKET_D0_PORT      GPIOD   // PD0-PD15
#define SOCKET_D15_PORT     GPIOD   // PD0-PD15
#define SOCKET_D15_PIN      GPIO15  // PD15
#define SOCKET_D16_PORT     GPIOE   // PE0-PE15
#define SOCKET_D31_PORT     GPIOE   // PE0-PE15
#define SOCKET_D31_PIN      GPIO15  // PE15
#define SOCKET_A17_PORT     GPIOA   // PA5
#define SOCKET_A17_PIN      GPIO5   // PA5
#define SOCKET_A0_PORT      GPIOC   // PC0-PC15
#define SOCKET_A13_PORT     GPIOA   // PA1-PA3
#define SOCKET_A16_PORT     GPIOA   // PA4-PA7
#define SOCKET_A16_PIN      GPIO4   // PA4
#define SOCKET_A18_PIN      GPIO6   // PA6
#define SOCKET_A19_PIN      GPIO7   // PA7

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
uint gpio_getmode(uint32_t GPIOx, uint pin);
void gpio_init(void);
void gpio_show(int whichport, int pins);
void gpio_assign(int whichport, int pins, const char *assign);
uint gpio_name_match(const char **name, uint16_t pins[NUM_GPIO_BANKS]);
char *gpio_to_str(uint32_t port, uint16_t pin);

#endif /* _GPIO_H */

