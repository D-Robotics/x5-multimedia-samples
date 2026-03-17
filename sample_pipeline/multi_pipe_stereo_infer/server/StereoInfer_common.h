#ifndef STEREO_INFER_COMMON_H
#define STEREO_INFER_COMMON_H


/* 双目深度推理模型版本 */
// #define STEREO_INFER_VER_V2_0
// #define STEREO_INFER_VER_V2_1
#define STEREO_INFER_VER_V2_2
// #define STEREO_INFER_VER_V2_6

#ifdef STEREO_INFER_VER_V2_0
	#define STEREO_INFER_VERSION	"V2.0"
#elif defined STEREO_INFER_VER_V2_1
	#define STEREO_INFER_VERSION	"V2.1"
#elif defined STEREO_INFER_VER_V2_2
	#define STEREO_INFER_VERSION	"V2.2"
#elif defined STEREO_INFER_VER_V2_6
	// V2.6: 速率高，但盲区较大，检测目标需要20cm以上距离
	#define STEREO_INFER_VERSION	"V2.6"
#endif


#endif