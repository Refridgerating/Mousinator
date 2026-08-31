#include "motion_controller.h"

#include "board.h"
#include "driver_control.h"
#include "stepper.h"

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/timer.h>

#define MOTION_SCHEDULER_TIMER TIM4
#define MOTION_SCHEDULER_TIMER_PRESCALER 71U
#define MOTION_SCHEDULER_TIMER_PERIOD 49U
#define MOTION_SCHEDULER_TIMER_IRQ_PRIORITY 0x40U

static axis_motion_state_t pan_state;
static axis_motion_state_t tilt_state;
static tilt_reference_state_t tilt_reference;
static uint8_t scheduler_ticks_until_control;

static void control_irq_disable(void)
{
	nvic_disable_irq(NVIC_TIM4_IRQ);
}

static void control_irq_enable(void)
{
	nvic_enable_irq(NVIC_TIM4_IRQ);
}

static axis_motion_state_t *axis_state(motion_axis_t axis)
{
	return axis == MOTION_AXIS_PAN ? &pan_state : &tilt_state;
}

static driver_axis_t driver_axis(motion_axis_t axis)
{
	return axis == MOTION_AXIS_PAN ? DRIVER_AXIS_PAN : DRIVER_AXIS_TILT;
}

static void stepper_set_enabled(motion_axis_t axis, bool enabled)
{
	if (axis == MOTION_AXIS_PAN) {
		stepper_pan_set_enabled(enabled);
	} else {
		stepper_tilt_set_enabled(enabled);
	}
}

static void stepper_set_direction(motion_axis_t axis, bool positive)
{
	if (axis == MOTION_AXIS_PAN) {
		stepper_pan_set_direction(positive);
	} else {
		stepper_tilt_set_direction(positive);
	}
}

static motion_velocity_result_t map_axis_velocity_result(
	axis_velocity_result_t result)
{
	switch (result) {
	case AXIS_VELOCITY_RANGE:
		return MOTION_VELOCITY_RANGE;
	case AXIS_VELOCITY_DISABLED:
		return MOTION_VELOCITY_DISABLED;
	case AXIS_VELOCITY_DISABLING:
		return MOTION_VELOCITY_DISABLING;
	case AXIS_VELOCITY_OK:
	default:
		return MOTION_VELOCITY_OK;
	}
}

static motion_velocity_result_t validate_tilt_velocity(int32_t velocity_steps_s)
{
	axis_velocity_result_t axis_result =
		axis_motion_validate_velocity(&tilt_state, velocity_steps_s);

	if (axis_result != AXIS_VELOCITY_OK) {
		return map_axis_velocity_result(axis_result);
	}
	if (tilt_reference.operation != TILT_OPERATION_IDLE) {
		return MOTION_VELOCITY_BUSY;
	}
	if (BOARD_TILT_DIRECTION_CALIBRATED == 0U) {
		return MOTION_VELOCITY_DIRECTION_UNCALIBRATED;
	}
	if (!tilt_reference.homed) {
		return MOTION_VELOCITY_NOT_HOMED;
	}
	if ((velocity_steps_s < 0) && (tilt_state.position_steps <= 0)) {
		return MOTION_VELOCITY_MIN_LIMIT;
	}
	return MOTION_VELOCITY_OK;
}

void motion_controller_init(void)
{
	axis_motion_init(&pan_state, PAN_MAX_ABSOLUTE_VELOCITY_STEPS_S);
	axis_motion_init(&tilt_state, TILT_MAX_ABSOLUTE_VELOCITY_STEPS_S);
	tilt_reference_init(&tilt_reference);
	scheduler_ticks_until_control =
		(uint8_t)MOTION_SCHEDULER_TICKS_PER_CONTROL_TICK;
	stepper_pan_init();
	stepper_tilt_init();

	/* APB1 timers receive 72 MHz; PSC=71 and ARR=49 produce 20 kHz. */
	rcc_periph_clock_enable(RCC_TIM4);
	rcc_periph_reset_pulse(RST_TIM4);
	timer_set_mode(MOTION_SCHEDULER_TIMER, TIM_CR1_CKD_CK_INT,
		       TIM_CR1_CMS_EDGE,
		       TIM_CR1_DIR_UP);
	timer_set_prescaler(MOTION_SCHEDULER_TIMER,
			    MOTION_SCHEDULER_TIMER_PRESCALER);
	timer_set_period(MOTION_SCHEDULER_TIMER,
			 MOTION_SCHEDULER_TIMER_PERIOD);
	timer_generate_event(MOTION_SCHEDULER_TIMER, TIM_EGR_UG);
	timer_clear_flag(MOTION_SCHEDULER_TIMER, TIM_SR_UIF);
	timer_enable_irq(MOTION_SCHEDULER_TIMER, TIM_DIER_UIE);
	nvic_set_priority(NVIC_TIM4_IRQ,
			  MOTION_SCHEDULER_TIMER_IRQ_PRIORITY);
	control_irq_enable();
	timer_enable_counter(MOTION_SCHEDULER_TIMER);
}

