#ifndef SENTRY_PAN_CONTROLLER_H
#define SENTRY_PAN_CONTROLLER_H

#include "motion.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	int64_t position_steps;
	int32_t current_velocity_steps_s;
	int32_t target_velocity_steps_s;
	bool enabled;
	bool moving;
	bool disabling;
	bool motion_timeout;
} pan_controller_snapshot_t;

void pan_controller_init(void);
void pan_controller_enable(void);
pan_disable_result_t pan_controller_disable(void);
pan_velocity_result_t pan_controller_set_velocity(int32_t velocity_steps_s);
void pan_controller_stop(void);
void pan_controller_get_snapshot(pan_controller_snapshot_t *snapshot);

#endif
