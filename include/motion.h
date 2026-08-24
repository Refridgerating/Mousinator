#ifndef SENTRY_MOTION_H
#define SENTRY_MOTION_H

#include <stdbool.h>
#include <stdint.h>

#define PAN_MAX_ABSOLUTE_VELOCITY_STEPS_S 1000
#define PAN_ACCELERATION_STEPS_S2 2000U
#define MOTION_CONTROL_FREQUENCY_HZ 1000U
#define MOTION_VELOCITY_DELTA_PER_TICK 2
#define MOTION_COMMAND_LEASE_TICKS 1000U

#if ((MOTION_VELOCITY_DELTA_PER_TICK * MOTION_CONTROL_FREQUENCY_HZ) != \
	PAN_ACCELERATION_STEPS_S2)
#error "control tick must represent the configured acceleration exactly"
#endif

typedef struct {
	int64_t position_steps;
	int32_t target_velocity_steps_s;
	int32_t current_velocity_steps_s;
	uint32_t phase_accumulator;
	uint32_t lease_ticks_remaining;
	bool enabled;
	bool moving;
	bool disabling;
	bool motion_timeout;
	bool direction_positive;
} pan_motion_state_t;

typedef enum {
	PAN_DISABLE_COMPLETE,
	PAN_DISABLE_PENDING,
} pan_disable_result_t;

typedef enum {
	PAN_VELOCITY_OK,
	PAN_VELOCITY_DISABLED,
	PAN_VELOCITY_DISABLING,
	PAN_VELOCITY_RANGE,
} pan_velocity_result_t;

typedef struct {
	bool emit_step;
	bool direction_changed;
	bool direction_positive;
	bool disable_driver;
} pan_motion_tick_result_t;

void pan_motion_init(pan_motion_state_t *state);
void pan_motion_enable(pan_motion_state_t *state);
pan_disable_result_t pan_motion_request_disable(pan_motion_state_t *state);
pan_velocity_result_t pan_motion_set_target_velocity(
	pan_motion_state_t *state, int32_t velocity_steps_s);
void pan_motion_stop(pan_motion_state_t *state);
pan_motion_tick_result_t pan_motion_tick(pan_motion_state_t *state);

#endif
