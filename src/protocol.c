#include "protocol.h"

#include "board.h"
#include "driver_control.h"
#include "motion_controller.h"

#include <limits.h>

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))
#define STATIC_ASSERT(name, condition) \
	typedef char static_assert_##name[(condition) ? 1 : -1]
#define MAX_COMMAND_TOKENS 4U

typedef struct {
	const char *data;
	size_t length;
} token_t;

typedef struct {
	uint8_t bytes[PROTOCOL_MAX_RESPONSE_SIZE];
	size_t length;
	bool overflow;
} response_builder_t;

static const uint8_t RESPONSE_OK[] = "OK\r\n";
static const uint8_t RESPONSE_PONG[] = "OK PONG\r\n";
static const uint8_t RESPONSE_INFO[] =
	"OK SENTRY-MCU 0.4.0 SKR_MINI_E3_V2\r\n";
static const uint8_t RESPONSE_PAN_ENABLED[] = "OK PAN ENABLED\r\n";
static const uint8_t RESPONSE_TILT_ENABLED[] = "OK TILT ENABLED\r\n";
static const uint8_t RESPONSE_PAN_DISABLED[] = "OK PAN DISABLED\r\n";
static const uint8_t RESPONSE_TILT_DISABLED[] = "OK TILT DISABLED\r\n";
static const uint8_t RESPONSE_PAN_DISABLING[] = "OK PAN DISABLING\r\n";
static const uint8_t RESPONSE_TILT_DISABLING[] = "OK TILT DISABLING\r\n";
static const uint8_t RESPONSE_EMPTY[] = "ERR EMPTY_COMMAND\r\n";
static const uint8_t RESPONSE_UNKNOWN[] = "ERR UNKNOWN_COMMAND\r\n";
static const uint8_t RESPONSE_TOO_LONG[] = "ERR COMMAND_TOO_LONG\r\n";
static const uint8_t RESPONSE_BAD_ARGUMENT[] = "ERR BAD_ARGUMENT\r\n";
static const uint8_t RESPONSE_VELOCITY_RANGE[] = "ERR VELOCITY_RANGE\r\n";
static const uint8_t RESPONSE_PAN_DISABLED_ERROR[] = "ERR PAN_DISABLED\r\n";
static const uint8_t RESPONSE_TILT_DISABLED_ERROR[] = "ERR TILT_DISABLED\r\n";
static const uint8_t RESPONSE_PAN_DISABLING_ERROR[] = "ERR PAN_DISABLING\r\n";
static const uint8_t RESPONSE_TILT_DISABLING_ERROR[] =
	"ERR TILT_DISABLING\r\n";
static const uint8_t RESPONSE_PAN_DRIVER_NOT_READY[] =
	"ERR PAN_DRIVER_NOT_READY\r\n";
static const uint8_t RESPONSE_TILT_DRIVER_NOT_READY[] =
	"ERR TILT_DRIVER_NOT_READY\r\n";
static const uint8_t RESPONSE_PAN_ENABLED_ERROR[] = "ERR PAN_ENABLED\r\n";
static const uint8_t RESPONSE_TILT_ENABLED_ERROR[] = "ERR TILT_ENABLED\r\n";
static const uint8_t RESPONSE_TILT_BUSY[] = "ERR TILT_BUSY\r\n";
static const uint8_t RESPONSE_TILT_NOT_HOMED[] = "ERR TILT_NOT_HOMED\r\n";
static const uint8_t RESPONSE_TILT_DIR_UNCALIBRATED[] =
	"ERR TILT_DIR_UNCALIBRATED\r\n";
static const uint8_t RESPONSE_TILT_MIN_LIMIT[] = "ERR TILT_MIN_LIMIT\r\n";
static const uint8_t RESPONSE_JOG_RANGE[] = "ERR JOG_RANGE\r\n";

STATIC_ASSERT(info_response_fits,
	      (ARRAY_LENGTH(RESPONSE_INFO) - 1U) <= PROTOCOL_MAX_RESPONSE_SIZE);
STATIC_ASSERT(disabling_response_fits,
	      (ARRAY_LENGTH(RESPONSE_PAN_DISABLING_ERROR) - 1U) <=
		      PROTOCOL_MAX_RESPONSE_SIZE);

static bool token_equals(token_t token, const char *expected,
			 size_t expected_length)
{
	size_t index;

	if (token.length != expected_length) {
		return false;
	}
	for (index = 0U; index < expected_length; ++index) {
		if (token.data[index] != expected[index]) {
			return false;
		}
	}
	return true;
}

