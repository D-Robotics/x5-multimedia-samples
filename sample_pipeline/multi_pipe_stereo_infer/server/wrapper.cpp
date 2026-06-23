#include <iostream>
#include "stereonet_process.h"
#include "wrapper.h"
#include "hobot_mipi_cam.hpp"

extern "C" {

	static stereonet::StereonetProcess stereonet_process = stereonet::StereonetProcess();
	static stereonet::CameraIntrinsic intr = {0, 0, 0, 0, 0};

	cv::Mat disp, uncert, depth;
	static int width, height;

	int stereo_infer_init(void)
	{
		int ret_code = 0;
		std::cout << "-+ ================== init ==========================" << std::endl;
		#ifdef STEREO_INFER_VER_V2_0
		ret_code = stereonet_process.init("x5baseplus_alldata_woIsaac.bin"); // V2.0
		#elif defined STEREO_INFER_VER_V2_1
		ret_code = stereonet_process.init("DStereoV2.1.bin"); // V2.1
		#elif defined STEREO_INFER_VER_V2_2
		ret_code = stereonet_process.init("DStereoV2.2.bin"); // V2.2
		#elif defined STEREO_INFER_VER_V2_6
		ret_code = stereonet_process.init("DStereoV2.6_int8.bin"); // V2.6
		#endif
		if (ret_code != 0) {
			std::cout << "-+ init failed" << std::endl;
			std::cout << "-+ intr [fx, fy, cx, cy, baseline(m)]: [" << intr.fx << ", " << intr.fy << ", " << intr.cx << ", "
					<< intr.cy << ", " << intr.baseline << "]" << std::endl;
		} else {
			stereonet_process.get_model_input_size(width, height);
			std::cout << "-+ init success" << std::endl;
		}

		return ret_code;
	}

#if 0
	int stereo_infer_process(void *left_nv12, void *right_nv12, void *depth_buffer)
	{
		int idle_tensor_id = 0;
		int ret_code = 0;
		ret_code = stereonet_process.forward((uint8_t *)left_nv12, (uint8_t *)right_nv12, idle_tensor_id);


		int width, height;
		stereonet_process.get_model_input_size(width, height);
		float *disp = new float[width * height];
		float *uncert = new float[width * height];
		// uint16_t *depth = new uint16_t[width * height];
		stereonet_process.postprocess_out_disp_depth(idle_tensor_id, 0.0f, intr, disp, uncert, (uint16_t *)depth_buffer);
		// cv::Mat depth_mat(height, width, CV_16UC1);
		// cv::Mat disp_mat(height, width, CV_32FC1);
		// memcpy(disp_mat.data, disp, width * height * sizeof(float));
		// memcpy(depth_mat.data, depth, width * height * sizeof(uint16_t));
		// cv::imwrite("depth.png", depth_mat);
		// cv::imwrite("disp.pfm", disp_mat);

		delete[] disp;
		delete[] uncert;
		// delete[] depth;

		return ret_code;
	}
#endif

	int stereo_infer(void *left_nv12, void *right_nv12, int *tensor_id)
	{
		int idle_tensor_id = 0;
		int ret_code = 0;
		ret_code = stereonet_process.forward((uint8_t *)left_nv12, (uint8_t *)right_nv12, idle_tensor_id);
		*tensor_id = idle_tensor_id;
		return ret_code;
	}

	int stereo_process(int tensor_id, void *depth_buffer)
	{
	#if 0
		cv::Mat disp, uncert, depth;
		stereonet_process.postprocess_out_disp_depth(tensor_id, 0.0f, intr, disp, uncert, depth);
		memcpy(depth_buffer, depth.data, depth.total() * sizeof(uint16_t));

		// disp.data float disp.total() * sizeof(float)
		// depth.data uint16_t depth.total() * sizeof(uint16_t)

		cv::imwrite("disp.pfm", disp);
		cv::imwrite("depth.png", depth);
	#else
		// cv::Mat depth;
		// stereonet_process.postprocess_out_depth(tensor_id, 0.0f, intr, depth);
		// memcpy(depth_buffer, depth.data, depth.total() * sizeof(uint16_t));
		stereonet_process.postprocess_out_disp_depth(tensor_id, 0.0f, intr, disp, uncert, depth);
		memcpy(depth_buffer, depth.data, depth.total() * sizeof(uint16_t));
	#endif

		return 0;
	}

	void *stereo_infer_get_depth(void)
	{
		// int idle_tensor_id = 0;
		// int width, height;
		// stereonet_process.get_model_input_size(width, height);
		// float *disp = new float[width * height];
		// float *uncert = new float[width * height];
		// uint16_t *depth = new uint16_t[width * height];
		// stereonet_process.postprocess_out_disp_depth(idle_tensor_id, 0.0f, intr, disp, uncert, depth);

		return nullptr;
	}

	void stereo_infer_close(void)
	{
		stereonet_process.~StereonetProcess();
	}

	void stereo_infer_dump_depth(char *filename)
	{

	}
	void stereo_infer_dump_disp(char *filename)
	{
		cv::imwrite(filename, disp);
	}

	int stereo_infer_gen_gdc_bin(int calibrate_type, const char *left_gdc_path, const char *right_gdc_path)
	{
		std::cout << "-+ ================== Gen GDC BIN =========================" << std::endl;

		hb_CameraIntrinsics hb_intr = {0, 0, 0, 0, 0};

		if (true != gen_dual_gdc_bin(calibrate_type, &hb_intr, left_gdc_path, right_gdc_path)) {
			return -1;
		}

		std::cout << "-+ ================== Camera Intrinsics =========================" << std::endl;
		std::cout << "fx = " << hb_intr.fx <<std::endl;
		std::cout << "fy = " << hb_intr.fy <<std::endl;
		std::cout << "cx = " << hb_intr.cx <<std::endl;
		std::cout << "cy = " << hb_intr.cy <<std::endl;
		std::cout << "baseline = " << hb_intr.baseline <<std::endl;

		#if 1
		// 双目算法使用的分辨率是 640x352，而GDC矫正后是 1280x1088，需要对内参做转换
		intr.fx = hb_intr.fx * (640.0/1280.0);
		intr.fy = hb_intr.fy * (352.0/1088.0);
		intr.cx = hb_intr.cx * (640.0/1280.0);
		intr.cy = hb_intr.cy * (352.0/1088.0);
		// if (hb_intr.baseline < 1.0)
		// 	intr.baseline = hb_intr.baseline;
		// else
		// 	intr.baseline = hb_intr.baseline / 1000.0;
		intr.baseline = hb_intr.baseline;
		std::cout << "-+ ================== Camera Intrinsics [After resolution] =========================" << std::endl;
		std::cout << "fx = " << intr.fx <<std::endl;
		std::cout << "fy = " << intr.fy <<std::endl;
		std::cout << "cx = " << intr.cx <<std::endl;
		std::cout << "cy = " << intr.cy <<std::endl;
		std::cout << "baseline = " << intr.baseline <<std::endl;
		#endif

		return 0;
	}

	void stereo_get_cam_intr(struct _CameraIntrinsics *_intr)
	{
		_intr->fx = intr.fx;
		_intr->fy = intr.fy;
		_intr->cx = intr.cx;
		_intr->cy = intr.cy;
		_intr->baseline = intr.baseline;
	}
}


