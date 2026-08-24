#include "tmc2209.h"

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

#define TMC2209_REGISTER_WRITE_FLAG 0x80U
#define TMC2209_REGISTER_ADDRESS_MASK 0x7FU
#define TMC2209_GCONF_CONFIGURATION_MASK 0x000003FFUL
#define TMC2209_GCONF_REQUIRED_SET_MASK \
	(TMC2209_GCONF_PDN_DISABLE | TMC2209_GCONF_MSTEP_REG_SELECT | \
	 TMC2209_GCONF_MULTISTEP_FILT)
#define TMC2209_GCONF_REQUIRED_CLEAR_MASK \
	(TMC2209_GCONF_I_SCALE_ANALOG | TMC2209_GCONF_INTERNAL_RSENSE | \
	 TMC2209_GCONF_EN_SPREADCYCLE | TMC2209_GCONF_TEST_MODE)
#define TMC2209_GCONF_VALUE TMC2209_GCONF_REQUIRED_SET_MASK
#define TMC2209_CHOPCONF_READBACK_MASK \
	((0x0FUL << TMC2209_CHOPCONF_TOFF_SHIFT) | \
	 (0x07UL << TMC2209_CHOPCONF_HSTRT_SHIFT) | \
	 (0x0FUL << TMC2209_CHOPCONF_HEND_SHIFT) | \
	 (0x03UL << TMC2209_CHOPCONF_TBL_SHIFT) | TMC2209_CHOPCONF_VSENSE | \
	 (0x0FUL << TMC2209_CHOPCONF_MRES_SHIFT) | \
	 TMC2209_CHOPCONF_INTPOL | TMC2209_CHOPCONF_DEDGE | \
	 TMC2209_CHOPCONF_DISS2G | TMC2209_CHOPCONF_DISS2VS)
#define TMC2209_PWMCONF_READBACK_MASK \
	((0xFFUL << TMC2209_PWMCONF_PWM_OFS_SHIFT) | \
	 (0xFFUL << TMC2209_PWMCONF_PWM_GRAD_SHIFT) | \
	 (0x03UL << TMC2209_PWMCONF_PWM_FREQ_SHIFT) | \
	 TMC2209_PWMCONF_PWM_AUTOSCALE | TMC2209_PWMCONF_PWM_AUTOGRAD | \
	 (0x03UL << TMC2209_PWMCONF_FREEWHEEL_SHIFT) | \
	 (0x0FUL << TMC2209_PWMCONF_PWM_REG_SHIFT) | \
	 (0x0FUL << TMC2209_PWMCONF_PWM_LIM_SHIFT))
#define TMC2209_IHOLD_IRUN_VALUE \
	((uint32_t)TMC2209_HOLD_CURRENT_SCALE | \
	 ((uint32_t)TMC2209_RUN_CURRENT_SCALE << 8) | \
	 ((uint32_t)TMC2209_HOLD_DELAY << 16))
#define TMC2209_CHOPCONF_VALUE \
	((uint32_t)TMC2209_CHOPPER_TOFF | \
	 ((uint32_t)TMC2209_CHOPPER_HSTRT << TMC2209_CHOPCONF_HSTRT_SHIFT) | \
	 ((uint32_t)TMC2209_CHOPPER_HEND << TMC2209_CHOPCONF_HEND_SHIFT) | \
	 ((uint32_t)TMC2209_CHOPPER_TBL << TMC2209_CHOPCONF_TBL_SHIFT) | \
	 TMC2209_CHOPCONF_VSENSE | \
	 ((uint32_t)TMC2209_MRES_16_MICROSTEPS << \
	  TMC2209_CHOPCONF_MRES_SHIFT) | \
	 TMC2209_CHOPCONF_INTPOL)
