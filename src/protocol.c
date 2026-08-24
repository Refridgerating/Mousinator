#include "protocol.h"

#include "driver_control.h"
#include "pan_controller.h"

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
	"OK SENTRY-MCU 0.3.0 SKR_MINI_E3_V2\r\n";
static const uint8_t RESPONSE_PAN_ENABLED[] = "OK PAN ENABLED\r\n";
static const uint8_t RESPONSE_PAN_DISABLED[] = "OK PAN DISABLED\r\n";
static const uint8_t RESPONSE_PAN_DISABLING[] = "OK PAN DISABLING\r\n";
static const uint8_t RESPONSE_EMPTY[] = "ERR EMPTY_COMMAND\r\n";
static const uint8_t RESPONSE_UNKNOWN[] = "ERR UNKNOWN_COMMAND\r\n";
static const uint8_t RESPONSE_TOO_LONG[] = "ERR COMMAND_TOO_LONG\r\n";
static const uint8_t RESPONSE_BAD_ARGUMENT[] = "ERR BAD_ARGUMENT\r\n";
static const uint8_t RESPONSE_VELOCITY_RANGE[] = "ERR VELOCITY_RANGE\r\n";
static const uint8_t RESPONSE_PAN_DISABLED_ERROR[] = "ERR PAN_DISABLED\r\n";
static const uint8_t RESPONSE_PAN_DISABLING_ERROR[] = "ERR PAN_DISABLING\r\n";
static const uint8_t RESPONSE_PAN_DRIVER_NOT_READY[] =
	"ERR PAN_DRIVER_NOT_READY\r\n";
static const uint8_t RESPONSE_PAN_ENABLED_ERROR[] = "ERR PAN_ENABLED\r\n";

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

static void protocol_emit(protocol_t *protocol, const uint8_t *response,
			  size_t response_length)
{
	(void)protocol->write(response, response_length, protocol->write_context);
}

static void protocol_emit_state(protocol_t *protocol)
{
	pan_controller_snapshot_t snapshot;
	response_builder_t response = {{0U}, 0U, false};

	pan_controller_get_snapshot(&snapshot);
	response_append_text(&response, "OK PAN ENABLED=");
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
	response_append_text(&response, "\r\n");

	if (!response.overflow) {
		protocol_emit(protocol, response.bytes, response.length);
	}
}

static void protocol_emit_driver(protocol_t *protocol, driver_axis_t axis)
{
	const tmc2209_device_t *device;
	pan_controller_snapshot_t pan_snapshot;
	response_builder_t response = {{0U}, 0U, false};
	const bool is_pan = axis == DRIVER_AXIS_PAN;
	bool present;

	driver_control_refresh(axis);
	device = driver_control_get(axis);
	if (is_pan) {
		if (device->fatal) {
			pan_controller_get_snapshot(&pan_snapshot);
			if (pan_snapshot.enabled) {
				pan_controller_stop();
				(void)pan_controller_disable();
			}
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
		&response, device->state == TMC2209_STATE_CONFIGURED ?
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

static void protocol_handle_driver_configure(protocol_t *protocol)
{
	pan_controller_snapshot_t snapshot;
	driver_configure_result_t result;
	const tmc2209_device_t *pan;
	const tmc2209_device_t *tilt;
	response_builder_t response = {{0U}, 0U, false};

	pan_controller_get_snapshot(&snapshot);
	if (snapshot.enabled || snapshot.moving || snapshot.disabling) {
		protocol_emit(protocol, RESPONSE_PAN_ENABLED_ERROR,
			      ARRAY_LENGTH(RESPONSE_PAN_ENABLED_ERROR) - 1U);
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

static void protocol_handle_velocity(protocol_t *protocol, token_t *tokens,
				     size_t token_count)
{
	int32_t velocity;
	pan_velocity_result_t result;

	if ((token_count != 3U) ||
	    !token_equals(tokens[1], "PAN", 3U) ||
	    !parse_int32(tokens[2], &velocity)) {
		protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
			      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		return;
	}
	if ((velocity > PAN_MAX_ABSOLUTE_VELOCITY_STEPS_S) ||
	    (velocity < -PAN_MAX_ABSOLUTE_VELOCITY_STEPS_S)) {
		protocol_emit(protocol, RESPONSE_VELOCITY_RANGE,
			      ARRAY_LENGTH(RESPONSE_VELOCITY_RANGE) - 1U);
		return;
	}

	result = pan_controller_set_velocity(velocity);
	switch (result) {
	case PAN_VELOCITY_OK:
		protocol_emit(protocol, RESPONSE_OK,
			      ARRAY_LENGTH(RESPONSE_OK) - 1U);
		break;
	case PAN_VELOCITY_DISABLED:
		protocol_emit(protocol, RESPONSE_PAN_DISABLED_ERROR,
			      ARRAY_LENGTH(RESPONSE_PAN_DISABLED_ERROR) - 1U);
		break;
	case PAN_VELOCITY_DISABLING:
		protocol_emit(protocol, RESPONSE_PAN_DISABLING_ERROR,
			      ARRAY_LENGTH(RESPONSE_PAN_DISABLING_ERROR) - 1U);
		break;
	case PAN_VELOCITY_RANGE:
		protocol_emit(protocol, RESPONSE_VELOCITY_RANGE,
			      ARRAY_LENGTH(RESPONSE_VELOCITY_RANGE) - 1U);
		break;
	}
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
		if ((token_count == 2U) &&
		    token_equals(tokens[1], "PAN", 3U)) {
			if (pan_controller_enable() == PAN_ENABLE_OK) {
				protocol_emit(
					protocol, RESPONSE_PAN_ENABLED,
					ARRAY_LENGTH(RESPONSE_PAN_ENABLED) - 1U);
			} else {
				protocol_emit(
					protocol, RESPONSE_PAN_DRIVER_NOT_READY,
					ARRAY_LENGTH(RESPONSE_PAN_DRIVER_NOT_READY) -
						1U);
			}
		} else {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		}
	} else if (token_equals(tokens[0], "DISABLE", 7U)) {
		if ((token_count != 2U) ||
		    !token_equals(tokens[1], "PAN", 3U)) {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		} else if (pan_controller_disable() == PAN_DISABLE_PENDING) {
			protocol_emit(protocol, RESPONSE_PAN_DISABLING,
				      ARRAY_LENGTH(RESPONSE_PAN_DISABLING) - 1U);
		} else {
			protocol_emit(protocol, RESPONSE_PAN_DISABLED,
				      ARRAY_LENGTH(RESPONSE_PAN_DISABLED) - 1U);
		}
	} else if (token_equals(tokens[0], "VEL", 3U)) {
		protocol_handle_velocity(protocol, tokens, token_count);
	} else if (token_equals(tokens[0], "STOP", 4U)) {
		if (token_count == 1U) {
			pan_controller_stop();
			protocol_emit(protocol, RESPONSE_OK,
				      ARRAY_LENGTH(RESPONSE_OK) - 1U);
		} else {
			protocol_emit(protocol, RESPONSE_BAD_ARGUMENT,
				      ARRAY_LENGTH(RESPONSE_BAD_ARGUMENT) - 1U);
		}
	} else if (token_equals(tokens[0], "STATE?", 6U)) {
		if (token_count == 1U) {
			protocol_emit_state(protocol);
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