motion_enable_result_t motion_controller_enable(motion_axis_t axis)
{
	axis_motion_state_t *state = axis_state(axis);

	control_irq_disable();
	/* Authoritative readiness is rechecked at the active-low pin boundary. */
	if (!driver_control_ready(driver_axis(axis))) {
		stepper_set_enabled(axis, false);
		control_irq_enable();
		return MOTION_ENABLE_DRIVER_NOT_READY;
	}
	if (!state->enabled) {
		axis_motion_enable(state);
		stepper_set_direction(axis, true);
		stepper_set_enabled(axis, true);
	}
	control_irq_enable();
	return MOTION_ENABLE_OK;
}

axis_disable_result_t motion_controller_disable(motion_axis_t axis)
{
	axis_motion_state_t *state = axis_state(axis);
	axis_disable_result_t result;

	control_irq_disable();
	if (axis == MOTION_AXIS_TILT) {
		tilt_reference_on_disable(&tilt_reference, state);
	}
	result = axis_motion_request_disable(state);
	if (result == AXIS_DISABLE_COMPLETE) {
		stepper_set_enabled(axis, false);
	}
	control_irq_enable();
	return result;
}

motion_velocity_result_t motion_controller_set_velocity(
	motion_axis_t axis, int32_t velocity_steps_s)
{
	motion_velocity_result_t result;

	control_irq_disable();
	if (axis == MOTION_AXIS_TILT) {
		result = validate_tilt_velocity(velocity_steps_s);
		if (result == MOTION_VELOCITY_OK) {
			(void)axis_motion_set_target_velocity(&tilt_state,
						      velocity_steps_s);
		}
	} else {
		result = map_axis_velocity_result(axis_motion_set_target_velocity(
			&pan_state, velocity_steps_s));
	}
	control_irq_enable();
	return result;
}

motion_dual_velocity_result_t motion_controller_set_both_velocities(
	int32_t pan_velocity_steps_s, int32_t tilt_velocity_steps_s)
{
	motion_dual_velocity_result_t result;
	axis_velocity_result_t pan_result;

	control_irq_disable();
	/* Range-check both arguments before considering either axis state. */
	result.pan_result =
		((pan_velocity_steps_s > PAN_MAX_ABSOLUTE_VELOCITY_STEPS_S) ||
		 (pan_velocity_steps_s < -PAN_MAX_ABSOLUTE_VELOCITY_STEPS_S)) ?
			MOTION_VELOCITY_RANGE : MOTION_VELOCITY_OK;
	result.tilt_result =
		((tilt_velocity_steps_s > TILT_MAX_ABSOLUTE_VELOCITY_STEPS_S) ||
		 (tilt_velocity_steps_s < -TILT_MAX_ABSOLUTE_VELOCITY_STEPS_S)) ?
			MOTION_VELOCITY_RANGE : MOTION_VELOCITY_OK;
	result.applied = false;
	if ((result.pan_result != MOTION_VELOCITY_OK) ||
	    (result.tilt_result != MOTION_VELOCITY_OK)) {
		control_irq_enable();
		return result;
	}
	pan_result = axis_motion_validate_velocity(&pan_state,
						  pan_velocity_steps_s);
	result.pan_result = map_axis_velocity_result(pan_result);
	result.tilt_result = validate_tilt_velocity(tilt_velocity_steps_s);
	if ((result.pan_result == MOTION_VELOCITY_OK) &&
	    (result.tilt_result == MOTION_VELOCITY_OK)) {
		(void)axis_motion_set_target_velocity(&pan_state,
						      pan_velocity_steps_s);
		(void)axis_motion_set_target_velocity(&tilt_state,
						      tilt_velocity_steps_s);
		result.applied = true;
	}
	control_irq_enable();
	return result;
}

