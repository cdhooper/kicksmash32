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

#include "board.h"
#include "main.h"
#include "printf.h"
#include "uart.h"
#include "gpio.h"
#include "timer.h"
#include "utils.h"
#include "m29f160xt.h"
#include <string.h>

#ifdef STM32F4
#include <libopencm3/stm32/f4/rcc.h>
#else
#include <libopencm3/stm32/f1/rcc.h>
#endif

#undef DEBUG_GPIO

#define ARRAY_SIZE(x) (int)((sizeof (x) / sizeof ((x)[0])))

#ifdef STM32F1
/**
 * spread8to32() will spread an 8-bit value to odd bits of a 32-bit value
 *
 * This is useful for STM32 registers where the combination of four
 * sequential bits will represent settings for a single GPIO pin.
 *
 * Algorithm
 *     00000000000000000000000011111111  Initial data
 *     00000000000011110000000000001111  (0x000000f0 << 12) | 0x0000000f
 *     00000011000000110000001100000011  (0x000c000c << 6) | 0x00030003
 *     00010001000100010001000100010001  (0x02020202 << 3) | 0x02020202
 */
static uint32_t
spread8to32(uint32_t v)
{
    v = ((v & 0x000000f0) << 12) | (v & 0x0000000f);
    v = ((v & 0x000c000c) << 6) | (v & 0x00030003);
    v = ((v & 0x22222222) << 3) | (v & 0x11111111);
    return (v);
}

#else

/**
 * spread16to32() will spread a 16-bit value to odd bits of a 32-bit value
 *
 * This is useful for STM32 registers where the combination of two
 * sequential bits will represent a mode for a single GPIO pin.
 *
 * Algorithm
 *     00000000000000001111111111111111  Initial data
 *     00000000111111110000000011111111  (0x0000ff00 << 8) | 0x000000ff
 *     00001111000011110000111100001111  (0x00f000f0 << 4) | 0x000f000f
 *     00110011001100110011001100110011  (0x0c0c0c0c << 2) | 0x03030303
 *     01010101010101010101010101010101  (0x22222222 << 1) | 0x11111111
 */
static uint32_t
spread16to32(uint32_t v)
{
    v = ((v & 0x0000ff00) << 8) | (v & 0x000000ff);
    v = ((v & 0x00f000f0) << 4) | (v & 0x000f000f);
    v = ((v & 0x0c0c0c0c) << 2) | (v & 0x03030303);
    v = ((v & 0x22222222) << 1) | (v & 0x11111111);
    return (v);
}
#endif


/*
 * gpio_set_1
 * ----------
 * Drives the specified GPIO bits to 1 values without affecting other bits.
 */
static void
gpio_set_1(uint32_t GPIOx, uint16_t GPIO_Pins)
{
    GPIO_BSRR(GPIOx) = GPIO_Pins;
}

/*
 * gpio_set_0
 * ----------
 * Drives the specified GPIO bits to 0 values without affecting other bits.
 */
static void
gpio_set_0(uint32_t GPIOx, uint16_t GPIO_Pins)
{
    GPIO_BSRR(GPIOx) = GPIO_Pins << 16;
}

/*
 * gpio_setv
 * ---------
 * Sets the specified GPIO bits to 0 or 1 values without affecting other bits.
 */
void
gpio_setv(uint32_t GPIOx, uint16_t GPIO_Pins, int value)
{
    if (value == 0)
        gpio_set_0(GPIOx, GPIO_Pins);
    else
        gpio_set_1(GPIOx, GPIO_Pins);
}

/*
 * gpio_getv
 * ---------
 * Gets the current output values (not input values) of the specified GPIO
 * port and pins.
 */
static uint
gpio_getv(uint32_t GPIOx, uint pin)
{
#ifdef STM32F1
    return (GPIO_ODR(GPIOx) & BIT(pin));
#endif
}

/*
 * gpio_setmode
 * ------------
 * Sets the complex input/output mode of the GPIO.
 *
 * STM32F1: value specifies the GPIO mode and configuration
 * 0x0 0000: Analog Input
 * 0x4 0100: Floating input (reset state)
 * 0x8 1000: Input with pull-up / pull-down
 * 0xc 1100: Reserved
 * 0x1 0001: Output 10 MHz, Push-Pull
 * 0x5 0101: Output 10 MHz, Open-Drain
 * 0x9 1001: Output 10 MHz, Alt function Push-Pull
 * 0xd 1101: Output 10 MHz, Alt function Open-Drain
 * 0x2 0010: Output 2 MHz, Push-Pull
 * 0x6 0110: Output 2 MHz, Open-Drain
 * 0xa 1010: Output 2 MHz, Alt function Push-Pull
 * 0xe 1110: Output 2 MHz, Alt function Open-Drain
 * 0x3 0011: Output 50 MHz, Push-Pull
 * 0x7 0111: Output 50 MHz, Open-Drain
 * 0xb 1011: Output 50 MHz, Alt function Push-Pull
 * 0xf 1111: Output 50 MHz, Alt function Open-Drain
 */
