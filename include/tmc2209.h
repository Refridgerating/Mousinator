#ifndef SENTRY_TMC2209_H
#define SENTRY_TMC2209_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TMC2209_SYNC_BYTE 0x05U
#define TMC2209_MASTER_ADDRESS 0xFFU
#define TMC2209_READ_DATAGRAM_SIZE 4U
#define TMC2209_WRITE_DATAGRAM_SIZE 8U
#define TMC2209_REPLY_DATAGRAM_SIZE 8U
#define TMC2209_MAX_CAPTURE_SIZE 16U
#define TMC2209_TRANSACTION_ATTEMPTS 3U

#define TMC2209_REGISTER_GCONF 0x00U
#define TMC2209_REGISTER_GSTAT 0x01U
#define TMC2209_REGISTER_IFCNT 0x02U
#define TMC2209_REGISTER_NODECONF 0x03U
#define TMC2209_REGISTER_IOIN 0x06U
#define TMC2209_REGISTER_IHOLD_IRUN 0x10U
#define TMC2209_REGISTER_TPOWERDOWN 0x11U
#define TMC2209_REGISTER_TPWMTHRS 0x13U
#define TMC2209_REGISTER_CHOPCONF 0x6CU
#define TMC2209_REGISTER_DRV_STATUS 0x6FU
#define TMC2209_REGISTER_PWMCONF 0x70U

#define TMC2209_GCONF_I_SCALE_ANALOG (1UL << 0)
#define TMC2209_GCONF_INTERNAL_RSENSE (1UL << 1)
#define TMC2209_GCONF_EN_SPREADCYCLE (1UL << 2)
#define TMC2209_GCONF_PDN_DISABLE (1UL << 6)
#define TMC2209_GCONF_MSTEP_REG_SELECT (1UL << 7)
#define TMC2209_GCONF_MULTISTEP_FILT (1UL << 8)
#define TMC2209_GCONF_TEST_MODE (1UL << 9)

#define TMC2209_GSTAT_RESET (1UL << 0)
#define TMC2209_GSTAT_DRV_ERR (1UL << 1)
#define TMC2209_GSTAT_UV_CP (1UL << 2)

#define TMC2209_IOIN_VERSION_SHIFT 24U
#define TMC2209_IOIN_VERSION_MASK (0xFFUL << TMC2209_IOIN_VERSION_SHIFT)
#define TMC2209_EXPECTED_VERSION 0x21U

#define TMC2209_NODECONF_SENDDELAY_SHIFT 8U
#define TMC2209_NODECONF_SENDDELAY_3X8_BITS 2U

#define TMC2209_CHOPCONF_TOFF_SHIFT 0U
#define TMC2209_CHOPCONF_HSTRT_SHIFT 4U
#define TMC2209_CHOPCONF_HEND_SHIFT 7U
#define TMC2209_CHOPCONF_TBL_SHIFT 15U
#define TMC2209_CHOPCONF_VSENSE (1UL << 17)
#define TMC2209_CHOPCONF_MRES_SHIFT 24U
#define TMC2209_CHOPCONF_INTPOL (1UL << 28)
#define TMC2209_CHOPCONF_DEDGE (1UL << 29)
#define TMC2209_CHOPCONF_DISS2G (1UL << 30)
#define TMC2209_CHOPCONF_DISS2VS (1UL << 31)

#define TMC2209_PWMCONF_PWM_OFS_SHIFT 0U
#define TMC2209_PWMCONF_PWM_GRAD_SHIFT 8U
#define TMC2209_PWMCONF_PWM_FREQ_SHIFT 16U
#define TMC2209_PWMCONF_PWM_AUTOSCALE (1UL << 18)
#define TMC2209_PWMCONF_PWM_AUTOGRAD (1UL << 19)
#define TMC2209_PWMCONF_FREEWHEEL_SHIFT 20U
#define TMC2209_PWMCONF_PWM_REG_SHIFT 24U
#define TMC2209_PWMCONF_PWM_LIM_SHIFT 28U

#define TMC2209_DRV_STATUS_OTPW (1UL << 0)
#define TMC2209_DRV_STATUS_OT (1UL << 1)
#define TMC2209_DRV_STATUS_S2GA (1UL << 2)
#define TMC2209_DRV_STATUS_S2GB (1UL << 3)
#define TMC2209_DRV_STATUS_S2VSA (1UL << 4)
#define TMC2209_DRV_STATUS_S2VSB (1UL << 5)
#define TMC2209_DRV_STATUS_T120 (1UL << 8)
#define TMC2209_DRV_STATUS_T143 (1UL << 9)
#define TMC2209_DRV_STATUS_T150 (1UL << 10)
#define TMC2209_DRV_STATUS_T157 (1UL << 11)
#define TMC2209_DRV_STATUS_CS_ACTUAL_SHIFT 16U
#define TMC2209_DRV_STATUS_CS_ACTUAL_MASK \
	(0x1FUL << TMC2209_DRV_STATUS_CS_ACTUAL_SHIFT)
