#ifndef __VP_SENSORS_H__
#define __VP_SENSORS_H__

#include <string.h>

#include "vin_cfg.h"
#include "isp_cfg.h"
#include "n2d_cfg.h"
#include "hb_camera_data_config.h"
#include "cam_def.h"

// Todo: remove define variable
#define MAGIC_NUMBER 0x12345678
#define AUTO_ALLOC_ID -1

#define VP_MAX_BUF_SIZE 256
#define VP_MAX_VCON_NUM 4

#define SENSOR_TYPE_NORMAL	0
#define SENSOR_TYPE_GMSL_RAW	1
#define SENSOR_TYPE_GMSL_YUV	2
#define SENSOR_TYPE_GMSL_RGBIR	3
#define SENSOR_TYPE_HSMT_RAW 4

#define SENSOR_DATA_TYPE_RAW12 0x2C
#define SENSOR_DATA_TYPE_RAW10 0x2B
#define SENSOR_DATA_TYPE_YUV422 0x1E

#define N2D_SCALE 		0
#define N2D_OVERLAY 	1
#define N2D_STITCH	 	2
#define N2D_CSC 		3
#define N2D_ROTATE 		4
#define N2D_SCALE_CROP  5

#define SIF_ONLINE_ISP      0
#define SIF_MCM_ISP      	1
#define SIF_OFFLINE_ISP     2

#define SENSOR_MODE_SUPPORT_COUNT 8

#define ALIGN_UP(a, size) (((a) + (size)-1u) & (~((size)-1u)))

typedef struct {
	int index;
	int is_valid;
	int mclk_is_not_configed;
	char sensor_config_list[128];
} csi_info_t;
//保证 0-3 的信息分别存储到 csi_info中，即使这个CSI下没有摄像头
typedef struct{
	int valid_count;
	int max_count;
	csi_info_t csi_info[VP_MAX_VCON_NUM];
} csi_list_info_t;

typedef struct vcon_properties {
	char device_path[VP_MAX_BUF_SIZE];
	char compatible[VP_MAX_BUF_SIZE];
	int32_t type;
	int32_t bus;
	int32_t rx_phy[2];
	char status[VP_MAX_BUF_SIZE];
	char pinctrl_names[VP_MAX_BUF_SIZE];
	int32_t pinctrl_0[8];
	int32_t gpio_oth[8];
} vcon_propertie_t;

typedef struct mipi_properties {
	char device_path[VP_MAX_BUF_SIZE];
	char status[VP_MAX_BUF_SIZE];
	char pinctrl_names[VP_MAX_BUF_SIZE];
	int32_t pinctrl_0[8];
	int32_t pinctrl_1[8];
	int32_t snrclk_idx[8];
} mipi_propertie_t;

typedef struct vp_csi_config_s{
	int index;
	int mclk_is_not_configed;
}vp_csi_config_t;

typedef struct vp_sensor_config_s {
	int16_t chip_id_reg;
	int16_t chip_id;
	// Some sensors use a different set of i2c addresses
	uint32_t sensor_i2c_addr_list[8];
	char sensor_name[128];
	char config_file[128];
	camera_config_t *camera_config;
	vin_node_attr_t *vin_node_attr;
	vin_ichn_attr_t *vin_ichn_attr;
	vin_ochn_attr_t *vin_ochn_attr;
	vin_attr_ex_t   *vin_attr_ex;
	isp_attr_t      *isp_attr;
	isp_ichn_attr_t *isp_ichn_attr;
	isp_ochn_attr_t *isp_ochn_attr;
	deserial_config_t *deserial_node_attr;
	mipi_config_t *mipi_cfg_attr;
	n2d_config_t *gpu2d_scale_crop_attr;
	uint16_t sensor_type;
	uint32_t support_sensor_mode[SENSOR_MODE_SUPPORT_COUNT];
} vp_sensor_config_t;

extern vp_sensor_config_t *vp_sensor_config_list[];

uint32_t vp_get_sensors_list_number();
void vp_show_sensors_list();
void vp_show_sensors_list_vse_limit(uint32_t width_limit, uint32_t height_limit);
vp_sensor_config_t *vp_get_sensor_config_by_name(char *sensor_name);
void vp_sensor_detect_structed(csi_list_info_t *csi_list_info);
int vp_get_sensor_info_by_name(const char *sensor_name, int *width , int *height, int *fps);
int32_t vp_sensor_fixed_mipi_host(vp_sensor_config_t *sensor_config, vp_csi_config_t* mipi_config);
int32_t vp_sensor_multi_fixed_mipi_host(vp_sensor_config_t *sensor_config, int used_mipi_host, vp_csi_config_t* mipi_config);
#endif // __VP_SENSORS_H__