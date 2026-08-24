#include "tmc2209.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
	uint32_t register_value;
	uint8_t errors_before_success;
	uint8_t calls;
	uint8_t recoveries;
	bool include_echo;
} fake_transport_t;

typedef struct {
	uint32_t registers[128];
	uint8_t ifcnt;
	uint8_t corrupt_read_register;
	uint32_t corrupt_read_mask;
	size_t write_count;
} register_transport_t;

static int check(bool condition, const char *name)
{
	if (!condition) {
		(void)fprintf(stderr, "FAIL: %s\n", name);
		return 1;
	}
	return 0;
}

static tmc2209_error_t fake_exchange(
	void *context, const uint8_t *transmit, size_t transmit_length,
	uint8_t *receive, size_t receive_capacity, size_t *receive_length)
{
	fake_transport_t *fake = context;
	uint8_t reply[TMC2209_REPLY_DATAGRAM_SIZE];
	size_t offset = 0U;
	size_t index;

	++fake->calls;
	if (fake->calls <= fake->errors_before_success) {
		return TMC2209_ERROR_TIMEOUT;
	}
	if (transmit_length != TMC2209_READ_DATAGRAM_SIZE) {
		if (receive_capacity < transmit_length) {
			return TMC2209_ERROR_CAPTURE_OVERFLOW;
		}
		for (index = 0U; index < transmit_length; ++index) {
			receive[index] = transmit[index];
		}
		*receive_length = transmit_length;
		return TMC2209_ERROR_NONE;
	}

	if (fake->include_echo) {
		if (receive_capacity < transmit_length) {
			return TMC2209_ERROR_CAPTURE_OVERFLOW;
		}
		for (index = 0U; index < transmit_length; ++index) {
			receive[index] = transmit[index];
		}
		offset = transmit_length;
	}
	if ((receive_capacity - offset) < TMC2209_REPLY_DATAGRAM_SIZE) {
		return TMC2209_ERROR_CAPTURE_OVERFLOW;
	}
	reply[0] = TMC2209_SYNC_BYTE;
	reply[1] = TMC2209_MASTER_ADDRESS;
	reply[2] = transmit[2];
	reply[3] = (uint8_t)(fake->register_value >> 24);
	reply[4] = (uint8_t)(fake->register_value >> 16);
	reply[5] = (uint8_t)(fake->register_value >> 8);
	reply[6] = (uint8_t)fake->register_value;
	reply[7] = tmc2209_crc(reply, TMC2209_REPLY_DATAGRAM_SIZE - 1U);
	for (index = 0U; index < TMC2209_REPLY_DATAGRAM_SIZE; ++index) {
		receive[offset + index] = reply[index];
	}
	*receive_length = offset + TMC2209_REPLY_DATAGRAM_SIZE;
	return TMC2209_ERROR_NONE;
}

static void fake_recover(void *context)
{
	fake_transport_t *fake = context;

	++fake->recoveries;
}

