#include "tilt_reference.h"

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

static bool run_tick(tilt_reference_state_t *reference,
		     axis_motion_state_t *axis, bool triggered)
{
	axis_motion_tick_result_t tick;

	tilt_reference_before_tick(reference, axis, triggered);
	tick = axis_motion_tick(axis);
	if (tick.emit_step &&
	    tilt_reference_allow_step(reference, axis, triggered)) {
		axis_motion_commit_step(axis);
		tilt_reference_step_committed(reference, axis, triggered);
		return true;
	}
	return false;
}

static uint32_t run_until_steps(tilt_reference_state_t *reference,
				axis_motion_state_t *axis, bool triggered,
				uint32_t requested)
{
	uint32_t emitted = 0U;
	uint32_t ticks = 0U;

	while ((emitted < requested) && (ticks < 200000U)) {
		if (run_tick(reference, axis, triggered)) {
			++emitted;
		}
		++ticks;
	}
	return emitted;
}

static void init_enabled(axis_motion_state_t *axis,
			 tilt_reference_state_t *reference)
{
	axis_motion_init(axis);
	axis_motion_enable(axis);
	tilt_reference_init(reference);
}

static int test_commissioning_constants(void)
{
	int failures = 0;

	failures += check(TILT_HOME_FAST_VELOCITY_STEPS_S == 500,
			  "fast home velocity is 500 steps/s");
	failures += check(TILT_HOME_SLOW_VELOCITY_STEPS_S == 100,
			  "slow home velocity is 100 steps/s");
	failures += check(TILT_HOME_MAX_TRAVEL_STEPS == 8000U,
			  "fast home search is bounded at 8000 steps");
	failures += check(TILT_HOME_RELEASE_SEARCH_MAX_STEPS == 500U,
			  "release search is bounded at 500 steps");
	failures += check(TILT_HOME_POST_RELEASE_CLEARANCE_STEPS == 100U,
			  "post-release clearance is exactly 100 steps");
	failures += check(TILT_ENDSTOP_STABLE_TICKS == 5U,
			  "endstop qualification remains five control ticks");
	return failures;
}

static int test_successful_home(void)
{
	axis_motion_state_t axis;
	tilt_reference_state_t reference;
	int64_t clearance_start_position;
	int64_t release_search_start_position;
	uint32_t index;
	int failures = 0;

	init_enabled(&axis, &reference);
	tilt_reference_start_home(&reference, false);
	failures += check(run_until_steps(&reference, &axis, false, 1500U) ==
				  1500U && axis.position_steps == -1500 &&
				  reference.home_phase ==
					  TILT_HOME_PHASE_FAST_APPROACH,
			  "fast home search succeeds beyond the old 1000-step bound");
	(void)run_tick(&reference, &axis, true);
	failures += check(axis.position_steps == -1500 &&
				  axis.current_velocity_steps_s == 0,
			  "raw first trigger immediately suppresses negative edges");
	for (index = 1U; index < TILT_ENDSTOP_STABLE_TICKS; ++index) {
		(void)run_tick(&reference, &axis, true);
	}
	failures += check(reference.home_phase ==
				  TILT_HOME_PHASE_RELEASE_SEARCH &&
				  axis.current_velocity_steps_s == 0,
			  "first trigger immediately stops and starts release search");
	release_search_start_position = axis.position_steps;
	failures += check(run_until_steps(&reference, &axis, true, 75U) == 75U &&
				  axis.position_steps ==
					  release_search_start_position + 75,
			  "release search remains active beyond 50 steps");
	for (index = 0U; index < TILT_ENDSTOP_STABLE_TICKS; ++index) {
		(void)run_tick(&reference, &axis, false);
	}
	failures += check(reference.home_phase ==
				  TILT_HOME_PHASE_POST_RELEASE_CLEARANCE,
			  "stable release starts post-release clearance");
	clearance_start_position = axis.position_steps;
	failures += check(run_until_steps(&reference, &axis, false,
					 TILT_HOME_POST_RELEASE_CLEARANCE_STEPS) ==
				  TILT_HOME_POST_RELEASE_CLEARANCE_STEPS &&
				  axis.position_steps == clearance_start_position +
					  TILT_HOME_POST_RELEASE_CLEARANCE_STEPS,
			  "stable release is followed by exactly 100 clearance steps");
	(void)run_tick(&reference, &axis, false);
	failures += check(reference.home_phase == TILT_HOME_PHASE_SLOW_APPROACH &&
				  axis.current_velocity_steps_s == 0,
			  "clearance completion stops before slow re-approach");
	(void)run_tick(&reference, &axis, false);
	failures += check(axis.target_velocity_steps_s ==
				  -TILT_HOME_SLOW_VELOCITY_STEPS_S,
			  "second approach uses configured slow negative velocity");
	(void)run_until_steps(&reference, &axis, false, 3U);
	for (index = 0U; index < TILT_ENDSTOP_STABLE_TICKS; ++index) {
		(void)run_tick(&reference, &axis, true);
	}
	failures += check(reference.homed &&
				  reference.home_status == TILT_HOME_STATUS_SUCCESS &&
				  axis.position_steps == 0 && reference.min_limit,
			  "second trigger establishes exact zero");
	return failures;
}

