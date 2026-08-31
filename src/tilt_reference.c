#include "tilt_reference.h"

static void update_stability(tilt_reference_state_t *reference,
			     bool endstop_triggered)
{
	if (endstop_triggered) {
		if (reference->stable_trigger_ticks < TILT_ENDSTOP_STABLE_TICKS) {
			++reference->stable_trigger_ticks;
		}
		reference->stable_release_ticks = 0U;
	} else {
		if (reference->stable_release_ticks < TILT_ENDSTOP_STABLE_TICKS) {
			++reference->stable_release_ticks;
		}
		reference->stable_trigger_ticks = 0U;
	}
}

static bool trigger_is_stable(const tilt_reference_state_t *reference)
{
	return reference->stable_trigger_ticks >= TILT_ENDSTOP_STABLE_TICKS;
}

static bool release_is_stable(const tilt_reference_state_t *reference)
{
	return reference->stable_release_ticks >= TILT_ENDSTOP_STABLE_TICKS;
}

static void fail_home(tilt_reference_state_t *reference,
		      axis_motion_state_t *axis, tilt_home_status_t status)
{
	axis_motion_force_stop(axis);
	reference->operation = TILT_OPERATION_IDLE;
	reference->home_phase = TILT_HOME_PHASE_NONE;
	reference->home_status = status;
	reference->homed = false;
	reference->min_limit = false;
	reference->disable_requested = true;
}

void tilt_reference_init(tilt_reference_state_t *reference)
{
	reference->operation = TILT_OPERATION_IDLE;
	reference->home_phase = TILT_HOME_PHASE_NONE;
	reference->home_status = TILT_HOME_STATUS_IDLE;
	reference->phase_steps = 0U;
	reference->stable_trigger_ticks = 0U;
	reference->stable_release_ticks = 0U;
	reference->operation_steps_remaining = 0U;
	reference->homed = false;
	reference->min_limit = false;
	reference->operation_positive = true;
	reference->direction_check_high = false;
	reference->disable_requested = false;
	reference->normal_trigger_latched = false;
}

void tilt_reference_start_home(tilt_reference_state_t *reference,
			       bool endstop_triggered)
{
	reference->operation = TILT_OPERATION_HOMING;
	reference->home_phase = endstop_triggered ?
		TILT_HOME_PHASE_INITIAL_RELEASE : TILT_HOME_PHASE_FAST_APPROACH;
	reference->home_status = TILT_HOME_STATUS_RUNNING;
	reference->phase_steps = 0U;
	reference->stable_trigger_ticks = endstop_triggered ? 1U : 0U;
	reference->stable_release_ticks = endstop_triggered ? 0U : 1U;
	reference->operation_steps_remaining = 0U;
	reference->homed = false;
	reference->min_limit = false;
	reference->disable_requested = false;
	reference->normal_trigger_latched = false;
}

void tilt_reference_start_jog(tilt_reference_state_t *reference, int32_t steps)
{
	const uint32_t magnitude = steps > 0 ? (uint32_t)steps :
		(uint32_t)(-(steps + 1)) + 1U;

	reference->operation = TILT_OPERATION_JOGGING;
	reference->operation_positive = steps > 0;
	reference->operation_steps_remaining = magnitude;
	reference->disable_requested = false;
}

void tilt_reference_start_direction_check(tilt_reference_state_t *reference,
					  bool direction_high)
{
	reference->operation = TILT_OPERATION_DIRECTION_CHECKING;
	reference->operation_positive = true;
	reference->direction_check_high = direction_high;
	reference->operation_steps_remaining = TILT_DIRECTION_CHECK_STEPS;
	reference->homed = false;
	reference->min_limit = false;
	reference->disable_requested = false;
}

