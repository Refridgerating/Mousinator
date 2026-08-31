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

static void set_steady_velocity(axis_motion_state_t *state,
				int32_t velocity_steps_s)
{
	state->target_velocity_steps_s = velocity_steps_s;
	state->current_velocity_steps_s = velocity_steps_s;
	state->phase_accumulator = 0U;
	state->moving = velocity_steps_s != 0;
	state->direction_positive = velocity_steps_s >= 0;
}

static uint32_t run_scheduler_ticks(axis_motion_state_t *state,
				    uint32_t ticks)
{
	uint32_t emitted = 0U;
	uint32_t index;

	for (index = 0U; index < ticks; ++index) {
		if (axis_motion_scheduler_tick(state)) {
			axis_motion_commit_step(state);
			++emitted;
		}
	}
	return emitted;
}

static uint32_t run_control_intervals(axis_motion_state_t *state,
				      uint32_t intervals)
{
	uint32_t emitted = 0U;
	uint32_t index;

	for (index = 0U; index < intervals; ++index) {
		(void)axis_motion_control_tick(state);
		emitted += run_scheduler_ticks(
			state, MOTION_SCHEDULER_TICKS_PER_CONTROL_TICK);
	}
	return emitted;
}

static int test_scheduler_rates(void)
{
	static const int32_t rates[] = {1000, 333, 1234, 4999};
	axis_motion_state_t state;
	uint32_t index;
	int failures = 0;

	for (index = 0U; index < sizeof(rates) / sizeof(rates[0]); ++index) {
		axis_motion_init(&state, TILT_MAX_ABSOLUTE_VELOCITY_STEPS_S);
		axis_motion_enable(&state);
		set_steady_velocity(&state, rates[index]);
		failures += check(run_scheduler_ticks(
					  &state,
					  MOTION_SCHEDULER_FREQUENCY_HZ) ==
					  (uint32_t)rates[index],
			  "scheduler emits exact one-second step count");
		failures += check(state.position_steps == rates[index],
				  "position follows committed scheduler edges");
	}

	axis_motion_init(&state, TILT_MAX_ABSOLUTE_VELOCITY_STEPS_S);
	axis_motion_enable(&state);
	set_steady_velocity(&state, 333);
	failures += check(run_scheduler_ticks(
				  &state, MOTION_SCHEDULER_FREQUENCY_HZ * 3U) ==
				  999U && state.position_steps == 999,
			  "non-integer scheduler rate remains exact long term");

	axis_motion_init(&state, (int32_t)MOTION_SCHEDULER_FREQUENCY_HZ);
	axis_motion_enable(&state);
	set_steady_velocity(&state,
			    (int32_t)MOTION_SCHEDULER_FREQUENCY_HZ);
	failures += check(run_scheduler_ticks(
				  &state, MOTION_SCHEDULER_FREQUENCY_HZ) ==
				  MOTION_SCHEDULER_FREQUENCY_HZ,
			  "scheduler capacity supports one edge on every 20 kHz tick");
	return failures;
}

static int test_simultaneous_high_rates(void)
{
	axis_motion_state_t pan;
	axis_motion_state_t tilt;
	uint32_t index;
	uint32_t pan_edges = 0U;
	uint32_t tilt_edges = 0U;
	bool simultaneous = false;
	int failures = 0;

	axis_motion_init(&pan, PAN_MAX_ABSOLUTE_VELOCITY_STEPS_S);
	axis_motion_init(&tilt, TILT_MAX_ABSOLUTE_VELOCITY_STEPS_S);
	axis_motion_enable(&pan);
	axis_motion_enable(&tilt);
	set_steady_velocity(&pan, 10000);
	set_steady_velocity(&tilt, -4999);
	for (index = 0U; index < MOTION_SCHEDULER_FREQUENCY_HZ; ++index) {
		const bool pan_step = axis_motion_scheduler_tick(&pan);
		const bool tilt_step = axis_motion_scheduler_tick(&tilt);

		if (pan_step) {
			axis_motion_commit_step(&pan);
			++pan_edges;
		}
		if (tilt_step) {
			axis_motion_commit_step(&tilt);
			++tilt_edges;
		}
		if (pan_step && tilt_step) {
			simultaneous = true;
		}
	}
	failures += check(pan_edges == 10000U && pan.position_steps == 10000,
			  "PAN boundary rate emits 10000 exact edges");
	failures += check(tilt_edges == 4999U && tilt.position_steps == -4999,
			  "TILT high non-integer rate emits exact negative edges");
	failures += check(simultaneous,
			  "PAN and TILT may emit on one scheduler interrupt");
	return failures;
}