static int test_triggered_start_and_failure(void)
{
	axis_motion_state_t axis;
	tilt_reference_state_t reference;
	uint32_t index;
	int failures = 0;

	init_enabled(&axis, &reference);
	tilt_reference_start_home(&reference, true);
	failures += check(reference.home_phase ==
				  TILT_HOME_PHASE_INITIAL_RELEASE,
			  "active-at-start switch backs off first");
	(void)run_until_steps(&reference, &axis, true,
			      TILT_HOME_RELEASE_SEARCH_MAX_STEPS);
	(void)run_tick(&reference, &axis, true);
	failures += check(reference.home_status ==
				  TILT_HOME_STATUS_SWITCH_STUCK &&
				  !reference.homed && reference.disable_requested,
			  "bounded release failure leaves unhomed");

	init_enabled(&axis, &reference);
	tilt_reference_start_home(&reference, false);
	(void)run_until_steps(&reference, &axis, false,
			      TILT_HOME_MAX_TRAVEL_STEPS);
	(void)run_tick(&reference, &axis, false);
	failures += check(reference.home_status == TILT_HOME_STATUS_NOT_FOUND &&
				  !reference.homed,
			  "bounded search failure leaves unhomed");

	init_enabled(&axis, &reference);
	tilt_reference_start_home(&reference, true);
	for (index = 0U; index < TILT_ENDSTOP_STABLE_TICKS; ++index) {
		(void)run_tick(&reference, &axis, false);
	}
	failures += check(reference.home_phase == TILT_HOME_PHASE_FAST_APPROACH,
			  "released active-at-start switch proceeds to fast seek");
	return failures;
}

static int test_post_trigger_release_failure(void)
{
	axis_motion_state_t axis;
	tilt_reference_state_t reference;
	uint32_t index;
	int failures = 0;

	init_enabled(&axis, &reference);
	tilt_reference_start_home(&reference, false);
	(void)run_until_steps(&reference, &axis, false, 20U);
	for (index = 0U; index < TILT_ENDSTOP_STABLE_TICKS; ++index) {
		(void)run_tick(&reference, &axis, true);
	}
	failures += check(reference.home_phase ==
				  TILT_HOME_PHASE_RELEASE_SEARCH,
			  "first trigger enters bounded release search");
	(void)run_until_steps(&reference, &axis, true,
			      TILT_HOME_RELEASE_SEARCH_MAX_STEPS);
	(void)run_tick(&reference, &axis, true);
	failures += check(reference.home_status ==
				  TILT_HOME_STATUS_SWITCH_STUCK &&
				  reference.operation == TILT_OPERATION_IDLE &&
				  !reference.homed && reference.disable_requested,
			  "switch still active after 500 release steps fails stuck");
	return failures;
}

static int test_exact_jog_and_minimum(void)
{
	axis_motion_state_t axis;
	tilt_reference_state_t reference;
	int failures = 0;

	init_enabled(&axis, &reference);
	reference.homed = true;
	axis_motion_set_position(&axis, 100);
	tilt_reference_start_jog(&reference, 50);
	failures += check(run_until_steps(&reference, &axis, false, 50U) == 50U,
			  "positive JOG emits exact edge count");
	failures += check(axis.position_steps == 150 &&
				  reference.operation == TILT_OPERATION_IDLE,
			  "positive JOG ends at exact target");

	tilt_reference_start_jog(&reference, -50);
	failures += check(run_until_steps(&reference, &axis, false, 50U) == 50U,
			  "negative JOG emits exact edge count");
	failures += check(axis.position_steps == 100,
			  "negative JOG preserves independent absolute position");

	axis_motion_set_position(&axis, 0);
	axis_motion_set_internal_velocity(&axis, -50);
	(void)run_tick(&reference, &axis, false);
	failures += check(axis.position_steps == 0 && !axis.moving &&
				  axis.target_velocity_steps_s == 0,
			  "homed minimum blocks negative edge below zero");
	return failures;
}

