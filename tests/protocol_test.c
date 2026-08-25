#include "board.h"
#include "driver_control.h"
#include "motion_controller.h"
#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CAPTURE_CAPACITY 4096U

typedef struct {
	uint8_t bytes[CAPTURE_CAPACITY];
	size_t length;
} capture_t;

static motion_controller_snapshot_t mock_snapshots[2];
static motion_enable_result_t mock_enable_results[2];
static axis_disable_result_t mock_disable_results[2];
static motion_velocity_result_t mock_velocity_results[2];
static motion_dual_velocity_result_t mock_dual_result;
static motion_home_result_t mock_home_result;
static motion_jog_result_t mock_jog_result;
static motion_direction_check_result_t mock_direction_result;
static int32_t mock_last_velocities[2];
static int32_t mock_last_jog;
static bool mock_endstop_raw_high;
static bool mock_endstop_triggered;
static unsigned int mock_stop_calls;
static unsigned int mock_fault_calls[2];
static tmc2209_device_t mock_pan_driver;
static tmc2209_device_t mock_tilt_driver;
static bool mock_pan_configure_success;
static bool mock_tilt_configure_success;
static tmc_uart_diagnostics_t mock_uart_diagnostics;

static void mock_reset(void)
{
	size_t index;

	(void)memset(mock_snapshots, 0, sizeof(mock_snapshots));
	for (index = 0U; index < 2U; ++index) {
		mock_enable_results[index] = MOTION_ENABLE_OK;
		mock_disable_results[index] = AXIS_DISABLE_COMPLETE;
		mock_velocity_results[index] = MOTION_VELOCITY_OK;
		mock_last_velocities[index] = 0;
		mock_snapshots[index].home_status = TILT_HOME_STATUS_IDLE;
	}
	mock_dual_result.pan_result = MOTION_VELOCITY_OK;
	mock_dual_result.tilt_result = MOTION_VELOCITY_OK;
	mock_dual_result.applied = true;
	mock_home_result = MOTION_HOME_OK;
	mock_jog_result = MOTION_JOG_OK;
	mock_direction_result = MOTION_DIRECTION_CHECK_OK;
	mock_last_jog = 0;
	mock_endstop_raw_high = false;
	mock_endstop_triggered = false;
	mock_stop_calls = 0U;
	mock_fault_calls[0] = 0U;
	mock_fault_calls[1] = 0U;
	mock_pan_configure_success = true;
	mock_tilt_configure_success = true;
	tmc_uart_diagnostics_init(&mock_uart_diagnostics);
	tmc2209_device_init(&mock_pan_driver, 2U,
			 &(tmc2209_transport_t){NULL, NULL, NULL, NULL});
	tmc2209_device_init(&mock_tilt_driver, 1U,
			 &(tmc2209_transport_t){NULL, NULL, NULL, NULL});
	mock_pan_driver.state = TMC2209_STATE_PRESENT;
	mock_tilt_driver.state = TMC2209_STATE_PRESENT;
	mock_pan_driver.ioin = (uint32_t)TMC2209_EXPECTED_VERSION <<
			       TMC2209_IOIN_VERSION_SHIFT;
	mock_tilt_driver.ioin = (uint32_t)TMC2209_EXPECTED_VERSION <<
				TMC2209_IOIN_VERSION_SHIFT;
}

void motion_controller_init(void)
{
}

motion_enable_result_t motion_controller_enable(motion_axis_t axis)
{
	if (mock_enable_results[axis] == MOTION_ENABLE_OK) {
		mock_snapshots[axis].enabled = true;
	}
	return mock_enable_results[axis];
}

axis_disable_result_t motion_controller_disable(motion_axis_t axis)
{
	if (mock_disable_results[axis] == AXIS_DISABLE_COMPLETE) {
		mock_snapshots[axis].enabled = false;
	}
	return mock_disable_results[axis];
}

motion_velocity_result_t motion_controller_set_velocity(
	motion_axis_t axis, int32_t velocity_steps_s)
{
	mock_last_velocities[axis] = velocity_steps_s;
	return mock_velocity_results[axis];
}

