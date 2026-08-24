#include "motion.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static int check(bool condition, const char *name)
{
	if (!condition) {
		(void)fprintf(stderr, "FAIL: %s\n", name);
		return 1;
	}
	return 0;
}

static int test_acceleration(void)
{
	pan_motion_state_t state;
	int failures = 0;

	pan_motion_init(&state);
	pan_motion_enable(&state);
	failures += check(pan_motion_set_target_velocity(&state, 10) ==
			  PAN_VELOCITY_OK, "accept positive velocity");
	(void)pan_motion_tick(&state);
	failures += check(state.current_velocity_steps_s == 2,
			  "positive acceleration tick");
	(void)pan_motion_tick(&state);
	failures += check(state.current_velocity_steps_s == 4,
			  "positive acceleration second tick");

	pan_motion_init(&state);
	pan_motion_enable(&state);
	(void)pan_motion_set_target_velocity(&state, -10);
	(void)pan_motion_tick(&state);
	failures += check(state.current_velocity_steps_s == -2,
			  "negative acceleration tick");
	return failures;
}

static int test_stop_and_reversal(void)
{
	pan_motion_state_t state;
	bool observed_zero = false;
	bool observed_negative = false;
	int failures = 0;
	uint32_t tick;

	pan_motion_init(&state);
	pan_motion_enable(&state);
	(void)pan_motion_set_target_velocity(&state, 8);
	for (tick = 0U; tick < 4U; ++tick) {
		(void)pan_motion_tick(&state);
	}
	pan_motion_stop(&state);
	(void)pan_motion_tick(&state);
	failures += check(state.current_velocity_steps_s == 6,
			  "controlled deceleration");

	(void)pan_motion_set_target_velocity(&state, -8);
	for (tick = 0U; tick < 12U; ++tick) {
		(void)pan_motion_tick(&state);
		if (state.current_velocity_steps_s == 0) {
			observed_zero = true;
		}
		if (state.current_velocity_steps_s < 0) {
			observed_negative = true;
			break;
		}
	}
	failures += check(observed_zero, "reversal passes through zero");
	failures += check(observed_negative, "reversal reaches negative velocity");
	return failures;
}

static int test_limits_and_disabled(void)
{
	pan_motion_state_t state;
	int failures = 0;

	pan_motion_init(&state);
	failures += check(pan_motion_set_target_velocity(&state, 1) ==
			  PAN_VELOCITY_DISABLED, "disabled rejection");
	pan_motion_enable(&state);
	failures += check(pan_motion_set_target_velocity(&state, 1000) ==
			  PAN_VELOCITY_OK, "positive maximum");
	failures += check(pan_motion_set_target_velocity(&state, -1000) ==
			  PAN_VELOCITY_OK, "negative maximum");
	failures += check(pan_motion_set_target_velocity(&state, 1001) ==
			  PAN_VELOCITY_RANGE, "positive range rejection");
	failures += check(pan_motion_set_target_velocity(&state, -1001) ==
			  PAN_VELOCITY_RANGE, "negative range rejection");
	return failures;
}

static int test_phase_and_position(void)
{
	pan_motion_state_t state;
	pan_motion_tick_result_t result;
	uint32_t tick;
	uint32_t steps = 0U;
	int failures = 0;

	pan_motion_init(&state);
	pan_motion_enable(&state);
	state.current_velocity_steps_s = 200;
	state.target_velocity_steps_s = 200;
	state.lease_ticks_remaining = MOTION_COMMAND_LEASE_TICKS;
	for (tick = 0U; tick < 100U; ++tick) {
		result = pan_motion_tick(&state);
		if (result.emit_step) {
			++steps;
		}
	}
	failures += check(steps == 20U, "phase accumulator positive rate");
	failures += check(state.position_steps == 20,
			  "positive position accounting");

	state.current_velocity_steps_s = -200;
	state.target_velocity_steps_s = -200;
	state.direction_positive = false;
	state.phase_accumulator = 0U;
	state.lease_ticks_remaining = MOTION_COMMAND_LEASE_TICKS;
	for (tick = 0U; tick < 100U; ++tick) {
		(void)pan_motion_tick(&state);
	}
	failures += check(state.position_steps == 0,
			  "negative position accounting");
	return failures;
}

static int test_deadman(void)
{
	pan_motion_state_t state;
	uint32_t tick;
	int failures = 0;

	pan_motion_init(&state);
	pan_motion_enable(&state);
	(void)pan_motion_set_target_velocity(&state, 100);
	for (tick = 0U; tick < 500U; ++tick) {
		(void)pan_motion_tick(&state);
	}
	(void)pan_motion_set_target_velocity(&state, 100);
	for (tick = 0U; tick < (MOTION_COMMAND_LEASE_TICKS - 1U); ++tick) {
		(void)pan_motion_tick(&state);
	}
	failures += check(!state.motion_timeout,
			  "refreshed lease valid before boundary");
	(void)pan_motion_tick(&state);
	failures += check(state.motion_timeout, "lease expires at boundary");
	failures += check(state.target_velocity_steps_s == 0,
			  "timeout targets zero");
	(void)pan_motion_set_target_velocity(&state, 50);
	failures += check(!state.motion_timeout, "nonzero velocity clears timeout");
	return failures;
}

static int test_pending_disable(void)
{
	pan_motion_state_t state;
	pan_motion_tick_result_t result = {false, false, true, false};
	int failures = 0;
	uint32_t tick;

	pan_motion_init(&state);
	pan_motion_enable(&state);
	state.current_velocity_steps_s = 6;
	state.target_velocity_steps_s = 6;
	failures += check(pan_motion_request_disable(&state) ==
			  PAN_DISABLE_PENDING, "moving disable is pending");
	failures += check(pan_motion_set_target_velocity(&state, 1) ==
			  PAN_VELOCITY_DISABLING,
			  "velocity rejected while disabling");
	for (tick = 0U; tick < 4U; ++tick) {
		result = pan_motion_tick(&state);
		if (result.disable_driver) {
			break;
		}
	}
	failures += check(result.disable_driver, "disable completes at zero");
	failures += check(!state.enabled && !state.moving,
			  "completed disable state");
	return failures;
}

int main(void)
{
	int failures = 0;

	failures += test_acceleration();
	failures += test_stop_and_reversal();
	failures += test_limits_and_disabled();
	failures += test_phase_and_position();
	failures += test_deadman();
	failures += test_pending_disable();

	if (failures == 0) {
		(void)puts("motion tests passed");
	}
	return failures;
}
