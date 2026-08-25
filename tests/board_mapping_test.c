#include "board.h"
#include "tmc2209.h"
#include "tmc_uart.h"

#include <stdbool.h>
#include <stdio.h>

static int check(bool condition, const char *name)
{
	if (!condition) {
		(void)fprintf(stderr, "FAIL: %s\n", name);
		return 1;
	}
	return 0;
}

int main(void)
{
	int failures = 0;

	failures += check(BOARD_PAN_STEP_GPIO_PORT == BOARD_GPIO_PORT_B &&
			  BOARD_PAN_STEP_PIN_NUMBER == 10U,
			  "PAN STEP is Y-driver PB10");
	failures += check(BOARD_PAN_DIRECTION_GPIO_PORT == BOARD_GPIO_PORT_B &&
			  BOARD_PAN_DIRECTION_PIN_NUMBER == 2U,
			  "PAN DIR is Y-driver PB2");
	failures += check(BOARD_PAN_ENABLE_GPIO_PORT == BOARD_GPIO_PORT_B &&
			  BOARD_PAN_ENABLE_PIN_NUMBER == 11U &&
			  BOARD_PAN_ENABLE_ACTIVE_LOW == 1U,
			  "PAN ENABLE is active-low Y-driver PB11");
	failures += check(BOARD_TILT_STEP_GPIO_PORT == BOARD_GPIO_PORT_B &&
			  BOARD_TILT_STEP_PIN_NUMBER == 0U &&
			  BOARD_TILT_DIRECTION_GPIO_PORT == BOARD_GPIO_PORT_C &&
			  BOARD_TILT_DIRECTION_PIN_NUMBER == 5U &&
			  BOARD_TILT_ENABLE_GPIO_PORT == BOARD_GPIO_PORT_B &&
			  BOARD_TILT_ENABLE_PIN_NUMBER == 1U,
			  "future TILT is Z-driver PB0/PC5/PB1");
	failures += check(BOARD_ZAM_ZBM_PARALLEL_OUTPUTS == 1U,
			  "ZAM and ZBM are one parallel driver output");
	failures += check(BOARD_TILT_ENABLE_ACTIVE_LOW == 1U,
			  "TILT enable remains active-low PB1");
	failures += check(BOARD_TILT_ENDSTOP_GPIO_PORT == BOARD_GPIO_PORT_C &&
			  BOARD_TILT_ENDSTOP_PIN_NUMBER == 2U &&
			  BOARD_TILT_ENDSTOP_ACTIVE_LOW == 0U,
			  "TILT Z-endstop is hardware-validated active-high PC2");
	failures += check(BOARD_TILT_DIRECTION_CALIBRATED == 0U,
			  "TILT direction remains gated until physical validation");
	failures += check(BOARD_TMC_UART_TX_GPIO_PORT == BOARD_GPIO_PORT_C &&
			  BOARD_TMC_UART_TX_PIN_NUMBER == 10U &&
			  BOARD_TMC_UART_RX_GPIO_PORT == BOARD_GPIO_PORT_C &&
			  BOARD_TMC_UART_RX_PIN_NUMBER == 11U,
			  "TMC UART is USART3 partial-remap PC10/PC11");
	failures += check(BOARD_TMC_USART_NUMBER == 3U &&
			  BOARD_TMC_USART3_PARTIAL_REMAP == 1U,
			  "TMC UART uses USART3 partial remap");
	failures += check(BOARD_PAN_TMC_ADDRESS == 2U &&
			  BOARD_TILT_TMC_ADDRESS == 1U,
			  "PAN/TILT TMC addresses are 2/1");
	failures += check(TMC_UART_BAUD_RATE == 40000U &&
			  TMC_UART_TRANSACTION_TIMEOUT_US == 5000U &&
			  TMC_UART_POST_READ_IDLE_BITS == 4U &&
			  TMC_UART_POST_READ_IDLE_US == 100U &&
			  TMC_UART_RECOVERY_IDLE_BITS == 12U &&
			  TMC_UART_RECOVERY_IDLE_US == 300U &&
			  TMC2209_TRANSACTION_ATTEMPTS == 3U &&
			  TMC2209_NODECONF_SENDDELAY_3X8_BITS == 2U,
			  "40 kbaud guards cover four and twelve full bit times");
	failures += check(
		TMC_UART_POST_READ_IDLE_US * TMC_UART_BAUD_RATE >=
			TMC_UART_POST_READ_IDLE_BITS * 1000000U &&
		TMC_UART_RECOVERY_IDLE_US * TMC_UART_BAUD_RATE >=
			TMC_UART_RECOVERY_IDLE_BITS * 1000000U,
		"baud-derived guards meet datasheet minima");

	if (failures == 0) {
		(void)puts("board mapping tests passed");
	}
	return failures;
}
