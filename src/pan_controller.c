#include "pan_controller.h"

#include "stepper.h"

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/timer.h>

#define CONTROL_TIMER TIM3
#define CONTROL_TIMER_PRESCALER 71U
#define CONTROL_TIMER_PERIOD 999U
#define CONTROL_TIMER_IRQ_PRIORITY 0x40U

static pan_motion_state_t pan_state;

static void control_irq_disable(void)
{
	nvic_disable_irq(NVIC_TIM3_IRQ);
}

static void control_irq_enable(void)
{
	nvic_enable_irq(NVIC_TIM3_IRQ);
}

void pan_controller_init(void)
{
	pan_motion_init(&pan_state);
	stepper_pan_init();

	/*
	 * APB1 is 36 MHz, and STM32F1 timer clocks double when the APB prescaler
	 * is not one. TIM3 therefore receives 72 MHz; PSC=71 and ARR=999 give a
	 * deterministic 1 kHz control and phase-accumulator tick.
	 */
	rcc_periph_clock_enable(RCC_TIM3);
	rcc_periph_reset_pulse(RST_TIM3);
	timer_set_mode(CONTROL_TIMER, TIM_CR1_CKD_CK_INT, TIM_CR1_CMS_EDGE,
		       TIM_CR1_DIR_UP);
	timer_set_prescaler(CONTROL_TIMER, CONTROL_TIMER_PRESCALER);
	timer_set_period(CONTROL_TIMER, CONTROL_TIMER_PERIOD);
	timer_generate_event(CONTROL_TIMER, TIM_EGR_UG);
	timer_clear_flag(CONTROL_TIMER, TIM_SR_UIF);
	timer_enable_irq(CONTROL_TIMER, TIM_DIER_UIE);
	nvic_set_priority(NVIC_TIM3_IRQ, CONTROL_TIMER_IRQ_PRIORITY);
	control_irq_enable();
	timer_enable_counter(CONTROL_TIMER);
}

void pan_controller_enable(void)
{
	control_irq_disable();
	if (!pan_state.enabled) {
		pan_motion_enable(&pan_state);
		stepper_pan_set_direction(true);
		stepper_pan_set_enabled(true);
	}
	control_irq_enable();
}

pan_disable_result_t pan_controller_disable(void)
{
	pan_disable_result_t result;

	control_irq_disable();
	result = pan_motion_request_disable(&pan_state);
	if (result == PAN_DISABLE_COMPLETE) {
		stepper_pan_set_enabled(false);
	}
	control_irq_enable();
	return result;
}

pan_velocity_result_t pan_controller_set_velocity(int32_t velocity_steps_s)
{
	pan_velocity_result_t result;

	control_irq_disable();
	result = pan_motion_set_target_velocity(&pan_state, velocity_steps_s);
	control_irq_enable();
	return result;
}

void pan_controller_stop(void)
{
	control_irq_disable();
	pan_motion_stop(&pan_state);
	control_irq_enable();
}

void pan_controller_get_snapshot(pan_controller_snapshot_t *snapshot)
{
	control_irq_disable();
	snapshot->position_steps = pan_state.position_steps;
	snapshot->current_velocity_steps_s = pan_state.current_velocity_steps_s;
	snapshot->target_velocity_steps_s = pan_state.target_velocity_steps_s;
	snapshot->enabled = pan_state.enabled;
	snapshot->moving = pan_state.moving;
	snapshot->disabling = pan_state.disabling;
	snapshot->motion_timeout = pan_state.motion_timeout;
	control_irq_enable();
}

void tim3_isr(void)
{
	pan_motion_tick_result_t result;

	if (!timer_get_flag(CONTROL_TIMER, TIM_SR_UIF)) {
		return;
	}
	timer_clear_flag(CONTROL_TIMER, TIM_SR_UIF);

	result = pan_motion_tick(&pan_state);
	if (result.direction_changed) {
		stepper_pan_set_direction(result.direction_positive);
	}
	if (result.emit_step) {
		stepper_pan_emit_pulse();
	}
	if (result.disable_driver) {
		/* The previous 4 us one-shot ended at least 996 us earlier. */
		stepper_pan_set_enabled(false);
	}
}