void
gpio_setmode(uint32_t GPIOx, uint16_t GPIO_Pins, uint value)
{
#ifdef DEBUG_GPIO
    char ch;
    switch ((uintptr_t)GPIOx) {
        case (uintptr_t)GPIOA:
            ch = 'A';
            break;
        case (uintptr_t)GPIOB:
            ch = 'B';
            break;
        case (uintptr_t)GPIOC:
            ch = 'C';
            break;
        case (uintptr_t)GPIOD:
            ch = 'D';
            break;
        case (uintptr_t)GPIOE:
            ch = 'E';
            break;
        case (uintptr_t)GPIOF:
            ch = 'F';
            break;
        default:
            ch = '?';
            break;
    }
    printf(" GPIO%c ", ch);
#endif
#ifdef STM32F1
    if (GPIO_Pins & 0xff) {
        uint32_t pins   = GPIO_Pins & 0xff;
        uint32_t spread = spread8to32(pins);
        uint32_t mask   = spread * 0xf;
        uint32_t newval;
        uint32_t temp;

        newval = spread * (value & 0xf);
        temp = (GPIO_CRL(GPIOx) & ~mask) | newval;

#ifdef DEBUG_GPIO
        printf("CRL v=%02x p=%04x sp=%08lx mask=%08lx %08lx^%08lx=%08lx\n",
               value, GPIO_Pins, spread, mask, GPIO_CRL(GPIOx), temp,
               GPIO_CRL(GPIOx) ^ temp);
#endif
        GPIO_CRL(GPIOx) = temp;
    }
    if (GPIO_Pins & 0xff00) {
        uint32_t pins   = (GPIO_Pins >> 8) & 0xff;
        uint32_t spread = spread8to32(pins);
        uint32_t mask   = spread * 0xf;
        uint32_t newval;
        uint32_t temp;

        newval = spread * (value & 0xf);
        temp   = (GPIO_CRH(GPIOx) & ~mask) | newval;

#ifdef DEBUG_GPIO
        printf("CRH v=%02x p=%04x sp=%08lx mask=%08lx %08lx^%08lx=%08lx\n",
               value, GPIO_Pins, spread, mask, GPIO_CRH(GPIOx), temp,
               GPIO_CRH(GPIOx) ^ temp);
#endif
        GPIO_CRH(GPIOx) = temp;
    }

#else  /* STM32F407 */
    uint32_t spread = spread16to32(GPIO_Pins);
    uint32_t mask   = spread * 0x3;
    uint32_t newval = spread * value;
    GPIO_MODER(GPIOx) = (GPIO_MODER(GPIOx) & ~mask) | newval;
    // XXX: Macros need to be implemented for STM32F4
#endif
}

/*
 * gpio_getmode
 * ------------
 * Get the input/output mode of the specified GPIO pins.
 */
static uint
gpio_getmode(uint32_t GPIOx, uint pin)
{
#ifdef STM32F1
    if (pin < 8) {
        uint shift = pin * 4;
        return ((GPIO_CRL(GPIOx) >> shift) & 0xf);
    } else {
        uint shift = (pin - 8) * 4;
        return ((GPIO_CRH(GPIOx) >> shift) & 0xf);
    }
#endif
}

/*
 * gpio_num_to_gpio
 * ----------------
 * Convert the specified GPIO number to its respective port address.
 */
static uint32_t
gpio_num_to_gpio(uint num)
{
    static const uint32_t gpios[] = {
        GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF
    };
    return (gpios[num]);
}

char *
gpio_to_str(uint32_t port, uint16_t pin)
{
    uint gpio;
    uint bit;
    static char name[8];
    static const uint32_t gpios[] = {
        GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF
    };
    for (gpio = 0; gpio < ARRAY_SIZE(gpios); gpio++)
        if (gpios[gpio] == port)
            break;
    for (bit = 0; bit < 16; bit++)
        if (pin & BIT(bit))
            break;
    sprintf(name, "P%c%u", gpio + 'A', bit);
    return (name);
}