static size_t split_tokens(const char *command, size_t command_length,
			   token_t *tokens, bool *too_many)
{
	size_t index = 0U;
	size_t count = 0U;

	*too_many = false;
	while (index < command_length) {
		size_t start;

		while ((index < command_length) &&
		       ((command[index] == ' ') || (command[index] == '\t'))) {
			++index;
		}
		if (index == command_length) {
			break;
		}

		start = index;
		while ((index < command_length) && (command[index] != ' ') &&
		       (command[index] != '\t')) {
			++index;
		}
		if (count >= MAX_COMMAND_TOKENS) {
			*too_many = true;
			return count;
		}
		tokens[count].data = &command[start];
		tokens[count].length = index - start;
		++count;
	}
	return count;
}

static bool parse_int32(token_t token, int32_t *value)
{
	uint32_t magnitude = 0U;
	uint32_t limit = (uint32_t)INT32_MAX;
	size_t index = 0U;
	bool negative = false;

	if (token.length == 0U) {
		return false;
	}
	if ((token.data[0] == '+') || (token.data[0] == '-')) {
		negative = token.data[0] == '-';
		index = 1U;
		if (index == token.length) {
			return false;
		}
	}
	if (negative) {
		limit = (uint32_t)INT32_MAX + 1U;
	}

	for (; index < token.length; ++index) {
		uint32_t digit;

		if ((token.data[index] < '0') || (token.data[index] > '9')) {
			return false;
		}
		digit = (uint32_t)(token.data[index] - '0');
		if (magnitude > ((limit - digit) / 10U)) {
			return false;
		}
		magnitude = (magnitude * 10U) + digit;
	}

	if (negative) {
		if (magnitude == ((uint32_t)INT32_MAX + 1U)) {
			*value = INT32_MIN;
		} else {
			*value = -(int32_t)magnitude;
		}
	} else {
		*value = (int32_t)magnitude;
	}
	return true;
}

static void response_append_byte(response_builder_t *builder, uint8_t byte)
{
	if (builder->length >= ARRAY_LENGTH(builder->bytes)) {
		builder->overflow = true;
		return;
	}
	builder->bytes[builder->length] = byte;
	++builder->length;
}

static void response_append_text(response_builder_t *builder, const char *text)
{
	while (*text != '\0') {
		response_append_byte(builder, (uint8_t)*text);
		++text;
	}
}

static void response_append_u64(response_builder_t *builder, uint64_t value)
{
	uint8_t digits[20];
	size_t count = 0U;

	do {
		digits[count] = (uint8_t)('0' + (value % 10U));
		value /= 10U;
		++count;
	} while (value != 0U);

	while (count > 0U) {
		--count;
		response_append_byte(builder, digits[count]);
	}
}

static void response_append_i64(response_builder_t *builder, int64_t value)
{
	uint64_t magnitude;

	if (value < 0) {
		response_append_byte(builder, (uint8_t)'-');
		magnitude = (uint64_t)(-(value + 1)) + 1U;
	} else {
		magnitude = (uint64_t)value;
	}
	response_append_u64(builder, magnitude);
}

static void response_append_hex32(response_builder_t *builder, uint32_t value)
{
	static const char HEX_DIGITS[] = "0123456789ABCDEF";
	uint8_t shift = 32U;

	response_append_text(builder, "0x");
	while (shift > 0U) {
		shift = (uint8_t)(shift - 4U);
		response_append_byte(
			builder, (uint8_t)HEX_DIGITS[(value >> shift) & 0x0FU]);
	}
}

static void response_append_hex8(response_builder_t *builder, uint8_t value)
{
	static const char HEX_DIGITS[] = "0123456789ABCDEF";

	response_append_text(builder, "0x");
	response_append_byte(builder,
			     (uint8_t)HEX_DIGITS[(value >> 4) & 0x0FU]);
	response_append_byte(builder, (uint8_t)HEX_DIGITS[value & 0x0FU]);
}

static void protocol_emit(protocol_t *protocol, const uint8_t *response,
			  size_t response_length)
{
	(void)protocol->write(response, response_length, protocol->write_context);
}