static tmc2209_error_t register_exchange(
	void *context, const uint8_t *transmit, size_t transmit_length,
	uint8_t *receive, size_t receive_capacity, size_t *receive_length)
{
	register_transport_t *fake = context;
	const uint8_t register_address = (uint8_t)(transmit[2] & 0x7FU);
	size_t index;

	if (receive_capacity < transmit_length) {
		return TMC2209_ERROR_CAPTURE_OVERFLOW;
	}
	for (index = 0U; index < transmit_length; ++index) {
		receive[index] = transmit[index];
	}
	if (transmit_length == TMC2209_WRITE_DATAGRAM_SIZE) {
		const uint32_t value = ((uint32_t)transmit[3] << 24) |
			((uint32_t)transmit[4] << 16) |
			((uint32_t)transmit[5] << 8) | (uint32_t)transmit[6];

		if (register_address == TMC2209_REGISTER_GSTAT) {
			/* GSTAT is read-and-write-one-to-clear. */
			fake->registers[register_address] &= ~value;
		} else {
			fake->registers[register_address] = value;
		}
		fake->ifcnt = (uint8_t)(fake->ifcnt + 1U);
		++fake->write_count;
		*receive_length = transmit_length;
		return TMC2209_ERROR_NONE;
	}
	if ((receive_capacity - transmit_length) < TMC2209_REPLY_DATAGRAM_SIZE) {
		return TMC2209_ERROR_CAPTURE_OVERFLOW;
	}
	{
		uint32_t value = register_address == TMC2209_REGISTER_IFCNT ?
			(uint32_t)fake->ifcnt : fake->registers[register_address];
		uint8_t *reply = &receive[transmit_length];

		if ((fake->corrupt_read_mask != 0U) &&
		    (register_address == fake->corrupt_read_register)) {
			value ^= fake->corrupt_read_mask;
		}
		reply[0] = TMC2209_SYNC_BYTE;
		reply[1] = TMC2209_MASTER_ADDRESS;
		reply[2] = register_address;
		reply[3] = (uint8_t)(value >> 24);
		reply[4] = (uint8_t)(value >> 16);
		reply[5] = (uint8_t)(value >> 8);
		reply[6] = (uint8_t)value;
		reply[7] = tmc2209_crc(reply, TMC2209_REPLY_DATAGRAM_SIZE - 1U);
	}
	*receive_length = transmit_length + TMC2209_REPLY_DATAGRAM_SIZE;
	return TMC2209_ERROR_NONE;
}

static int test_crc_and_datagrams(void)
{
	static const uint8_t read_prefix[] = {0x05U, 0x02U, 0x06U};
	static const uint8_t reply_prefix[] = {
		0x05U, 0xFFU, 0x06U, 0x21U, 0x00U, 0x00U, 0x00U};
	static const uint8_t expected_read[] = {0x05U, 0x02U, 0x06U, 0x34U};
	static const uint8_t expected_write[] = {
		0x05U, 0x02U, 0x80U, 0x00U, 0x00U, 0x01U, 0xC0U, 0x80U};
	uint8_t read_datagram[TMC2209_READ_DATAGRAM_SIZE];
	uint8_t write_datagram[TMC2209_WRITE_DATAGRAM_SIZE];
	int failures = 0;
	size_t index;

	failures += check(tmc2209_crc(read_prefix, sizeof(read_prefix)) == 0x34U,
			  "CRC read golden vector");
	failures += check(tmc2209_crc(reply_prefix, sizeof(reply_prefix)) == 0x41U,
			  "CRC reply golden vector");
	failures += check(tmc2209_build_read_datagram(2U, TMC2209_REGISTER_IOIN,
						      read_datagram,
						      sizeof(read_datagram)),
			  "build read datagram");
	failures += check(tmc2209_build_write_datagram(
				  2U, TMC2209_REGISTER_GCONF, 0x000001C0UL,
				  write_datagram, sizeof(write_datagram)),
			  "build write datagram");
	for (index = 0U; index < sizeof(read_datagram); ++index) {
		failures += check(read_datagram[index] == expected_read[index],
				  "read datagram byte order");
	}
	for (index = 0U; index < sizeof(write_datagram); ++index) {
		failures += check(write_datagram[index] == expected_write[index],
				  "write datagram byte order");
	}
	return failures;
}