#ifdef STM32F1
static const char * const gpio_mode_short[] = {
    "A", "O1", "O2", "O5",      // AnalogI, Output {10, 2, 50} MHz
    "I", "OD1", "OD2", "OD5",   // Input, Output Open Drain
    "PUD", "AO1", "AO2", "AO5", // Input Pull Up/Down, AF Output
    "Rsv", "AD1", "AD2", "AD5", // Reserved, AF OpenDrain
};

static const char * const gpio_mode_long[] = {
    "Analog Input", "O10 Output 10MHz", "O2 Output 2MHz", "O5 Output 50MHz",
    "Input", "OD10 Open Drain 10MHz",
        "OD2 Open Drain 2MHz", "OD5 Open Drain 50MHz",
    "PUD", "AO10 AltFunc Output 10MHz",
        "AO2 AltFunc Output 2MHz", "AO5 AltFunc Output 50MHz",
    "Rsv", "AD1 AltFunc Open Drain 10MHz",
        "AD2 AltFunc Open Drain 2MHz", "AD5 AltFunc Open Drain 50MHz",
};
#endif

/*
 * gpio_show
 * ---------
 * Display current values and input/output state of GPIOs.
 */
void
gpio_show(int whichport, int whichpin)
{
    int port;
    int pin;
    uint mode;
    uint print_all = (whichport < 0) && (whichpin < 0);

    if (print_all) {
#if BOARD_REV == 1
        printf("Socket A0-A15=PE0-PE15 A16-A19=PC6-PC9\n"
               "Socket D0-D15=PD0-PD15 D16-D23=PA0-PA15 D24-D27=PC0-PC3\n"
               "Socket D28=PB4 D29=PB5 D30=PB8 D31=PB9  OE=PB15\n"
               "Flash  A18=PC4 A19=PC5 RP=PB1 RB1=PB10 RB2=PB11\n"
               "Flash  WE=PB12 OE=PB13 CE=PB14\n"
               "USB    V5=PA9 CC1=PA8 CC2=PA10 DM=PA11 DP=PA12\n");
#elif BOARD_REV == 2
        printf("Socket A0-A15=PC0-PC15 A13-A19=PA1-PA7\n"
               "Socket D0-D15=PD0-PD15 D16-D31=PE0-PE15\n"
               "Socket OE=PA0 LED=PB9 KBRST=PB4 CC1=PA8 CC2=PA10\n"
               "Flash  A18=PB10 A19=PB11 RP=PB1 RB1=PB15 RB2=PB0\n"
               "Flash  CE=PB12 OE=PB13 WE=PB14\n"
               "USB    V5=PA9 CC1=PA8 CC2=PA10 DM=PA11 DP=PA12\n");
#else
        printf("Socket OE=PA0 LED=PB8 KBRST=PB4 CC1=PA8 CC2=PA10\n"
               "Socket A0-A15=PC0-PC15 A13-A19=PA1-PA7\n"
               "Flash  D0-D15=PD0-PD15 D16-D31=PE0-PE15\n"
               "Flash  A18=PB10 A19=PB11 RP=PB1 RB1=PB15 RB2=PB0\n"
               "Flash  CE=PB12 OE=PB13 WE=PB14 OEWE=PB9\n"
               "USB    V5=PA9 CC1=PA8 CC2=PA10 DM=PA11 DP=PA12\n");
#endif
        printf("\nMODE  ");
        for (pin = 15; pin >= 0; pin--)
            printf("%4d", pin);
        printf("\n");
    }

    for (port = 0; port < 5; port++) {
        uint32_t gpio = gpio_num_to_gpio(port);
        if ((whichport >= 0) && (port != whichport))
            continue;
        if (print_all)
            printf("GPIO%c ", 'A' + port);
        for (pin = 15; pin >= 0; pin--) {
            const char *mode_txt;
            if ((whichpin >= 0) && (pin != whichpin))
                continue;
            mode = gpio_getmode(gpio, pin);
#ifdef STM32F1
            if (print_all) {
                mode_txt = gpio_mode_short[mode];
                if (mode == GPIO_SETMODE_INPUT_PULLUPDOWN)
                    mode_txt = gpio_getv(gpio, pin) ? "PU" : "PD";
            } else {
                mode_txt = gpio_mode_long[mode];
                if (mode == GPIO_SETMODE_INPUT_PULLUPDOWN)
                    mode_txt = gpio_getv(gpio, pin) ? "Input PU" : "Input PD";
            }
#endif
            /* Pull-up or pull down depending on output register state */
            if (print_all) {
                printf("%4s", mode_txt);
            } else {
                char mode_extra[8];
                uint pinstate = !!(gpio_get(gpio, BIT(pin)));
                mode_extra[0] = '\0';
                if ((gpio_getmode(gpio, pin) & 3) != 0) {
                    /* Output */
                    uint outval = !!gpio_getv(gpio, pin);
                    if (outval != pinstate)
                        sprintf(mode_extra, "=%d>", outval);
                }
                printf("P%c%d=%s (%s%d)\n",
                        'A' + port, pin, mode_txt, mode_extra, pinstate);
            }
        }
        if (print_all)
            printf("\n");
    }

    if (!print_all)
        return;

    printf("\nState ");
    for (pin = 15; pin >= 0; pin--)
        printf("%4d", pin);
    printf("\n");

    for (port = 0; port < 5; port++) {
        uint32_t gpio = gpio_num_to_gpio(port);
        printf("GPIO%c ", 'A' + port);
        for (pin = 15; pin >= 0; pin--) {
            uint pinstate = !!(gpio_get(gpio, BIT(pin)));
            if ((gpio_getmode(gpio, pin) & 3) != 0) {
                /* Not in an input mode */
                uint outval = !!gpio_getv(gpio, pin);
                if (outval != pinstate) {
                    printf(" %d>%d", outval, pinstate);
                    continue;
                }
            }
            printf("%4d", pinstate);
        }
        printf("\n");
    }
}

