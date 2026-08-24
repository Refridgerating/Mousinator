#include "protocol.h"
#include "pan_controller.h"

#include <stdbool.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define CAPTURE_CAPACITY 2048U

typedef struct {
	uint8_t bytes[CAPTURE_CAPACITY];
	size_t length;
} capture_t;

static pan_controller_snapshot_t mock_snapshot;
static pan_disable_result_t mock_disable_result;
static pan_velocity_result_t mock_velocity_result;
static int32_t mock_last_velocity;

static void mock_reset(void)
{
	mock_snapshot.position_steps = 0;
	mock_snapshot.current_velocity_steps_s = 0;
	mock_snapshot.target_velocity_steps_s = 0;
	mock_snapshot.enabled = false;
	mock_snapshot.moving = false;
	mock_snapshot.disabling = false;
	mock_snapshot.motion_timeout = false;
	mock_disable_result = PAN_DISABLE_COMPLETE;
	mock_velocity_result = PAN_VELOCITY_OK;
	mock_last_velocity = 0;
}

void pan_controller_init(void)
{
}

void pan_controller_enable(void)
{
	mock_snapshot.enabled = true;
}

pan_disable_result_t pan_controller_disable(void)
{
	return mock_disable_result;
}

pan_velocity_result_t pan_controller_set_velocity(int32_t velocity_steps_s)
{
	mock_last_velocity = velocity_steps_s;
	return mock_velocity_result;
}

void pan_controller_stop(void)
{
	mock_snapshot.target_velocity_steps_s = 0;
}

void pan_controller_get_snapshot(pan_controller_snapshot_t *snapshot)
{
	*snapshot = mock_snapshot;
}

static bool capture_write(const uint8_t *data, size_t length, void *context)
{
	capture_t *capture = context;
	size_t index;

	if (length > (CAPTURE_CAPACITY - capture->length)) {
		return false;
	}

	for (index = 0U; index < length; ++index) {
		capture->bytes[capture->length + index] = data[index];
	}
	capture->length += length;
	return true;
}

static bool bytes_equal(const uint8_t *actual, size_t actual_length,
			const char *expected, size_t expected_length)
{
	size_t index;

	if (actual_length != expected_length) {
		return false;
	}
	for (index = 0U; index < expected_length; ++index) {
		if (actual[index] != (uint8_t)expected[index]) {
			return false;
		}
	}
	return true;
}

static int check_case(const char *name, const uint8_t *input, size_t input_length,
		      const char *expected, size_t expected_length)
{
	protocol_t protocol;
	capture_t capture = {{0U}, 0U};

	mock_reset();
	protocol_init(&protocol, capture_write, &capture);
	protocol_receive(&protocol, input, input_length);
	if (!bytes_equal(capture.bytes, capture.length, expected, expected_length)) {
		(void)fprintf(stderr, "FAIL: %s\n", name);
		return 1;
	}
	return 0;
}

static int check_fragmented(void)
{
	static const uint8_t first[] = "PI";
	static const uint8_t second[] = "NG\r";
	static const char expected[] = "OK PONG\r\n";
	protocol_t protocol;
	capture_t capture = {{0U}, 0U};

	mock_reset();
	protocol_init(&protocol, capture_write, &capture);
	protocol_receive(&protocol, first, sizeof(first) - 1U);
	protocol_receive(&protocol, second, sizeof(second) - 1U);
	if (!bytes_equal(capture.bytes, capture.length, expected,
			 sizeof(expected) - 1U)) {
		(void)fprintf(stderr, "FAIL: fragmented input\n");
		return 1;
	}
	return 0;
}

