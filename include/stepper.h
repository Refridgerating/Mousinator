#ifndef SENTRY_STEPPER_H
#define SENTRY_STEPPER_H

#include <stdbool.h>

void stepper_pan_init(void);
void stepper_pan_set_enabled(bool enabled);
void stepper_pan_set_direction(bool positive);
void stepper_pan_emit_pulse(void);

void stepper_tilt_init(void);
void stepper_tilt_set_enabled(bool enabled);
void stepper_tilt_set_direction(bool positive);
void stepper_tilt_set_direction_raw(bool high);
void stepper_tilt_emit_pulse(void);

#endif
