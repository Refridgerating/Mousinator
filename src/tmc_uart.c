#include "tmc_uart.h"

#include "board.h"

#include <libopencm3/cm3/dwt.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>

#include <stddef.h>
#include <stdint.h>

#define TMC_USART USART3
#define TMC_UART_CLOCKS_PER_US 72U
#define TMC_UART_TURNAROUND_CAPTURE_US 100U
#define TMC_UART_ERROR_FLAGS \
	(USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)

static bool uart_initialized;

static bool deadline_expired(uint32_t start_cycles, uint32_t timeout_us)
{
	return (uint32_t)(DWT_CYCCNT - start_cycles) >=
	       (timeout_us * TMC_UART_CLOCKS_PER_US);
}

static void clear_receive_state(void)
{
	while ((USART_SR(TMC_USART) & USART_SR_RXNE) != 0U) {
		(void)USART_DR(TMC_USART);
	}
	if ((USART_SR(TMC_USART) & TMC_UART_ERROR_FLAGS) != 0U) {
		(void)USART_SR(TMC_USART);
		(void)USART_DR(TMC_USART);
	}
}

static tmc2209_error_t capture_available(uint8_t *receive,
					 size_t receive_capacity,
					 size_t *receive_length)
{
	if ((USART_SR(TMC_USART) & TMC_UART_ERROR_FLAGS) != 0U) {
		(void)USART_SR(TMC_USART);
		(void)USART_DR(TMC_USART);
		return TMC2209_ERROR_UART;
	}
	while ((USART_SR(TMC_USART) & USART_SR_RXNE) != 0U) {
		if (*receive_length >= receive_capacity) {
			(void)USART_DR(TMC_USART);
			return TMC2209_ERROR_CAPTURE_OVERFLOW;
		}
		receive[*receive_length] = (uint8_t)USART_DR(TMC_USART);
		++(*receive_length);
	}
	return TMC2209_ERROR_NONE;
}

static tmc2209_error_t wait_for_flag(uint32_t flag, uint32_t start_cycles,
				     uint8_t *receive,
				     size_t receive_capacity,
				     size_t *receive_length)
{
	while ((USART_SR(TMC_USART) & flag) == 0U) {
		tmc2209_error_t error = capture_available(
			receive, receive_capacity, receive_length);

		if (error != TMC2209_ERROR_NONE) {
			return error;
		}
		if (deadline_expired(start_cycles,
				     TMC_UART_TRANSACTION_TIMEOUT_US)) {
			return TMC2209_ERROR_TIMEOUT;
		}
	}
	return capture_available(receive, receive_capacity, receive_length);
}

static tmc2209_error_t tmc_uart_exchange(
	void *context, const uint8_t *transmit, size_t transmit_length,
	uint8_t *receive, size_t receive_capacity, size_t *receive_length)
{
	uint32_t start_cycles;
	size_t index;
	tmc2209_error_t error;

	(void)context;
	if (!uart_initialized ||
	    ((transmit_length != TMC2209_READ_DATAGRAM_SIZE) &&
	     (transmit_length != TMC2209_WRITE_DATAGRAM_SIZE))) {
		return TMC2209_ERROR_UART;
	}

	clear_receive_state();
	*receive_length = 0U;
	start_cycles = DWT_CYCCNT;
	for (index = 0U; index < transmit_length; ++index) {
		error = wait_for_flag(USART_SR_TXE, start_cycles, receive,
				      receive_capacity, receive_length);
		if (error != TMC2209_ERROR_NONE) {
			return error;
		}
		usart_send(TMC_USART, transmit[index]);
	}
	error = wait_for_flag(USART_SR_TC, start_cycles, receive, receive_capacity,
			      receive_length);
	if (error != TMC2209_ERROR_NONE) {
		return error;
	}

	if (transmit_length == TMC2209_WRITE_DATAGRAM_SIZE) {
		const uint32_t turnaround_start = DWT_CYCCNT;

		while (!deadline_expired(turnaround_start,
					 TMC_UART_TURNAROUND_CAPTURE_US)) {
			error = capture_available(receive, receive_capacity,
						  receive_length);
			if (error != TMC2209_ERROR_NONE) {
				return error;
			}
		}
		return TMC2209_ERROR_NONE;
	}

	for (;;) {
		size_t expected_length = TMC2209_REPLY_DATAGRAM_SIZE;

		error = capture_available(receive, receive_capacity, receive_length);
		if (error != TMC2209_ERROR_NONE) {
			return error;
		}
		if ((*receive_length >= transmit_length) &&
		    (receive[0] == transmit[0]) && (receive[1] == transmit[1]) &&
		    (receive[2] == transmit[2]) && (receive[3] == transmit[3])) {
			expected_length += transmit_length;
		}
		if (*receive_length >= expected_length) {
			return TMC2209_ERROR_NONE;
		}
		if (deadline_expired(start_cycles,
				     TMC_UART_TRANSACTION_TIMEOUT_US)) {
			return TMC2209_ERROR_TIMEOUT;
		}
	}
}

static void tmc_uart_recover(void *context)
{
	const uint32_t start_cycles = DWT_CYCCNT;

	(void)context;
	clear_receive_state();
	while (!deadline_expired(start_cycles, TMC_UART_RECOVERY_IDLE_US)) {
		/* UART recovery requires at least 12 idle bit times. */
	}
}

bool tmc_uart_init(void)
{
	uart_initialized = false;
	if (!dwt_enable_cycle_counter()) {
		return false;
	}

	board_tmc_uart_use_usart();
	rcc_periph_clock_enable(RCC_USART3);
	rcc_periph_reset_pulse(RST_USART3);
	usart_set_baudrate(TMC_USART, TMC_UART_BAUD_RATE);
	usart_set_databits(TMC_USART, 8U);
	usart_set_stopbits(TMC_USART, USART_STOPBITS_1);
	usart_set_mode(TMC_USART, USART_MODE_TX_RX);
	usart_set_parity(TMC_USART, USART_PARITY_NONE);
	usart_set_flow_control(TMC_USART, USART_FLOWCONTROL_NONE);
	usart_enable(TMC_USART);
	clear_receive_state();
	uart_initialized = true;
	return true;
}

tmc2209_transport_t tmc_uart_transport(void)
{
	tmc2209_transport_t transport;

	transport.exchange = tmc_uart_exchange;
	transport.recover = tmc_uart_recover;
	transport.context = NULL;
	return transport;
}