static int check_motion_commands(void)
{
	static const uint8_t commands[] =
		"ENABLE PAN\nVEL PAN +200\nSTOP\nDISABLE PAN\nSTATE?\n";
	static const char expected[] =
		"OK PAN ENABLED\r\n"
		"OK\r\n"
		"OK\r\n"
		"OK PAN DISABLED\r\n"
		"OK PAN ENABLED=1 DISABLING=0 MOVING=0 POS=0 VEL=0 TARGET=0 TIMEOUT=0\r\n";
	protocol_t protocol;
	capture_t capture = {{0U}, 0U};

	mock_reset();
	protocol_init(&protocol, capture_write, &capture);
	protocol_receive(&protocol, commands, sizeof(commands) - 1U);
	if (!bytes_equal(capture.bytes, capture.length, expected,
			 sizeof(expected) - 1U) ||
	    (mock_last_velocity != 200)) {
		(void)fprintf(stderr, "FAIL: motion commands\n");
		return 1;
	}
	return 0;
}

static int check_velocity_errors(void)
{
	static const uint8_t commands[] =
		"VEL PAN 2147483648\n"
		"VEL PAN -2147483649\n"
		"VEL PAN +\n"
		"VEL TILT 1\n"
		"VEL PAN 1001\n"
		"VEL PAN -1001\n";
	static const char expected[] =
		"ERR BAD_ARGUMENT\r\n"
		"ERR BAD_ARGUMENT\r\n"
		"ERR BAD_ARGUMENT\r\n"
		"ERR BAD_ARGUMENT\r\n"
		"ERR VELOCITY_RANGE\r\n"
		"ERR VELOCITY_RANGE\r\n";
	int failures;

	failures = check_case("velocity errors", commands, sizeof(commands) - 1U,
			      expected, sizeof(expected) - 1U);
	return failures;
}

static int check_velocity_state_errors(void)
{
	static const uint8_t velocity[] = "VEL PAN 100\n";
	static const char disabled[] = "ERR PAN_DISABLED\r\n";
	static const char disabling[] = "ERR PAN_DISABLING\r\n";
	protocol_t protocol;
	capture_t capture = {{0U}, 0U};
	int failures = 0;

	mock_reset();
	mock_velocity_result = PAN_VELOCITY_DISABLED;
	protocol_init(&protocol, capture_write, &capture);
	protocol_receive(&protocol, velocity, sizeof(velocity) - 1U);
	if (!bytes_equal(capture.bytes, capture.length, disabled,
			 sizeof(disabled) - 1U)) {
		(void)fprintf(stderr, "FAIL: disabled velocity\n");
		++failures;
	}

	capture.length = 0U;
	mock_velocity_result = PAN_VELOCITY_DISABLING;
	protocol_receive(&protocol, velocity, sizeof(velocity) - 1U);
	if (!bytes_equal(capture.bytes, capture.length, disabling,
			 sizeof(disabling) - 1U)) {
		(void)fprintf(stderr, "FAIL: disabling velocity\n");
		++failures;
	}
	return failures;
}

static int check_pending_disable(void)
{
	static const uint8_t command[] = "DISABLE PAN\n";
	static const char expected[] = "OK PAN DISABLING\r\n";
	protocol_t protocol;
	capture_t capture = {{0U}, 0U};

	mock_reset();
	mock_disable_result = PAN_DISABLE_PENDING;
	protocol_init(&protocol, capture_write, &capture);
	protocol_receive(&protocol, command, sizeof(command) - 1U);
	if (!bytes_equal(capture.bytes, capture.length, expected,
			 sizeof(expected) - 1U)) {
		(void)fprintf(stderr, "FAIL: pending disable\n");
		return 1;
	}
	return 0;
}