#define TMC2209_DRV_STATUS_STEALTH (1UL << 30)
#define TMC2209_DRV_STATUS_STST (1UL << 31)

#define TMC2209_SENSE_RESISTANCE_MILLIOHM 110U
#define TMC2209_INTERNAL_SENSE_MILLIOHM 20U
#define TMC2209_HIGH_SENSITIVITY_MILLIVOLT 180U
#define TMC2209_MAX_CONFIGURED_CURRENT_MA 580U
#define TMC2209_RUN_CURRENT_SCALE 16U
#define TMC2209_HOLD_CURRENT_SCALE 8U
#define TMC2209_HOLD_DELAY 8U
#define TMC2209_POWERDOWN_DELAY 20U
#define TMC2209_RUN_CURRENT_MA 520U
#define TMC2209_HOLD_CURRENT_MA 275U
#define TMC2209_MICROSTEPS 16U
#define TMC2209_MRES_16_MICROSTEPS 4U

#define TMC2209_CHOPPER_TOFF 5U
#define TMC2209_CHOPPER_HSTRT 4U
#define TMC2209_CHOPPER_HEND 0U
#define TMC2209_CHOPPER_TBL 2U

#define TMC2209_PWM_OFFSET 36U
#define TMC2209_PWM_GRADIENT 14U
#define TMC2209_PWM_FREQUENCY 1U
#define TMC2209_PWM_REGULATOR 8U
#define TMC2209_PWM_LIMIT 12U

typedef enum {
	TMC2209_STATE_UNKNOWN = 0,
	TMC2209_STATE_PRESENT,
	TMC2209_STATE_CONFIGURED,
	TMC2209_STATE_ERROR
} tmc2209_state_t;

typedef enum {
	TMC2209_ERROR_NONE = 0,
	TMC2209_ERROR_TIMEOUT,
	TMC2209_ERROR_UART,
	TMC2209_ERROR_CAPTURE_OVERFLOW,
	TMC2209_ERROR_ECHO,
	TMC2209_ERROR_SYNC,
	TMC2209_ERROR_ADDRESS,
	TMC2209_ERROR_REGISTER,
	TMC2209_ERROR_CRC,
	TMC2209_ERROR_IFCNT,
	TMC2209_ERROR_READBACK,
	TMC2209_ERROR_VERSION,
	TMC2209_ERROR_RANGE,
	TMC2209_ERROR_FATAL_STATUS
} tmc2209_error_t;

typedef tmc2209_error_t (*tmc2209_exchange_fn)(
	void *context, const uint8_t *transmit, size_t transmit_length,
	uint8_t *receive, size_t receive_capacity, size_t *receive_length);
typedef void (*tmc2209_recover_fn)(void *context);

typedef struct {
	tmc2209_exchange_fn exchange;
	tmc2209_recover_fn recover;
	void *context;
} tmc2209_transport_t;

typedef struct {
	tmc2209_transport_t transport;
	uint8_t address;
	tmc2209_state_t state;
	tmc2209_error_t error;
	uint32_t ioin;
	uint32_t gstat;
	uint32_t drv_status;
	uint8_t ifcnt;
	uint16_t run_current_ma;
	uint16_t hold_current_ma;
	uint16_t microsteps;
	bool interpolate;
	bool stealthchop;
	bool fatal;
} tmc2209_device_t;

uint8_t tmc2209_crc(const uint8_t *data, size_t length);
bool tmc2209_build_read_datagram(uint8_t address, uint8_t register_address,
				 uint8_t *datagram, size_t capacity);
bool tmc2209_build_write_datagram(uint8_t address, uint8_t register_address,
				  uint32_t value, uint8_t *datagram,
				  size_t capacity);
tmc2209_error_t tmc2209_parse_read_response(
	const uint8_t *request, size_t request_length, const uint8_t *received,
	size_t received_length, uint8_t expected_register, uint32_t *value);
bool tmc2209_ifcnt_advanced(uint8_t before, uint8_t after);
bool tmc2209_calculate_current(uint16_t requested_ma, uint16_t maximum_ma,
			       uint8_t *current_scale,
			       uint16_t *actual_ma);
bool tmc2209_encode_microsteps(uint16_t microsteps, uint8_t *mres);
bool tmc2209_status_is_fatal(uint32_t gstat, uint32_t drv_status);

void tmc2209_device_init(tmc2209_device_t *device, uint8_t address,
			 const tmc2209_transport_t *transport);
tmc2209_error_t tmc2209_read_register(tmc2209_device_t *device,
				      uint8_t register_address,
				      uint32_t *value);
tmc2209_error_t tmc2209_probe(tmc2209_device_t *device);
tmc2209_error_t tmc2209_configure(tmc2209_device_t *device);
tmc2209_error_t tmc2209_refresh_status(tmc2209_device_t *device);
const char *tmc2209_state_name(tmc2209_state_t state);
const char *tmc2209_error_name(tmc2209_error_t error);

#endif