/*
 * gpio_assign
 * -----------
 * Assign a GPIO input/output state or output value according to the
 * user-specified string.
 */
void
gpio_assign(int whichport, int whichpin, const char *assign)
{
    uint mode;
    uint gpio;
    uint pins;
    if (*assign == '?') {
        printf("Valid modes:");
        for (mode = 0; mode < ARRAY_SIZE(gpio_mode_short); mode++)
            printf(" %s", gpio_mode_short[mode]);
        printf(" 0 1 A I O PU PD\n");
        return;
    }
    gpio = gpio_num_to_gpio(whichport);
    pins = BIT(whichpin);
    for (mode = 0; mode < ARRAY_SIZE(gpio_mode_short); mode++) {
        if (strcasecmp(gpio_mode_short[mode], assign) == 0) {
            gpio_setmode(gpio, pins, mode);
            return;
        }
    }
    switch (*assign) {
        case 'a':
        case 'A':
            if (assign[1] == '\0') {
                gpio_setmode(gpio, pins, GPIO_SETMODE_INPUT_ANALOG);
                return;
            }
            break;
        case 'i':
        case 'I':
            if (assign[1] == '\0') {
                gpio_setmode(gpio, pins, GPIO_SETMODE_INPUT);
                return;
            }
            break;
        case 'o':
        case 'O':
            if (assign[1] == '\0') {
                gpio_setmode(gpio, pins, GPIO_SETMODE_OUTPUT_PPULL_2);
                return;
            }
            break;
        case '0':
            if (assign[1] == '\0') {
                gpio_setv(gpio, pins, 0);
                mode = gpio_getmode(gpio, whichpin);
                if ((mode & 3) == 0) {
                    /* Currently an input mode -- default to 2MHz Output */
                    gpio_setmode(gpio, pins, GPIO_SETMODE_OUTPUT_PPULL_2);
                }
                return;
            }
            break;
        case '1':
            if (assign[1] == '\0') {
                gpio_setv(gpio, pins, 1);
                if ((mode & 3) == 0) {
                    /* Currently an input mode -- default to 2MHz Output */
                    gpio_setmode(gpio, pins, GPIO_SETMODE_OUTPUT_PPULL_2);
                }
                return;
            }
            break;
        case 'p':
        case 'P':
            if (assign[2] == '\0') {
                switch (assign[1]) {
                    case 'u':
                    case 'U':
                        gpio_setmode(gpio, pins, GPIO_SETMODE_INPUT_PULLUPDOWN);
                        gpio_setv(gpio, pins, 1);
                        return;
                    case 'd':
                    case 'D':
                        gpio_setmode(gpio, pins, GPIO_SETMODE_INPUT_PULLUPDOWN);
                        gpio_setv(gpio, pins, 0);
                        return;
                    default:
                        break;
                }
            }
            break;
        default:
            break;
    }

    printf("Invalid mode %s for GPIO\n", assign);
}