#define TMC2209_PWMCONF_VALUE \
	(((uint32_t)TMC2209_PWM_OFFSET << TMC2209_PWMCONF_PWM_OFS_SHIFT) | \
	 ((uint32_t)TMC2209_PWM_GRADIENT << TMC2209_PWMCONF_PWM_GRAD_SHIFT) | \
	 ((uint32_t)TMC2209_PWM_FREQUENCY << TMC2209_PWMCONF_PWM_FREQ_SHIFT) | \
	 TMC2209_PWMCONF_PWM_AUTOSCALE | TMC2209_PWMCONF_PWM_AUTOGRAD | \
	 ((uint32_t)TMC2209_PWM_REGULATOR << TMC2209_PWMCONF_PWM_REG_SHIFT) | \
	 ((uint32_t)TMC2209_PWM_LIMIT << TMC2209_PWMCONF_PWM_LIM_SHIFT))
#define TMC2209_FATAL_DRV_STATUS_MASK \
	(TMC2209_DRV_STATUS_OT | TMC2209_DRV_STATUS_S2GA | \
	 TMC2209_DRV_STATUS_S2GB | TMC2209_DRV_STATUS_S2VSA | \
	 TMC2209_DRV_STATUS_S2VSB)

_Static_assert((TMC2209_GCONF_VALUE & TMC2209_GCONF_REQUIRED_SET_MASK) ==
		       TMC2209_GCONF_REQUIRED_SET_MASK,
	       "GCONF must set every UART/MRES prerequisite");
_Static_assert((TMC2209_GCONF_VALUE & TMC2209_GCONF_REQUIRED_CLEAR_MASK) == 0U,
	       "GCONF must clear analog/current/SpreadCycle/test prerequisites");

static bool bytes_equal(const uint8_t *left, const uint8_t *right,
			size_t length)
{
	size_t index;

	for (index = 0U; index < length; ++index) {
		if (left[index] != right[index]) {
			return false;
		}
	}
	return true;
}

uint8_t tmc2209_crc(const uint8_t *data, size_t length)
{
	uint8_t crc = 0U;
	size_t byte_index;

	for (byte_index = 0U; byte_index < length; ++byte_index) {
		uint8_t current_byte = data[byte_index];
		uint8_t bit_index;

		for (bit_index = 0U; bit_index < 8U; ++bit_index) {
			if ((((crc >> 7) ^ current_byte) & 0x01U) != 0U) {
				crc = (uint8_t)((uint8_t)(crc << 1) ^ 0x07U);
			} else {
				crc = (uint8_t)(crc << 1);
			}
			current_byte = (uint8_t)(current_byte >> 1);
		}
	}
	return crc;
}

bool tmc2209_build_read_datagram(uint8_t address, uint8_t register_address,
				 uint8_t *datagram, size_t capacity)
{
	if ((address > 3U) || (register_address > TMC2209_REGISTER_ADDRESS_MASK) ||
	    (capacity < TMC2209_READ_DATAGRAM_SIZE)) {
		return false;
	}

	datagram[0] = TMC2209_SYNC_BYTE;
	datagram[1] = address;
	datagram[2] = register_address;
	datagram[3] = tmc2209_crc(datagram, TMC2209_READ_DATAGRAM_SIZE - 1U);
	return true;
}

bool tmc2209_build_write_datagram(uint8_t address, uint8_t register_address,
				  uint32_t value, uint8_t *datagram,
				  size_t capacity)
{
	if ((address > 3U) || (register_address > TMC2209_REGISTER_ADDRESS_MASK) ||
	    (capacity < TMC2209_WRITE_DATAGRAM_SIZE)) {
		return false;
	}

	datagram[0] = TMC2209_SYNC_BYTE;
	datagram[1] = address;
	datagram[2] = (uint8_t)(register_address | TMC2209_REGISTER_WRITE_FLAG);
	datagram[3] = (uint8_t)(value >> 24);
	datagram[4] = (uint8_t)(value >> 16);
	datagram[5] = (uint8_t)(value >> 8);
	datagram[6] = (uint8_t)value;
	datagram[7] = tmc2209_crc(datagram, TMC2209_WRITE_DATAGRAM_SIZE - 1U);
	return true;
}

