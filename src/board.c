#include "board.h"

#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>

/* X, Y, Z, and E0 driver outputs from the SKR Mini E3 V2.0 schematic. */
#define MOTOR_ENABLE_GPIOB (GPIO14 | GPIO11 | GPIO1)
#define MOTOR_ENABLE_GPIOD GPIO2
#define MOTOR_STEP_GPIOB (GPIO13 | GPIO10 | GPIO0 | GPIO3)
#define MOTOR_DIR_GPIOB (GPIO12 | GPIO2 | GPIO4)
#define MOTOR_DIR_GPIOC GPIO5

/* Low turns off the low-side MOSFETs for both heaters and both PWM fans. */
#define UNUSED_POWER_GPIOC (GPIO6 | GPIO7 | GPIO8 | GPIO9)

#define USB_CONNECT_GPIO GPIOA
#define USB_CONNECT_PIN GPIO14

#define PAN_STEP_PIN BOARD_PAN_STEP_PIN_MASK
#define PAN_DIRECTION_PIN BOARD_PAN_DIRECTION_PIN_MASK
#define PAN_ENABLE_PIN BOARD_PAN_ENABLE_PIN_MASK

#define SENTRY_AFIO_REMAPS \
	(AFIO_MAPR_TIM2_REMAP_PARTIAL_REMAP2 | \
	 AFIO_MAPR_USART3_REMAP_PARTIAL_REMAP)

void board_safe_init(void)
{
	/* The bootloader occupies the first 28 KiB of flash. */
	SCB_VTOR = SENTRY_APPLICATION_ORIGIN;

	rcc_periph_clock_enable(RCC_AFIO);
	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_GPIOB);
	rcc_periph_clock_enable(RCC_GPIOC);
	rcc_periph_clock_enable(RCC_GPIOD);

	/*
	 * PA14 (USB connect) and PB3/PB4 (E0 STEP/DIR) overlap SWJ pins.
	 * Disable both JTAG and SWD before configuring those safety outputs.
	 */
	gpio_primary_remap(AFIO_MAPR_SWJ_CFG_JTAG_OFF_SW_OFF, 0U);

	/* Preload safe output levels through BSRR before changing GPIO modes. */
	gpio_set(GPIOB, MOTOR_ENABLE_GPIOB);
	gpio_set(GPIOD, MOTOR_ENABLE_GPIOD);
	gpio_clear(GPIOB, MOTOR_STEP_GPIOB | MOTOR_DIR_GPIOB);
	gpio_clear(GPIOC, MOTOR_DIR_GPIOC | UNUSED_POWER_GPIOC);
	gpio_set(USB_CONNECT_GPIO, USB_CONNECT_PIN);

	gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL,
		      MOTOR_ENABLE_GPIOB | MOTOR_STEP_GPIOB | MOTOR_DIR_GPIOB);
	gpio_set_mode(GPIOD, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL,
		      MOTOR_ENABLE_GPIOD);
	gpio_set_mode(GPIOC, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL,
		      MOTOR_DIR_GPIOC | UNUSED_POWER_GPIOC);
	gpio_set_mode(USB_CONNECT_GPIO, GPIO_MODE_OUTPUT_2_MHZ,
		      GPIO_CNF_OUTPUT_PUSHPULL, USB_CONNECT_PIN);
}

void board_clock_init(void)
{
	rcc_clock_setup_in_hse_8mhz_out_72mhz();
	/* 72 MHz PLL divided by 1.5 supplies the required 48 MHz USB clock. */
	rcc_set_usbpre(RCC_CFGR_USBPRE_PLL_CLK_DIV1_5);
}

void board_usb_connect(bool connected)
{
	if (connected) {
		gpio_clear(USB_CONNECT_GPIO, USB_CONNECT_PIN);
	} else {
		gpio_set(USB_CONNECT_GPIO, USB_CONNECT_PIN);
	}
}

void board_pan_set_enabled(bool enabled)
{
	if (enabled) {
		/* ENABLE is active low. */
		gpio_clear(GPIOB, PAN_ENABLE_PIN);
	} else {
		gpio_set(GPIOB, PAN_ENABLE_PIN);
	}
}

void board_pan_set_direction(bool positive)
{
	/* Positive is logical only; its eventual physical sense is unspecified. */
	if (positive) {
		gpio_clear(GPIOB, PAN_DIRECTION_PIN);
	} else {
		gpio_set(GPIOB, PAN_DIRECTION_PIN);
	}
}

void board_pan_step_use_timer(void)
{
	/*
	 * TIM2 partial remap 2 routes CH3 to PB10 and CH4 to PB11. CH4 remains
	 * disabled and PB11 remains an ordinary GPIO for active-low PAN enable.
	 * Repeat the established SWJ setting because those bits are write-only.
	 */
	gpio_primary_remap(AFIO_MAPR_SWJ_CFG_JTAG_OFF_SW_OFF,
			   SENTRY_AFIO_REMAPS);
	gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_2_MHZ,
		      GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, PAN_STEP_PIN);
}

void board_tmc_uart_use_usart(void)
{
	/*
	 * USART3 partial remap routes TX/RX to PC10/PC11. Full remap would use
	 * PD8/PD9 and is not valid for the STM32F103RC package used here.
	 * The SKR combines TX and RX into the TMC single-wire bus through
	 * R72/R73 (100 ohm) and R74 (1 kohm), so transmitted bytes reach RX.
	 */
	gpio_primary_remap(AFIO_MAPR_SWJ_CFG_JTAG_OFF_SW_OFF,
			   SENTRY_AFIO_REMAPS);
	gpio_set_mode(GPIOC, GPIO_MODE_OUTPUT_2_MHZ,
		      GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO10);
	gpio_set_mode(GPIOC, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO11);
}