void motion_controller_stop_all(void)
{
	control_irq_disable();
	axis_motion_stop(&pan_state);
	tilt_reference_abort(&tilt_reference, &tilt_state);
	axis_motion_stop(&tilt_state);
	control_irq_enable();
}

motion_home_result_t motion_controller_home_tilt(void)
{
	control_irq_disable();
	if (BOARD_TILT_DIRECTION_CALIBRATED == 0U) {
		control_irq_enable();
		return MOTION_HOME_DIRECTION_UNCALIBRATED;
	}
	if ((tilt_reference.operation != TILT_OPERATION_IDLE) ||
	    tilt_state.moving || tilt_state.disabling) {
		control_irq_enable();
		return MOTION_HOME_BUSY;
	}
	if (!driver_control_ready(DRIVER_AXIS_TILT)) {
		stepper_tilt_set_enabled(false);
		control_irq_enable();
		return MOTION_HOME_DRIVER_NOT_READY;
	}
	if (!tilt_state.enabled) {
		axis_motion_enable(&tilt_state);
		stepper_tilt_set_direction(true);
		stepper_tilt_set_enabled(true);
	}
	tilt_reference_start_home(&tilt_reference,
				  board_tilt_endstop_triggered());
	control_irq_enable();
	return MOTION_HOME_OK;
}

motion_jog_result_t motion_controller_jog_tilt(int32_t steps)
{
	control_irq_disable();
	if ((steps == 0) || (steps > TILT_JOG_MAX_STEPS) ||
	    (steps < -TILT_JOG_MAX_STEPS)) {
		control_irq_enable();
		return MOTION_JOG_RANGE;
	}
	if (!tilt_state.enabled || tilt_state.disabling) {
		control_irq_enable();
		return MOTION_JOG_DISABLED;
	}
	if (!tilt_reference.homed) {
		control_irq_enable();
		return MOTION_JOG_NOT_HOMED;
	}
	if ((tilt_reference.operation != TILT_OPERATION_IDLE) ||
	    tilt_state.moving) {
		control_irq_enable();
		return MOTION_JOG_BUSY;
	}
	if ((steps < 0) && ((int64_t)(-steps) > tilt_state.position_steps)) {
		control_irq_enable();
		return MOTION_JOG_MIN_LIMIT;
	}
	tilt_reference_start_jog(&tilt_reference, steps);
	control_irq_enable();
	return MOTION_JOG_OK;
}

motion_direction_check_result_t motion_controller_direction_check_tilt(
	bool direction_high)
{
	control_irq_disable();
	if (BOARD_TILT_DIRECTION_CALIBRATED != 0U) {
		control_irq_enable();
		return MOTION_DIRECTION_CHECK_ALREADY_CALIBRATED;
	}
	if (tilt_state.enabled || tilt_state.moving || tilt_state.disabling ||
	    (tilt_reference.operation != TILT_OPERATION_IDLE)) {
		control_irq_enable();
		return MOTION_DIRECTION_CHECK_BUSY;
	}
	if (board_tilt_endstop_triggered()) {
		control_irq_enable();
		return MOTION_DIRECTION_CHECK_ENDSTOP_ACTIVE;
	}
	if (!driver_control_ready(DRIVER_AXIS_TILT)) {
		stepper_tilt_set_enabled(false);
		control_irq_enable();
		return MOTION_DIRECTION_CHECK_DRIVER_NOT_READY;
	}
	axis_motion_enable(&tilt_state);
	stepper_tilt_set_direction_raw(direction_high);
	stepper_tilt_set_enabled(true);
	tilt_reference_start_direction_check(&tilt_reference, direction_high);
	control_irq_enable();
	return MOTION_DIRECTION_CHECK_OK;
}

