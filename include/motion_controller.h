#ifndef SENTRY_MOTION_CONTROLLER_H
#define SENTRY_MOTION_CONTROLLER_H

#include "motion.h"
#include "tilt_reference.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	MOTION_AXIS_PAN = 0,
	MOTION_AXIS_TILT,
} motion_axis_t;

typedef struct {
	int64_t position_steps;
	int32_t current_velocity_steps_s;
	int32_t target_velocity_steps_s;
	bool enabled;
	bool moving;
	bool disabling;
	bool motion_timeout;
	bool homed;
	bool homing;
	bool jogging;
	bool direction_checking;
	bool direction_calibrated;
	bool min_limit;
	tilt_home_status_t home_status;
} motion_controller_snapshot_t;

typedef enum {
	MOTION_ENABLE_OK = 0,
	MOTION_ENABLE_DRIVER_NOT_READY,
} motion_enable_result_t;

typedef enum {
	MOTION_VELOCITY_OK = 0,
	MOTION_VELOCITY_RANGE,
	MOTION_VELOCITY_DISABLED,
	MOTION_VELOCITY_DISABLING,
	MOTION_VELOCITY_BUSY,
	MOTION_VELOCITY_DIRECTION_UNCALIBRATED,
	MOTION_VELOCITY_NOT_HOMED,
	MOTION_VELOCITY_MIN_LIMIT,
} motion_velocity_result_t;

typedef struct {
	motion_velocity_result_t pan_result;
	motion_velocity_result_t tilt_result;
	bool applied;
} motion_dual_velocity_result_t;

typedef enum {
	MOTION_HOME_OK = 0,
	MOTION_HOME_DRIVER_NOT_READY,
	MOTION_HOME_DIRECTION_UNCALIBRATED,
	MOTION_HOME_BUSY,
} motion_home_result_t;

typedef enum {
	MOTION_JOG_OK = 0,
	MOTION_JOG_RANGE,
	MOTION_JOG_DISABLED,
	MOTION_JOG_NOT_HOMED,
	MOTION_JOG_BUSY,
	MOTION_JOG_MIN_LIMIT,
} motion_jog_result_t;

typedef enum {
	MOTION_DIRECTION_CHECK_OK = 0,
	MOTION_DIRECTION_CHECK_DRIVER_NOT_READY,
	MOTION_DIRECTION_CHECK_ENDSTOP_ACTIVE,
	MOTION_DIRECTION_CHECK_BUSY,
	MOTION_DIRECTION_CHECK_ALREADY_CALIBRATED,
} motion_direction_check_result_t;

void motion_controller_init(void);
motion_enable_result_t motion_controller_enable(motion_axis_t axis);
axis_disable_result_t motion_controller_disable(motion_axis_t axis);
motion_velocity_result_t motion_controller_set_velocity(
	motion_axis_t axis, int32_t velocity_steps_s);
motion_dual_velocity_result_t motion_controller_set_both_velocities(
	int32_t pan_velocity_steps_s, int32_t tilt_velocity_steps_s);
void motion_controller_stop_all(void);
motion_home_result_t motion_controller_home_tilt(void);
motion_jog_result_t motion_controller_jog_tilt(int32_t steps);
motion_direction_check_result_t motion_controller_direction_check_tilt(
	bool direction_high);
void motion_controller_get_snapshot(motion_axis_t axis,
				    motion_controller_snapshot_t *snapshot);
bool motion_controller_axis_inactive(motion_axis_t axis);
void motion_controller_driver_fault(motion_axis_t axis);

#endif
