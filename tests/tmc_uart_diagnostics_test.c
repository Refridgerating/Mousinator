#include "tmc_uart_diagnostics.h"

#include <stdbool.h>
#include <stdint.h>
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
	tmc_uart_diagnostics_t diagnostics;
	int failures = 0;

	tmc_uart_diagnostics_init(&diagnostics);
	tmc_uart_diagnostics_record(
		&diagnostics,
		TMC2209_UART_FLAG_ORE | TMC2209_UART_FLAG_NE |
			TMC2209_UART_FLAG_FE | TMC2209_UART_FLAG_PE,
		true, 1U, TMC2209_OPERATION_READ, TMC2209_REGISTER_IOIN);
	failures += check(diagnostics.overrun_count == 1U &&
			  diagnostics.noise_count == 1U &&
			  diagnostics.framing_count == 1U &&
			  diagnostics.parity_count == 1U,
			  "simultaneous USART flags counted independently");
	failures += check(diagnostics.last_valid &&
			  diagnostics.last_address == 1U &&
			  diagnostics.last_operation == TMC2209_OPERATION_READ &&
			  diagnostics.last_register == TMC2209_REGISTER_IOIN &&
			  diagnostics.last_flags == 0x0FU,
			  "last USART target metadata retained");

	diagnostics.overrun_count = UINT32_MAX;
	tmc_uart_diagnostics_record(&diagnostics, TMC2209_UART_FLAG_ORE,
				    true, 2U, TMC2209_OPERATION_WRITE,
				    TMC2209_REGISTER_GCONF);
	failures += check(diagnostics.overrun_count == UINT32_MAX,
			  "USART counters saturate");

	tmc_uart_diagnostics_record(&diagnostics, TMC2209_UART_FLAG_FE,
				    false, 0U, TMC2209_OPERATION_NONE, 0U);
	failures += check(!diagnostics.last_valid &&
			  diagnostics.last_operation == TMC2209_OPERATION_NONE &&
			  diagnostics.last_flags == TMC2209_UART_FLAG_FE,
			  "unattributed startup flag remains observable");

	if (failures == 0) {
		(void)puts("tmc uart diagnostic tests passed");
	}
	return failures;
}