static int test_axis_specific_validation_and_atomicity(void)
{
	axis_motion_state_t pan;
	axis_motion_state_t tilt;
	axis_velocity_result_t pan_result;
	axis_velocity_result_t tilt_result;
	int failures = 0;

	axis_motion_init(&pan, PAN_MAX_ABSOLUTE_VELOCITY_STEPS_S);
	axis_motion_init(&tilt, TILT_MAX_ABSOLUTE_VELOCITY_STEPS_S);
	axis_motion_enable(&pan);
	axis_motion_enable(&tilt);
	failures += check(axis_motion_set_target_velocity(&pan, 10000) ==
				  AXIS_VELOCITY_OK,
			  "PAN accepts its 10000 steps/s boundary");
	failures += check(axis_motion_set_target_velocity(&pan, 10001) ==
				  AXIS_VELOCITY_RANGE &&
				  axis_motion_set_target_velocity(&pan, -10001) ==
				  AXIS_VELOCITY_RANGE,
			  "PAN rejects commands beyond its boundary");
	failures += check(axis_motion_set_target_velocity(&tilt, 5000) ==
				  AXIS_VELOCITY_OK &&
				  axis_motion_set_target_velocity(&tilt, -5000) ==
				  AXIS_VELOCITY_OK,
			  "TILT accepts both 5000 steps/s boundaries");
	failures += check(axis_motion_set_target_velocity(&tilt, 5001) ==
				  AXIS_VELOCITY_RANGE,
			  "TILT rejects commands beyond its boundary");

	pan.target_velocity_steps_s = 11;
	tilt.target_velocity_steps_s = 22;
	failures += check(!axis_motion_set_two_velocities(
				   &pan, 9000, &tilt, 5001, &pan_result,
				   &tilt_result),
			  "dual update rejects an over-limit TILT value");
	failures += check(pan.target_velocity_steps_s == 11 &&
				  tilt.target_velocity_steps_s == 22,
			  "failed dual validation mutates neither target");
	return failures;
}

static int test_control_rate_acceleration_reversal_and_lease(void)
{
	axis_motion_state_t state;
	bool saw_zero = false;
	bool negative_direction_change = false;
	uint32_t index;
	int failures = 0;

	axis_motion_init(&state, PAN_MAX_ABSOLUTE_VELOCITY_STEPS_S);
	axis_motion_enable(&state);
	(void)axis_motion_set_target_velocity(&state, 10000);
	(void)axis_motion_control_tick(&state);
	failures += check(state.current_velocity_steps_s == 2,
			  "one control tick applies one acceleration increment");
	(void)run_scheduler_ticks(
		&state, MOTION_SCHEDULER_TICKS_PER_CONTROL_TICK - 1U);
	failures += check(state.current_velocity_steps_s == 2,
			  "scheduler ticks do not scale acceleration");
	(void)axis_motion_control_tick(&state);
	failures += check(state.current_velocity_steps_s == 4,
			  "acceleration updates again only at the next control tick");
	(void)run_control_intervals(&state, 498U);
	failures += check(state.current_velocity_steps_s == 1000,
			  "500 control ticks represent exactly 500 ms of acceleration");

	(void)axis_motion_set_target_velocity(&state, -1000);
	for (index = 0U; index < 1001U; ++index) {
		axis_motion_control_result_t control =
			axis_motion_control_tick(&state);

		if (state.current_velocity_steps_s == 0) {
			saw_zero = true;
		}
		if (saw_zero && (state.current_velocity_steps_s < 0)) {
			negative_direction_change = control.direction_changed &&
				!control.direction_positive;
			break;
		}
	}
	failures += check(saw_zero && state.current_velocity_steps_s < 0 &&
				  negative_direction_change,
			  "reversal reaches zero before reporting its new direction");

	(void)axis_motion_set_target_velocity(&state, 100);
	for (index = 0U; index < MOTION_COMMAND_LEASE_TICKS - 1U; ++index) {
		(void)axis_motion_control_tick(&state);
	}
	failures += check(!state.motion_timeout,
			  "lease remains valid through 999 control milliseconds");
	(void)axis_motion_control_tick(&state);
	failures += check(state.motion_timeout &&
				  state.target_velocity_steps_s == 0,
			  "lease expires at exactly 1000 control milliseconds");
	return failures;
}

static int test_commit_and_disable_semantics(void)
{
	axis_motion_state_t pan;
	axis_motion_state_t tilt;
	uint32_t index;
	int failures = 0;

	axis_motion_init(&pan, PAN_MAX_ABSOLUTE_VELOCITY_STEPS_S);
	axis_motion_enable(&pan);
	set_steady_velocity(&pan, 10000);
	failures += check(!axis_motion_scheduler_tick(&pan) &&
				  axis_motion_scheduler_tick(&pan),
			  "10000 steps/s schedules one edge every two ticks");
	failures += check(pan.position_steps == 0,
			  "scheduled but uncommitted edge does not change position");
	axis_motion_commit_step(&pan);
	failures += check(pan.position_steps == 1,
			  "only an explicitly committed edge changes position");

	axis_motion_init(&tilt, TILT_MAX_ABSOLUTE_VELOCITY_STEPS_S);
	axis_motion_enable(&tilt);
	(void)axis_motion_set_target_velocity(&tilt, 200);
	(void)run_control_intervals(&tilt, 100U);
	failures += check(axis_motion_request_disable(&tilt) ==
				  AXIS_DISABLE_PENDING,
			  "moving disable remains a controlled transition");
	for (index = 0U; tilt.enabled && (index < 200U); ++index) {
		(void)axis_motion_control_tick(&tilt);
	}
	failures += check(!tilt.enabled && !tilt.moving,
			  "controlled disable completes at the control rate");
	return failures;
}

int main(void)
{
	int failures = 0;

	failures += test_scheduler_rates();
	failures += test_simultaneous_high_rates();
	failures += test_axis_specific_validation_and_atomicity();
	failures += test_control_rate_acceleration_reversal_and_lease();
	failures += test_commit_and_disable_semantics();
	if (failures == 0) {
		(void)puts("motion tests passed");
	}
	return failures == 0 ? 0 : 1;
}
