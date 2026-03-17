#ifndef WRAPPER_H_
#define WRAPPER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "StereoInfer_common.h"

#if 0

#define HB_CSI_PACK_MAGIC    0xDBAA5555

typedef struct {
	uint32_t magicNumber; /**< magic number */
	uint32_t size;
	uint32_t crc32;

	uint32_t reserved[10]; /**< reserved use */
} pack_header_t;


typedef struct {
	uint32_t offset; /**< magic number */
	uint32_t size; /**< reserved use */
	uint64_t timestamps;
} ;

#endif

/**
 * @brief 相机内参
 */
struct _CameraIntrinsics
{
    double fx, fy, cx, cy, baseline;
};

#define MAX_TENSOR_ID       5

int stereo_infer_init(void);
int stereo_infer(void *left_nv12, void *right_nv12, int *tensor_id);
int stereo_process(int tensor_id, void *depth_buffer);
void *stereo_infer_get_depth(void);
void stereo_infer_close(void);
void stereo_infer_dump_depth(char *filename);
void stereo_infer_dump_disp(char *filename);
int stereo_infer_gen_gdc_bin(int calibrate_type, const char *left_gdc_path, const char *right_gdc_path);
void stereo_get_cam_intr(struct _CameraIntrinsics *_intr);

#ifdef __cplusplus
}
#endif /* extern "C" */

#endif // WRAPPER_H_
