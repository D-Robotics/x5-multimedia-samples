#ifndef HOBOT_MIPI_CAM_H
#define HOBOT_MIPI_CAM_H

/**
 * @brief 相机内参
 */
struct hb_CameraIntrinsics
{
	double fx, fy, cx, cy, baseline;
};

typedef enum {
	CALIBRATE_IN_EEPROM = 0,
	CALIBRATE_IN_FILE
}CALIBRATE_TYPE_E;

bool gen_dual_gdc_bin(int calib_type, hb_CameraIntrinsics *cam_intr, const char *left_gdc_path, const char *right_gdc_path);

#endif