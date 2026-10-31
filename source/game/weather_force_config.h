#ifndef WEATHER_FORCE_CONFIG_H
#define WEATHER_FORCE_CONFIG_H

#include <stdint.h>
#include <string.h>
#include "config.h"

/* Main's debug lever assignment, shared with the end-to-end regression. */
static inline uint8_t weather_force_from_config(struct config* config,
											uint8_t* out) {
	const char* wf = config_read_string(config, "debug.weather_force", NULL);
	int force_int = config_read_int(config, "debug.weather_force_int", 0);
	uint8_t force = 0;
	if((wf && strcmp(wf, "rain") == 0) || (!wf && force_int == 1))
		force = 1;
	else if((wf && strcmp(wf, "thunder") == 0) || (!wf && force_int == 2))
		force = 2;
	*out = force;
	return force;
}

#endif