tmc2209_error_t tmc2209_parse_read_response(
	const uint8_t *request, size_t request_length, const uint8_t *received,
	size_t received_length, uint8_t expected_register, uint32_t *value)
{
	const uint8_t *reply = received;

	if (received_length == (request_length + TMC2209_REPLY_DATAGRAM_SIZE)) {
		if (!bytes_equal(request, received, request_length)) {
			return TMC2209_ERROR_ECHO;
		}
		reply = &received[request_length];
	} else if (received_length != TMC2209_REPLY_DATAGRAM_SIZE) {
		return TMC2209_ERROR_TIMEOUT;
	}

	if (reply[0] != TMC2209_SYNC_BYTE) {
		return TMC2209_ERROR_SYNC;
	}
	if (reply[1] != TMC2209_MASTER_ADDRESS) {
		return TMC2209_ERROR_ADDRESS;
	}
	if (reply[2] != expected_register) {
		return TMC2209_ERROR_REGISTER;
	}
	if (tmc2209_crc(reply, TMC2209_REPLY_DATAGRAM_SIZE - 1U) != reply[7]) {
		return TMC2209_ERROR_CRC;
	}

	*value = ((uint32_t)reply[3] << 24) | ((uint32_t)reply[4] << 16) |
		 ((uint32_t)reply[5] << 8) | (uint32_t)reply[6];
	return TMC2209_ERROR_NONE;
}

bool tmc2209_ifcnt_advanced(uint8_t before, uint8_t after)
{
	return after == (uint8_t)(before + 1U);
}

static uint16_t current_for_scale(uint8_t current_scale)
{
	const uint64_t numerator =
		(uint64_t)(current_scale + 1U) *
		(uint64_t)TMC2209_HIGH_SENSITIVITY_MILLIVOLT * 1000000ULL;
	const uint64_t denominator =
		32ULL *
		(uint64_t)(TMC2209_SENSE_RESISTANCE_MILLIOHM +
			   TMC2209_INTERNAL_SENSE_MILLIOHM) *
		1414ULL;

	return (uint16_t)((numerator + (denominator / 2ULL)) / denominator);
}

bool tmc2209_calculate_current(uint16_t requested_ma, uint16_t maximum_ma,
			       uint8_t *current_scale,
			       uint16_t *actual_ma)
{
	uint8_t scale;
	uint8_t best_scale = 0U;
	uint16_t best_current = 0U;
	uint16_t best_error = UINT16_MAX;

	if ((requested_ma == 0U) || (requested_ma > maximum_ma) ||
	    (maximum_ma > TMC2209_MAX_CONFIGURED_CURRENT_MA)) {
		return false;
	}

	for (scale = 0U; scale < 32U; ++scale) {
		const uint16_t candidate = current_for_scale(scale);
		const uint16_t error = candidate > requested_ma ?
			(uint16_t)(candidate - requested_ma) :
			(uint16_t)(requested_ma - candidate);

		if ((candidate <= maximum_ma) && (error < best_error)) {
			best_error = error;
			best_scale = scale;
			best_current = candidate;
		}
	}

	if (best_error == UINT16_MAX) {
		return false;
	}
	*current_scale = best_scale;
	*actual_ma = best_current;
	return true;
}

bool tmc2209_encode_microsteps(uint16_t microsteps, uint8_t *mres)
{
	uint16_t candidate = 256U;
	uint8_t code;

	for (code = 0U; code <= 8U; ++code) {
		if (candidate == microsteps) {
			*mres = code;
			return true;
		}
		candidate = (uint16_t)(candidate / 2U);
	}
	return false;
}

bool tmc2209_status_is_fatal(uint32_t gstat, uint32_t drv_status)
{
	return ((gstat & (TMC2209_GSTAT_DRV_ERR | TMC2209_GSTAT_UV_CP)) != 0U) ||
	       ((drv_status & TMC2209_FATAL_DRV_STATUS_MASK) != 0U);
}