static void protocol_emit_state(protocol_t *protocol, motion_axis_t axis)
{
	motion_controller_snapshot_t snapshot;
	response_builder_t response = {{0U}, 0U, false};
	const bool is_pan = axis == MOTION_AXIS_PAN;

	motion_controller_get_snapshot(axis, &snapshot);
	response_append_text(&response, is_pan ? "OK PAN ENABLED=" :
					      "OK TILT ENABLED=");
	response_append_byte(&response, snapshot.enabled ? (uint8_t)'1' :
							 (uint8_t)'0');
	response_append_text(&response, " DISABLING=");
	response_append_byte(&response, snapshot.disabling ? (uint8_t)'1' :
							   (uint8_t)'0');
	response_append_text(&response, " MOVING=");
	response_append_byte(&response, snapshot.moving ? (uint8_t)'1' :
							(uint8_t)'0');
	response_append_text(&response, " POS=");
	response_append_i64(&response, snapshot.position_steps);
	response_append_text(&response, " VEL=");
	response_append_i64(&response,
			    (int64_t)snapshot.current_velocity_steps_s);
	response_append_text(&response, " TARGET=");
	response_append_i64(&response,
			    (int64_t)snapshot.target_velocity_steps_s);
	response_append_text(&response, " TIMEOUT=");
	response_append_byte(&response, snapshot.motion_timeout ? (uint8_t)'1' :
						       (uint8_t)'0');
	if (!is_pan) {
		response_append_text(&response, " HOMED=");
		response_append_byte(&response,
			snapshot.homed ? (uint8_t)'1' : (uint8_t)'0');
		response_append_text(&response, " HOMING=");
		response_append_byte(&response,
			snapshot.homing ? (uint8_t)'1' : (uint8_t)'0');
		response_append_text(&response, " HOME_STATUS=");
		response_append_text(&response,
			tilt_home_status_name(snapshot.home_status));
		response_append_text(&response, " JOGGING=");
		response_append_byte(&response,
			snapshot.jogging ? (uint8_t)'1' : (uint8_t)'0');
		response_append_text(&response, " DIR_CHECKING=");
		response_append_byte(&response,
			snapshot.direction_checking ? (uint8_t)'1' :
						      (uint8_t)'0');
		response_append_text(&response, " DIR_CALIBRATED=");
		response_append_byte(&response,
			snapshot.direction_calibrated ? (uint8_t)'1' :
						       (uint8_t)'0');
		response_append_text(&response, " MIN_LIMIT=");
		response_append_byte(&response,
			snapshot.min_limit ? (uint8_t)'1' : (uint8_t)'0');
		response_append_text(&response,
				     " MAX_CONFIGURED=0 MAX_LIMIT=0");
	}
	response_append_text(&response, "\r\n");

	if (!response.overflow) {
		protocol_emit(protocol, response.bytes, response.length);
	}
}

static void protocol_emit_driver(protocol_t *protocol, driver_axis_t axis)
{
	const tmc2209_device_t *device;
	motion_controller_snapshot_t snapshot;
	response_builder_t response = {{0U}, 0U, false};
	const bool is_pan = axis == DRIVER_AXIS_PAN;
	bool present;

	driver_control_refresh(axis);
	device = driver_control_get(axis);
	if (device->fatal || (device->error == TMC2209_ERROR_RESET)) {
		const motion_axis_t motion_axis = is_pan ? MOTION_AXIS_PAN :
							       MOTION_AXIS_TILT;

		motion_controller_get_snapshot(motion_axis, &snapshot);
		if (snapshot.enabled) {
			motion_controller_driver_fault(motion_axis);
		}
	}
	present = ((uint8_t)(device->ioin >> TMC2209_IOIN_VERSION_SHIFT) ==
		   TMC2209_EXPECTED_VERSION);

	response_append_text(&response, "OK DRIVER ");
	response_append_text(&response, is_pan ? "PAN" : "TILT");
	response_append_text(&response, " ADDR=");
	response_append_u64(&response, device->address);
	response_append_text(&response, " PRESENT=");
	response_append_byte(&response, present ? (uint8_t)'1' : (uint8_t)'0');
	response_append_text(&response, " CONFIGURED=");
	response_append_byte(
		&response, (device->state == TMC2209_STATE_CONFIGURED) &&
				   device->configuration_valid &&
				   (device->error == TMC2209_ERROR_NONE) ?
			   (uint8_t)'1' :
			   (uint8_t)'0');
	response_append_text(&response, " ERR=");
	response_append_text(&response, tmc2209_error_name(device->error));
	response_append_text(&response, " IFCNT=");
	response_append_u64(&response, device->ifcnt);
	response_append_text(&response, " IOIN=");
	response_append_hex32(&response, device->ioin);
	response_append_text(&response, " GSTAT=");
	response_append_hex32(&response, device->gstat);
	response_append_text(&response, " DRV=");
	response_append_hex32(&response, device->drv_status);
	response_append_text(&response, " RUN_MA=");
	response_append_u64(&response, device->run_current_ma);
	response_append_text(&response, " HOLD_MA=");
	response_append_u64(&response, device->hold_current_ma);
	response_append_text(&response, " MSTEP=");
	response_append_u64(&response, device->microsteps);
	response_append_text(&response, " INTPOL=");
	response_append_byte(&response,
			     device->interpolate ? (uint8_t)'1' : (uint8_t)'0');
	response_append_text(&response, " MODE=");
	response_append_text(&response,
			     device->stealthchop ? "STEALTHCHOP" : "UNCONFIGURED");
	response_append_text(&response, " OTPW=");
	response_append_byte(
		&response, (device->drv_status & TMC2209_DRV_STATUS_OTPW) != 0U ?
			   (uint8_t)'1' :
			   (uint8_t)'0');
	response_append_text(&response, " STST=");
	response_append_byte(
		&response, (device->drv_status & TMC2209_DRV_STATUS_STST) != 0U ?
			   (uint8_t)'1' :
			   (uint8_t)'0');
	response_append_text(&response, " STEALTH=");
	response_append_byte(
		&response, (device->drv_status & TMC2209_DRV_STATUS_STEALTH) != 0U ?
			   (uint8_t)'1' :
			   (uint8_t)'0');
	response_append_text(&response, " FATAL=");
	response_append_byte(&response, device->fatal ? (uint8_t)'1' :
						       (uint8_t)'0');
	response_append_text(&response, "\r\n");

	if (!response.overflow) {
		protocol_emit(protocol, response.bytes, response.length);
	}
}

