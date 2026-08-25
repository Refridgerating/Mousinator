#ifndef SENTRY_TMC_UART_DIAGNOSTICS_H
#define SENTRY_TMC_UART_DIAGNOSTICS_H

#include "tmc2209.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t overrun_count;
	uint32_t noise_count;
	uint32_t framing_count;
	uint32_t parity_count;
	bool last_valid;
	uint8_t last_address;
	tmc2209_operation_t last_operation;
	uint8_t last_register;
	uint8_t last_flags;
} tmc_uart_diagnostics_t;

void tmc_uart_diagnostics_init(tmc_uart_diagnostics_t *diagnostics);
void tmc_uart_diagnostics_record(tmc_uart_diagnostics_t *diagnostics,
				 uint8_t flags, bool target_valid,
				 uint8_t address,
				 tmc2209_operation_t operation,
				 uint8_t register_address);

#endif
