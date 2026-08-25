#include "driver_control.h"

#include "board.h"
#include "tmc_uart.h"

static tmc2209_device_t pan_driver;
static tmc2209_device_t tilt_driver;

void driver_control_init(void)
{
	tmc2209_transport_t transport = tmc_uart_transport();
	const bool uart_ready = tmc_uart_init();

	tmc2209_device_init(&pan_driver, BOARD_PAN_TMC_ADDRESS, &transport);
	tmc2209_device_init(&tilt_driver, BOARD_TILT_TMC_ADDRESS, &transport);
	if (!uart_ready) {
		pan_driver.state = TMC2209_STATE_ERROR;
		pan_driver.error = TMC2209_ERROR_UART;
		tilt_driver.state = TMC2209_STATE_ERROR;
		tilt_driver.error = TMC2209_ERROR_UART;
		return;
	}

	(void)tmc2209_probe(&pan_driver);
	(void)tmc2209_probe(&tilt_driver);
}

static bool configure_driver(tmc2209_device_t *device)
{
	if ((device->state == TMC2209_STATE_CONFIGURED) &&
	    device->configuration_valid &&
	    (device->error == TMC2209_ERROR_NONE) && !device->fatal) {
		return true;
	}
	if ((device->state != TMC2209_STATE_PRESENT) &&
	    (device->state != TMC2209_STATE_CONFIGURED)) {
		if (tmc2209_probe(device) != TMC2209_ERROR_NONE) {
			return false;
		}
	}
	return tmc2209_configure(device) == TMC2209_ERROR_NONE;
}

driver_configure_result_t driver_control_configure(void)
{
	driver_configure_result_t result;

	result.pan_configured = configure_driver(&pan_driver);
	result.tilt_configured = configure_driver(&tilt_driver);
	return result;
}

void driver_control_refresh(driver_axis_t axis)
{
	tmc2209_device_t *device =
		axis == DRIVER_AXIS_PAN ? &pan_driver : &tilt_driver;

	(void)tmc2209_refresh_status(device);
}

const tmc2209_device_t *driver_control_get(driver_axis_t axis)
{
	return axis == DRIVER_AXIS_PAN ? &pan_driver : &tilt_driver;
}

bool driver_control_pan_ready(void)
{
	return (pan_driver.state == TMC2209_STATE_CONFIGURED) &&
	       pan_driver.configuration_valid &&
	       (pan_driver.error == TMC2209_ERROR_NONE) && !pan_driver.fatal;
}

void driver_control_get_uart_diagnostics(tmc_uart_diagnostics_t *diagnostics)
{
	tmc_uart_get_diagnostics(diagnostics);
}
