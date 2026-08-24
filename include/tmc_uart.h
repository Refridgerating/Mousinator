#ifndef SENTRY_TMC_UART_H
#define SENTRY_TMC_UART_H

#include "tmc2209.h"

#include <stdbool.h>

#define TMC_UART_BAUD_RATE 115200U
#define TMC_UART_TRANSACTION_TIMEOUT_US 5000U
#define TMC_UART_RECOVERY_IDLE_US 200U

bool tmc_uart_init(void);
tmc2209_transport_t tmc_uart_transport(void);

#endif