static void update_home(tilt_reference_state_t *reference,
			axis_motion_state_t *axis, bool endstop_triggered)
{
	switch (reference->home_phase) {
	case TILT_HOME_PHASE_INITIAL_RELEASE:
		if (release_is_stable(reference)) {
			axis_motion_force_stop(axis);
			reference->home_phase = TILT_HOME_PHASE_FAST_APPROACH;
			reference->phase_steps = 0U;
			return;
		}
		if (reference->phase_steps >=
		    TILT_HOME_RELEASE_SEARCH_MAX_STEPS) {
			axis_motion_force_stop(axis);
			if (endstop_triggered) {
				fail_home(reference, axis,
					  TILT_HOME_STATUS_SWITCH_STUCK);
			}
			return;
		}
		axis_motion_set_internal_velocity(
			axis, TILT_HOME_BACKOFF_VELOCITY_STEPS_S);
		break;

	case TILT_HOME_PHASE_RELEASE_SEARCH:
		if (release_is_stable(reference)) {
			axis_motion_force_stop(axis);
			reference->home_phase =
				TILT_HOME_PHASE_POST_RELEASE_CLEARANCE;
			reference->phase_steps = 0U;
			return;
		}
		if (reference->phase_steps >=
		    TILT_HOME_RELEASE_SEARCH_MAX_STEPS) {
			axis_motion_force_stop(axis);
			if (endstop_triggered) {
				fail_home(reference, axis,
					  TILT_HOME_STATUS_SWITCH_STUCK);
			}
			return;
		}
		axis_motion_set_internal_velocity(
			axis, TILT_HOME_BACKOFF_VELOCITY_STEPS_S);
		break;

	case TILT_HOME_PHASE_POST_RELEASE_CLEARANCE:
		if (reference->phase_steps >=
		    TILT_HOME_POST_RELEASE_CLEARANCE_STEPS) {
			axis_motion_force_stop(axis);
			reference->home_phase = TILT_HOME_PHASE_SLOW_APPROACH;
			reference->phase_steps = 0U;
			return;
		}
		axis_motion_set_internal_velocity(
			axis, TILT_HOME_BACKOFF_VELOCITY_STEPS_S);
		break;

	case TILT_HOME_PHASE_FAST_APPROACH:
	case TILT_HOME_PHASE_SLOW_APPROACH:
		if (endstop_triggered) {
			/* Never schedule another negative edge into the switch. */
			axis_motion_force_stop(axis);
			if (trigger_is_stable(reference)) {
				if (reference->home_phase ==
				    TILT_HOME_PHASE_FAST_APPROACH) {
					reference->home_phase =
						TILT_HOME_PHASE_RELEASE_SEARCH;
					reference->phase_steps = 0U;
				} else {
					axis_motion_set_position(axis, 0);
					reference->home_phase =
						TILT_HOME_PHASE_POST_HOME_PARK;
					reference->phase_steps = 0U;
					reference->min_limit = true;
				}
			}
			return;
		}
		if (reference->phase_steps >=
		    (reference->home_phase == TILT_HOME_PHASE_FAST_APPROACH ?
			     TILT_HOME_MAX_TRAVEL_STEPS :
			     TILT_HOME_RELEASE_SEARCH_MAX_STEPS +
				     TILT_HOME_POST_RELEASE_CLEARANCE_STEPS)) {
			fail_home(reference, axis, TILT_HOME_STATUS_NOT_FOUND);
			return;
		}
		axis_motion_set_internal_velocity(
			axis, reference->home_phase == TILT_HOME_PHASE_FAST_APPROACH ?
				      -TILT_HOME_FAST_VELOCITY_STEPS_S :
				      -TILT_HOME_SLOW_VELOCITY_STEPS_S);
		break;

	case TILT_HOME_PHASE_POST_HOME_PARK:
		if (reference->phase_steps >= TILT_HOME_POST_HOME_PARK_STEPS) {
			axis_motion_force_stop(axis);
			reference->operation = TILT_OPERATION_IDLE;
			reference->home_phase = TILT_HOME_PHASE_NONE;
			reference->home_status = TILT_HOME_STATUS_SUCCESS;
			reference->homed = true;
			reference->min_limit = false;
			return;
		}
		axis_motion_set_internal_velocity(
			axis, TILT_HOME_POST_HOME_VELOCITY_STEPS_S);
		break;

	case TILT_HOME_PHASE_NONE:
	default:
		fail_home(reference, axis, TILT_HOME_STATUS_ABORTED);
		break;
	}
}