void motion_controller_get_snapshot(motion_axis_t axis,
				    motion_controller_snapshot_t *snapshot)
{
	const axis_motion_state_t *state;

	control_irq_disable();
	state = axis_state(axis);
	snapshot->position_steps = state->position_steps;
	snapshot->current_velocity_steps_s = state->current_velocity_steps_s;
	snapshot->target_velocity_steps_s = state->target_velocity_steps_s;
	snapshot->enabled = state->enabled;
	snapshot->moving = state->moving;
	snapshot->disabling = state->disabling;
	snapshot->motion_timeout = state->motion_timeout;
	snapshot->homed = axis == MOTION_AXIS_TILT && tilt_reference.homed;
	snapshot->homing = axis == MOTION_AXIS_TILT &&
		tilt_reference.operation == TILT_OPERATION_HOMING;
	snapshot->jogging = axis == MOTION_AXIS_TILT &&
		tilt_reference.operation == TILT_OPERATION_JOGGING;
	snapshot->direction_checking = axis == MOTION_AXIS_TILT &&
		tilt_reference.operation == TILT_OPERATION_DIRECTION_CHECKING;
	snapshot->direction_calibrated = axis == MOTION_AXIS_TILT &&
		BOARD_TILT_DIRECTION_CALIBRATED != 0U;
	snapshot->min_limit = axis == MOTION_AXIS_TILT &&
		tilt_reference.min_limit;
	snapshot->home_status = axis == MOTION_AXIS_TILT ?
		tilt_reference.home_status : TILT_HOME_STATUS_IDLE;
	control_irq_enable();
}

bool motion_controller_axis_inactive(motion_axis_t axis)
{
	motion_controller_snapshot_t snapshot;

	motion_controller_get_snapshot(axis, &snapshot);
	return !snapshot.enabled && !snapshot.moving && !snapshot.disabling &&
	       !snapshot.homing && !snapshot.jogging &&
	       !snapshot.direction_checking;
}

void motion_controller_driver_fault(motion_axis_t axis)
{
	axis_motion_state_t *state = axis_state(axis);

	control_irq_disable();
	if (axis == MOTION_AXIS_TILT) {
		tilt_reference_on_driver_fault(&tilt_reference, state);
	} else {
		axis_motion_stop(state);
	}
	if (axis_motion_request_disable(state) == AXIS_DISABLE_COMPLETE) {
		stepper_set_enabled(axis, false);
	}
	control_irq_enable();
}

void tim4_isr(void)
{
	axis_motion_control_result_t pan_control;
	axis_motion_control_result_t tilt_control;
	bool endstop_triggered;
	bool pan_emit_step;
	bool tilt_emit_step;

	if (!timer_get_flag(MOTION_SCHEDULER_TIMER, TIM_SR_UIF)) {
		return;
	}
	timer_clear_flag(MOTION_SCHEDULER_TIMER, TIM_SR_UIF);
	endstop_triggered = board_tilt_endstop_triggered();

	--scheduler_ticks_until_control;
	if (scheduler_ticks_until_control == 0U) {
		scheduler_ticks_until_control =
			(uint8_t)MOTION_SCHEDULER_TICKS_PER_CONTROL_TICK;
		tilt_reference_before_tick(&tilt_reference, &tilt_state,
					   endstop_triggered);
		pan_control = axis_motion_control_tick(&pan_state);
		tilt_control = axis_motion_control_tick(&tilt_state);

		if (pan_control.direction_changed) {
			stepper_pan_set_direction(pan_control.direction_positive);
		}
		if (tilt_control.direction_changed) {
			if (tilt_reference.operation ==
			    TILT_OPERATION_DIRECTION_CHECKING) {
				stepper_tilt_set_direction_raw(
					tilt_reference.direction_check_high);
			} else {
				stepper_tilt_set_direction(
					tilt_control.direction_positive);
			}
		}
		if (pan_control.disable_driver) {
			stepper_pan_set_enabled(false);
		}
		if (tilt_control.disable_driver) {
			stepper_tilt_set_enabled(false);
		}
		if (tilt_reference_take_disable_request(&tilt_reference)) {
			if (axis_motion_request_disable(&tilt_state) ==
			    AXIS_DISABLE_COMPLETE) {
				stepper_tilt_set_enabled(false);
			}
		}
	}

	pan_emit_step = axis_motion_scheduler_tick(&pan_state);
	tilt_emit_step = axis_motion_scheduler_tick(&tilt_state);
	if (pan_emit_step) {
		axis_motion_commit_step(&pan_state);
		stepper_pan_emit_pulse();
	}
	if (tilt_emit_step &&
	    tilt_reference_allow_step(&tilt_reference, &tilt_state,
				      endstop_triggered)) {
		axis_motion_commit_step(&tilt_state);
		stepper_tilt_emit_pulse();
		tilt_reference_step_committed(&tilt_reference, &tilt_state,
					      endstop_triggered);
	}
}