static void protocol_emit_driver_diagnostics(protocol_t *protocol,
					     driver_axis_t axis)
{
	const tmc2209_device_t *device = driver_control_get(axis);
	const tmc2209_diagnostics_t *driver = &device->diagnostics;
	tmc_uart_diagnostics_t uart;
	response_builder_t response = {{0U}, 0U, false};
	const bool is_pan = axis == DRIVER_AXIS_PAN;

	driver_control_get_uart_diagnostics(&uart);
	response_append_text(&response, "OK DRIVER_DIAG ");
	response_append_text(&response, is_pan ? "PAN" : "TILT");
	response_append_text(&response, " ADDR=");
	response_append_u64(&response, device->address);
	response_append_text(&response, " UART_ORE=");
	response_append_u64(&response, uart.overrun_count);
	response_append_text(&response, " UART_NE=");
	response_append_u64(&response, uart.noise_count);
	response_append_text(&response, " UART_FE=");
	response_append_u64(&response, uart.framing_count);
	response_append_text(&response, " UART_PE=");
	response_append_u64(&response, uart.parity_count);
	response_append_text(&response, " UART_LAST_ADDR=");
	if (uart.last_valid) {
		response_append_u64(&response, uart.last_address);
	} else {
		response_append_text(&response, "NONE");
	}
	response_append_text(&response, " UART_LAST_OP=");
	response_append_text(&response,
			     tmc2209_operation_name(uart.last_operation));
	response_append_text(&response, " UART_LAST_REG=");
	response_append_hex8(&response, uart.last_register);
	response_append_text(&response, " UART_FLAGS=");
	response_append_hex8(&response, uart.last_flags);
	response_append_text(&response, " UART_RETRIES=");
	response_append_u64(&response, driver->retry_count);
	response_append_text(&response, " LAST_OP=");
	response_append_text(&response,
			     tmc2209_operation_name(driver->last_operation));
	response_append_text(&response, " LAST_REG=");
	response_append_hex8(&response, driver->last_register);
	response_append_text(&response, " LAST_ATTEMPT=");
	response_append_u64(&response, driver->last_attempt);
	response_append_text(&response, " TRANSPORT=");
	response_append_text(
		&response, tmc2209_error_name(driver->last_transport_error));
	response_append_text(&response, " PARSER=");
	response_append_text(
		&response, tmc2209_error_name(driver->last_parser_error));
	response_append_text(&response, " LAST_CFG_STAGE=");
	response_append_text(
		&response,
		tmc2209_failure_stage_name(driver->last_configuration_stage));
	response_append_text(&response, " LAST_CFG_REG=");
	response_append_hex8(&response, driver->last_configuration_register);
	response_append_text(&response, " LAST_CFG_PHASE=");
	response_append_text(
		&response,
		tmc2209_failure_phase_name(driver->last_configuration_phase));
	response_append_text(&response, " LAST_CFG_ERR=");
	response_append_text(
		&response,
		tmc2209_error_name(driver->last_configuration_error));
	response_append_text(&response, "\r\n");

	if (!response.overflow) {
		protocol_emit(protocol, response.bytes, response.length);
	}
}

static void protocol_handle_driver_configure(protocol_t *protocol)
{
	driver_configure_result_t result;
	const tmc2209_device_t *pan;
	const tmc2209_device_t *tilt;
	response_builder_t response = {{0U}, 0U, false};

	if (!motion_controller_axis_inactive(MOTION_AXIS_PAN)) {
		protocol_emit(protocol, RESPONSE_PAN_ENABLED_ERROR,
			      ARRAY_LENGTH(RESPONSE_PAN_ENABLED_ERROR) - 1U);
		return;
	}
	if (!motion_controller_axis_inactive(MOTION_AXIS_TILT)) {
		protocol_emit(protocol, RESPONSE_TILT_ENABLED_ERROR,
			      ARRAY_LENGTH(RESPONSE_TILT_ENABLED_ERROR) - 1U);
		return;
	}
	result = driver_control_configure();
	pan = driver_control_get(DRIVER_AXIS_PAN);
	tilt = driver_control_get(DRIVER_AXIS_TILT);

	response_append_text(&response, result.pan_configured &&
						 result.tilt_configured ?
					     "OK DRIVERS PAN=" :
					     "ERR DRIVER_CONFIG PAN=");
	response_append_text(&response, tmc2209_state_name(pan->state));
	response_append_text(&response, " TILT=");
	response_append_text(&response, tmc2209_state_name(tilt->state));
	response_append_text(&response, "\r\n");
	if (!response.overflow) {
		protocol_emit(protocol, response.bytes, response.length);
	}
}

