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

/* ZAM and ZBM are parallel outputs from the same TILT/Z driver. */
#define BOARD_TILT_STEP_GPIO_PORT BOARD_GPIO_PORT_B
#define BOARD_TILT_STEP_PIN_NUMBER 0U
#define BOARD_TILT_STEP_PIN_MASK (1U << BOARD_TILT_STEP_PIN_NUMBER)
#define BOARD_TILT_DIRECTION_GPIO_PORT BOARD_GPIO_PORT_C
#define BOARD_TILT_DIRECTION_PIN_NUMBER 5U
#define BOARD_TILT_DIRECTION_PIN_MASK (1U << BOARD_TILT_DIRECTION_PIN_NUMBER)
#define BOARD_TILT_ENABLE_GPIO_PORT BOARD_GPIO_PORT_B
#define BOARD_TILT_ENABLE_PIN_NUMBER 1U
#define BOARD_TILT_ENABLE_PIN_MASK (1U << BOARD_TILT_ENABLE_PIN_NUMBER)
#define BOARD_TILT_ENABLE_ACTIVE_LOW 1U
#define BOARD_ZAM_ZBM_PARALLEL_OUTPUTS 1U
#define BOARD_TILT_ENDSTOP_GPIO_PORT BOARD_GPIO_PORT_C
#define BOARD_TILT_ENDSTOP_PIN_NUMBER 2U
#define BOARD_TILT_ENDSTOP_PIN_MASK (1U << BOARD_TILT_ENDSTOP_PIN_NUMBER)
#define BOARD_TILT_ENDSTOP_ACTIVE_LOW 1U

/* Set these only after the bounded physical PC5 direction check. */
#define BOARD_TILT_DIRECTION_CALIBRATED 0U
#define BOARD_TILT_POSITIVE_DIRECTION_HIGH 0U

/* Shared TMC2209 UART uses USART3 partial remap on PC10/PC11. */
#define BOARD_TMC_UART_TX_GPIO_PORT BOARD_GPIO_PORT_C
#define BOARD_TMC_UART_TX_PIN_NUMBER 10U
#define BOARD_TMC_UART_RX_GPIO_PORT BOARD_GPIO_PORT_C
#define BOARD_TMC_UART_RX_PIN_NUMBER 11U
#define BOARD_TMC_USART_NUMBER 3U
#define BOARD_TMC_USART3_PARTIAL_REMAP 1U
#define BOARD_PAN_TMC_ADDRESS 2U
#define BOARD_TILT_TMC_ADDRESS 1U

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

void board_tilt_set_enabled(bool enabled);
void board_tilt_set_direction(bool positive);
void board_tilt_set_direction_raw(bool high);
void board_tilt_step_use_timer(void);
bool board_tilt_endstop_raw_high(void);
bool board_tilt_endstop_triggered(void);

/* Configure USART3 partial remap without disturbing the PAN timer remap. */
void board_tmc_uart_use_usart(void);

#endif
