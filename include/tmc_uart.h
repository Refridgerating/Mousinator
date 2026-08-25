#ifndef SENTRY_TMC_UART_H
#define SENTRY_TMC_UART_H

#include "tmc2209.h"
#include "tmc_uart_diagnostics.h"

#include <stdbool.h>

#define TMC_UART_BAUD_RATE 40000U
#define TMC_UART_TRANSACTION_TIMEOUT_US 5000U
#define TMC_UART_POST_READ_IDLE_BITS 4U
#define TMC_UART_RECOVERY_IDLE_BITS 12U
#define TMC_UART_BITS_TO_US_CEILING(bit_count) \
	(((bit_count) * 1000000U + TMC_UART_BAUD_RATE - 1U) / \
	 TMC_UART_BAUD_RATE)
#define TMC_UART_POST_READ_IDLE_US \
	TMC_UART_BITS_TO_US_CEILING(TMC_UART_POST_READ_IDLE_BITS)
#define TMC_UART_RECOVERY_IDLE_US \
	TMC_UART_BITS_TO_US_CEILING(TMC_UART_RECOVERY_IDLE_BITS)

_Static_assert(TMC_UART_POST_READ_IDLE_US * TMC_UART_BAUD_RATE >=
		       TMC_UART_POST_READ_IDLE_BITS * 1000000U,
	       "post-read guard must cover four complete bit times");
_Static_assert(TMC_UART_RECOVERY_IDLE_US * TMC_UART_BAUD_RATE >=
		       TMC_UART_RECOVERY_IDLE_BITS * 1000000U,
	       "recovery guard must cover twelve complete bit times");
_Static_assert(TMC_UART_POST_READ_IDLE_US < TMC_UART_TRANSACTION_TIMEOUT_US,
	       "post-read guard must fit inside the transaction timeout");

bool tmc_uart_init(void);
tmc2209_transport_t tmc_uart_transport(void);
void tmc_uart_get_diagnostics(tmc_uart_diagnostics_t *diagnostics);

#endif
