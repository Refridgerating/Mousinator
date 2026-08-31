#ifndef SENTRY_TILT_REFERENCE_H
#define SENTRY_TILT_REFERENCE_H

#include "motion.h"

#include <stdbool.h>
#include <stdint.h>

#define TILT_HOME_FAST_VELOCITY_STEPS_S 4000
#define TILT_HOME_BACKOFF_VELOCITY_STEPS_S 3000
#define TILT_HOME_SLOW_VELOCITY_STEPS_S 400
#define TILT_HOME_POST_HOME_VELOCITY_STEPS_S 2000
#define TILT_HOME_MAX_TRAVEL_STEPS 20000U
#define TILT_HOME_RELEASE_SEARCH_MAX_STEPS 500U
#define TILT_HOME_POST_RELEASE_CLEARANCE_STEPS 100U
#define TILT_HOME_POST_HOME_PARK_STEPS 800U
#define TILT_ENDSTOP_STABLE_TICKS 5U
#define TILT_DIRECTION_CHECK_STEPS 10U
#define TILT_DIRECTION_CHECK_VELOCITY_STEPS_S 25
#define TILT_JOG_MAX_STEPS 50
#define TILT_JOG_VELOCITY_STEPS_S 50

#if ((TILT_HOME_FAST_VELOCITY_STEPS_S > \
	TILT_MAX_ABSOLUTE_VELOCITY_STEPS_S) || \
	(TILT_HOME_BACKOFF_VELOCITY_STEPS_S > \
	TILT_MAX_ABSOLUTE_VELOCITY_STEPS_S) || \
	(TILT_HOME_SLOW_VELOCITY_STEPS_S > \
	TILT_MAX_ABSOLUTE_VELOCITY_STEPS_S) || \
	(TILT_HOME_POST_HOME_VELOCITY_STEPS_S > \
	TILT_MAX_ABSOLUTE_VELOCITY_STEPS_S))
#error "TILT homing velocities must not exceed the TILT axis limit"
#endif

typedef enum {
	TILT_HOME_STATUS_IDLE = 0,
	TILT_HOME_STATUS_RUNNING,
	TILT_HOME_STATUS_SUCCESS,
	TILT_HOME_STATUS_SWITCH_STUCK,
	TILT_HOME_STATUS_NOT_FOUND,
	TILT_HOME_STATUS_DRIVER_FAULT,
	TILT_HOME_STATUS_ABORTED,
} tilt_home_status_t;

typedef enum {
	TILT_OPERATION_IDLE = 0,
	TILT_OPERATION_HOMING,
	TILT_OPERATION_JOGGING,
	TILT_OPERATION_DIRECTION_CHECKING,
} tilt_operation_t;

typedef enum {
	TILT_HOME_PHASE_NONE = 0,
	TILT_HOME_PHASE_INITIAL_RELEASE,
	TILT_HOME_PHASE_FAST_APPROACH,
	TILT_HOME_PHASE_RELEASE_SEARCH,
	TILT_HOME_PHASE_POST_RELEASE_CLEARANCE,
	TILT_HOME_PHASE_SLOW_APPROACH,
	TILT_HOME_PHASE_POST_HOME_PARK,
} tilt_home_phase_t;

typedef struct {
	tilt_operation_t operation;
	tilt_home_phase_t home_phase;
	tilt_home_status_t home_status;
	uint32_t phase_steps;
	uint32_t stable_trigger_ticks;
	uint32_t stable_release_ticks;
	uint32_t operation_steps_remaining;
	bool homed;
	bool min_limit;
	bool operation_positive;
	bool direction_check_high;
	bool disable_requested;
	bool normal_trigger_latched;
} tilt_reference_state_t;

void tilt_reference_init(tilt_reference_state_t *reference);
void tilt_reference_start_home(tilt_reference_state_t *reference,
			       bool endstop_triggered);
void tilt_reference_start_jog(tilt_reference_state_t *reference,
			      int32_t steps);
void tilt_reference_start_direction_check(tilt_reference_state_t *reference,
					  bool direction_high);
void tilt_reference_before_tick(tilt_reference_state_t *reference,
				axis_motion_state_t *axis,
				bool endstop_triggered);
bool tilt_reference_allow_step(const tilt_reference_state_t *reference,
			       const axis_motion_state_t *axis,
			       bool endstop_triggered);
void tilt_reference_step_committed(tilt_reference_state_t *reference,
				   axis_motion_state_t *axis,
				   bool endstop_triggered);
void tilt_reference_abort(tilt_reference_state_t *reference,
			  axis_motion_state_t *axis);
void tilt_reference_on_disable(tilt_reference_state_t *reference,
			       axis_motion_state_t *axis);
void tilt_reference_on_driver_fault(tilt_reference_state_t *reference,
				    axis_motion_state_t *axis);
bool tilt_reference_take_disable_request(tilt_reference_state_t *reference);
const char *tilt_home_status_name(tilt_home_status_t status);

#endif