motion_dual_velocity_result_t motion_controller_set_both_velocities(
	int32_t pan_velocity_steps_s, int32_t tilt_velocity_steps_s)
{
	if (mock_dual_result.applied) {
		mock_last_velocities[MOTION_AXIS_PAN] = pan_velocity_steps_s;
		mock_last_velocities[MOTION_AXIS_TILT] = tilt_velocity_steps_s;
	}
	return mock_dual_result;
}

void motion_controller_stop_all(void)
{
	++mock_stop_calls;
	mock_snapshots[MOTION_AXIS_PAN].target_velocity_steps_s = 0;
	mock_snapshots[MOTION_AXIS_TILT].target_velocity_steps_s = 0;
}

motion_home_result_t motion_controller_home_tilt(void)
{
	return mock_home_result;
}

motion_jog_result_t motion_controller_jog_tilt(int32_t steps)
{
	mock_last_jog = steps;
	return mock_jog_result;
}

motion_direction_check_result_t motion_controller_direction_check_tilt(
	bool direction_high)
{
	(void)direction_high;
	return mock_direction_result;
}

void motion_controller_get_snapshot(motion_axis_t axis,
				    motion_controller_snapshot_t *snapshot)
{
	*snapshot = mock_snapshots[axis];
}

bool motion_controller_axis_inactive(motion_axis_t axis)
{
	const motion_controller_snapshot_t *snapshot = &mock_snapshots[axis];

	return !snapshot->enabled && !snapshot->moving &&
	       !snapshot->disabling && !snapshot->homing && !snapshot->jogging &&
	       !snapshot->direction_checking;
}

void motion_controller_driver_fault(motion_axis_t axis)
{
	++mock_fault_calls[axis];
	mock_snapshots[axis].enabled = false;
}

const char *tilt_home_status_name(tilt_home_status_t status)
{
	return status == TILT_HOME_STATUS_SUCCESS ? "SUCCESS" :
	       status == TILT_HOME_STATUS_RUNNING ? "RUNNING" : "IDLE";
}

bool board_tilt_endstop_raw_high(void)
{
	return mock_endstop_raw_high;
}

bool board_tilt_endstop_triggered(void)
{
	return mock_endstop_triggered;
}

void driver_control_init(void)
{
}

driver_configure_result_t driver_control_configure(void)
{
	driver_configure_result_t result = {mock_pan_configure_success,
					    mock_tilt_configure_success};

	mock_pan_driver.state = mock_pan_configure_success ?
		TMC2209_STATE_CONFIGURED : TMC2209_STATE_ERROR;
	mock_pan_driver.configuration_valid = mock_pan_configure_success;
	mock_pan_driver.run_current_ma = TMC2209_RUN_CURRENT_MA;
	mock_pan_driver.hold_current_ma = TMC2209_HOLD_CURRENT_MA;
	mock_pan_driver.microsteps = TMC2209_MICROSTEPS;
	mock_pan_driver.interpolate = true;
	mock_pan_driver.stealthchop = true;
	mock_tilt_driver.state = mock_tilt_configure_success ?
		TMC2209_STATE_CONFIGURED : TMC2209_STATE_ERROR;
	mock_tilt_driver.configuration_valid = mock_tilt_configure_success;
	return result;
}

void driver_control_refresh(driver_axis_t axis)
{
	(void)axis;
}

const tmc2209_device_t *driver_control_get(driver_axis_t axis)
{
	return axis == DRIVER_AXIS_PAN ? &mock_pan_driver : &mock_tilt_driver;
}

bool driver_control_ready(driver_axis_t axis)
{
	const tmc2209_device_t *device = driver_control_get(axis);

	return device->state == TMC2209_STATE_CONFIGURED &&
	       device->configuration_valid && device->error == TMC2209_ERROR_NONE &&
	       !device->fatal;
}