void tmc2209_device_init(tmc2209_device_t *device, uint8_t address,
			 const tmc2209_transport_t *transport)
{
	device->transport = *transport;
	device->address = address;
	device->state = TMC2209_STATE_UNKNOWN;
	device->error = TMC2209_ERROR_NONE;
	device->configuration_valid = false;
	device->ioin = 0U;
	device->gstat = 0U;
	device->drv_status = 0U;
	device->ifcnt = 0U;
	device->run_current_ma = 0U;
	device->hold_current_ma = 0U;
	device->microsteps = 0U;
	device->interpolate = false;
	device->stealthchop = false;
	device->fatal = false;
}

static tmc2209_error_t exchange_once(tmc2209_device_t *device,
				      const uint8_t *transmit,
				      size_t transmit_length, uint8_t *receive,
				      size_t *receive_length)
{
	*receive_length = 0U;
	return device->transport.exchange(
		device->transport.context, transmit, transmit_length, receive,
		TMC2209_MAX_CAPTURE_SIZE, receive_length);
}

tmc2209_error_t tmc2209_read_register(tmc2209_device_t *device,
				      uint8_t register_address,
				      uint32_t *value)
{
	uint8_t request[TMC2209_READ_DATAGRAM_SIZE];
	uint8_t received[TMC2209_MAX_CAPTURE_SIZE];
	size_t received_length;
	tmc2209_error_t error;
	uint8_t attempt;

	if (!tmc2209_build_read_datagram(device->address, register_address,
					 request, ARRAY_LENGTH(request))) {
		return TMC2209_ERROR_RANGE;
	}

	for (attempt = 0U; attempt < TMC2209_TRANSACTION_ATTEMPTS; ++attempt) {
		error = exchange_once(device, request, ARRAY_LENGTH(request), received,
				      &received_length);
		if (error == TMC2209_ERROR_NONE) {
			error = tmc2209_parse_read_response(
				request, ARRAY_LENGTH(request), received, received_length,
				register_address, value);
		}
		if (error == TMC2209_ERROR_NONE) {
			return error;
		}
		if (device->transport.recover != NULL) {
			device->transport.recover(device->transport.context);
		}
	}
	return error;
}

static tmc2209_error_t write_register(tmc2209_device_t *device,
				      uint8_t register_address, uint32_t value)
{
	uint8_t datagram[TMC2209_WRITE_DATAGRAM_SIZE];
	uint8_t received[TMC2209_MAX_CAPTURE_SIZE];
	size_t received_length;
	tmc2209_error_t error = TMC2209_ERROR_UART;
	uint8_t attempt;

	if (!tmc2209_build_write_datagram(device->address, register_address,
					  value, datagram,
					  ARRAY_LENGTH(datagram))) {
		return TMC2209_ERROR_RANGE;
	}

	for (attempt = 0U; attempt < TMC2209_TRANSACTION_ATTEMPTS; ++attempt) {
		error = exchange_once(device, datagram, ARRAY_LENGTH(datagram), received,
				      &received_length);
		if ((error == TMC2209_ERROR_NONE) &&
		    ((received_length == 0U) ||
		     ((received_length == ARRAY_LENGTH(datagram)) &&
		      bytes_equal(datagram, received, ARRAY_LENGTH(datagram))))) {
			return TMC2209_ERROR_NONE;
		}
		if (error == TMC2209_ERROR_NONE) {
			error = TMC2209_ERROR_ECHO;
		}
		if (device->transport.recover != NULL) {
			device->transport.recover(device->transport.context);
		}
	}
	return error;
}