static int test_response_validation(void)
{
	uint8_t request[TMC2209_READ_DATAGRAM_SIZE];
	uint8_t reply[TMC2209_REPLY_DATAGRAM_SIZE] = {
		0x05U, 0xFFU, 0x06U, 0x21U, 0x00U, 0x00U, 0x00U, 0x41U};
	uint8_t echoed[TMC2209_READ_DATAGRAM_SIZE + TMC2209_REPLY_DATAGRAM_SIZE];
	uint32_t value = 0U;
	int failures = 0;
	size_t index;

	(void)tmc2209_build_read_datagram(2U, TMC2209_REGISTER_IOIN, request,
					  sizeof(request));
	for (index = 0U; index < sizeof(request); ++index) {
		echoed[index] = request[index];
	}
	for (index = 0U; index < sizeof(reply); ++index) {
		echoed[sizeof(request) + index] = reply[index];
	}
	failures += check(tmc2209_parse_read_response(
				  request, sizeof(request), echoed, sizeof(echoed),
				  TMC2209_REGISTER_IOIN, &value) ==
				  TMC2209_ERROR_NONE &&
				  value == 0x21000000UL,
			  "accept exact echo and reply");
	reply[7] ^= 1U;
	failures += check(tmc2209_parse_read_response(
				  request, sizeof(request), reply, sizeof(reply),
				  TMC2209_REGISTER_IOIN, &value) == TMC2209_ERROR_CRC,
			  "reject response CRC");
	reply[7] ^= 1U;
	reply[1] = 0x02U;
	failures += check(tmc2209_parse_read_response(
				  request, sizeof(request), reply, sizeof(reply),
				  TMC2209_REGISTER_IOIN, &value) ==
				  TMC2209_ERROR_ADDRESS,
			  "reject wrong master address");
	reply[1] = TMC2209_MASTER_ADDRESS;
	reply[2] = TMC2209_REGISTER_GSTAT;
	failures += check(tmc2209_parse_read_response(
				  request, sizeof(request), reply, sizeof(reply),
				  TMC2209_REGISTER_IOIN, &value) ==
				  TMC2209_ERROR_REGISTER,
			  "reject wrong register");
	return failures;
}

static int test_retry_and_timeout(void)
{
	fake_transport_t fake = {0x21000000UL, 2U, 0U, 0U, true};
	tmc2209_transport_t transport = {fake_exchange, fake_recover, &fake};
	tmc2209_device_t device;
	uint32_t value = 0U;
	int failures = 0;

	tmc2209_device_init(&device, 2U, &transport);
	failures += check(tmc2209_read_register(&device, TMC2209_REGISTER_IOIN,
						&value) == TMC2209_ERROR_NONE,
			  "retry reaches successful read");
	failures += check(fake.calls == 3U && fake.recoveries == 2U,
			  "retry and recovery are bounded");
	fake.calls = 0U;
	fake.recoveries = 0U;
	fake.errors_before_success = 3U;
	failures += check(tmc2209_read_register(&device, TMC2209_REGISTER_IOIN,
						&value) == TMC2209_ERROR_TIMEOUT,
			  "timeout after three attempts");
	return failures;
}

static int test_current_microsteps_and_status(void)
{
	uint8_t scale = 0U;
	uint8_t mres = 0U;
	uint16_t actual_ma = 0U;
	int failures = 0;

	failures += check(tmc2209_ifcnt_advanced(255U, 0U),
			  "IFCNT wraparound");
	failures += check(tmc2209_calculate_current(
				  520U, TMC2209_MAX_CONFIGURED_CURRENT_MA, &scale,
				  &actual_ma) &&
				  scale == 16U && actual_ma == 520U,
			  "current calculation");
	failures += check(tmc2209_calculate_current(
				  275U, TMC2209_MAX_CONFIGURED_CURRENT_MA, &scale,
				  &actual_ma) &&
				  scale == 8U && actual_ma == 275U,
			  "hold current calculation");
	failures += check(!tmc2209_calculate_current(
				   581U, TMC2209_MAX_CONFIGURED_CURRENT_MA, &scale,
				   &actual_ma),
			  "current safety limit");
	failures += check(tmc2209_encode_microsteps(16U, &mres) && mres == 4U,
			  "16 microstep encoding");
	failures += check(!tmc2209_encode_microsteps(3U, &mres),
			  "reject invalid microsteps");
	failures += check(tmc2209_status_is_fatal(TMC2209_GSTAT_UV_CP, 0U),
			  "charge pump undervoltage fatal");
	failures += check(!tmc2209_status_is_fatal(TMC2209_GSTAT_RESET, 0U),
			  "GSTAT reset is informational, not fatal");
	failures += check(tmc2209_status_is_fatal(
				  0U, TMC2209_DRV_STATUS_OT |
					      TMC2209_DRV_STATUS_S2GA),
			  "temperature and short fatal");
	failures += check(!tmc2209_status_is_fatal(
				   0U, TMC2209_DRV_STATUS_OTPW |
					TMC2209_DRV_STATUS_STST),
			  "warning and standstill nonfatal");
	return failures;
}