static bool token_to_axis(token_t token, motion_axis_t *axis)
{
	if (token_equals(token, "PAN", 3U)) {
		*axis = MOTION_AXIS_PAN;
		return true;
	}
	if (token_equals(token, "TILT", 4U)) {
		*axis = MOTION_AXIS_TILT;
		return true;
	}
	return false;
}

static void protocol_emit_velocity_result(protocol_t *protocol,
					  motion_axis_t axis,
					  motion_velocity_result_t result)
{
	switch (result) {
	case MOTION_VELOCITY_OK:
		protocol_emit(protocol, RESPONSE_OK,
			      ARRAY_LENGTH(RESPONSE_OK) - 1U);
		break;
	case MOTION_VELOCITY_RANGE:
		protocol_emit(protocol, RESPONSE_VELOCITY_RANGE,
			      ARRAY_LENGTH(RESPONSE_VELOCITY_RANGE) - 1U);
		break;
	case MOTION_VELOCITY_DISABLED:
		protocol_emit(protocol,
			      axis == MOTION_AXIS_PAN ?
				      RESPONSE_PAN_DISABLED_ERROR :
				      RESPONSE_TILT_DISABLED_ERROR,
			      axis == MOTION_AXIS_PAN ?
				      ARRAY_LENGTH(RESPONSE_PAN_DISABLED_ERROR) - 1U :
				      ARRAY_LENGTH(RESPONSE_TILT_DISABLED_ERROR) - 1U);
		break;
	case MOTION_VELOCITY_DISABLING:
		protocol_emit(protocol,
			      axis == MOTION_AXIS_PAN ?
				      RESPONSE_PAN_DISABLING_ERROR :
				      RESPONSE_TILT_DISABLING_ERROR,
			      axis == MOTION_AXIS_PAN ?
				      ARRAY_LENGTH(RESPONSE_PAN_DISABLING_ERROR) - 1U :
				      ARRAY_LENGTH(RESPONSE_TILT_DISABLING_ERROR) - 1U);
		break;
	case MOTION_VELOCITY_BUSY:
		protocol_emit(protocol, RESPONSE_TILT_BUSY,
			      ARRAY_LENGTH(RESPONSE_TILT_BUSY) - 1U);
		break;
	case MOTION_VELOCITY_DIRECTION_UNCALIBRATED:
		protocol_emit(protocol, RESPONSE_TILT_DIR_UNCALIBRATED,
			      ARRAY_LENGTH(RESPONSE_TILT_DIR_UNCALIBRATED) - 1U);
		break;
	case MOTION_VELOCITY_NOT_HOMED:
		protocol_emit(protocol, RESPONSE_TILT_NOT_HOMED,
			      ARRAY_LENGTH(RESPONSE_TILT_NOT_HOMED) - 1U);
		break;
	case MOTION_VELOCITY_MIN_LIMIT:
		protocol_emit(protocol, RESPONSE_TILT_MIN_LIMIT,
			      ARRAY_LENGTH(RESPONSE_TILT_MIN_LIMIT) - 1U);
		break;
	}
}

static void protocol_handle_velocity(protocol_t *protocol, token_t *tokens,
				     size_t token_count)
{
	int32_t first_velocity;
	int32_t second_velocity;
	motion_axis_t axis;

	if ((token_count == 3U) && token_to_axis(tokens[1], &axis) &&
	    parse_int32(tokens[2], &first_velocity)) {
		protocol_emit_velocity_result(
			protocol, axis,
			motion_controller_set_velocity(axis, first_velocity));
		return;
	}
	if ((token_count == 4U) && token_equals(tokens[1], "BOTH", 4U) &&
	    parse_int32(tokens[2], &first_velocity) &&
	    parse_int32(tokens[3], &second_velocity)) {
		motion_dual_velocity_result_t result =
			motion_controller_set_both_velocities(first_velocity,
							      second_velocity);

		if (result.applied) {
			protocol_emit(protocol, RESPONSE_OK,
				      ARRAY_LENGTH(RESPONSE_OK) - 1U);
		} else if (result.pan_result != MOTION_VELOCITY_OK) {
			protocol_emit_velocity_result(protocol, MOTION_AXIS_PAN,
						      result.pan_result);
		} else {
			protocol_emit_velocity_result(protocol, MOTION_AXIS_TILT,
						      result.tilt_result);
		}
		return;
	}
	protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
		      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
}

