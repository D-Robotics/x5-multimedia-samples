#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "bpu_mobilenetv2.h"
#include "mobilenetv2_image_labels.h"

#define ALIGN_16(v) ((v + (16 - 1)) / 16 * 16)

#define BPU_LOGE(fmt, ...) printf("[BPU ERROR] " fmt "\n", ##__VA_ARGS__)
#define BPU_LOGI(fmt, ...) printf("[BPU INFO] " fmt "\n", ##__VA_ARGS__)

#define HB_CHECK_SUCCESS(value, errmsg)                                  \
	do {                                                             \
		int32_t ret_code = value;                                \
		if (ret_code != 0) {                                     \
			BPU_LOGE("%s, error code:%d", errmsg, ret_code); \
			return (ret_code);                               \
		}                                                        \
	} while (0);

static int32_t print_model_info(hbPackedDNNHandle_t *packed_dnn_handle)
{
	int32_t i = 0, j = 0;
	hbDNNHandle_t dnn_handle;
	const char **model_name_list;
	int32_t model_count = 0;
	hbDNNTensorProperties properties;

	HB_CHECK_SUCCESS(hbDNNGetModelNameList(&model_name_list, &model_count, packed_dnn_handle),
			 "hbDNNGetModelNameList failed");
	if (model_count <= 0) {
		printf("Model count <= 0\n");
		return -1;
	}
	HB_CHECK_SUCCESS(hbDNNGetModelHandle(&dnn_handle, packed_dnn_handle, model_name_list[0]),
			 "hbDNNGetModelHandle failed");

	printf("Model info:\nmodel_name: %s\n", model_name_list[0]);

	int32_t input_count = 0;
	int32_t output_count = 0;
	HB_CHECK_SUCCESS(hbDNNGetInputCount(&input_count, dnn_handle), "hbDNNGetInputCount failed");
	HB_CHECK_SUCCESS(hbDNNGetOutputCount(&output_count, dnn_handle), "hbDNNGetOutputCount failed");

	printf("Input count: %d\n", input_count);
	for (i = 0; i < input_count; i++) {
		HB_CHECK_SUCCESS(hbDNNGetInputTensorProperties(&properties, dnn_handle, i),
				 "hbDNNGetInputTensorProperties failed");
		printf("input[%d]: tensorLayout: %d tensorType: %d validShape:(", i, properties.tensorLayout,
		       properties.tensorType);
		for (j = 0; j < properties.validShape.numDimensions; j++)
			printf("%d, ", properties.validShape.dimensionSize[j]);
		printf("), alignedShape:(");
		for (j = 0; j < properties.alignedShape.numDimensions; j++)
			printf("%d, ", properties.alignedShape.dimensionSize[j]);
		printf(")\n");
	}

	printf("Output count: %d\n", output_count);
	for (i = 0; i < output_count; i++) {
		HB_CHECK_SUCCESS(hbDNNGetOutputTensorProperties(&properties, dnn_handle, i),
				 "hbDNNGetOutputTensorProperties failed");
		printf("Output[%d]: tensorLayout: %d tensorType: %d validShape:(", i, properties.tensorLayout,
		       properties.tensorType);
		for (j = 0; j < properties.validShape.numDimensions; j++)
			printf("%d, ", properties.validShape.dimensionSize[j]);
		printf("), alignedShape:(");
		for (j = 0; j < properties.alignedShape.numDimensions; j++)
			printf("%d, ", properties.alignedShape.dimensionSize[j]);
		printf(")\n");
	}

	return 0;
}

static int32_t prepare_output_tensor(hbDNNTensor *output_tensor, hbDNNHandle_t dnn_handle)
{
	int32_t ret = 0;
	int32_t i = 0;
	int32_t output_count = 0;

	hbDNNGetOutputCount(&output_count, dnn_handle);
	for (i = 0; i < output_count; ++i) {
		HB_CHECK_SUCCESS(hbDNNGetOutputTensorProperties(&output_tensor[i].properties, dnn_handle, i),
				 "hbDNNGetOutputTensorProperties failed");
		HB_CHECK_SUCCESS(hbSysAllocCachedMem(&output_tensor[i].sysMem[0],
						     output_tensor[i].properties.alignedByteSize),
				 "hbSysAllocCachedMem failed");
	}

	return ret;
}