static int check_state_extremes(void)
{
	static const uint8_t command[] = "STATE?\n";
	static const char expected[] =
		"OK PAN ENABLED=1 DISABLING=1 MOVING=1 POS=-9223372036854775808 "
		"VEL=-1000 TARGET=1000 TIMEOUT=1\r\n";
	protocol_t protocol;
	capture_t capture = {{0U}, 0U};

	mock_reset();
	mock_snapshot.position_steps = INT64_MIN;
	mock_snapshot.current_velocity_steps_s = -1000;
	mock_snapshot.target_velocity_steps_s = 1000;
	mock_snapshot.enabled = true;
	mock_snapshot.moving = true;
	mock_snapshot.disabling = true;
	mock_snapshot.motion_timeout = true;
	protocol_init(&protocol, capture_write, &capture);
	protocol_receive(&protocol, command, sizeof(command) - 1U);
	if (!bytes_equal(capture.bytes, capture.length, expected,
			 sizeof(expected) - 1U)) {
		(void)fprintf(stderr, "FAIL: state extremes\n");
		return 1;
	}
	return 0;
}

static int check_boundaries(void)
{
	uint8_t exact[PROTOCOL_LINE_CAPACITY];
	uint8_t oversized[PROTOCOL_LINE_CAPACITY + 1U];
	size_t index;
	int failures = 0;
	static const char unknown[] = "ERR UNKNOWN_COMMAND\r\n";
	static const char too_long[] = "ERR COMMAND_TOO_LONG\r\n";

	for (index = 0U; index < (PROTOCOL_LINE_CAPACITY - 1U); ++index) {
		exact[index] = (uint8_t)'A';
	}
	exact[PROTOCOL_LINE_CAPACITY - 1U] = (uint8_t)'\n';

	for (index = 0U; index < PROTOCOL_LINE_CAPACITY; ++index) {
		oversized[index] = (uint8_t)'A';
	}
	oversized[PROTOCOL_LINE_CAPACITY] = (uint8_t)'\n';

	failures += check_case("63-byte command", exact, sizeof(exact), unknown,
			       sizeof(unknown) - 1U);
	failures += check_case("64-byte command", oversized, sizeof(oversized),
			       too_long, sizeof(too_long) - 1U);
	return failures;
}

int main(void)
{
	static const uint8_t line_endings[] = "PING\rINFO\nPING\r\n";
	static const char line_endings_expected[] =
		"OK PONG\r\n"
		"OK SENTRY-MCU 0.2.1 SKR_MINI_E3_V2\r\n"
		"OK PONG\r\n";
	static const uint8_t whitespace[] = " \tPING\t \n";
	static const char pong[] = "OK PONG\r\n";
	static const uint8_t empty[] = "\n";
	static const char empty_response[] = "ERR EMPTY_COMMAND\r\n";
	static const uint8_t unknown[] = "ping\nWAT 1\r\n";
	static const char unknown_response[] =
		"ERR UNKNOWN_COMMAND\r\nERR UNKNOWN_COMMAND\r\n";
	static const uint8_t recovered[] =
		"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\nPING\n";
	static const char recovered_response[] =
		"ERR COMMAND_TOO_LONG\r\nOK PONG\r\n";
	int failures = 0;

	failures += check_case("CR LF CRLF", line_endings,
			       sizeof(line_endings) - 1U, line_endings_expected,
			       sizeof(line_endings_expected) - 1U);
	failures += check_case("trim whitespace", whitespace,
			       sizeof(whitespace) - 1U, pong, sizeof(pong) - 1U);
	failures += check_case("empty", empty, sizeof(empty) - 1U,
			       empty_response, sizeof(empty_response) - 1U);
	failures += check_case("case and malformed", unknown,
			       sizeof(unknown) - 1U, unknown_response,
			       sizeof(unknown_response) - 1U);
	failures += check_case("oversize recovery", recovered,
			       sizeof(recovered) - 1U, recovered_response,
			       sizeof(recovered_response) - 1U);
	failures += check_fragmented();
	failures += check_boundaries();
	failures += check_motion_commands();
	failures += check_velocity_errors();
	failures += check_velocity_state_errors();
	failures += check_pending_disable();
	failures += check_state_extremes();

	if (failures == 0) {
		(void)puts("protocol tests passed");
	}
	return failures;
}