static void protocol_handle_enable(protocol_t *protocol, token_t axis_token)
{
	motion_axis_t axis;
	motion_enable_result_t result;

	if (!token_to_axis(axis_token, &axis)) {
		protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
			      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		return;
	}
	result = motion_controller_enable(axis);
	if (result == MOTION_ENABLE_OK) {
		protocol_emit(protocol,
			      axis == MOTION_AXIS_PAN ? RESPONSE_PAN_ENABLED :
							 RESPONSE_TILT_ENABLED,
			      axis == MOTION_AXIS_PAN ?
				      ARRAY_LENGTH(RESPONSE_PAN_ENABLED) - 1U :
				      ARRAY_LENGTH(RESPONSE_TILT_ENABLED) - 1U);
	} else {
		protocol_emit(protocol,
			      axis == MOTION_AXIS_PAN ?
				      RESPONSE_PAN_DRIVER_NOT_READY :
				      RESPONSE_TILT_DRIVER_NOT_READY,
			      axis == MOTION_AXIS_PAN ?
				      ARRAY_LENGTH(RESPONSE_PAN_DRIVER_NOT_READY) - 1U :
				      ARRAY_LENGTH(RESPONSE_TILT_DRIVER_NOT_READY) - 1U);
	}
}

static void protocol_handle_disable(protocol_t *protocol, token_t axis_token)
{
	motion_axis_t axis;
	axis_disable_result_t result;

	if (!token_to_axis(axis_token, &axis)) {
		protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
			      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		return;
	}
	result = motion_controller_disable(axis);
	if (result == AXIS_DISABLE_PENDING) {
		protocol_emit(protocol,
			      axis == MOTION_AXIS_PAN ? RESPONSE_PAN_DISABLING :
							 RESPONSE_TILT_DISABLING,
			      axis == MOTION_AXIS_PAN ?
				      ARRAY_LENGTH(RESPONSE_PAN_DISABLING) - 1U :
				      ARRAY_LENGTH(RESPONSE_TILT_DISABLING) - 1U);
	} else {
		protocol_emit(protocol,
			      axis == MOTION_AXIS_PAN ? RESPONSE_PAN_DISABLED :
							 RESPONSE_TILT_DISABLED,
			      axis == MOTION_AXIS_PAN ?
				      ARRAY_LENGTH(RESPONSE_PAN_DISABLED) - 1U :
				      ARRAY_LENGTH(RESPONSE_TILT_DISABLED) - 1U);
	}
}

static void protocol_emit_endstop(protocol_t *protocol)
{
	response_builder_t response = {{0U}, 0U, false};
	const bool raw_high = board_tilt_endstop_raw_high();
	const bool triggered = board_tilt_endstop_triggered();

	response_append_text(&response, "OK TILT ENDSTOP=");
	response_append_byte(&response, triggered ? (uint8_t)'1' : (uint8_t)'0');
	response_append_text(&response, " RAW=");
	response_append_byte(&response, raw_high ? (uint8_t)'1' : (uint8_t)'0');
	response_append_text(&response, "\r\n");
	protocol_emit(protocol, response.bytes, response.length);
}