static tmc2209_error_t write_verified(tmc2209_device_t *device,
				      uint8_t register_address, uint32_t value,
				      bool readable, uint32_t readback_mask)
{
	uint32_t before_value;
	uint32_t after_value;
	uint32_t readback;
	tmc2209_error_t error;

	error = tmc2209_read_register(device, TMC2209_REGISTER_IFCNT,
				      &before_value);
	if (error != TMC2209_ERROR_NONE) {
		return error;
	}
	error = write_register(device, register_address, value);
	if (error != TMC2209_ERROR_NONE) {
		return error;
	}
	error = tmc2209_read_register(device, TMC2209_REGISTER_IFCNT,
				      &after_value);
	if (error != TMC2209_ERROR_NONE) {
		return error;
	}
	device->ifcnt = (uint8_t)after_value;
	if (!tmc2209_ifcnt_advanced((uint8_t)before_value,
				     (uint8_t)after_value)) {
		return TMC2209_ERROR_IFCNT;
	}
	if (!readable) {
		return TMC2209_ERROR_NONE;
	}
	error = tmc2209_read_register(device, register_address, &readback);
	if (error != TMC2209_ERROR_NONE) {
		return error;
	}
	if ((readback & readback_mask) != (value & readback_mask)) {
		return TMC2209_ERROR_READBACK;
	}
	return TMC2209_ERROR_NONE;
}

static tmc2209_error_t set_error(tmc2209_device_t *device,
				 tmc2209_error_t error)
{
	device->state = TMC2209_STATE_ERROR;
	device->error = error;
	if (error == TMC2209_ERROR_FATAL_STATUS) {
		device->fatal = true;
	}
	return error;
}

static void invalidate_configuration(tmc2209_device_t *device)
{
	device->configuration_valid = false;
	device->run_current_ma = 0U;
	device->hold_current_ma = 0U;
	device->microsteps = 0U;
	device->interpolate = false;
	device->stealthchop = false;
}

tmc2209_error_t tmc2209_probe(tmc2209_device_t *device)
{
	bool reset_invalidated_configuration = false;
	uint32_t gconf;
	uint32_t value;
	tmc2209_error_t error;

	device->state = TMC2209_STATE_UNKNOWN;
	device->error = TMC2209_ERROR_NONE;
	device->fatal = false;

	error = write_verified(
		device, TMC2209_REGISTER_NODECONF,
		(uint32_t)TMC2209_NODECONF_SENDDELAY_3X8_BITS <<
			TMC2209_NODECONF_SENDDELAY_SHIFT,
		false, 0U);
	if (error != TMC2209_ERROR_NONE) {
		return set_error(device, error);
	}
	error = tmc2209_read_register(device, TMC2209_REGISTER_IOIN,
				      &device->ioin);
	if (error != TMC2209_ERROR_NONE) {
		return set_error(device, error);
	}
	if ((uint8_t)(device->ioin >> TMC2209_IOIN_VERSION_SHIFT) !=
	    TMC2209_EXPECTED_VERSION) {
		return set_error(device, TMC2209_ERROR_VERSION);
	}
	error = tmc2209_read_register(device, TMC2209_REGISTER_GCONF, &gconf);
	if (error != TMC2209_ERROR_NONE) {
		return set_error(device, error);
	}
	error = write_verified(device, TMC2209_REGISTER_GCONF, gconf, true,
			       TMC2209_GCONF_CONFIGURATION_MASK);
	if (error != TMC2209_ERROR_NONE) {
		return set_error(device, error);
	}
	error = tmc2209_read_register(device, TMC2209_REGISTER_IFCNT, &value);
	if (error != TMC2209_ERROR_NONE) {
		return set_error(device, error);
	}
	device->ifcnt = (uint8_t)value;
	error = tmc2209_read_register(device, TMC2209_REGISTER_GSTAT,
				      &device->gstat);
	if (error != TMC2209_ERROR_NONE) {
		return set_error(device, error);
	}
	error = tmc2209_read_register(device, TMC2209_REGISTER_DRV_STATUS,
				      &device->drv_status);
	if (error != TMC2209_ERROR_NONE) {
		return set_error(device, error);
	}
	device->fatal = tmc2209_status_is_fatal(device->gstat,
						device->drv_status);
	if (device->configuration_valid &&
	    ((device->gstat & TMC2209_GSTAT_RESET) != 0U)) {
		invalidate_configuration(device);
		reset_invalidated_configuration = true;
	}
	if (device->fatal) {
		return set_error(device, TMC2209_ERROR_FATAL_STATUS);
	}
	device->state = TMC2209_STATE_PRESENT;
	device->error = reset_invalidated_configuration ?
		TMC2209_ERROR_RESET : TMC2209_ERROR_NONE;
	return TMC2209_ERROR_NONE;
}