static void parse_classification_result(hbDNNTensor *tensor, int32_t *idx, float *score_top1)
{
	float *scores = (float *)(tensor->sysMem[0].virAddr);
	int32_t *shape = tensor->properties.validShape.dimensionSize;
	for (int32_t i = 0; i < shape[1] * shape[2] * shape[3]; i++) {
		float score = scores[i];
		if (score > *score_top1) {
			*idx = i;
			*score_top1 = score;
		}
	}
}

void bpu_set_ori_hw(bpu_handle_t *handle, int32_t width, int32_t height)
{
	if (handle == NULL)
		return;

	handle->m_image_info.m_ori_height = height;
	handle->m_image_info.m_ori_width = width;
}

int32_t bpu_mobilenetv2_init(bpu_handle_t *bpu_handle, char *model_path)
{
	int32_t ret = 0;
	const char **model_name_list;
	int32_t model_count = 0;
	hbDNNTensorProperties properties;
	hbPackedDNNHandle_t packed_dnn_handle;
	hbDNNHandle_t dnn_handle;

	if (NULL == bpu_handle) {
		BPU_LOGE("bpu_handle is NULL");
		return -1;
	}

	BPU_LOGI("model_path[%s]", model_path);

	HB_CHECK_SUCCESS(hbDNNInitializeFromFiles(&packed_dnn_handle, (char const **)&model_path, 1),
			 "hbDNNInitializeFromFiles failed");

	print_model_info(packed_dnn_handle);

	HB_CHECK_SUCCESS(hbDNNGetModelNameList(&model_name_list, &model_count, packed_dnn_handle),
			 "hbDNNGetModelNameList failed");

	if (model_count <= 0) {
		printf("Model count <= 0\n");
		return -1;
	}
	BPU_LOGI("model_name_list[0]:%s", model_name_list[0]);

	HB_CHECK_SUCCESS(hbDNNGetModelHandle(&dnn_handle, packed_dnn_handle, model_name_list[0]),
			 "hbDNNGetModelHandle failed");

	bpu_handle->m_packed_dnn_handle = packed_dnn_handle;
	bpu_handle->m_dnn_handle = dnn_handle;
	BPU_LOGI("packed_dnn_handle: %p, dnn_handle: %p", packed_dnn_handle, dnn_handle);

	HB_CHECK_SUCCESS(hbDNNGetInputTensorProperties(&properties, dnn_handle, 0),
			 "hbDNNGetInputTensorProperties failed");
	hbDNNTensorShape *input_tensor_shape = &properties.validShape;
	bpu_handle->m_image_info.m_model_h = (input_tensor_shape->dimensionSize)[2];
	bpu_handle->m_image_info.m_model_w = (input_tensor_shape->dimensionSize)[3];
	BPU_LOGI("model input NCHW = (1, 3, %d, %d)", bpu_handle->m_image_info.m_model_h,
		 bpu_handle->m_image_info.m_model_w);

	bpu_handle->m_image_info.m_ori_height = 1080;
	bpu_handle->m_image_info.m_ori_width = 1920;

	bpu_handle->m_cur_input_tensor = 0;
	bpu_handle->m_output_prepared = 0;
	for (int i = 0; i < BPU_INPUT_BUFFER_NUM; i++) {
		memset(&bpu_handle->m_input_tensors[i], 0, sizeof(bpu_tensor_info_t));
	}
	memset(&bpu_handle->m_output_tensor, 0, sizeof(hbDNNTensor));

	return ret;
}

int bpu_mobilenetv2_deinit(bpu_handle_t *handle)
{
	int32_t ret = 0;

	if (handle == NULL)
		return 0;

	if (handle->m_output_prepared) {
		hbSysFreeMem(&(handle->m_output_tensor.sysMem[0]));
		handle->m_output_prepared = 0;
	}

	HB_CHECK_SUCCESS(hbDNNRelease(handle->m_packed_dnn_handle), "hbDNNRelease failed");

	BPU_LOGI("bpu_wrap_deinit successful");
	return ret;
}