void tilt_reference_before_tick(tilt_reference_state_t *reference,
				axis_motion_state_t *axis,
				bool endstop_triggered)
{
	update_stability(reference, endstop_triggered);

	if (reference->operation == TILT_OPERATION_HOMING) {
		update_home(reference, axis, endstop_triggered);
		return;
	}
	if (reference->operation == TILT_OPERATION_JOGGING) {
		axis_motion_set_internal_velocity(
			axis, reference->operation_positive ?
				      TILT_JOG_VELOCITY_STEPS_S :
				      -TILT_JOG_VELOCITY_STEPS_S);
		return;
	}
	if (reference->operation == TILT_OPERATION_DIRECTION_CHECKING) {
		if (endstop_triggered) {
			axis_motion_force_stop(axis);
			reference->operation = TILT_OPERATION_IDLE;
			reference->operation_steps_remaining = 0U;
			reference->disable_requested = true;
			return;
		}
		axis_motion_set_internal_velocity(
			axis, TILT_DIRECTION_CHECK_VELOCITY_STEPS_S);
		return;
	}

	if (reference->homed && endstop_triggered &&
	    ((axis->current_velocity_steps_s < 0) ||
	     (axis->target_velocity_steps_s < 0))) {
		reference->normal_trigger_latched = true;
		axis_motion_force_stop(axis);
	}
	if (reference->normal_trigger_latched && trigger_is_stable(reference)) {
		axis_motion_set_position(axis, 0);
		reference->min_limit = true;
		reference->normal_trigger_latched = false;
	} else if (reference->normal_trigger_latched && !endstop_triggered) {
		reference->normal_trigger_latched = false;
	}
	if (reference->homed && (axis->position_steps <= 0) &&
	    ((axis->current_velocity_steps_s < 0) ||
	     (axis->target_velocity_steps_s < 0))) {
		axis_motion_force_stop(axis);
	}
	if (reference->homed && (axis->position_steps == 0)) {
		reference->min_limit = true;
	} else if (!endstop_triggered) {
		reference->min_limit = false;
	}
}

bool tilt_reference_allow_step(const tilt_reference_state_t *reference,
			       const axis_motion_state_t *axis,
			       bool endstop_triggered)
{
	if (!axis->direction_positive && endstop_triggered) {
		return false;
	}
	if (!axis->direction_positive && reference->homed &&
	    (axis->position_steps <= 0)) {
		return false;
	}
	if (reference->operation == TILT_OPERATION_HOMING) {
		switch (reference->home_phase) {
		case TILT_HOME_PHASE_INITIAL_RELEASE:
		case TILT_HOME_PHASE_RELEASE_SEARCH:
			if (reference->phase_steps >=
			    TILT_HOME_RELEASE_SEARCH_MAX_STEPS) {
				return false;
			}
			break;
		case TILT_HOME_PHASE_FAST_APPROACH:
			if (reference->phase_steps >=
			    TILT_HOME_MAX_TRAVEL_STEPS) {
				return false;
			}
			break;
		case TILT_HOME_PHASE_POST_RELEASE_CLEARANCE:
			if (reference->phase_steps >=
			    TILT_HOME_POST_RELEASE_CLEARANCE_STEPS) {
				return false;
			}
			break;
		case TILT_HOME_PHASE_SLOW_APPROACH:
			if (reference->phase_steps >=
			    TILT_HOME_RELEASE_SEARCH_MAX_STEPS +
				    TILT_HOME_POST_RELEASE_CLEARANCE_STEPS) {
				return false;
			}
			break;
		case TILT_HOME_PHASE_POST_HOME_PARK:
			if (reference->phase_steps >=
			    TILT_HOME_POST_HOME_PARK_STEPS) {
				return false;
			}
			break;
		case TILT_HOME_PHASE_NONE:
		default:
			break;
		}
	}
	if (((reference->operation == TILT_OPERATION_JOGGING) ||
	     (reference->operation == TILT_OPERATION_DIRECTION_CHECKING)) &&
	    (reference->operation_steps_remaining == 0U)) {
		return false;
	}
	return true;
}

