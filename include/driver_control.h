#ifndef SENTRY_DRIVER_CONTROL_H
#define SENTRY_DRIVER_CONTROL_H

#include "tmc2209.h"

#include <stdbool.h>

typedef enum {
	DRIVER_AXIS_PAN = 0,
	DRIVER_AXIS_TILT
} driver_axis_t;

typedef struct {
	bool pan_configured;
	bool tilt_configured;
} driver_configure_result_t;

void driver_control_init(void);
driver_configure_result_t driver_control_configure(void);
void driver_control_refresh(driver_axis_t axis);
const tmc2209_device_t *driver_control_get(driver_axis_t axis);
bool driver_control_pan_ready(void);

#endif
