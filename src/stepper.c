#include "stepper.h"

#include "board.h"

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/timer.h>

#define PAN_STEP_TIMER TIM2
#define PAN_STEP_TIMER_PRESCALER 71U
#define PAN_STEP_TIMER_PERIOD 3U
#define PAN_STEP_TIMER_RISE_TICK 2U

void stepper_pan_init(void)
{
	board_pan_set_enabled(false);
	board_pan_set_direction(true);

	rcc_periph_clock_enable(RCC_TIM2);
	rcc_periph_reset_pulse(RST_TIM2);
	timer_set_mode(PAN_STEP_TIMER, TIM_CR1_CKD_CK_INT, TIM_CR1_CMS_EDGE,
		       TIM_CR1_DIR_UP);
	timer_set_prescaler(PAN_STEP_TIMER, PAN_STEP_TIMER_PRESCALER);
	timer_set_period(PAN_STEP_TIMER, PAN_STEP_TIMER_PERIOD);
	timer_one_shot_mode(PAN_STEP_TIMER);

	/*
	 * PWM2 is low until CCR3 and high until the update event. TIM2_CH3 on
	 * PB10 therefore produces a rising edge at 2 us and a falling edge at
	 * 4 us, then remains low after the one-shot stops.
	 */
	timer_set_oc_mode(PAN_STEP_TIMER, TIM_OC3, TIM_OCM_PWM2);
	timer_set_oc_value(PAN_STEP_TIMER, TIM_OC3, PAN_STEP_TIMER_RISE_TICK);
	timer_set_oc_polarity_high(PAN_STEP_TIMER, TIM_OC3);
	timer_enable_oc_output(PAN_STEP_TIMER, TIM_OC3);

	/* Load preloaded registers while STEP is still an ordinary low GPIO. */
	timer_generate_event(PAN_STEP_TIMER, TIM_EGR_UG);
	timer_clear_flag(PAN_STEP_TIMER, TIM_SR_UIF);
	timer_set_counter(PAN_STEP_TIMER, 0U);
	board_pan_step_use_timer();
}

void stepper_pan_set_enabled(bool enabled)
{
	board_pan_set_enabled(enabled);
}

void stepper_pan_set_direction(bool positive)
{
	board_pan_set_direction(positive);
}

void stepper_pan_emit_pulse(void)
{
	/* The 4 us one-shot always completes before the next 1 ms control tick. */
	timer_set_counter(PAN_STEP_TIMER, 0U);
	timer_clear_flag(PAN_STEP_TIMER, TIM_SR_UIF);
	timer_enable_counter(PAN_STEP_TIMER);
}