static void protocol_dispatch(protocol_t *protocol, const char *command,
			      size_t command_length)
{
	token_t tokens[MAX_COMMAND_TOKENS];
	bool too_many;
	size_t token_count = split_tokens(command, command_length, tokens,
					&too_many);

	if (too_many || (token_count == 0U)) {
		protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
			      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		return;
	}
	if (token_equals(tokens[0], "PING", 4U)) {
		if (token_count == 1U) {
			protocol_emit(protocol, RESPONSE_PONG,
				      ARRAY_LENGTH(RESPONSE_PONG) - 1U);
		} else {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		}
	} else if (token_equals(tokens[0], "INFO", 4U)) {
		if (token_count == 1U) {
			protocol_emit(protocol, RESPONSE_INFO,
				      ARRAY_LENGTH(RESPONSE_INFO) - 1U);
		} else {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		}
	} else if (token_equals(tokens[0], "ENABLE", 6U)) {
		if (token_count == 2U) {
			protocol_handle_enable(protocol, tokens[1]);
		} else {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		}
	} else if (token_equals(tokens[0], "DISABLE", 7U)) {
		if (token_count != 2U) {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		} else {
			protocol_handle_disable(protocol, tokens[1]);
		}
	} else if (token_equals(tokens[0], "VEL", 3U)) {
		protocol_handle_velocity(protocol, tokens, token_count);
	} else if (token_equals(tokens[0], "STOP", 4U)) {
		if (token_count == 1U) {
			motion_controller_stop_all();
			protocol_emit(protocol, RESPONSE_OK,
				      ARRAY_LENGTH(RESPONSE_OK) - 1U);
		} else {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		}
	} else if (token_equals(tokens[0], "STATE?", 6U)) {
		if (token_count == 1U) {
			protocol_emit_state(protocol, MOTION_AXIS_PAN);
		} else if ((token_count == 2U) &&
			   token_equals(tokens[1], "PAN", 3U)) {
			protocol_emit_state(protocol, MOTION_AXIS_PAN);
		} else if ((token_count == 2U) &&
			   token_equals(tokens[1], "TILT", 4U)) {
			protocol_emit_state(protocol, MOTION_AXIS_TILT);
		} else {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		}
	} else if (token_equals(tokens[0], "ENDSTOP?", 8U)) {
		if ((token_count == 2U) &&
		    token_equals(tokens[1], "TILT", 4U)) {
			protocol_emit_endstop(protocol);
		} else {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		}
	} else if (token_equals(tokens[0], "HOME", 4U)) {
		if ((token_count == 2U) &&
		    token_equals(tokens[1], "TILT", 4U)) {
			switch (motion_controller_home_tilt()) {
			case MOTION_HOME_OK: {
				static const uint8_t response[] =
					"OK TILT HOMING\r\n";
				protocol_emit(protocol, response,
					      ARRAY_LENGTH(response) - 1U);
				break;
			}
			case MOTION_HOME_DRIVER_NOT_READY:
				protocol_emit(protocol,
					      RESPONSE_TILT_DRIVER_NOT_READY,
					      ARRAY_LENGTH(
						      RESPONSE_TILT_DRIVER_NOT_READY) -
						      1U);
				break;
			case MOTION_HOME_DIRECTION_UNCALIBRATED:
				protocol_emit(protocol,
					      RESPONSE_TILT_DIR_UNCALIBRATED,
					      ARRAY_LENGTH(
						      RESPONSE_TILT_DIR_UNCALIBRATED) -
						      1U);
				break;
			case MOTION_HOME_BUSY:
				protocol_emit(protocol, RESPONSE_TILT_BUSY,
					      ARRAY_LENGTH(RESPONSE_TILT_BUSY) - 1U);
				break;
			}
		} else {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		}
	} else if (token_equals(tokens[0], "JOG", 3U)) {
		int32_t steps;

		if ((token_count != 3U) ||
		    !token_equals(tokens[1], "TILT", 4U) ||
		    !parse_int32(tokens[2], &steps)) {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		} else {
			switch (motion_controller_jog_tilt(steps)) {
			case MOTION_JOG_OK: {
				static const uint8_t response[] =
					"OK TILT JOGGING\r\n";
				protocol_emit(protocol, response,
					      ARRAY_LENGTH(response) - 1U);
				break;
			}
			case MOTION_JOG_RANGE:
				protocol_emit(protocol, RESPONSE_JOG_RANGE,
					      ARRAY_LENGTH(RESPONSE_JOG_RANGE) - 1U);
				break;
			case MOTION_JOG_DISABLED:
				protocol_emit(protocol, RESPONSE_TILT_DISABLED_ERROR,
					      ARRAY_LENGTH(
						      RESPONSE_TILT_DISABLED_ERROR) -
						      1U);
				break;
			case MOTION_JOG_NOT_HOMED:
				protocol_emit(protocol, RESPONSE_TILT_NOT_HOMED,
					      ARRAY_LENGTH(RESPONSE_TILT_NOT_HOMED) - 1U);
				break;
			case MOTION_JOG_BUSY:
				protocol_emit(protocol, RESPONSE_TILT_BUSY,
					      ARRAY_LENGTH(RESPONSE_TILT_BUSY) - 1U);
				break;
			case MOTION_JOG_MIN_LIMIT:
				protocol_emit(protocol, RESPONSE_TILT_MIN_LIMIT,
					      ARRAY_LENGTH(RESPONSE_TILT_MIN_LIMIT) - 1U);
				break;
			}
		}
	} else if (token_equals(tokens[0], "DIR-CHECK", 9U)) {
		if ((token_count == 3U) &&
		    token_equals(tokens[1], "TILT", 4U) &&
		    (token_equals(tokens[2], "HIGH", 4U) ||
		     token_equals(tokens[2], "LOW", 3U))) {
			const bool high = token_equals(tokens[2], "HIGH", 4U);
			motion_direction_check_result_t result =
				motion_controller_direction_check_tilt(high);

			if (result == MOTION_DIRECTION_CHECK_OK) {
				static const uint8_t response[] =
					"OK TILT DIR_CHECKING\r\n";
				protocol_emit(protocol, response,
					      ARRAY_LENGTH(response) - 1U);
			} else if (result ==
				   MOTION_DIRECTION_CHECK_DRIVER_NOT_READY) {
				protocol_emit(protocol,
					      RESPONSE_TILT_DRIVER_NOT_READY,
					      ARRAY_LENGTH(
						      RESPONSE_TILT_DRIVER_NOT_READY) -
						      1U);
			} else if (result ==
				   MOTION_DIRECTION_CHECK_ENDSTOP_ACTIVE) {
				static const uint8_t response[] =
					"ERR TILT_ENDSTOP_ACTIVE\r\n";
				protocol_emit(protocol, response,
					      ARRAY_LENGTH(response) - 1U);
			} else if (result ==
				   MOTION_DIRECTION_CHECK_ALREADY_CALIBRATED) {
				static const uint8_t response[] =
					"ERR TILT_DIR_ALREADY_CALIBRATED\r\n";
				protocol_emit(protocol, response,
					      ARRAY_LENGTH(response) - 1U);
			} else {
				protocol_emit(protocol, RESPONSE_TILT_BUSY,
					      ARRAY_LENGTH(RESPONSE_TILT_BUSY) - 1U);
			}
		} else {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		}
	} else if (token_equals(tokens[0], "DRIVER?", 7U)) {
		if ((token_count == 2U) &&
		    token_equals(tokens[1], "PAN", 3U)) {
			protocol_emit_driver(protocol, DRIVER_AXIS_PAN);
		} else if ((token_count == 2U) &&
			   token_equals(tokens[1], "TILT", 4U)) {
			protocol_emit_driver(protocol, DRIVER_AXIS_TILT);
		} else {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		}
	} else if (token_equals(tokens[0], "DRIVER-DIAG?", 12U)) {
		if ((token_count == 2U) &&
		    token_equals(tokens[1], "PAN", 3U)) {
			protocol_emit_driver_diagnostics(protocol, DRIVER_AXIS_PAN);
		} else if ((token_count == 2U) &&
			   token_equals(tokens[1], "TILT", 4U)) {
			protocol_emit_driver_diagnostics(protocol, DRIVER_AXIS_TILT);
		} else {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		}
	} else if (token_equals(tokens[0], "DRIVER", 6U)) {
		if ((token_count == 2U) &&
		    token_equals(tokens[1], "CONFIGURE", 9U)) {
			protocol_handle_driver_configure(protocol);
		} else {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		}
	} else {
		protocol_emit(protocol, RESPONSE_UNKNOWN,
			      ARRAY_LENGTH(RESPONSE_UNKNOWN) - 1U);
	}
}

