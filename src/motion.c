#include "motion.h"

#include <limits.h>

static int32_t approach_velocity(int32_t current, int32_t target)
{
	int32_t difference;

	if (current == target) {
		return current;
	}

	difference = target - current;
	if (difference > MOTION_VELOCITY_DELTA_PER_TICK) {
		return current + MOTION_VELOCITY_DELTA_PER_TICK;
	}
	if (difference < -MOTION_VELOCITY_DELTA_PER_TICK) {
		return current - MOTION_VELOCITY_DELTA_PER_TICK;
	}
	return target;
}

static void apply_velocity(axis_motion_state_t *state,
			   int32_t velocity_steps_s)
{
	state->target_velocity_steps_s = velocity_steps_s;
	if (velocity_steps_s == 0) {
		state->lease_ticks_remaining = 0U;
	} else {
		state->lease_ticks_remaining = MOTION_COMMAND_LEASE_TICKS;
		state->motion_timeout = false;
	}
}

void axis_motion_init(axis_motion_state_t *state,
		      int32_t max_absolute_velocity_steps_s)
{
	state->position_steps = 0;
	state->target_velocity_steps_s = 0;
	state->current_velocity_steps_s = 0;
	state->max_absolute_velocity_steps_s = max_absolute_velocity_steps_s;
	state->phase_accumulator = 0U;
	state->lease_ticks_remaining = 0U;
	state->enabled = false;
	state->moving = false;
	state->disabling = false;
	state->motion_timeout = false;
	state->direction_positive = true;
}

void axis_motion_enable(axis_motion_state_t *state)
{
	if (!state->enabled) {
		state->target_velocity_steps_s = 0;
		state->current_velocity_steps_s = 0;
		state->phase_accumulator = 0U;
		state->lease_ticks_remaining = 0U;
		state->moving = false;
		state->disabling = false;
		state->direction_positive = true;
		state->enabled = true;
	}
}

axis_disable_result_t axis_motion_request_disable(axis_motion_state_t *state)
{
	state->target_velocity_steps_s = 0;
	state->lease_ticks_remaining = 0U;

	if (!state->enabled || (state->current_velocity_steps_s == 0)) {
		axis_motion_force_stop(state);
		state->disabling = false;
		state->enabled = false;
		return AXIS_DISABLE_COMPLETE;
	}

	state->disabling = true;
	return AXIS_DISABLE_PENDING;
}

axis_velocity_result_t axis_motion_validate_velocity(
	const axis_motion_state_t *state, int32_t velocity_steps_s)
{
	if ((velocity_steps_s > state->max_absolute_velocity_steps_s) ||
	    (velocity_steps_s < -state->max_absolute_velocity_steps_s)) {
		return AXIS_VELOCITY_RANGE;
	}
	if (!state->enabled) {
		return AXIS_VELOCITY_DISABLED;
	}
	if (state->disabling) {
		return AXIS_VELOCITY_DISABLING;
	}
	return AXIS_VELOCITY_OK;
}

axis_velocity_result_t axis_motion_set_target_velocity(
	axis_motion_state_t *state, int32_t velocity_steps_s)
{
	axis_velocity_result_t result =
		axis_motion_validate_velocity(state, velocity_steps_s);

	if (result == AXIS_VELOCITY_OK) {
		apply_velocity(state, velocity_steps_s);
	}
	return result;
}

bool axis_motion_set_two_velocities(axis_motion_state_t *first,
				    int32_t first_velocity_steps_s,
				    axis_motion_state_t *second,
				    int32_t second_velocity_steps_s,
				    axis_velocity_result_t *first_result,
				    axis_velocity_result_t *second_result)
{
	*first_result = axis_motion_validate_velocity(first,
						     first_velocity_steps_s);
	*second_result = axis_motion_validate_velocity(second,
						      second_velocity_steps_s);
	if ((*first_result != AXIS_VELOCITY_OK) ||
	    (*second_result != AXIS_VELOCITY_OK)) {
		return false;
	}

	apply_velocity(first, first_velocity_steps_s);
	apply_velocity(second, second_velocity_steps_s);
	return true;
}

void axis_motion_set_internal_velocity(axis_motion_state_t *state,
				       int32_t velocity_steps_s)
{
	state->target_velocity_steps_s = velocity_steps_s;
	state->lease_ticks_remaining = 0U;
	state->motion_timeout = false;
}

void axis_motion_stop(axis_motion_state_t *state)
{
	state->target_velocity_steps_s = 0;
	state->lease_ticks_remaining = 0U;
}

void axis_motion_force_stop(axis_motion_state_t *state)
{
	state->target_velocity_steps_s = 0;
	state->current_velocity_steps_s = 0;
	state->phase_accumulator = 0U;
	state->lease_ticks_remaining = 0U;
	state->moving = false;
}

void axis_motion_set_position(axis_motion_state_t *state,
			      int64_t position_steps)
{
	state->position_steps = position_steps;
}

axis_motion_control_result_t axis_motion_control_tick(
	axis_motion_state_t *state)
{
	axis_motion_control_result_t result = {
		.direction_changed = false,
		.direction_positive = state->direction_positive,
		.disable_driver = false,
	};
	int32_t ramp_target = state->target_velocity_steps_s;

	if (state->lease_ticks_remaining > 0U) {
		--state->lease_ticks_remaining;
		if (state->lease_ticks_remaining == 0U) {
			state->target_velocity_steps_s = 0;
			state->motion_timeout = true;
			ramp_target = 0;
		}
	}

	/* A reversal must reach an explicit zero-velocity tick first. */
	if (((state->current_velocity_steps_s > 0) && (ramp_target < 0)) ||
	    ((state->current_velocity_steps_s < 0) && (ramp_target > 0))) {
		ramp_target = 0;
	}

	state->current_velocity_steps_s = approach_velocity(
		state->current_velocity_steps_s, ramp_target);
	state->moving = state->current_velocity_steps_s != 0;

	if (!state->moving) {
		state->phase_accumulator = 0U;
		if (state->disabling) {
			state->disabling = false;
			state->enabled = false;
			result.disable_driver = true;
		}
		return result;
	}

	if (state->current_velocity_steps_s > 0) {
		if (!state->direction_positive) {
			state->direction_positive = true;
			result.direction_changed = true;
		}
	} else {
		if (state->direction_positive) {
			state->direction_positive = false;
			result.direction_changed = true;
		}
	}
	result.direction_positive = state->direction_positive;
	return result;
}

bool axis_motion_scheduler_tick(axis_motion_state_t *state)
{
	uint32_t absolute_velocity;

	if (state->current_velocity_steps_s == 0) {
		state->phase_accumulator = 0U;
		return false;
	}
	absolute_velocity = state->current_velocity_steps_s > 0 ?
		(uint32_t)state->current_velocity_steps_s :
		(uint32_t)(-state->current_velocity_steps_s);
	state->phase_accumulator += absolute_velocity;
	if (state->phase_accumulator < MOTION_SCHEDULER_FREQUENCY_HZ) {
		return false;
	}
	state->phase_accumulator -= MOTION_SCHEDULER_FREQUENCY_HZ;
	return true;
}

void axis_motion_commit_step(axis_motion_state_t *state)
{
	/* Define wrap explicitly rather than relying on signed-overflow behavior. */
	if (state->direction_positive) {
		if (state->position_steps == INT64_MAX) {
			state->position_steps = INT64_MIN;
		} else {
			++state->position_steps;
		}
	} else if (state->position_steps == INT64_MIN) {
		state->position_steps = INT64_MAX;
	} else {
		--state->position_steps;
	}
}