static int test_probe_configuration_and_readback(void)
{
	register_transport_t fake = {{0U}, 0U, 0U, 0U, 0U};
	tmc2209_transport_t transport = {register_exchange, NULL, &fake};
	tmc2209_device_t device;
	int failures = 0;

	fake.registers[TMC2209_REGISTER_IOIN] = 0x21000000UL;
	fake.registers[TMC2209_REGISTER_GSTAT] = TMC2209_GSTAT_RESET;
	tmc2209_device_init(&device, 2U, &transport);
	failures += check(tmc2209_probe(&device) == TMC2209_ERROR_NONE &&
			  device.state == TMC2209_STATE_PRESENT,
			  "probe transitions to present");
	failures += check(fake.ifcnt == 2U &&
			  fake.registers[TMC2209_REGISTER_NODECONF] == 0x00000200UL,
			  "probe verifies NODECONF and GCONF writes");
	failures += check(tmc2209_configure(&device) == TMC2209_ERROR_NONE &&
			  device.state == TMC2209_STATE_CONFIGURED &&
			  device.configuration_valid,
			  "configuration state transition");
	failures += check(fake.ifcnt == 9U && fake.write_count == 9U,
			  "all configuration writes verified by IFCNT");
	failures += check(fake.registers[TMC2209_REGISTER_GSTAT] == 0U,
			  "configuration establishes cleared reset baseline");
	{
		const uint8_t configured_ifcnt = fake.ifcnt;
		const size_t configured_writes = fake.write_count;

		failures += check(tmc2209_configure(&device) ==
					  TMC2209_ERROR_NONE &&
				  fake.ifcnt == configured_ifcnt &&
				  fake.write_count == configured_writes,
				  "configuration is idempotent when already ready");
	}
	failures += check(fake.registers[TMC2209_REGISTER_GCONF] == 0x000001C0UL,
			  "fixed GCONF value");
	failures += check(
		(fake.registers[TMC2209_REGISTER_GCONF] &
		 (TMC2209_GCONF_PDN_DISABLE | TMC2209_GCONF_MSTEP_REG_SELECT)) ==
			(TMC2209_GCONF_PDN_DISABLE |
			 TMC2209_GCONF_MSTEP_REG_SELECT) &&
		(fake.registers[TMC2209_REGISTER_GCONF] &
		 (TMC2209_GCONF_I_SCALE_ANALOG | TMC2209_GCONF_INTERNAL_RSENSE |
		  TMC2209_GCONF_EN_SPREADCYCLE | TMC2209_GCONF_TEST_MODE)) == 0U,
		"GCONF establishes UART, MRES, digital scaling, and StealthChop");
	failures += check(fake.registers[TMC2209_REGISTER_CHOPCONF] ==
				  0x14030045UL,
			  "fixed CHOPCONF value and MRES encoding");
	failures += check(fake.registers[TMC2209_REGISTER_PWMCONF] ==
				  0xC80D0E24UL,
			  "fixed PWMCONF value");
	failures += check(fake.registers[TMC2209_REGISTER_IHOLD_IRUN] ==
				  0x00081008UL,
			  "fixed current scales");

	fake.registers[TMC2209_REGISTER_DRV_STATUS] = TMC2209_DRV_STATUS_OT;
	failures += check(tmc2209_refresh_status(&device) ==
				  TMC2209_ERROR_FATAL_STATUS &&
			  device.state == TMC2209_STATE_ERROR && device.fatal,
			  "fatal status revokes configured state");

	fake = (register_transport_t){{0U}, 0U, 0U, false, 0U};
	fake.registers[TMC2209_REGISTER_IOIN] = 0x21000000UL;
	tmc2209_device_init(&device, 2U, &transport);
	failures += check(tmc2209_probe(&device) == TMC2209_ERROR_NONE,
			  "second probe succeeds");
	fake.corrupt_read_register = TMC2209_REGISTER_CHOPCONF;
	fake.corrupt_read_mask = 1U;
	failures += check(tmc2209_configure(&device) == TMC2209_ERROR_READBACK &&
			  device.state == TMC2209_STATE_ERROR,
			  "configuration rejects masked readback mismatch");

	fake = (register_transport_t){{0U}, 0U, 0U, 0U, 0U};
	fake.registers[TMC2209_REGISTER_IOIN] = 0x21000000UL;
	tmc2209_device_init(&device, 2U, &transport);
	failures += check(tmc2209_probe(&device) == TMC2209_ERROR_NONE,
			  "reserved-bit CHOPCONF probe succeeds");
	fake.corrupt_read_register = TMC2209_REGISTER_CHOPCONF;
	fake.corrupt_read_mask = 1UL << 11;
	failures += check(tmc2209_configure(&device) == TMC2209_ERROR_NONE,
			  "CHOPCONF readback ignores reserved fields");

	fake = (register_transport_t){{0U}, 0U, 0U, 0U, 0U};
	fake.registers[TMC2209_REGISTER_IOIN] = 0x21000000UL;
	tmc2209_device_init(&device, 2U, &transport);
	failures += check(tmc2209_probe(&device) == TMC2209_ERROR_NONE,
			  "reserved-bit PWMCONF probe succeeds");
	fake.corrupt_read_register = TMC2209_REGISTER_PWMCONF;
	fake.corrupt_read_mask = 1UL << 22;
	failures += check(tmc2209_configure(&device) == TMC2209_ERROR_NONE,
			  "PWMCONF readback ignores reserved fields");
	return failures;
}

