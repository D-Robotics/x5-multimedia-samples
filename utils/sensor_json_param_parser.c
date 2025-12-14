#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "cjson/cJSON.h"
#include "vp_sensors.h"

/**
 * @brief Parse LPWM channel attribute array
 * @param lpwm_chn_array cJSON array object (lpwm_chn_attr)
 * @param lpwm_attr Output parameter to store parsed LPWM attributes
 */
static void parse_lpwm_chn_attr(const cJSON *lpwm_chn_array, lpwm_attr_t *lpwm_attr)
{
	if (lpwm_chn_array == NULL)
	{
		printf("Error: parse_lpwm_chn_attr - lpwm_chn_array is NULL\n");
		return;
	}
	if (lpwm_attr == NULL)
	{
		printf("Error: parse_lpwm_chn_attr - lpwm_attr is NULL\n");
		return;
	}
	if (!cJSON_IsArray(lpwm_chn_array))
	{
		printf("Error: parse_lpwm_chn_attr - lpwm_chn_array is not a valid JSON array\n");
		return;
	}

	int array_size = cJSON_GetArraySize(lpwm_chn_array);
	int parse_count = (array_size > LPWM_CHN_NUM) ? LPWM_CHN_NUM : array_size;
	// printf("Info: lpwm_chn_attr array size is %d, will parse %d items (max: %d)\n", array_size, parse_count, LPWM_CHN_NUM);

	for (int i = 0; i < parse_count; i++)
	{
		const cJSON *chn_item = cJSON_GetArrayItem(lpwm_chn_array, i);
		if (chn_item == NULL)
		{
			printf("Error: parse_lpwm_chn_attr - channel [%d] item is NULL\n", i);
			continue;
		}
		if (!cJSON_IsObject(chn_item))
		{
			printf("Error: parse_lpwm_chn_attr - channel [%d] item is not a valid JSON object\n", i);
			continue;
		}

		cJSON *trigger_source = cJSON_GetObjectItem(chn_item, "trigger_source");
		if (cJSON_IsNumber(trigger_source))
		{
			lpwm_attr->lpwm_chn_attr[i].trigger_source = trigger_source->valueint;
			printf("	Set channel [%d]'s trigger_source to %d\n", i, trigger_source->valueint);
		}
		else if (trigger_source != NULL)
		{ // 字段存在但类型不是数字
			printf("Warning: parse_lpwm_chn_attr - channel [%d] trigger_source is not a number\n", i);
		}

		cJSON *trigger_mode = cJSON_GetObjectItem(chn_item, "trigger_mode");
		if (cJSON_IsNumber(trigger_mode))
		{
			lpwm_attr->lpwm_chn_attr[i].trigger_mode = trigger_mode->valueint;
			printf("	Set channel [%d]'s trigger_mode to %d\n", i, trigger_mode->valueint);
		}
		else if (trigger_mode != NULL)
		{
			printf("Warning: parse_lpwm_chn_attr - channel [%d] trigger_mode is not a number\n", i);
		}

		cJSON *period = cJSON_GetObjectItem(chn_item, "period");
		if (cJSON_IsNumber(period))
		{
			lpwm_attr->lpwm_chn_attr[i].period = cJSON_GetNumberValue(period);
			printf("	Set channel [%d]'s period to %lld\n", i, (long long)cJSON_GetNumberValue(period));
		}
		else if (period != NULL)
		{
			printf("Warning: parse_lpwm_chn_attr - channel [%d] period is not a number\n", i);
		}

		cJSON *offset = cJSON_GetObjectItem(chn_item, "offset");
		if (cJSON_IsNumber(offset))
		{
			lpwm_attr->lpwm_chn_attr[i].offset = offset->valueint;
			printf("	Set channel [%d]'s offset to %d\n", i, offset->valueint);
		}
		else if (offset != NULL)
		{
			printf("Warning: parse_lpwm_chn_attr - channel [%d] offset is not a number\n", i);
		}

		cJSON *duty_time = cJSON_GetObjectItem(chn_item, "duty_time");
		if (cJSON_IsNumber(duty_time))
		{
			lpwm_attr->lpwm_chn_attr[i].duty_time = duty_time->valueint;
			printf("	Set channel [%d]'s duty_time to %d\n", i, duty_time->valueint);
		}
		else if (duty_time != NULL)
		{
			printf("Warning: parse_lpwm_chn_attr - channel [%d] duty_time is not a number\n", i);
		}

		cJSON *threshold = cJSON_GetObjectItem(chn_item, "threshold");
		if (cJSON_IsNumber(threshold))
		{
			lpwm_attr->lpwm_chn_attr[i].threshold = threshold->valueint;
			printf("	Set channel [%d]'s threshold to %d\n", i, threshold->valueint);
		}
		else if (threshold != NULL)
		{
			printf("Warning: parse_lpwm_chn_attr - channel [%d] threshold is not a number\n", i);
		}

		cJSON *adjust_step = cJSON_GetObjectItem(chn_item, "adjust_step");
		if (cJSON_IsNumber(adjust_step))
		{
			lpwm_attr->lpwm_chn_attr[i].adjust_step = adjust_step->valueint;
			printf("	Set channel [%d]'s adjust_step to %d\n", i, adjust_step->valueint);
		}
		else if (adjust_step != NULL)
		{
			printf("Warning: parse_lpwm_chn_attr - channel [%d] adjust_step is not a number\n", i);
		}
	}
}

