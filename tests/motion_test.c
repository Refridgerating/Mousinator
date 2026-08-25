#include "motion.h"

#include <stdbool.h>
#include <stdio.h>

static int check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}

static uint32_t run_ticks(axis_motion_state_t *state, uint32_t ticks)
{
	uint32_t emitted = 0U;
	uint32_t index;

	for (index = 0U; index < ticks; ++index) {
		axis_motion_tick_result_t result = axis_motion_tick(state);

		if (result.emit_step) {
			axis_motion_commit_step(state);
			++emitted;
		}
	}
	return emitted;
}

static int test_acceleration_and_position(void)
{
	axis_motion_state_t state;
	int failures = 0;

	axis_motion_init(&state);
	axis_motion_enable(&state);
	failures += check(axis_motion_set_target_velocity(&state, 800) ==
				  AXIS_VELOCITY_OK,
			  "accept positive velocity");
	failures += check(run_ticks(&state, 500U) == 240U,
			  "acceleration emits deterministic steps in 500 ms");
	failures += check(state.current_velocity_steps_s == 800,
			  "velocity reaches 800 steps/s");
	failures += check(state.position_steps == 240,
			  "committed rising edges define position");

	axis_motion_stop(&state);
	(void)run_ticks(&state, 400U);
	failures += check(state.current_velocity_steps_s == 0 && !state.moving,
			  "controlled stop reaches zero");
	return failures;
}

static int test_reversal_and_deadman(void)
{
	axis_motion_state_t state;
	bool saw_zero = false;
	uint32_t index;
	int failures = 0;

	axis_motion_init(&state);
	axis_motion_enable(&state);
	(void)axis_motion_set_target_velocity(&state, 300);
	(void)run_ticks(&state, 200U);
	(void)axis_motion_set_target_velocity(&state, -300);
	for (index = 0U; index < 400U; ++index) {
		axis_motion_tick_result_t result = axis_motion_tick(&state);

		if (state.current_velocity_steps_s == 0) {
			saw_zero = true;
		}
		if (result.emit_step) {
			axis_motion_commit_step(&state);
		}
	}
	failures += check(saw_zero && state.current_velocity_steps_s < 0,
			  "reversal passes through zero");

	(void)axis_motion_set_target_velocity(&state, 100);
	(void)run_ticks(&state, MOTION_COMMAND_LEASE_TICKS);
	failures += check(state.motion_timeout &&
				  state.target_velocity_steps_s == 0,
			  "independent lease expiry targets zero");
	return failures;
}

static int test_validation_and_disable(void)
{
	axis_motion_state_t state;
	int failures = 0;

	axis_motion_init(&state);
	failures += check(axis_motion_set_target_velocity(&state, 1) ==
				  AXIS_VELOCITY_DISABLED,
			  "disabled rejection");
	axis_motion_enable(&state);
	failures += check(axis_motion_set_target_velocity(&state, 1001) ==
				  AXIS_VELOCITY_RANGE,
			  "positive range rejection");
	failures += check(axis_motion_set_target_velocity(&state, -1001) ==
				  AXIS_VELOCITY_RANGE,
			  "negative range rejection");
	(void)axis_motion_set_target_velocity(&state, 200);
	(void)run_ticks(&state, 100U);
	failures += check(axis_motion_request_disable(&state) ==
				  AXIS_DISABLE_PENDING,
			  "moving disable is pending");
	while (state.enabled) {
		(void)axis_motion_tick(&state);
	}
	failures += check(!state.enabled && !state.moving,
			  "controlled disable completes");
	return failures;
}

static int test_two_axis_atomicity_and_independence(void)
{
	axis_motion_state_t pan;
	axis_motion_state_t tilt;
	axis_velocity_result_t pan_result;
	axis_velocity_result_t tilt_result;
	uint32_t index;
	bool saw_simultaneous_step = false;
	int failures = 0;

	axis_motion_init(&pan);
	axis_motion_init(&tilt);
	axis_motion_enable(&pan);
	axis_motion_enable(&tilt);
	failures += check(axis_motion_set_two_velocities(
				  &pan, 300, &tilt, -200, &pan_result,
				  &tilt_result),
			  "valid dual velocity applies");
	for (index = 0U; index < 1000U; ++index) {
		axis_motion_tick_result_t pan_tick = axis_motion_tick(&pan);
		axis_motion_tick_result_t tilt_tick = axis_motion_tick(&tilt);

		if (pan_tick.emit_step) {
			axis_motion_commit_step(&pan);
		}
		if (tilt_tick.emit_step) {
			axis_motion_commit_step(&tilt);
		}
		if (pan_tick.emit_step && tilt_tick.emit_step) {
			saw_simultaneous_step = true;
		}
	}
	failures += check(pan.position_steps > 0 && tilt.position_steps < 0,
			  "simultaneous opposite directions are independent");
	failures += check(saw_simultaneous_step,
			  "both axes may emit in the same control tick");

	pan.target_velocity_steps_s = 11;
	tilt.target_velocity_steps_s = 22;
	failures += check(!axis_motion_set_two_velocities(
				   &pan, 400, &tilt, 1001, &pan_result,
				   &tilt_result),
			  "range failure rejects dual update");
	failures += check(pan.target_velocity_steps_s == 11 &&
				  tilt.target_velocity_steps_s == 22,
			  "failed dual update mutates neither axis");
	return failures;
}

static int test_independent_leases_and_disable(void)
{
	axis_motion_state_t pan;
	axis_motion_state_t tilt;
	uint32_t index;
	int failures = 0;

	axis_motion_init(&pan);
	axis_motion_init(&tilt);
	axis_motion_enable(&pan);
	axis_motion_enable(&tilt);
	(void)axis_motion_set_target_velocity(&pan, 100);
	(void)axis_motion_set_target_velocity(&tilt, 100);
	for (index = 0U; index < MOTION_COMMAND_LEASE_TICKS; ++index) {
		axis_motion_tick_result_t pan_tick;
		axis_motion_tick_result_t tilt_tick;

		if ((index % 100U) == 0U) {
			(void)axis_motion_set_target_velocity(&pan, 100);
		}
		pan_tick = axis_motion_tick(&pan);
		tilt_tick = axis_motion_tick(&tilt);
		if (pan_tick.emit_step) {
			axis_motion_commit_step(&pan);
		}
		if (tilt_tick.emit_step) {
			axis_motion_commit_step(&tilt);
		}
	}
	failures += check(!pan.motion_timeout && pan.target_velocity_steps_s == 100,
			  "refreshed PAN lease continues");
	failures += check(tilt.motion_timeout &&
				  tilt.target_velocity_steps_s == 0,
			  "expired TILT lease stops only TILT");

	(void)axis_motion_request_disable(&tilt);
	for (index = 0U; index < 100U; ++index) {
		(void)axis_motion_tick(&tilt);
		(void)axis_motion_set_target_velocity(&pan, 100);
		(void)axis_motion_tick(&pan);
	}
	failures += check(!tilt.enabled && pan.enabled &&
				  pan.target_velocity_steps_s == 100,
			  "disabling TILT does not stop PAN");
	return failures;
}

int main(void)
{
	int failures = 0;

	failures += test_acceleration_and_position();
	failures += test_reversal_and_deadman();
	failures += test_validation_and_disable();
	failures += test_two_axis_atomicity_and_independence();
	failures += test_independent_leases_and_disable();
	if (failures == 0) {
		(void)puts("motion tests passed");
	}
	return failures == 0 ? 0 : 1;
}