static void protocol_finish_line(protocol_t *protocol)
{
	size_t start = 0U;
	size_t end = protocol->line_length;

	if (protocol->discarding_oversized_line) {
		protocol_emit(protocol, RESPONSE_TOO_LONG,
			      ARRAY_LENGTH(RESPONSE_TOO_LONG) - 1U);
		protocol->discarding_oversized_line = false;
		protocol->line_length = 0U;
		return;
	}

	while ((start < end) &&
	       ((protocol->line[start] == ' ') || (protocol->line[start] == '\t'))) {
		++start;
	}
	while ((end > start) &&
	       ((protocol->line[end - 1U] == ' ') ||
		(protocol->line[end - 1U] == '\t'))) {
		--end;
	}

	if (start == end) {
		protocol_emit(protocol, RESPONSE_EMPTY,
			      ARRAY_LENGTH(RESPONSE_EMPTY) - 1U);
	} else {
		protocol_dispatch(protocol, &protocol->line[start], end - start);
	}
	protocol->line_length = 0U;
}

void protocol_init(protocol_t *protocol, protocol_write_fn write,
		   void *write_context)
{
	protocol->line_length = 0U;
	protocol->discarding_oversized_line = false;
	protocol->previous_was_carriage_return = false;
	protocol->write = write;
	protocol->write_context = write_context;
}

void protocol_receive(protocol_t *protocol, const uint8_t *data, size_t length)
{
	size_t index;

	for (index = 0U; index < length; ++index) {
		const uint8_t byte = data[index];

		if (byte == (uint8_t)'\r') {
			protocol_finish_line(protocol);
			protocol->previous_was_carriage_return = true;
			continue;
		}
		if (byte == (uint8_t)'\n') {
			if (protocol->previous_was_carriage_return) {
				protocol->previous_was_carriage_return = false;
			} else {
				protocol_finish_line(protocol);
			}
			continue;
		}

		protocol->previous_was_carriage_return = false;
		if (protocol->discarding_oversized_line) {
			continue;
		}
		if (protocol->line_length >= (PROTOCOL_LINE_CAPACITY - 1U)) {
			protocol->discarding_oversized_line = true;
			continue;
		}
		protocol->line[protocol->line_length] = (char)byte;
		++protocol->line_length;
	}
}