void driver_control_get_uart_diagnostics(tmc_uart_diagnostics_t *diagnostics)
{
	*diagnostics = mock_uart_diagnostics;
}

static bool capture_write(const uint8_t *data, size_t length, void *context)
{
	capture_t *capture = context;

	if (length > (CAPTURE_CAPACITY - capture->length)) {
		return false;
	}
	(void)memcpy(&capture->bytes[capture->length], data, length);
	capture->length += length;
	return true;
}

static int check_capture(const char *name, const capture_t *capture,
			 const char *expected)
{
	const size_t expected_length = strlen(expected);

	if ((capture->length != expected_length) ||
	    (memcmp(capture->bytes, expected, expected_length) != 0)) {
		(void)fprintf(stderr, "FAIL: %s\nexpected: %s\n", name, expected);
		return 1;
	}
	return 0;
}

static int run_case(const char *name, const char *commands,
		    const char *expected)
{
	protocol_t protocol;
	capture_t capture = {{0U}, 0U};

	mock_reset();
	protocol_init(&protocol, capture_write, &capture);
	protocol_receive(&protocol, (const uint8_t *)commands, strlen(commands));
	return check_capture(name, &capture, expected);
}

static int test_basic_and_compatibility(void)
{
	return run_case(
		"basic and PAN state compatibility",
		"PING\nINFO\nSTATE?\nSTATE? PAN\n",
		"OK PONG\r\n"
		"OK SENTRY-MCU 0.4.0 SKR_MINI_E3_V2\r\n"
		"OK PAN ENABLED=0 DISABLING=0 MOVING=0 POS=0 VEL=0 "
		"TARGET=0 TIMEOUT=0\r\n"
		"OK PAN ENABLED=0 DISABLING=0 MOVING=0 POS=0 VEL=0 "
		"TARGET=0 TIMEOUT=0\r\n");
}

static int test_axis_commands(void)
{
	return run_case(
		"two-axis motion commands",
		"ENABLE PAN\nENABLE TILT\nVEL PAN 200\nVEL TILT -50\n"
		"VEL BOTH 300 100\nSTOP\nDISABLE PAN\nDISABLE TILT\n",
		"OK PAN ENABLED\r\nOK TILT ENABLED\r\nOK\r\nOK\r\nOK\r\n"
		"OK\r\nOK PAN DISABLED\r\nOK TILT DISABLED\r\n");
}

static int test_tilt_state_and_commissioning(void)
{
	protocol_t protocol;
	capture_t capture = {{0U}, 0U};
	static const char commands[] =
		"STATE? TILT\nENDSTOP? TILT\nHOME TILT\nJOG TILT +50\n"
		"DIR-CHECK TILT HIGH\n";
	static const char expected[] =
		"OK TILT ENABLED=1 DISABLING=0 MOVING=0 POS=123 VEL=0 "
		"TARGET=0 TIMEOUT=0 HOMED=1 HOMING=0 HOME_STATUS=SUCCESS "
		"JOGGING=0 DIR_CHECKING=0 DIR_CALIBRATED=0 MIN_LIMIT=0 "
		"MAX_CONFIGURED=0 MAX_LIMIT=0\r\n"
		"OK TILT ENDSTOP=0 RAW=0\r\n"
		"OK TILT HOMING\r\nOK TILT JOGGING\r\n"
		"OK TILT DIR_CHECKING\r\n";

	mock_reset();
	mock_snapshots[MOTION_AXIS_TILT].enabled = true;
	mock_snapshots[MOTION_AXIS_TILT].position_steps = 123;
	mock_snapshots[MOTION_AXIS_TILT].homed = true;
	mock_snapshots[MOTION_AXIS_TILT].home_status =
		TILT_HOME_STATUS_SUCCESS;
	protocol_init(&protocol, capture_write, &capture);
	protocol_receive(&protocol, (const uint8_t *)commands,
			 strlen(commands));
	return check_capture("TILT status and commissioning", &capture, expected);
}