tmc2209_error_t tmc2209_configure(tmc2209_device_t *device)
{
	static const struct {
		uint8_t register_address;
		uint32_t value;
		bool readable;
		uint32_t mask;
	} configuration[] = {
		/*
		 * This full GCONF write explicitly enables UART/register MRES and
		 * clears analog scaling, internal sensing, SpreadCycle, and test mode.
		 */
		{TMC2209_REGISTER_GCONF, TMC2209_GCONF_VALUE,
		 true, TMC2209_GCONF_CONFIGURATION_MASK},
		{TMC2209_REGISTER_CHOPCONF, TMC2209_CHOPCONF_VALUE, true,
		 TMC2209_CHOPCONF_READBACK_MASK},
		{TMC2209_REGISTER_PWMCONF, TMC2209_PWMCONF_VALUE, true,
		 TMC2209_PWMCONF_READBACK_MASK},
		{TMC2209_REGISTER_IHOLD_IRUN, TMC2209_IHOLD_IRUN_VALUE, false,
		 0U},
		{TMC2209_REGISTER_TPOWERDOWN, TMC2209_POWERDOWN_DELAY, false,
		 0U},
		{TMC2209_REGISTER_TPWMTHRS, 0U, false, 0U},
	};
	uint8_t calculated_run_scale;
	uint8_t calculated_hold_scale;
	uint16_t calculated_run_current;
	uint16_t calculated_hold_current;
	size_t index;
	tmc2209_error_t error;

	if ((device->state == TMC2209_STATE_CONFIGURED) &&
	    device->configuration_valid &&
	    (device->error == TMC2209_ERROR_NONE) && !device->fatal) {
		return TMC2209_ERROR_NONE;
	}
	if ((device->state != TMC2209_STATE_PRESENT) &&
	    (device->state != TMC2209_STATE_CONFIGURED)) {
		return set_error(device, TMC2209_ERROR_UART);
	}
	/* A failed partial rewrite makes the previous applied metadata stale. */
	invalidate_configuration(device);
	if (!tmc2209_calculate_current(
		    TMC2209_RUN_CURRENT_MA, TMC2209_MAX_CONFIGURED_CURRENT_MA,
		    &calculated_run_scale, &calculated_run_current) ||
	    !tmc2209_calculate_current(
		    TMC2209_HOLD_CURRENT_MA, TMC2209_MAX_CONFIGURED_CURRENT_MA,
		    &calculated_hold_scale, &calculated_hold_current) ||
	    (calculated_run_scale != TMC2209_RUN_CURRENT_SCALE) ||
	    (calculated_run_current != TMC2209_RUN_CURRENT_MA) ||
	    (calculated_hold_scale != TMC2209_HOLD_CURRENT_SCALE) ||
	    (calculated_hold_current != TMC2209_HOLD_CURRENT_MA)) {
		return set_error(device, TMC2209_ERROR_RANGE);
	}

	for (index = 0U; index < ARRAY_LENGTH(configuration); ++index) {
		error = write_verified(device, configuration[index].register_address,
				       configuration[index].value,
				       configuration[index].readable,
				       configuration[index].mask);
		if (error != TMC2209_ERROR_NONE) {
			return set_error(device, error);
		}
	}

	/*
	 * GSTAT.reset is latched. Clear it after applying the configuration so
	 * any later observation proves that the driver reset afterwards.
	 */
	error = write_verified(device, TMC2209_REGISTER_GSTAT,
			       TMC2209_GSTAT_RESET, false, 0U);
	if (error != TMC2209_ERROR_NONE) {
		return set_error(device, error);
	}

	device->run_current_ma = calculated_run_current;
	device->hold_current_ma = calculated_hold_current;
	device->microsteps = TMC2209_MICROSTEPS;
	device->interpolate = true;
	device->stealthchop = true;
	device->configuration_valid = true;

	error = tmc2209_refresh_status(device);
	if (error != TMC2209_ERROR_NONE) {
		return error;
	}
	if (!device->configuration_valid) {
		return TMC2209_ERROR_RESET;
	}
	return TMC2209_ERROR_NONE;
}