void tilt_reference_step_committed(tilt_reference_state_t *reference,
				   axis_motion_state_t *axis,
				   bool endstop_triggered)
{
	(void)endstop_triggered;
	if (reference->operation == TILT_OPERATION_HOMING) {
		++reference->phase_steps;
	} else if ((reference->operation == TILT_OPERATION_JOGGING) ||
		   (reference->operation == TILT_OPERATION_DIRECTION_CHECKING)) {
		if (reference->operation_steps_remaining > 0U) {
			--reference->operation_steps_remaining;
		}
		if (reference->operation_steps_remaining == 0U) {
			const bool was_direction_check =
				reference->operation ==
				TILT_OPERATION_DIRECTION_CHECKING;

			axis_motion_force_stop(axis);
			reference->operation = TILT_OPERATION_IDLE;
			if (was_direction_check) {
				reference->disable_requested = true;
			}
		}
	}
	if (reference->homed ||
	    (reference->home_phase == TILT_HOME_PHASE_POST_HOME_PARK)) {
		reference->min_limit = axis->position_steps == 0;
	}
}

void tilt_reference_abort(tilt_reference_state_t *reference,
			  axis_motion_state_t *axis)
{
	const bool was_homing = reference->operation == TILT_OPERATION_HOMING;
	const bool was_direction_check =
		reference->operation == TILT_OPERATION_DIRECTION_CHECKING;

	axis_motion_stop(axis);
	reference->operation = TILT_OPERATION_IDLE;
	reference->home_phase = TILT_HOME_PHASE_NONE;
	reference->operation_steps_remaining = 0U;
	if (was_homing) {
		reference->home_status = TILT_HOME_STATUS_ABORTED;
		reference->homed = false;
		reference->min_limit = false;
	}
	if (was_direction_check) {
		reference->disable_requested = true;
	}
}

void tilt_reference_on_disable(tilt_reference_state_t *reference,
			       axis_motion_state_t *axis)
{
	const bool was_homing = reference->operation == TILT_OPERATION_HOMING;

	axis_motion_stop(axis);
	reference->operation = TILT_OPERATION_IDLE;
	reference->home_phase = TILT_HOME_PHASE_NONE;
	reference->home_status = was_homing ? TILT_HOME_STATUS_ABORTED :
					       TILT_HOME_STATUS_IDLE;
	reference->operation_steps_remaining = 0U;
	reference->homed = false;
	reference->min_limit = false;
	reference->disable_requested = false;
	reference->normal_trigger_latched = false;
}

void tilt_reference_on_driver_fault(tilt_reference_state_t *reference,
				    axis_motion_state_t *axis)
{
	axis_motion_stop(axis);
	reference->operation = TILT_OPERATION_IDLE;
	reference->home_phase = TILT_HOME_PHASE_NONE;
	reference->home_status = TILT_HOME_STATUS_DRIVER_FAULT;
	reference->operation_steps_remaining = 0U;
	reference->homed = false;
	reference->min_limit = false;
	reference->disable_requested = true;
	reference->normal_trigger_latched = false;
}

bool tilt_reference_take_disable_request(tilt_reference_state_t *reference)
{
	const bool requested = reference->disable_requested;

	reference->disable_requested = false;
	return requested;
}

const char *tilt_home_status_name(tilt_home_status_t status)
{
	switch (status) {
	case TILT_HOME_STATUS_RUNNING:
		return "RUNNING";
	case TILT_HOME_STATUS_SUCCESS:
		return "SUCCESS";
	case TILT_HOME_STATUS_SWITCH_STUCK:
		return "SWITCH_STUCK";
	case TILT_HOME_STATUS_NOT_FOUND:
		return "NOT_FOUND";
	case TILT_HOME_STATUS_DRIVER_FAULT:
		return "DRIVER_FAULT";
	case TILT_HOME_STATUS_ABORTED:
		return "ABORTED";
	case TILT_HOME_STATUS_IDLE:
	default:
		return "IDLE";
	}
}