/**
 * @brief Parse LPWM attribute object
 * @param lpwm_attr_obj cJSON object (lpwm_attr)
 * @param config Output parameter to store parsed LPWM attributes
 */
static void parse_lpwm_attr(const cJSON *lpwm_attr_obj, vp_sensor_config_t *config)
{
	if (lpwm_attr_obj == NULL)
	{
		printf("Error: parse_lpwm_attr - lpwm_attr_obj is NULL\n");
		return;
	}
	if (config == NULL)
	{
		printf("Error: parse_lpwm_attr - config is NULL\n");
		return;
	}
	if (!cJSON_IsObject(lpwm_attr_obj))
	{
		printf("Error: parse_lpwm_attr - lpwm_attr_obj is not a valid JSON object\n");
		return;
	}

	const cJSON *lpwm_chn_array = cJSON_GetObjectItem(lpwm_attr_obj, "lpwm_chn_attr");
	if (lpwm_chn_array == NULL)
	{
		printf("Error: parse_lpwm_attr - lpwm_chn_attr field not found in lpwm_attr_obj\n");
		return;
	}

	parse_lpwm_chn_attr(lpwm_chn_array, &config->vin_node_attr->lpwm_attr);
}

/**
 * @brief Parse sensor parameter from JSON string
 * @param json_str Input JSON string
 * @param config Output parameter to store parsed sensor configuration
 * @return 0 on success, -1 on failure
 */
int sensor_param_parse(const char *json_str, vp_sensor_config_t *config)
{
	if (json_str == NULL)
	{
		printf("Error: sensor_param_parse - json_str is NULL\n");
		return -1;
	}
	if (config == NULL)
	{
		printf("Error: sensor_param_parse - config is NULL\n");
		return -1;
	}
	if (strlen(json_str) == 0)
	{
		printf("Error: sensor_param_parse - json_str is an empty string\n");
		return -1;
	}
	cJSON *root = cJSON_Parse(json_str);
	if (root == NULL)
	{
		const char *error_ptr = cJSON_GetErrorPtr();
		if (error_ptr != NULL)
		{
			printf("Error: sensor_param_parse - JSON parse error at position: %s\n", error_ptr);
		}
		else
		{
			printf("Error: sensor_param_parse - Failed to parse JSON string (unknown error)\n");
		}
		return -1;
	}

	cJSON *settle = cJSON_GetObjectItem(root, "settle");
	if (cJSON_IsNumber(settle))
	{
		config->camera_config->mipi_cfg->rx_attr.settle = settle->valueint;
		printf("	Set settle to %d\n", settle->valueint);
	}
	else if (settle != NULL)
	{
		printf("Warning: sensor_param_parse - settle field is not a number\n");
	}

	const cJSON *lpwm_attr_obj = cJSON_GetObjectItem(root, "lpwm_attr");
	if (lpwm_attr_obj != NULL)
	{
		parse_lpwm_attr(lpwm_attr_obj, config);
	}
	else
	{
		// printf("Info: sensor_param_parse - lpwm_attr field not found, skip parsing\n");
	}

	cJSON_Delete(root);

	return 0;
}
/**
 * @brief Parse sensor parameter from JSON file
 * @param json_file_name Input JSON file path/name
 * @param config Output parameter to store parsed sensor configuration
 * @return 0 on success, -1 on failure
 */
int sensor_param_parse_from_file(const char *json_file_name, vp_sensor_config_t *config)
{
	if (json_file_name == NULL)
	{
		printf("Error: json_file_name is NULL\n");
		return -1;
	}
	if (config == NULL)
	{
		printf("Error: config is NULL\n");
		return -1;
	}
	FILE *fp = fopen(json_file_name, "r");
	if (fp == NULL)
	{
		printf("Error: Failed to open file: %s (file not exist or permission denied)\n", json_file_name);
		return -1;
	}

	fseek(fp, 0, SEEK_END);
	long file_size = ftell(fp);
	if (file_size <= 0)
	{
		printf("Error: File %s is empty or invalid (size: %ld)\n", json_file_name, file_size);
		fclose(fp);
		return -1;
	}
	fseek(fp, 0, SEEK_SET);

	char *json_str = (char *)malloc(file_size + 1);
	if (json_str == NULL)
	{
		printf("Error: Failed to allocate memory (need %ld bytes)\n", file_size + 1);
		fclose(fp);
		return -1;
	}

	size_t read_size = fread(json_str, 1, file_size, fp);
	if (read_size != (size_t)file_size)
	{
		printf("Error: Read file failed (read %zu bytes, expected %ld bytes)\n", read_size, file_size);
		free(json_str);
		fclose(fp);
		return -1;
	}
	json_str[file_size] = '\0';
	int ret = sensor_param_parse(json_str, config);
	free(json_str);
	fclose(fp);
	if (ret != 0)
	{
		printf("Error: Parse file %s failed\n", json_file_name);
	}
	return ret;
}