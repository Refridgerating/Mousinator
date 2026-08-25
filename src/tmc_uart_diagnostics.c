#include "tmc_uart_diagnostics.h"

#include <stdint.h>

static void increment_saturating(uint32_t *value)
{
	if (*value != UINT32_MAX) {
		++(*value);
	}
}

void tmc_uart_diagnostics_init(tmc_uart_diagnostics_t *diagnostics)
{
	*diagnostics = (tmc_uart_diagnostics_t){0};
	diagnostics->last_operation = TMC2209_OPERATION_NONE;
}

void tmc_uart_diagnostics_record(tmc_uart_diagnostics_t *diagnostics,
				 uint8_t flags, bool target_valid,
				 uint8_t address,
				 tmc2209_operation_t operation,
				 uint8_t register_address)
{
	if ((flags & TMC2209_UART_FLAG_ORE) != 0U) {
		increment_saturating(&diagnostics->overrun_count);
	}
	if ((flags & TMC2209_UART_FLAG_NE) != 0U) {
		increment_saturating(&diagnostics->noise_count);
	}
	if ((flags & TMC2209_UART_FLAG_FE) != 0U) {
		increment_saturating(&diagnostics->framing_count);
	}
	if ((flags & TMC2209_UART_FLAG_PE) != 0U) {
		increment_saturating(&diagnostics->parity_count);
	}
	if (flags == 0U) {
		return;
	}
	diagnostics->last_valid = target_valid;
	diagnostics->last_address = address;
	diagnostics->last_operation = target_valid ? operation :
		TMC2209_OPERATION_NONE;
	diagnostics->last_register = register_address;
	diagnostics->last_flags = flags;
}
