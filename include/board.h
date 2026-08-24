#ifndef SENTRY_BOARD_H
#define SENTRY_BOARD_H

#include <stdbool.h>

#define SENTRY_APPLICATION_ORIGIN 0x08007000U

/* Logical Sentry axes mapped to physical SKR Mini E3 V2.0 driver outputs. */
#define BOARD_GPIO_PORT_B 1U
#define BOARD_GPIO_PORT_C 2U
#define BOARD_PAN_STEP_GPIO_PORT BOARD_GPIO_PORT_B
#define BOARD_PAN_STEP_PIN_NUMBER 10U
#define BOARD_PAN_STEP_PIN_MASK (1U << BOARD_PAN_STEP_PIN_NUMBER)
#define BOARD_PAN_DIRECTION_GPIO_PORT BOARD_GPIO_PORT_B
#define BOARD_PAN_DIRECTION_PIN_NUMBER 2U
#define BOARD_PAN_DIRECTION_PIN_MASK (1U << BOARD_PAN_DIRECTION_PIN_NUMBER)
#define BOARD_PAN_ENABLE_GPIO_PORT BOARD_GPIO_PORT_B
#define BOARD_PAN_ENABLE_PIN_NUMBER 11U
#define BOARD_PAN_ENABLE_PIN_MASK (1U << BOARD_PAN_ENABLE_PIN_NUMBER)
#define BOARD_PAN_ENABLE_ACTIVE_LOW 1U

/* TILT is not implemented; ZAM and ZBM are parallel Z-driver connectors. */
#define BOARD_TILT_STEP_GPIO_PORT BOARD_GPIO_PORT_B
#define BOARD_TILT_STEP_PIN_NUMBER 0U
#define BOARD_TILT_STEP_PIN_MASK (1U << BOARD_TILT_STEP_PIN_NUMBER)
#define BOARD_TILT_DIRECTION_GPIO_PORT BOARD_GPIO_PORT_C
#define BOARD_TILT_DIRECTION_PIN_NUMBER 5U
#define BOARD_TILT_DIRECTION_PIN_MASK (1U << BOARD_TILT_DIRECTION_PIN_NUMBER)
#define BOARD_TILT_ENABLE_GPIO_PORT BOARD_GPIO_PORT_B
#define BOARD_TILT_ENABLE_PIN_NUMBER 1U
#define BOARD_TILT_ENABLE_PIN_MASK (1U << BOARD_TILT_ENABLE_PIN_NUMBER)
#define BOARD_ZAM_ZBM_PARALLEL_OUTPUTS 1U

/*
 * Configure every safety-critical output while the MCU is still running from
 * its reset clock. Motor-enable signals are active low on this board.
 */
void board_safe_init(void);

/* Configure the documented 8 MHz HSE for a 72 MHz system / 48 MHz USB clock. */
void board_clock_init(void);

/* PA14 controls the board's external USB D+ pull-up and is active low. */
void board_usb_connect(bool connected);

/* PAN/Y-driver hardware access is centralized here for safety auditing. */
void board_pan_set_enabled(bool enabled);
void board_pan_set_direction(bool positive);
void board_pan_step_use_timer(void);

#endif