tmc2209_error_t tmc2209_refresh_status(tmc2209_device_t *device)
{
	uint32_t value;
	bool reset_invalidated_configuration = false;
	tmc2209_error_t error;

	error = tmc2209_read_register(device, TMC2209_REGISTER_IOIN,
				      &device->ioin);
	if (error != TMC2209_ERROR_NONE) {
		return set_error(device, error);
	}
	if ((uint8_t)(device->ioin >> TMC2209_IOIN_VERSION_SHIFT) !=
	    TMC2209_EXPECTED_VERSION) {
		return set_error(device, TMC2209_ERROR_VERSION);
	}
	error = tmc2209_read_register(device, TMC2209_REGISTER_IFCNT, &value);
	if (error != TMC2209_ERROR_NONE) {
		return set_error(device, error);
	}
	device->ifcnt = (uint8_t)value;
	error = tmc2209_read_register(device, TMC2209_REGISTER_GSTAT,
				      &device->gstat);
	if (error != TMC2209_ERROR_NONE) {
		return set_error(device, error);
	}
	error = tmc2209_read_register(device, TMC2209_REGISTER_DRV_STATUS,
				      &device->drv_status);
	if (error != TMC2209_ERROR_NONE) {
		return set_error(device, error);
	}
	device->fatal = tmc2209_status_is_fatal(device->gstat,
						device->drv_status);
	if (device->configuration_valid &&
	    ((device->gstat & TMC2209_GSTAT_RESET) != 0U)) {
		invalidate_configuration(device);
		reset_invalidated_configuration = true;
	}
	if (device->fatal) {
		return set_error(device, TMC2209_ERROR_FATAL_STATUS);
	}
	device->state = device->configuration_valid ?
		TMC2209_STATE_CONFIGURED : TMC2209_STATE_PRESENT;
	device->error = reset_invalidated_configuration ?
		TMC2209_ERROR_RESET : TMC2209_ERROR_NONE;
	return TMC2209_ERROR_NONE;
}

const char *tmc2209_state_name(tmc2209_state_t state)
{
	switch (state) {
	case TMC2209_STATE_UNKNOWN:
		return "UNKNOWN";
	case TMC2209_STATE_PRESENT:
		return "PRESENT";
	case TMC2209_STATE_CONFIGURED:
		return "CONFIGURED";
	case TMC2209_STATE_ERROR:
		return "ERROR";
	}
	return "ERROR";
}

const char *tmc2209_error_name(tmc2209_error_t error)
{
	switch (error) {
	case TMC2209_ERROR_NONE:
		return "NONE";
	case TMC2209_ERROR_TIMEOUT:
		return "TIMEOUT";
	case TMC2209_ERROR_UART:
		return "UART";
	case TMC2209_ERROR_CAPTURE_OVERFLOW:
		return "OVERFLOW";
	case TMC2209_ERROR_ECHO:
		return "ECHO";
	case TMC2209_ERROR_SYNC:
		return "SYNC";
	case TMC2209_ERROR_ADDRESS:
		return "ADDRESS";
	case TMC2209_ERROR_REGISTER:
		return "REGISTER";
	case TMC2209_ERROR_CRC:
		return "CRC";
	case TMC2209_ERROR_IFCNT:
		return "IFCNT";
	case TMC2209_ERROR_READBACK:
		return "READBACK";
	case TMC2209_ERROR_VERSION:
		return "VERSION";
	case TMC2209_ERROR_RANGE:
		return "RANGE";
	case TMC2209_ERROR_RESET:
		return "RESET";
	case TMC2209_ERROR_FATAL_STATUS:
		return "FATAL";
	}
	return "UART";
}
