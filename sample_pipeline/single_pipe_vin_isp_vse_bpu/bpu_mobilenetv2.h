#ifndef BPU_MOBILENETV2_H_
#define BPU_MOBILENETV2_H_

#include "dnn/hb_dnn.h"
#include "hbn_api.h"

typedef struct {
	int32_t m_model_w;
	int32_t m_model_h;
	int32_t m_ori_width;
	int32_t m_ori_height;
} bpu_image_info_t;

typedef struct {
	hbDNNTensor m_dnn_tensor;
} bpu_tensor_info_t;

#define BPU_INPUT_BUFFER_NUM 3
#define BPU_RESULT_BUF_SIZE 256

typedef struct {
	int32_t m_vpp_id;
	hbPackedDNNHandle_t m_packed_dnn_handle;
	hbDNNHandle_t m_dnn_handle;
	bpu_image_info_t m_image_info;
	bpu_tensor_info_t m_input_tensors[BPU_INPUT_BUFFER_NUM];
	int32_t m_cur_input_tensor;
	hbDNNTensor m_output_tensor;
	int32_t m_output_prepared;
} bpu_handle_t;

void bpu_set_ori_hw(bpu_handle_t *handle, int32_t width, int32_t height);
int32_t bpu_mobilenetv2_init(bpu_handle_t *bpu_handle, char *model_path);
int bpu_mobilenetv2_deinit(bpu_handle_t *handle);
int32_t bpu_mobilenetv2_infer(bpu_handle_t *handle, hbn_vnode_image_t *vse_frame, int32_t *out_class_id, char *result,
			      int32_t result_size);

#endif	// BPU_MOBILENETV2_H_
