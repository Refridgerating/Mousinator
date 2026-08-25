#include "driver_control.h"
#include "board.h"
#include "tmc_uart.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static unsigned int pan_probe_calls;
static unsigned int tilt_probe_calls;
static unsigned int pan_configure_calls;
static unsigned int tilt_configure_calls;
static bool pan_configure_success;
static bool tilt_configure_success;
static bool tilt_refresh_fails;
static tmc_uart_diagnostics_t mock_uart_diagnostics;

static int check(bool condition, const char *name)
{
	if (!condition) {
		(void)fprintf(stderr, "FAIL: %s\n", name);
		return 1;
	}
	return 0;
}

bool tmc_uart_init(void)
{
	return true;
}

tmc2209_transport_t tmc_uart_transport(void)
{
	return (tmc2209_transport_t){NULL, NULL, NULL, NULL};
}

void tmc2209_device_init(tmc2209_device_t *device, uint8_t address,
			 const tmc2209_transport_t *transport)
{
	*device = (tmc2209_device_t){0};
	device->transport = *transport;
	device->address = address;
	device->state = TMC2209_STATE_UNKNOWN;
}

tmc2209_error_t tmc2209_probe(tmc2209_device_t *device)
{
	if (device->address == BOARD_PAN_TMC_ADDRESS) {
		++pan_probe_calls;
	} else {
		++tilt_probe_calls;
	}
	device->state = TMC2209_STATE_PRESENT;
	device->error = TMC2209_ERROR_NONE;
	device->fatal = false;
	device->ioin = (uint32_t)TMC2209_EXPECTED_VERSION <<
		       TMC2209_IOIN_VERSION_SHIFT;
	return TMC2209_ERROR_NONE;
}

tmc2209_error_t tmc2209_configure(tmc2209_device_t *device)
{
	const bool is_pan = device->address == BOARD_PAN_TMC_ADDRESS;
	const bool succeeds = is_pan ? pan_configure_success :
					tilt_configure_success;

	if (is_pan) {
		++pan_configure_calls;
	} else {
		++tilt_configure_calls;
	}
	if (!succeeds) {
		device->state = TMC2209_STATE_ERROR;
		device->error = TMC2209_ERROR_TIMEOUT;
		device->configuration_valid = false;
		return TMC2209_ERROR_TIMEOUT;
	}
	device->state = TMC2209_STATE_CONFIGURED;
	device->error = TMC2209_ERROR_NONE;
	device->configuration_valid = true;
	device->fatal = false;
	device->run_current_ma = TMC2209_RUN_CURRENT_MA;
	device->hold_current_ma = TMC2209_HOLD_CURRENT_MA;
	device->microsteps = TMC2209_MICROSTEPS;
	device->interpolate = true;
	device->stealthchop = true;
	return TMC2209_ERROR_NONE;
}

tmc2209_error_t tmc2209_refresh_status(tmc2209_device_t *device)
{
	if ((device->address == BOARD_TILT_TMC_ADDRESS) && tilt_refresh_fails) {
		device->state = TMC2209_STATE_ERROR;
		device->error = TMC2209_ERROR_TIMEOUT;
		device->diagnostics.last_operation = TMC2209_OPERATION_READ;
		device->diagnostics.last_register = TMC2209_REGISTER_IOIN;
		device->diagnostics.last_attempt = 3U;
		device->diagnostics.last_transport_error = TMC2209_ERROR_TIMEOUT;
		return TMC2209_ERROR_TIMEOUT;
	}
	device->state = device->configuration_valid ?
		TMC2209_STATE_CONFIGURED : TMC2209_STATE_PRESENT;
	device->error = TMC2209_ERROR_NONE;
	return TMC2209_ERROR_NONE;
}

void tmc_uart_get_diagnostics(tmc_uart_diagnostics_t *diagnostics)
{
	*diagnostics = mock_uart_diagnostics;
}

int main(void)
{
	const tmc2209_device_t *tilt;
	const tmc2209_device_t *pan;
	tmc_uart_diagnostics_t uart_snapshot;
	driver_configure_result_t result;
	int failures = 0;

	pan_configure_success = true;
	tilt_configure_success = true;
	tilt_refresh_fails = false;
	tmc_uart_diagnostics_init(&mock_uart_diagnostics);
	mock_uart_diagnostics.overrun_count = 1U;
	driver_control_init();
	pan_probe_calls = 0U;
	tilt_probe_calls = 0U;

	result = driver_control_configure();
	failures += check(result.pan_configured && result.tilt_configured &&
			  pan_configure_calls == 1U &&
			  tilt_configure_calls == 1U,
			  "initial independent configuration");
	result = driver_control_configure();
	failures += check(result.pan_configured && result.tilt_configured &&
			  pan_configure_calls == 1U &&
			  tilt_configure_calls == 1U,
			  "repeated configure performs no rewrites");

	tilt_refresh_fails = true;
	driver_control_refresh(DRIVER_AXIS_TILT);
	tilt = driver_control_get(DRIVER_AXIS_TILT);
	pan = driver_control_get(DRIVER_AXIS_PAN);
	driver_control_get_uart_diagnostics(&uart_snapshot);
	failures += check(tilt->state == TMC2209_STATE_ERROR &&
			  tilt->configuration_valid &&
			  tilt->diagnostics.last_operation == TMC2209_OPERATION_READ &&
			  pan->diagnostics.last_operation == TMC2209_OPERATION_NONE &&
			  uart_snapshot.overrun_count == 1U &&
			  driver_control_pan_ready(),
			  "TILT communication failure leaves PAN independently ready");
	tilt_refresh_fails = false;
	driver_control_refresh(DRIVER_AXIS_TILT);
	failures += check(tilt->state == TMC2209_STATE_CONFIGURED &&
			  tilt->configuration_valid &&
			  driver_control_pan_ready(),
			  "TILT refresh recovery does not affect PAN");

	tilt_refresh_fails = true;
	driver_control_refresh(DRIVER_AXIS_TILT);
	tilt_refresh_fails = false;
	tilt_configure_success = false;
	result = driver_control_configure();
	failures += check(result.pan_configured && !result.tilt_configured &&
			  pan_configure_calls == 1U &&
			  tilt_configure_calls == 2U &&
			  pan_probe_calls == 0U && tilt_probe_calls == 1U &&
			  driver_control_pan_ready(),
			  "TILT recovery failure never rewrites or revokes PAN");

	if (failures == 0) {
		(void)puts("driver control tests passed");
	}
	return failures;
}