/*
 * gpio_init
 * ---------
 * Initialize most board GPIO states.
 */
void
gpio_init(void)
{
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_GPIOC);
    rcc_periph_clock_enable(RCC_GPIOD);
    rcc_periph_clock_enable(RCC_GPIOE);
    rcc_periph_clock_enable(RCC_AFIO);

    /*
     * Pin            Default State  Override state     Description
     * KBRST          INPUT, PD      OUTPUT 0           Amiga in reset
     * FLASH_RP       INPUT, PU
     * FLASH_RB1      INPUT, PU
     * FLASH_RB2      INPUT, PU
     * FLASH_WE       OUTPUT 1       0 if writing       Flash write enable
     * FLASH_CE       OUTPUT 0       x                  Flash enable
     * FLASH_OE       INPUT          0 if reading
     * SOCKET_OE      INPUT, PU      x
     * SOCKET_A0-A19  INPUT          OUTPUT if !KBRST
     * SOCKET_D0-D31  INPUT          OUTPUT if SOCKET_OE & !FLASH_OE
     * USB_CC1-CC2    INPUT          OUTPUT if USB reset desired
     */

    /*
     * Weak pull-down on Amiga reset, used to detect if Amiga KBRST wire
     * is properly connected. Need another means to detect whether Amiga
     * is connected.
     */
    gpio_setv(KBRST_PORT, KBRST_PIN, 0);
    gpio_setmode(KBRST_PORT, KBRST_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);

    /*
     * RP high takes the devices out of reset
     * RB1 is busy status (0=Busy) for the low flash part (D0-D15)
     * RB2 is busy status (0=Busy) for the high flash part (D16-D31)
     */
    gpio_setv(FLASH_RP_PORT, FLASH_RP_PIN | FLASH_RB1_PIN | FLASH_RB2_PIN, 1);
    gpio_setmode(FLASH_RP_PORT, FLASH_RP_PIN | FLASH_RB1_PIN | FLASH_RB2_PIN,
                 GPIO_SETMODE_INPUT_PULLUPDOWN);

    /* Deassert flash WE# (write enable) */
    gpio_setv(FLASH_WE_PORT, FLASH_WE_PIN, 1);
    gpio_setmode(FLASH_WE_PORT, FLASH_WE_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);

    /* Deassert flash OEWE (WE# follows socket OE#) */
    gpio_setv(FLASH_OEWE_PORT, FLASH_OEWE_PIN, 0);
    gpio_setmode(FLASH_OEWE_PORT, FLASH_OEWE_PIN, GPIO_SETMODE_OUTPUT_PPULL_50);

    /* Assert flash CE# (chip enable) */
    gpio_setv(FLASH_CE_PORT, FLASH_CE_PIN, 0);
    gpio_setmode(FLASH_CE_PORT, FLASH_CE_PIN, GPIO_SETMODE_OUTPUT_PPULL_2);

    /* Weakly pull up socket OE# (output enable) */
    gpio_setv(SOCKET_OE_PORT, SOCKET_OE_PIN, 1);
    gpio_setmode(SOCKET_OE_PORT, SOCKET_OE_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);

    /* Weakly pull up flash OE# (output enable) */
    gpio_setv(FLASH_OE_PORT, FLASH_OE_PIN, 1);
    gpio_setmode(FLASH_OE_PORT, FLASH_OE_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);

    /* Give flash A18 and A19 weak pull down */
    gpio_setv(FLASH_A18_PORT, FLASH_A18_PIN | FLASH_A19_PIN, 0);
    gpio_setmode(FLASH_A18_PORT, FLASH_A18_PIN | FLASH_A19_PIN,
                 GPIO_SETMODE_INPUT_PULLUPDOWN);

    /* Give D0-D31 weak pull up */
    gpio_setv(FLASH_D0_PORT,  0xffff, 1);
    gpio_setv(FLASH_D16_PORT, 0xffff, 1);
    gpio_setmode(FLASH_D0_PORT,  0xffff, GPIO_SETMODE_INPUT_PULLUPDOWN);
    gpio_setmode(FLASH_D16_PORT, 0xffff, GPIO_SETMODE_INPUT_PULLUPDOWN);

    rcc_periph_clock_enable(RCC_AFIO);
    AFIO_MAPR |= AFIO_MAPR_SWJ_CFG_FULL_SWJ_NO_JNTRST;
    ee_disable();
}
