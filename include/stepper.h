#ifndef SENTRY_STEPPER_H
#define SENTRY_STEPPER_H

#include <stdbool.h>

void stepper_pan_init(void);
void stepper_pan_set_enabled(bool enabled);
void stepper_pan_set_direction(bool positive);
void stepper_pan_emit_pulse(void);

#endif
