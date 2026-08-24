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

static void record_step(pan_motion_state_t *state)
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

void pan_motion_init(pan_motion_state_t *state)
{
	state->position_steps = 0;
	state->target_velocity_steps_s = 0;
	state->current_velocity_steps_s = 0;
	state->phase_accumulator = 0U;
	state->lease_ticks_remaining = 0U;
	state->enabled = false;
	state->moving = false;
	state->disabling = false;
	state->motion_timeout = false;
	state->direction_positive = true;
}

void pan_motion_enable(pan_motion_state_t *state)
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

pan_disable_result_t pan_motion_request_disable(pan_motion_state_t *state)
{
	state->target_velocity_steps_s = 0;
	state->lease_ticks_remaining = 0U;

	if (!state->enabled || (state->current_velocity_steps_s == 0)) {
		state->current_velocity_steps_s = 0;
		state->phase_accumulator = 0U;
		state->moving = false;
		state->disabling = false;
		state->enabled = false;
		return PAN_DISABLE_COMPLETE;
	}

	state->disabling = true;
	return PAN_DISABLE_PENDING;
}

pan_velocity_result_t pan_motion_set_target_velocity(
	pan_motion_state_t *state, int32_t velocity_steps_s)
{
	if ((velocity_steps_s > PAN_MAX_ABSOLUTE_VELOCITY_STEPS_S) ||
	    (velocity_steps_s < -PAN_MAX_ABSOLUTE_VELOCITY_STEPS_S)) {
		return PAN_VELOCITY_RANGE;
	}
	if (!state->enabled) {
		return PAN_VELOCITY_DISABLED;
	}
	if (state->disabling) {
		return PAN_VELOCITY_DISABLING;
	}

	state->target_velocity_steps_s = velocity_steps_s;
	if (velocity_steps_s == 0) {
		state->lease_ticks_remaining = 0U;
	} else {
		state->lease_ticks_remaining = MOTION_COMMAND_LEASE_TICKS;
		state->motion_timeout = false;
	}
	return PAN_VELOCITY_OK;
}

void pan_motion_stop(pan_motion_state_t *state)
{
	state->target_velocity_steps_s = 0;
	state->lease_ticks_remaining = 0U;
}

pan_motion_tick_result_t pan_motion_tick(pan_motion_state_t *state)
{
	pan_motion_tick_result_t result = {
		.emit_step = false,
		.direction_changed = false,
		.direction_positive = state->direction_positive,
		.disable_driver = false,
	};
	int32_t ramp_target = state->target_velocity_steps_s;
	uint32_t absolute_velocity;

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
		absolute_velocity = (uint32_t)state->current_velocity_steps_s;
		if (!state->direction_positive) {
			state->direction_positive = true;
			result.direction_changed = true;
		}
	} else {
		absolute_velocity = (uint32_t)(-state->current_velocity_steps_s);
		if (state->direction_positive) {
			state->direction_positive = false;
			result.direction_changed = true;
		}
	}
	result.direction_positive = state->direction_positive;

	state->phase_accumulator += absolute_velocity;
	if (state->phase_accumulator >= MOTION_CONTROL_FREQUENCY_HZ) {
		state->phase_accumulator -= MOTION_CONTROL_FREQUENCY_HZ;
		result.emit_step = true;
		record_step(state);
	}

	return result;
}
