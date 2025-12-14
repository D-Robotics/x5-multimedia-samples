#ifndef __SENSOR_JSON_PARAM_PARSE_HH__
#define __SENSOR_JSON_PARAM_PARSE_HH__
#include "vp_sensors.h"

int sensor_json_param_parse(const char *json_str, vp_sensor_config_t *config);
int sensor_param_parse_from_file(const char *json_file_name, vp_sensor_config_t *config);
#endif
