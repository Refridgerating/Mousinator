#ifndef SENTRY_MOTION_H
#define SENTRY_MOTION_H

#include <stdbool.h>
#include <stdint.h>

#define PAN_MAX_ABSOLUTE_VELOCITY_STEPS_S 10000
#define TILT_MAX_ABSOLUTE_VELOCITY_STEPS_S 5000
#define AXIS_ACCELERATION_STEPS_S2 2000U
#define MOTION_CONTROL_FREQUENCY_HZ 1000U
#define MOTION_SCHEDULER_FREQUENCY_HZ 20000U
#define MOTION_SCHEDULER_TICKS_PER_CONTROL_TICK 20U
#define MOTION_VELOCITY_DELTA_PER_TICK 2
#define MOTION_COMMAND_LEASE_TICKS 1000U

#if ((MOTION_VELOCITY_DELTA_PER_TICK * MOTION_CONTROL_FREQUENCY_HZ) != \
	AXIS_ACCELERATION_STEPS_S2)
#error "control tick must represent the configured acceleration exactly"
#endif

#if ((MOTION_CONTROL_FREQUENCY_HZ * \
	MOTION_SCHEDULER_TICKS_PER_CONTROL_TICK) != \
	MOTION_SCHEDULER_FREQUENCY_HZ)
#error "scheduler divider must represent the control frequency exactly"
#endif

#if ((PAN_MAX_ABSOLUTE_VELOCITY_STEPS_S > \
	MOTION_SCHEDULER_FREQUENCY_HZ) || \
	(TILT_MAX_ABSOLUTE_VELOCITY_STEPS_S > \
	MOTION_SCHEDULER_FREQUENCY_HZ))
#error "axis velocity limits must not exceed scheduler capacity"
#endif

typedef struct {
	int64_t position_steps;
	int32_t target_velocity_steps_s;
	int32_t current_velocity_steps_s;
	int32_t max_absolute_velocity_steps_s;
	uint32_t phase_accumulator;
	uint32_t lease_ticks_remaining;
	bool enabled;
	bool moving;
	bool disabling;
	bool motion_timeout;
	bool direction_positive;
} axis_motion_state_t;

typedef enum {
	AXIS_DISABLE_COMPLETE,
	AXIS_DISABLE_PENDING,
} axis_disable_result_t;

typedef enum {
	AXIS_VELOCITY_OK,
	AXIS_VELOCITY_DISABLED,
	AXIS_VELOCITY_DISABLING,
	AXIS_VELOCITY_RANGE,
} axis_velocity_result_t;

typedef struct {
	bool direction_changed;
	bool direction_positive;
	bool disable_driver;
} axis_motion_control_result_t;

void axis_motion_init(axis_motion_state_t *state,
		      int32_t max_absolute_velocity_steps_s);
void axis_motion_enable(axis_motion_state_t *state);
axis_disable_result_t axis_motion_request_disable(axis_motion_state_t *state);
axis_velocity_result_t axis_motion_validate_velocity(
	const axis_motion_state_t *state, int32_t velocity_steps_s);
axis_velocity_result_t axis_motion_set_target_velocity(
	axis_motion_state_t *state, int32_t velocity_steps_s);
bool axis_motion_set_two_velocities(axis_motion_state_t *first,
				    int32_t first_velocity_steps_s,
				    axis_motion_state_t *second,
				    int32_t second_velocity_steps_s,
				    axis_velocity_result_t *first_result,
				    axis_velocity_result_t *second_result);
void axis_motion_set_internal_velocity(axis_motion_state_t *state,
				       int32_t velocity_steps_s);
void axis_motion_stop(axis_motion_state_t *state);
void axis_motion_force_stop(axis_motion_state_t *state);
void axis_motion_set_position(axis_motion_state_t *state,
			      int64_t position_steps);
axis_motion_control_result_t axis_motion_control_tick(
	axis_motion_state_t *state);
bool axis_motion_scheduler_tick(axis_motion_state_t *state);
void axis_motion_commit_step(axis_motion_state_t *state);

#endif