static int test_atomic_error_and_axis_errors(void)
{
	protocol_t protocol;
	capture_t capture = {{0U}, 0U};
	int failures = 0;

	mock_reset();
	mock_dual_result.applied = false;
	mock_dual_result.tilt_result = MOTION_VELOCITY_NOT_HOMED;
	protocol_init(&protocol, capture_write, &capture);
	protocol_receive(&protocol, (const uint8_t *)"VEL BOTH 300 200\n", 17U);
	failures += check_capture("dual all-or-none error", &capture,
				  "ERR TILT_NOT_HOMED\r\n");
	if ((mock_last_velocities[0] != 0) || (mock_last_velocities[1] != 0)) {
		(void)fprintf(stderr, "FAIL: rejected dual update mutated a target\n");
		++failures;
	}

	mock_reset();
	mock_velocity_results[MOTION_AXIS_TILT] =
		MOTION_VELOCITY_DIRECTION_UNCALIBRATED;
	capture.length = 0U;
	protocol_init(&protocol, capture_write, &capture);
	protocol_receive(&protocol, (const uint8_t *)"VEL TILT 1\n", 11U);
	failures += check_capture("uncalibrated velocity", &capture,
				  "ERR TILT_DIR_UNCALIBRATED\r\n");
	return failures;
}

static int test_driver_independence_and_config_gate(void)
{
	protocol_t protocol;
	capture_t capture = {{0U}, 0U};
	int failures = 0;

	mock_reset();
	mock_pan_driver.state = TMC2209_STATE_CONFIGURED;
	mock_pan_driver.configuration_valid = true;
	mock_pan_driver.fatal = true;
	mock_snapshots[MOTION_AXIS_PAN].enabled = true;
	protocol_init(&protocol, capture_write, &capture);
	protocol_receive(&protocol, (const uint8_t *)"DRIVER? PAN\n", 12U);
	if ((mock_fault_calls[MOTION_AXIS_PAN] != 1U) ||
	    (mock_fault_calls[MOTION_AXIS_TILT] != 0U)) {
		(void)fprintf(stderr, "FAIL: fatal PAN did not remain axis-local\n");
		++failures;
	}

	mock_reset();
	mock_snapshots[MOTION_AXIS_TILT].enabled = true;
	capture.length = 0U;
	protocol_init(&protocol, capture_write, &capture);
	protocol_receive(&protocol, (const uint8_t *)"DRIVER CONFIGURE\n", 17U);
	failures += check_capture("configure gate includes TILT", &capture,
				  "ERR TILT_ENABLED\r\n");
	return failures;
}

static int test_bounded_parser(void)
{
	char oversized[PROTOCOL_LINE_CAPACITY + 3U];
	protocol_t protocol;
	capture_t capture = {{0U}, 0U};
	size_t index;
	int failures;

	for (index = 0U; index < sizeof(oversized) - 1U; ++index) {
		oversized[index] = 'X';
	}
	oversized[sizeof(oversized) - 1U] = '\n';
	mock_reset();
	protocol_init(&protocol, capture_write, &capture);
	protocol_receive(&protocol, (const uint8_t *)oversized,
			 sizeof(oversized));
	failures = check_capture("bounded oversized input", &capture,
				 "ERR COMMAND_TOO_LONG\r\n");
	failures += run_case("malformed commands",
			     "\nVEL BOTH 1 nope\nENDSTOP? PAN\nHOME PAN\n",
			     "ERR EMPTY_COMMAND\r\nERR BAD_ARGUMENT\r\n"
			     "ERR BAD_ARGUMENT\r\nERR BAD_ARGUMENT\r\n");
	return failures;
}

int main(void)
{
	int failures = 0;

	failures += test_basic_and_compatibility();
	failures += test_axis_commands();
	failures += test_tilt_state_and_commissioning();
	failures += test_atomic_error_and_axis_errors();
	failures += test_driver_independence_and_config_gate();
	failures += test_bounded_parser();
	if (failures == 0) {
		(void)puts("protocol tests passed");
	}
	return failures == 0 ? 0 : 1;
}