static int test_configuration_validity_transitions(void)
{
	register_transport_t registers = {{0U}, 0U, 0U, 0U, 0U};
	tmc2209_transport_t register_transport = {
		register_exchange, NULL, &registers};
	fake_transport_t timeout = {0U, UINT8_MAX, 0U, 0U, false};
	tmc2209_transport_t timeout_transport = {
		fake_exchange, fake_recover, &timeout};
	tmc2209_device_t device;
	int failures = 0;

	registers.registers[TMC2209_REGISTER_IOIN] = 0x21000000UL;
	tmc2209_device_init(&device, 2U, &register_transport);
	failures += check(tmc2209_probe(&device) == TMC2209_ERROR_NONE &&
			  tmc2209_configure(&device) == TMC2209_ERROR_NONE,
			  "configured device prepared for state tests");

	device.transport = timeout_transport;
	failures += check(tmc2209_refresh_status(&device) ==
				  TMC2209_ERROR_TIMEOUT &&
			  device.state == TMC2209_STATE_ERROR &&
			  device.configuration_valid &&
			  device.run_current_ma == TMC2209_RUN_CURRENT_MA,
			  "transient UART error blocks readiness but retains validity");
	timeout.calls = 0U;
	failures += check(tmc2209_probe(&device) == TMC2209_ERROR_TIMEOUT &&
			  device.state == TMC2209_STATE_ERROR &&
			  device.configuration_valid &&
			  device.microsteps == TMC2209_MICROSTEPS,
			  "failed recovery probe retains verified configuration");

	device.transport = register_transport;
	failures += check(tmc2209_refresh_status(&device) ==
				  TMC2209_ERROR_NONE &&
			  device.state == TMC2209_STATE_CONFIGURED &&
			  device.error == TMC2209_ERROR_NONE &&
			  device.configuration_valid,
			  "successful refresh restores configured state");

	registers.registers[TMC2209_REGISTER_GSTAT] = TMC2209_GSTAT_RESET;
	failures += check(tmc2209_refresh_status(&device) ==
				  TMC2209_ERROR_NONE &&
			  device.state == TMC2209_STATE_PRESENT &&
			  device.error == TMC2209_ERROR_RESET &&
			  !device.configuration_valid && !device.fatal,
			  "post-configuration reset invalidates configuration");
	failures += check(device.run_current_ma == 0U &&
			  device.hold_current_ma == 0U &&
			  device.microsteps == 0U && !device.interpolate &&
			  !device.stealthchop,
			  "invalid configuration cannot retain applied metadata");
	return failures;
}

int main(void)
{
	int failures = 0;

	failures += test_crc_and_datagrams();
	failures += test_response_validation();
	failures += test_retry_and_timeout();
	failures += test_current_microsteps_and_status();
	failures += test_probe_configuration_and_readback();
	failures += test_configuration_validity_transitions();
	if (failures == 0) {
		(void)puts("tmc2209 tests passed");
	}
	return failures;
}