int32_t bpu_mobilenetv2_infer(bpu_handle_t *handle, hbn_vnode_image_t *vse_frame, int32_t *out_class_id, char *result,
			      int32_t result_size)
{
	int32_t ret = 0;
	int32_t output_count = 0;

	if (handle == NULL || vse_frame == NULL || result == NULL)
		return -1;

	hbDNNHandle_t dnn_handle = handle->m_dnn_handle;

	if (!handle->m_output_prepared) {
		ret = prepare_output_tensor(&handle->m_output_tensor, dnn_handle);
		if (ret) {
			BPU_LOGE("prepare model output tensor failed");
			return ret;
		}
		handle->m_output_prepared = 1;
	}

	bpu_tensor_info_t *tensor_info = &handle->m_input_tensors[handle->m_cur_input_tensor];
	hbDNNTensor *input_tensor = &tensor_info->m_dnn_tensor;

	input_tensor->properties.tensorLayout = HB_DNN_LAYOUT_NCHW;
	input_tensor->properties.tensorType = HB_DNN_IMG_TYPE_NV12_SEPARATE;

	uint32_t y_size = vse_frame->buffer.stride * vse_frame->buffer.height;
	uint32_t uv_size = y_size / 2;
	uint32_t total_size = y_size + uv_size;

	// Y 分量：映射 VSE buffer 物理地址和虚拟地址
	input_tensor->sysMem[0].phyAddr = vse_frame->buffer.phys_addr[0];
	input_tensor->sysMem[0].virAddr = vse_frame->buffer.virt_addr[0];
	input_tensor->sysMem[0].memSize = total_size;

	// UV 分量：映射 VSE buffer 物理地址和虚拟地址
	input_tensor->sysMem[1].phyAddr = vse_frame->buffer.phys_addr[1];
	input_tensor->sysMem[1].virAddr = vse_frame->buffer.virt_addr[1];
	input_tensor->sysMem[1].memSize = uv_size;

	input_tensor->properties.validShape.numDimensions = 4;
	input_tensor->properties.validShape.dimensionSize[0] = 1;
	input_tensor->properties.validShape.dimensionSize[1] = 3;
	input_tensor->properties.validShape.dimensionSize[2] = vse_frame->buffer.height;
	input_tensor->properties.validShape.dimensionSize[3] = ALIGN_16(vse_frame->buffer.stride);
	input_tensor->properties.alignedShape = input_tensor->properties.validShape;

	hbDNNInferCtrlParam infer_ctrl_param;
	HB_DNN_INITIALIZE_INFER_CTRL_PARAM(&infer_ctrl_param);

	hbDNNTaskHandle_t task_handle = NULL;
	hbDNNTensor *output = &handle->m_output_tensor;

	ret = hbDNNInfer(&task_handle, &output, input_tensor, dnn_handle, &infer_ctrl_param);
	if (ret) {
		BPU_LOGE("hbDNNInfer failed");
		return ret;
	}

	ret = hbDNNWaitTaskDone(task_handle, 0);
	if (ret) {
		BPU_LOGE("hbDNNWaitTaskDone failed");
		hbDNNReleaseTask(task_handle);
		return ret;
	}

	hbDNNGetOutputCount(&output_count, dnn_handle);
	for (int32_t i = 0; i < output_count; i++) {
		hbSysFlushMem(&handle->m_output_tensor.sysMem[0], HB_SYS_MEM_CACHE_INVALIDATE);
	}

	ret = hbDNNReleaseTask(task_handle);
	if (ret) {
		BPU_LOGE("hbDNNReleaseTask failed");
		return ret;
	}

	float score_top1 = 0.0;
	int32_t idx = 0;
	parse_classification_result(&handle->m_output_tensor, &idx, &score_top1);

	snprintf(result, result_size, "[classification result: id=%d(%s), score=%.3f]", idx, get_image_class_name(idx),
		 score_top1);

	if (out_class_id)
		*out_class_id = idx;

	handle->m_cur_input_tensor++;
	handle->m_cur_input_tensor %= BPU_INPUT_BUFFER_NUM;

	return 0;
}