static int test_normal_endstop_and_direction_check(void)
{
	axis_motion_state_t axis;
	tilt_reference_state_t reference;
	uint32_t index;
	int failures = 0;

	init_enabled(&axis, &reference);
	reference.homed = true;
	axis_motion_set_position(&axis, 25);
	axis_motion_set_internal_velocity(&axis, -100);
	for (index = 0U; index < TILT_ENDSTOP_STABLE_TICKS; ++index) {
		(void)run_tick(&reference, &axis, true);
	}
	failures += check(axis.position_steps == 0 && reference.homed &&
				  reference.min_limit,
			  "normal negative switch hit safely re-zeroes");

	init_enabled(&axis, &reference);
	tilt_reference_start_direction_check(&reference, true);
	failures += check(run_until_steps(&reference, &axis, false,
					 TILT_DIRECTION_CHECK_STEPS) ==
				  TILT_DIRECTION_CHECK_STEPS,
			  "direction check emits exactly ten hardware edges");
	failures += check(reference.operation == TILT_OPERATION_IDLE &&
				  reference.disable_requested && !reference.homed,
			  "direction check self-terminates and requests disable");

	init_enabled(&axis, &reference);
	tilt_reference_start_direction_check(&reference, false);
	(void)run_until_steps(&reference, &axis, false, 2U);
	(void)run_tick(&reference, &axis, true);
	failures += check(reference.operation == TILT_OPERATION_IDLE &&
				  reference.disable_requested && !axis.moving,
			  "direction check stops on either raw-level endstop hit");
	return failures;
}

static int test_home_does_not_modify_pan(void)
{
	axis_motion_state_t pan;
	axis_motion_state_t tilt;
	tilt_reference_state_t reference;
	int64_t pan_position_before;
	uint32_t index;
	int failures = 0;

	axis_motion_init(&pan);
	axis_motion_enable(&pan);
	(void)axis_motion_set_target_velocity(&pan, 100);
	init_enabled(&tilt, &reference);
	tilt_reference_start_home(&reference, false);
	for (index = 0U; index < 500U; ++index) {
		axis_motion_tick_result_t pan_tick = axis_motion_tick(&pan);

		(void)run_tick(&reference, &tilt, false);
		if (pan_tick.emit_step) {
			axis_motion_commit_step(&pan);
		}
	}
	pan_position_before = pan.position_steps;
	tilt_reference_abort(&reference, &tilt);
	failures += check(pan.position_steps == pan_position_before &&
				  pan.target_velocity_steps_s == 100 && pan.enabled,
			  "TILT home and abort leave PAN state untouched");
	return failures;
}

static int test_disable_and_reset_clear_home(void)
{
	axis_motion_state_t axis;
	tilt_reference_state_t reference;
	int failures = 0;

	init_enabled(&axis, &reference);
	reference.homed = true;
	reference.home_status = TILT_HOME_STATUS_SUCCESS;
	tilt_reference_on_disable(&reference, &axis);
	failures += check(!reference.homed &&
				  reference.home_status == TILT_HOME_STATUS_IDLE,
			  "disable clears established home reference");

	tilt_reference_start_home(&reference, false);
	tilt_reference_on_disable(&reference, &axis);
	failures += check(!reference.homed &&
				  reference.home_status == TILT_HOME_STATUS_ABORTED,
			  "disable reports an interrupted home");
	tilt_reference_init(&reference);
	failures += check(!reference.homed &&
				  reference.operation == TILT_OPERATION_IDLE &&
				  reference.home_status == TILT_HOME_STATUS_IDLE,
			  "reset initialization performs no homing motion");
	return failures;
}

int main(void)
{
	int failures = 0;

	failures += test_commissioning_constants();
	failures += test_successful_home();
	failures += test_triggered_start_and_failure();
	failures += test_post_trigger_release_failure();
	failures += test_exact_jog_and_minimum();
	failures += test_normal_endstop_and_direction_check();
	failures += test_home_does_not_modify_pan();
	failures += test_disable_and_reset_clear_home();
	if (failures == 0) {
		(void)puts("tilt reference tests passed");
	}
	return failures == 0 ? 0 : 1;
}
