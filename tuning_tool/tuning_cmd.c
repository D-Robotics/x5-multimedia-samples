/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include "tuning_cmd.h"

void tuning_dump_sif_raw(tuning_context_t *ctx)
{
	int32_t i = 0, ret;
	uint32_t dump_cnt = 0;
	hbn_vnode_image_t raw_img = {0};
	char file_name[128] = {0};
	static int32_t raw_stream_cnt = 0;

	if (!ctx->is_offline) {
		pr_tuning("cannot dump raw when sif otf isp\n");
		return ;
	}
	if (BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK)) {
		pr_tuning("can not dump raw in feedback mode\n");
		return;
	}

	read_p("typing the number to dump: ", "%d", &dump_cnt);

	for (i = 0; i < dump_cnt; i++) {
		ret = hbn_vnode_getframe_cond(ctx->vnode_fd[0], 0, 1000, 0, &raw_img);
		if (ret) {
			pr_tuning("get buffer from sif fail\n");
			continue;
		}

		if (!ctx->dump_stream_flag)
			tuning_get_filename(file_name, DEF_DUMP_PATH, &raw_img, SIF_MNI);
		else
			snprintf(file_name, TUNING_PRINT_SIZE_MAX, "%s/stream%d.raw", DEF_DUMP_PATH, raw_stream_cnt);

		tuning_dump_file(file_name, &raw_img);

		hbn_vnode_releaseframe(ctx->vnode_fd[0], 0, &raw_img);
	}
	raw_stream_cnt++;
}

void tuning_handle_set_expsoure(tuning_context_t *ctx)
{
	hbn_isp_exposure_attr_t exp_attr = {0};
	uint32_t exp_mode;

	read_p("typing the expsoure mode, manual(0)/auto(1): ", "%d", &exp_mode);

	if (exp_mode == 0) {
		exp_attr.mode = HBN_ISP_MODE_MANUAL;
		read_p("expsoure time(units: s): ", "%f", &exp_attr.manual_attr.exp_time);
		read_p("again: ", "%f", &exp_attr.manual_attr.again);
		read_p("dgain: ", "%f", &exp_attr.manual_attr.dgain);
		read_p("ispgain: ", "%f", &exp_attr.manual_attr.ispgain);
	} else if (exp_mode == 1) {
		ISP_API_EQ(hbn_isp_get_exposure_attr(ctx->vnode_fd[1], &exp_attr), return);
		exp_attr.mode = HBN_ISP_MODE_AUTO;
		read_p("speed_over: ", "%f", &exp_attr.auto_attr.speed_over);
		read_p("speed_under: ", "%f", &exp_attr.auto_attr.speed_under);
		read_p("dampover_gain: ", "%f", &exp_attr.auto_attr.dampover_gain);
		read_p("dampover_ratio: ", "%f", &exp_attr.auto_attr.dampover_ratio);
		read_p("dampunder_gain: ", "%f", &exp_attr.auto_attr.dampunder_gain);
		read_p("dampunder_ratio: ", "%f", &exp_attr.auto_attr.dampunder_ratio);
		read_p("tolerance: ", "%f", &exp_attr.auto_attr.tolerance);
		read_p("target: ", "%f", &exp_attr.auto_attr.target);
		read_p("flicker_freq: ", "%f", &exp_attr.auto_attr.flicker_freq);
		read_p("anti_flicker_status: ", "%d", &exp_attr.auto_attr.anti_flicker_status);
	} else {
		printf("Unknown mode: %d\n", exp_mode);
		return;
	}

	ISP_API_EQ(hbn_isp_set_exposure_attr(ctx->vnode_fd[1], &exp_attr), return);
}

void tuning_handle_get_expsoure(tuning_context_t *ctx)
{
	uint32_t lines_per_second;
	hbn_isp_exposure_attr_t exp_attr = {0};

	ISP_API_EQ(hbn_isp_get_exposure_attr(ctx->vnode_fd[1], &exp_attr), return);
	ISP_API_EQ(hbn_isp_get_lines_persecond(ctx->vnode_fd[1], &lines_per_second), return);

	printf("Currently AE is in %s mode", (exp_attr.mode == HBN_ISP_MODE_MANUAL)?"manual":"auto");
	if (exp_attr.mode == HBN_ISP_MODE_AUTO) {
		if (exp_attr.lock_state) {
			printf(", lock state\n");
		} else {
			printf(", running state\n");
		}
	} else {
		printf("\n");
	}

	printf("exp_time: %f\n", exp_attr.manual_attr.exp_time);
	printf("lines_per_second: %d\n", lines_per_second);
	printf("again: %f\n", exp_attr.manual_attr.again);
	printf("dgain: %f\n", exp_attr.manual_attr.dgain);
	printf("ispgain: %f\n", exp_attr.manual_attr.ispgain);
	printf("ae_exp: %f\n",exp_attr.manual_attr.ae_exp);
	printf("mode: %d\n", exp_attr.auto_attr.mode);

	printf("exp range %f~%f\n", exp_attr.auto_attr.exp_time_range.min, exp_attr.auto_attr.exp_time_range.max);
	printf("again range %f~%f\n", exp_attr.auto_attr.again_range.min, exp_attr.auto_attr.again_range.max);
	printf("dgain range %f~%f\n", exp_attr.auto_attr.dgain_range.min, exp_attr.auto_attr.dgain_range.max);
	printf("isp dgain range %f~%f\n", exp_attr.auto_attr.isp_dgain_range.min, exp_attr.auto_attr.isp_dgain_range.max);

	printf("speed_over %f\n", exp_attr.auto_attr.speed_over);
	printf("speed_under %f\n", exp_attr.auto_attr.speed_under);
	printf("dampover_gain %f\n", exp_attr.auto_attr.dampover_gain);
	printf("dampover_ratio %f\n", exp_attr.auto_attr.dampover_ratio);
	printf("dampunder_gain %f\n", exp_attr.auto_attr.dampunder_gain);
	printf("dampunder_ratio %f\n", exp_attr.auto_attr.dampunder_ratio);

	printf("tolerance %f\n", exp_attr.auto_attr.tolerance);
	printf("target %f\n", exp_attr.auto_attr.target);
	printf("flicker_freq %f\n", exp_attr.auto_attr.flicker_freq);
	printf("anti_flicker_status %d\n", exp_attr.auto_attr.anti_flicker_status);
}

void tuning_handle_set_white_balance(tuning_context_t *ctx)
{
	hbn_isp_awb_attr_t awb_attr = {0};
	uint32_t awb_mode;

	read_p("typing the awb mode, manual(0)/auto(1): ", "%d", &awb_mode);

	if (awb_mode == 0) {
		awb_attr.mode = HBN_ISP_MODE_MANUAL;

		read_p("rgain: ", "%f", &awb_attr.manual_attr.gain.rgain);
		read_p("grgain: ", "%f", &awb_attr.manual_attr.gain.grgain);
		read_p("gbgain: ", "%f", &awb_attr.manual_attr.gain.gbgain);
		read_p("bgain: ", "%f", &awb_attr.manual_attr.gain.bgain);
	} else if (awb_mode == 1) {
		ISP_API_EQ(hbn_isp_get_awb_attr(ctx->vnode_fd[1], &awb_attr), return);

		read_p("use_damping: ", "%d", &awb_attr.auto_attr.use_damping);
		read_p("use_manual_damp_coff: ", "%d", &awb_attr.auto_attr.use_manual_damp_coff);
		read_p("manual_damp_coff: ", "%f", &awb_attr.auto_attr.manual_damp_coff);
		read_p("lock_tolerance: ", "%f", &awb_attr.auto_attr.lock_tolerance);
		read_p("unlock_tolerance: ", "%f", &awb_attr.auto_attr.unlock_tolerance);
		awb_attr.mode = HBN_ISP_MODE_AUTO;
	} else {
		printf("Unknown mode: %d\n", awb_mode);
		return;
	}

	ISP_API_EQ(hbn_isp_set_awb_attr(ctx->vnode_fd[1], &awb_attr), return);
}

void tuning_handle_get_white_balance(tuning_context_t *ctx)
{
	hbn_isp_awb_attr_t awb_attr = {0};

	ISP_API_EQ(hbn_isp_get_awb_attr(ctx->vnode_fd[1], &awb_attr), return);

	printf("Currently AWB is in %s mode", (awb_attr.mode == HBN_ISP_MODE_MANUAL)?"manual":"auto");
	if (awb_attr.mode == HBN_ISP_MODE_AUTO) {
		if (awb_attr.lock_state) {
			printf(", lock state\n");
		} else {
			printf(", running state\n");
		}
	} else {
		printf("\n");
	}

	printf("use_damping: %d\n", awb_attr.auto_attr.use_damping);
	printf("use_manual_damp_coff:  %d\n", awb_attr.auto_attr.use_manual_damp_coff);
	printf("manual_damp_coff:  %f\n", awb_attr.auto_attr.manual_damp_coff);
	printf("lock_tolerance:  %f\n", awb_attr.auto_attr.lock_tolerance);
	printf("unlock_tolerance:  %f\n", awb_attr.auto_attr.unlock_tolerance);

	printf("temper %d\n", awb_attr.auto_attr.temper);
	printf("rgain %f\n", awb_attr.auto_attr.gain.rgain);
	printf("grgain %f\n", awb_attr.auto_attr.gain.grgain);
	printf("gbgain %f\n", awb_attr.auto_attr.gain.gbgain);
	printf("bgain %f\n", awb_attr.auto_attr.gain.bgain);
}

void tuning_hanle_set_ae_table(tuning_context_t *ctx)
{
	int32_t i;
	hbn_isp_exposure_table_t ae_table_attr = {0};

	for (i = 0; i < HBN_ISP_EXP_TABLE_NUM; i++) {
		printf("input table %d, typing [0] to finish:\n", i);
		read_p("exposure_time: ", "%f", &ae_table_attr.exp_table[i].exposure_time);
		if (ae_table_attr.exp_table[i].exposure_time == 0)
			break;

		read_p("again: ", "%f", &ae_table_attr.exp_table[i].again);
		read_p("dgain: ", "%f", &ae_table_attr.exp_table[i].dgain);
		read_p("isp_gain: ", "%f", &ae_table_attr.exp_table[i].isp_gain);
	}
	ae_table_attr.valid_num = i == HBN_ISP_EXP_TABLE_NUM - 1 ? HBN_ISP_EXP_TABLE_NUM : i;

	ISP_API_EQ(hbn_isp_set_exposure_table(ctx->vnode_fd[1], &ae_table_attr), return);
}

void tuning_hanle_get_ae_table(tuning_context_t *ctx)
{
	int32_t i;
	hbn_isp_exposure_table_t ae_table_attr = {0};

	ISP_API_EQ(hbn_isp_get_exposure_table(ctx->vnode_fd[1], &ae_table_attr), return);

	for (i = 0; i < ae_table_attr.valid_num; i++) {
		printf("---------table %d---------\n", i);
		printf("exposure_time: %f\n", ae_table_attr.exp_table[i].exposure_time);
		printf("again: %f\n", ae_table_attr.exp_table[i].again);
		printf("dgain: %f\n", ae_table_attr.exp_table[i].dgain);
		printf("isp_gain: %f\n", ae_table_attr.exp_table[i].isp_gain);
	}
}

void tuning_hanle_set_exp_roi(tuning_context_t *ctx)
{
	int32_t i;
	hbn_isp_exposure_roi_t exp_roi = {0};

	read_p("roi_num: ", "%d", &exp_roi.roi_num);
	read_p("roi_weight: ", "%f", &exp_roi.roi_weight);

	for (i = 0; i < HBN_ISP_ROI_WINDOWS_MAX; i++) {
		printf("input table %d, typing [0] to finish:\n", i);
		read_p("weight: ", "%f", &exp_roi.roi_window[i].weight);
		if (exp_roi.roi_window[i].weight == 0)
			break;

		read_p("h_offset: ", "%d", &exp_roi.roi_window[i].window.h_offset);
		read_p("v_offset: ", "%d", &exp_roi.roi_window[i].window.v_offset);
		read_p("width: ", "%d", &exp_roi.roi_window[i].window.width);
		read_p("height: ", "%d", &exp_roi.roi_window[i].window.height);
	}
	exp_roi.roi_num = i == HBN_ISP_ROI_WINDOWS_MAX - 1 ? HBN_ISP_ROI_WINDOWS_MAX : i;

	ISP_API_EQ(hbn_isp_set_exposure_roi(ctx->vnode_fd[1], &exp_roi), return);
}

void tuning_hanle_get_exp_roi(tuning_context_t *ctx)
{
	int32_t i;
	hbn_isp_exposure_roi_t exp_roi = {0};

	ISP_API_EQ(hbn_isp_get_exposure_roi(ctx->vnode_fd[1], &exp_roi), return);

	printf("roi_weight: %f\n", exp_roi.roi_weight);
	for (i = 0; i < exp_roi.roi_num; i++) {
		printf("---------table %d---------\n", i);
		printf("weight: %f\n", exp_roi.roi_window[i].weight);
		printf("h_offset: %d\n", exp_roi.roi_window[i].window.h_offset);
		printf("v_offset: %d\n", exp_roi.roi_window[i].window.v_offset);
		printf("width: %d\n", exp_roi.roi_window[i].window.width);
		printf("height: %d\n", exp_roi.roi_window[i].window.height);
	}
}

void tuning_hanle_set_ae_zone_weight(tuning_context_t *ctx)
{
	int32_t i, j;
	hbn_isp_ae_zone_weight_attr_t weight_attr = {0};

	for (i = 0; i < HBN_ISP_AE_ZONE_GRID_NUM; i++) {
		for (j = 0; j < HBN_ISP_AE_ZONE_GRID_NUM; j++) {
			if (i > 10 && i < 20 && j > 10 && j < 20) {
				weight_attr.weight[i * HBN_ISP_AE_ZONE_GRID_NUM + j].weight = 2.0;
			} else {
				weight_attr.weight[i * HBN_ISP_AE_ZONE_GRID_NUM + j].weight = 1.0;
			}
		}
	}

	ISP_API_EQ(hbn_isp_set_ae_zone_weight_attr(ctx->vnode_fd[1], &weight_attr), return);
}

void tuning_hanle_get_ae_zone_weight(tuning_context_t *ctx)
{
	int32_t i, j;
	hbn_isp_ae_zone_weight_attr_t weight_attr = {0};

	ISP_API_EQ(hbn_isp_get_ae_zone_weight_attr(ctx->vnode_fd[1], &weight_attr), return);

	for (i = 0; i < HBN_ISP_AE_ZONE_GRID_NUM; i++) {
		printf("%d:", i);
		for (j = 0; j < HBN_ISP_AE_ZONE_GRID_NUM; j++) {
			printf(" %.1f", weight_attr.weight[i * HBN_ISP_AE_ZONE_GRID_NUM + j].weight);
		}
		printf("\n");
	}
}

void tuning_dump_yuv(tuning_context_t *ctx)
{
	read_p("typing the number to dump: ", "%d", &ctx->yuv_dump_cnt);
}

void tuning_get_ae_statistics(tuning_context_t *ctx)
{
	int32_t col, row;
	uint32_t *luma;
	hbn_isp_ae_statistics_t ae_statistics = {0};

	ISP_API_EQ(hbn_isp_get_ae_statistics(ctx->vnode_fd[1], &ae_statistics), return);

	printf("Ae statistics current frameid: %d, timestamps: %ld\n", ae_statistics.frame_id, ae_statistics.timestamps);
	printf("Datatype: %d\n", ae_statistics.datatype);

	for (col = 0; col < HBN_ISP_AE_ZONE_GRID_NUM * HBN_ISP_PIXEL_CHANNEL; col += HBN_ISP_PIXEL_CHANNEL) {
		for (row = 0; row < HBN_ISP_AE_ZONE_GRID_NUM; row++) {
			luma = &ae_statistics.expStat[HBN_ISP_AE_ZONE_GRID_NUM * HBN_ISP_PIXEL_CHANNEL * row + col];
			printf("(%d, %d, %d, %d) ", *luma, *(luma+1), *(luma+2), *(luma+3));
		}
		printf("\n");
	}
	printf("done\n");
}

void tuning_set_module_control(tuning_context_t *ctx)
{
	hbn_isp_module_ctrl_t module_ctrl = {0};
	uint32_t key;

	printf("Typing [1] to enable, [0] to disable\n");
	read_p("CCM: ", "%d", &key); module_ctrl.module.u32Key |= key << 0;
	read_p("CNR: ", "%d", &key); module_ctrl.module.u32Key |= key << 1;
	read_p("CPROC: ", "%d", &key); module_ctrl.module.u32Key |= key << 2;
	read_p("DG: ", "%d", &key); module_ctrl.module.u32Key |= key << 3;
	read_p("Demosaic: ", "%d", &key); module_ctrl.module.u32Key |= key << 4;
	read_p("DPCC: ", "%d", &key); module_ctrl.module.u32Key |= key << 5;
	read_p("2DNR: ", "%d", &key); module_ctrl.module.u32Key |= key << 6;
	read_p("3DNR: ", "%d", &key); module_ctrl.module.u32Key |= key << 7;
	read_p("EE: ", "%d", &key); module_ctrl.module.u32Key |= key << 8;
	read_p("LSC: ", "%d", &key); module_ctrl.module.u32Key |= key << 9;
	read_p("LUT3D: ", "%d", &key); module_ctrl.module.u32Key |= key << 10;
	read_p("WDR: ", "%d", &key); module_ctrl.module.u32Key |= key << 11;
	read_p("YNR: ", "%d", &key); module_ctrl.module.u32Key |= key << 12;
	read_p("GE: ", "%d", &key); module_ctrl.module.u32Key |= key << 13;
	read_p("WB: ", "%d", &key); module_ctrl.module.u32Key |= key << 14;

	ISP_API_EQ(hbn_isp_set_module_control(ctx->vnode_fd[1], &module_ctrl), return);
}

void tuning_get_module_control(tuning_context_t *ctx)
{
	hbn_isp_module_ctrl_t module_ctrl = {0};
	ISP_API_EQ(hbn_isp_get_module_control(ctx->vnode_fd[1], &module_ctrl), return);

	printf("module_ctrl.module.u32Key %d\n", module_ctrl.module.u32Key);

	printf("CCM: %s\n", (module_ctrl.module.u32Key & 1 << 0)?"Enable":"Disable");
	printf("CNR: %s\n", (module_ctrl.module.u32Key & 1 << 1)?"Enable":"Disable");
	printf("CPROC: %s\n", (module_ctrl.module.u32Key & 1 << 2)?"Enable":"Disable");
	printf("DG: %s\n", (module_ctrl.module.u32Key & 1 << 3)?"Enable":"Disable");
	printf("Demosaic: %s\n", (module_ctrl.module.u32Key & 1 << 4)?"Enable":"Disable");
	printf("DPCC: %s\n", (module_ctrl.module.u32Key & 1 << 5)?"Enable":"Disable");
	printf("2DNR: %s\n", (module_ctrl.module.u32Key & 1 << 6)?"Enable":"Disable");
	printf("3DNR: %s\n", (module_ctrl.module.u32Key & 1 << 7)?"Enable":"Disable");
	printf("EE: %s\n", (module_ctrl.module.u32Key & 1 << 8)?"Enable":"Disable");
	printf("LSC: %s\n", (module_ctrl.module.u32Key & 1 << 9)?"Enable":"Disable");
	printf("LUT3D: %s\n", (module_ctrl.module.u32Key & 1 << 10)?"Enable":"Disable");
	printf("WDR: %s\n", (module_ctrl.module.u32Key & 1 << 11)?"Enable":"Disable");
	printf("YNR: %s\n", (module_ctrl.module.u32Key & 1 << 12)?"Enable":"Disable");
	printf("GE: %s\n", (module_ctrl.module.u32Key & 1 << 13)?"Enable":"Disable");
	printf("WB: %s\n", (module_ctrl.module.u32Key & 1 << 14)?"Enable":"Disable");
}

void tuning_get_af_statistics(tuning_context_t *ctx)
{
	int32_t i, j, pos;
	hbn_isp_af_statistics_t af_statistics = {0};

	ISP_API_EQ(hbn_isp_get_af_statistics(ctx->vnode_fd[1], &af_statistics), return);

	printf("frame_id: %d\n", af_statistics.frame_id);
	printf("sharpnessLowPass:\n");
	for (i = 0; i < 15; i++) {
		for (j = 0; j < 15; j++) {
			pos = i * 15 + j;
			printf(" %d", af_statistics.sharpnessLowPass[pos]);
		}
		printf("\n");
	}
	printf("\n");

	printf("sharpnessHighPass:\n");
	for (i = 0; i < 15; i++) {
		for (j = 0; j < 15; j++) {
			pos = i * 15 + j;
			printf(" %d", af_statistics.sharpnessHighPass[pos]);
		}
		printf("\n");
	}
	printf("\n");

	printf("histLowData:\n");
	for (i = 0; i < 15; i++) {
		for (j = 0; j < 15; j++) {
			pos = i * 15 + j;
			printf(" %d", af_statistics.histLowData[pos]);
		}
		printf("\n");
	}
	printf("\n");

	printf("histHighData:\n");
	for (i = 0; i < 15; i++) {
		for (j = 0; j < 15; j++) {
			pos = i * 15 + j;
			printf(" %d", af_statistics.histHighData[pos]);
		}
		printf("\n");
	}
}

void tuning_hanle_set_2dnr_attr(tuning_context_t *ctx)
{
	uint32_t mode;
	hbn_isp_2dnr_attr_t dnr2_attr = {0};

	read_p("typing the 2dnr mode, manual(0)/auto(1): ", "%d", &mode);
	ISP_API_EQ(hbn_isp_get_2dnr_attr(ctx->vnode_fd[1], &dnr2_attr), return);

	if (mode == 0) {
		dnr2_attr.mode = HBN_ISP_MODE_MANUAL;
	} else if (mode == 1) {
		dnr2_attr.mode = HBN_ISP_MODE_AUTO;
	} else {
		printf("Unknown mode: %d\n", mode);
		return;
	}

	ISP_API_EQ(hbn_isp_set_2dnr_attr(ctx->vnode_fd[1], &dnr2_attr), return);
}

void tuning_hanle_get_2dnr_attr(tuning_context_t *ctx)
{
	hbn_isp_2dnr_attr_t dnr2_attr = {0};

	ISP_API_EQ(hbn_isp_get_2dnr_attr(ctx->vnode_fd[1], &dnr2_attr), return);

	printf("2dnr is in %s mode\n", (dnr2_attr.mode == HBN_ISP_MODE_MANUAL)?"manual":"auto");
	printf("2dnr current value:\n");
	printf("blend_static: %f\n", dnr2_attr.manual_attr.blend_static);
	printf("blend_motion: %f\n", dnr2_attr.manual_attr.blend_motion);
	printf("blend_slope: %f\n", dnr2_attr.manual_attr.blend_slope);
	printf("vst_factor: %f\n", dnr2_attr.manual_attr.vst_factor);

	pr_linear("sigma_scale", HBN_ISP_2DNR_SIGMA_NUM, " %f", dnr2_attr.manual_attr.sigma_scale);
	pr_linear("sigma_factor_mul", HBN_ISP_2DNR_SIGMA_NUM, " %f", dnr2_attr.manual_attr.sigma_factor_mul);

	printf("sigma_factor_motion_max: %d\n", dnr2_attr.manual_attr.sigma_factor_motion_max);
	printf("sigma_factor_motion_min: %d\n", dnr2_attr.manual_attr.sigma_factor_motion_min);
	printf("sigma_offset: %d\n", dnr2_attr.manual_attr.sigma_offset);

	pr_double("static_detail_thresh", HBN_ISP_2DNR_STATIC_X_NUM, HBN_ISP_2DNR_STATIC_Y_NUM,
		" %d", dnr2_attr.manual_attr.static_detail_thresh);
	pr_double("static_detail_boost_thresh", HBN_ISP_2DNR_STATIC_X_NUM, HBN_ISP_2DNR_STATIC_Y_NUM,
		" %d", dnr2_attr.manual_attr.static_detail_boost_thresh);
	pr_double("static_detail_boost", HBN_ISP_2DNR_STATIC_X_NUM, HBN_ISP_2DNR_STATIC_Y_NUM,
		" %f", dnr2_attr.manual_attr.static_detail_boost);
	pr_double("static_detail_clip_thresh", HBN_ISP_2DNR_STATIC_X_NUM, HBN_ISP_2DNR_STATIC_Y_NUM,
		" %d", dnr2_attr.manual_attr.static_detail_clip_thresh);

	pr_double("moving_detail_thresh", HBN_ISP_2DNR_MOVING_X_NUM, HBN_ISP_2DNR_MOVING_Y_NUM,
		" %d", dnr2_attr.manual_attr.moving_detail_thresh);
	pr_double("moving_detail_boost_thresh", HBN_ISP_2DNR_MOVING_X_NUM, HBN_ISP_2DNR_MOVING_Y_NUM,
		" %d", dnr2_attr.manual_attr.moving_detail_boost_thresh);
	pr_double("moving_detail_boost", HBN_ISP_2DNR_MOVING_X_NUM, HBN_ISP_2DNR_MOVING_Y_NUM,
		" %f", dnr2_attr.manual_attr.moving_detail_boost);
	pr_double("moving_detail_clip_thresh", HBN_ISP_2DNR_MOVING_X_NUM, HBN_ISP_2DNR_MOVING_Y_NUM,
		" %d", dnr2_attr.manual_attr.moving_detail_clip_thresh);
	pr_linear("static_factor", HBN_ISP_2DNR_SIGMA_NUM, " %f", dnr2_attr.manual_attr.static_factor);

	printf("luma_curve_cfg:\n");
	pr_linear("    ary_x", HBN_ISP_2DNR_CURVE_SIZE, " %d", dnr2_attr.manual_attr.luma_curve_cfg.ary_x);
	pr_linear("    ary_y", HBN_ISP_2DNR_CURVE_SIZE, " %d", dnr2_attr.manual_attr.luma_curve_cfg.ary_y);
	pr_linear("    ary_px", HBN_ISP_2DNR_CURVE_SIZE, " %d", dnr2_attr.manual_attr.luma_curve_cfg.ary_px);
	printf("    interp_mode: %d\n", dnr2_attr.manual_attr.luma_curve_cfg.interp_mode);

	printf("lsc_comp_curve_cfg:\n");
	pr_linear("    ary_x", HBN_ISP_2DNR_CURVE_SIZE, " %d", dnr2_attr.manual_attr.lsc_comp_curve_cfg.ary_x);
	pr_linear("    ary_y", HBN_ISP_2DNR_CURVE_SIZE, " %d", dnr2_attr.manual_attr.lsc_comp_curve_cfg.ary_y);
	pr_linear("    ary_px", HBN_ISP_2DNR_CURVE_SIZE, " %d", dnr2_attr.manual_attr.lsc_comp_curve_cfg.ary_px);
	printf("    interp_mode: %d\n", dnr2_attr.manual_attr.lsc_comp_curve_cfg.interp_mode);

	printf("motion_cfg:\n");
	pr_linear("    motion_anchor_x", HBN_ISP_2DNR_MOTION_SIZE, " %d", dnr2_attr.manual_attr.motion_cfg.motion_anchor_x);
	pr_linear("    ary_x", HBN_ISP_2DNR_CURVE_SIZE, " %d", dnr2_attr.manual_attr.motion_cfg.curve_cfg.ary_x);
	pr_linear("    ary_y", HBN_ISP_2DNR_CURVE_SIZE, " %d", dnr2_attr.manual_attr.motion_cfg.curve_cfg.ary_y);
	pr_linear("    ary_px", HBN_ISP_2DNR_CURVE_SIZE, " %d", dnr2_attr.manual_attr.motion_cfg.curve_cfg.ary_px);
	printf("    interp_mode: %d\n", dnr2_attr.manual_attr.motion_cfg.curve_cfg.interp_mode);


	// printf("2dnr auto config value:\n");
	printf("auto_level: %d\n", dnr2_attr.auto_attr.auto_level);
	// pr_linear("gain", dnr2_attr.auto_attr.auto_level, " %f", dnr2_attr.auto_attr.gain);
	// pr_linear("vst_factor", dnr2_attr.auto_attr.auto_level, " %f", dnr2_attr.auto_attr.vst_factor);
	// pr_linear("blend_static", dnr2_attr.auto_attr.auto_level, " %f", dnr2_attr.auto_attr.blend_static);
	// pr_linear("blend_motion", dnr2_attr.auto_attr.auto_level, " %f", dnr2_attr.auto_attr.blend_motion);
	// pr_linear("blend_slope", dnr2_attr.auto_attr.auto_level, " %f", dnr2_attr.auto_attr.blend_slope);
	// pr_linear("sigma_offset", dnr2_attr.auto_attr.auto_level, " %d", dnr2_attr.auto_attr.sigma_offset);
	// pr_double("luma_curve_y", dnr2_attr.auto_attr.auto_level, HBN_ISP_2DNR_CURVE_SIZE, " %d", dnr2_attr.auto_attr.luma_curve_y);
	// pr_double("lsc_comp_curve_y", dnr2_attr.auto_attr.auto_level, HBN_ISP_2DNR_CURVE_SIZE, " %d", dnr2_attr.auto_attr.lsc_comp_curve_y);
	// pr_double("motion_fac_curve_y", dnr2_attr.auto_attr.auto_level, HBN_ISP_2DNR_CURVE_SIZE, " %d", dnr2_attr.auto_attr.motion_fac_curve_y);
	// pr_double("motion_anchor_x", dnr2_attr.auto_attr.auto_level, HBN_ISP_2DNR_MOTION_SIZE, " %d", dnr2_attr.auto_attr.motion_anchor_x);

	// pr_double("sigma_scale", dnr2_attr.auto_attr.auto_level, HBN_ISP_2DNR_SIGMA_NUM, " %f", dnr2_attr.auto_attr.sigma_scale);
	// pr_double("static_factor", dnr2_attr.auto_attr.auto_level, HBN_ISP_2DNR_SIGMA_NUM, " %f", dnr2_attr.auto_attr.static_factor);
	// pr_double("sigma_factor_mul", dnr2_attr.auto_attr.auto_level, HBN_ISP_2DNR_SIGMA_NUM, " %f", dnr2_attr.auto_attr.sigma_factor_mul);
	// pr_linear("sigma_factor_motion_max", dnr2_attr.auto_attr.auto_level, " %d", dnr2_attr.auto_attr.sigma_factor_motion_max);

	// pr_triple("static_detail_thresh", dnr2_attr.auto_attr.auto_level,
	// 	HBN_ISP_2DNR_STATIC_X_NUM, HBN_ISP_2DNR_STATIC_Y_NUM, " %d", dnr2_attr.auto_attr.static_detail_thresh);
	// pr_triple("static_detail_boost_thresh", dnr2_attr.auto_attr.auto_level,
	// 	HBN_ISP_2DNR_STATIC_X_NUM, HBN_ISP_2DNR_STATIC_Y_NUM, " %d", dnr2_attr.auto_attr.static_detail_boost_thresh);
	// pr_triple("static_detail_boost", dnr2_attr.auto_attr.auto_level,
	// 	HBN_ISP_2DNR_STATIC_X_NUM, HBN_ISP_2DNR_STATIC_Y_NUM, " %f", dnr2_attr.auto_attr.static_detail_boost);
	// pr_triple("static_detail_clip_thresh", dnr2_attr.auto_attr.auto_level,
	// 	HBN_ISP_2DNR_STATIC_X_NUM, HBN_ISP_2DNR_STATIC_Y_NUM, " %d", dnr2_attr.auto_attr.static_detail_clip_thresh);
	
	// pr_triple("moving_detail_thresh", dnr2_attr.auto_attr.auto_level,
	// 	HBN_ISP_2DNR_MOVING_X_NUM, HBN_ISP_2DNR_MOVING_Y_NUM, " %d", dnr2_attr.auto_attr.moving_detail_thresh);
	// pr_triple("moving_detail_boost_thresh", dnr2_attr.auto_attr.auto_level,
	// 	HBN_ISP_2DNR_MOVING_X_NUM, HBN_ISP_2DNR_MOVING_Y_NUM, " %d", dnr2_attr.auto_attr.moving_detail_boost_thresh);
	// pr_triple("moving_detail_boost", dnr2_attr.auto_attr.auto_level,
	// 	HBN_ISP_2DNR_MOVING_X_NUM, HBN_ISP_2DNR_MOVING_Y_NUM, " %f", dnr2_attr.auto_attr.moving_detail_boost);
	// pr_triple("moving_detail_clip_thresh", dnr2_attr.auto_attr.auto_level,
	// 	HBN_ISP_2DNR_MOVING_X_NUM, HBN_ISP_2DNR_MOVING_Y_NUM, " %d", dnr2_attr.auto_attr.moving_detail_clip_thresh);
	
}

void tuning_hanle_set_3dnr_attr(tuning_context_t *ctx)
{
	uint32_t mode;
	hbn_isp_3dnr_attr_t dnr3_attr = {0};

	read_p("typing the 2dnr mode, manual(0)/auto(1): ", "%d", &mode);
	ISP_API_EQ(hbn_isp_get_3dnr_attr(ctx->vnode_fd[1], &dnr3_attr), return);

	if (mode == 0) {
		dnr3_attr.mode = HBN_ISP_MODE_MANUAL;
	} else if (mode == 1) {
		dnr3_attr.mode = HBN_ISP_MODE_AUTO;
	} else {
		printf("Unknown mode: %d\n", mode);
		return;
	}

	ISP_API_EQ(hbn_isp_set_3dnr_attr(ctx->vnode_fd[1], &dnr3_attr), return);
}

void tuning_hanle_get_3dnr_attr(tuning_context_t *ctx)
{
	hbn_isp_3dnr_attr_t dnr3_attr = {0};

	ISP_API_EQ(hbn_isp_get_3dnr_attr(ctx->vnode_fd[1], &dnr3_attr), return);

	printf("3dnr is in %s mode\n", (dnr3_attr.mode == HBN_ISP_MODE_MANUAL)?"manual":"auto");
	printf("3dnr current value:\n");
	printf("vst_factor: %f\n", dnr3_attr.manual_attr.vst_factor);
	printf("tnr_strength: %d\n", dnr3_attr.manual_attr.tnr_strength);
	printf("tnr_strength2: %d\n", dnr3_attr.manual_attr.tnr_strength2);
	printf("filter_len: %d\n", dnr3_attr.manual_attr.filter_len);
	printf("filter_len2: %d\n", dnr3_attr.manual_attr.filter_len2);
	printf("motion_smooth_factor: %f\n", dnr3_attr.manual_attr.motion_smooth_factor);
	printf("range_h: %d\n", dnr3_attr.manual_attr.range_h);
	printf("sad_weight: %d\n", dnr3_attr.manual_attr.sad_weight);
	printf("diff_type: %d\n", dnr3_attr.manual_attr.diff_type);
	printf("sqr_diff_factor: %d\n", dnr3_attr.manual_attr.sqr_diff_factor);
	printf("motion_smooth_lvl: %d\n", dnr3_attr.manual_attr.motion_smooth_lvl);
	printf("dilate_h: %d\n", dnr3_attr.manual_attr.dilate_h);
	printf("noise_level: %d\n", dnr3_attr.manual_attr.noise_level);
	printf("thr_motion_slope: %d\n", dnr3_attr.manual_attr.thr_motion_slope);
	pr_linear("tnr_luma_curve_x", HBN_ISP_3DNR_THR_LUMA_CURVE_NUM, " %d", dnr3_attr.manual_attr.tnr_luma_curve_x);
	pr_linear("tnr_luma_curve_y", HBN_ISP_3DNR_THR_LUMA_CURVE_NUM, " %d", dnr3_attr.manual_attr.tnr_luma_curve_y);
	pr_linear("tnr_motion_slop_y", HBN_ISP_3DNR_THR_LUMA_CURVE_NUM, " %d", dnr3_attr.manual_attr.tnr_motion_slop_y);
	printf("noise_cfg:\n");
	printf("    input_bits: %d\n", dnr3_attr.manual_attr.noise_cfg.input_bits);
	printf("    fix_curve_start: %d\n", dnr3_attr.manual_attr.noise_cfg.fix_curve_start);
	printf("    noisemodel_a: %f\n", dnr3_attr.manual_attr.noise_cfg.noisemodel_a);
	printf("    noisemodel_b: %f\n", dnr3_attr.manual_attr.noise_cfg.noisemodel_b);
	pr_linear("    bls_exp", HBN_ISP_3DNR_BLS_EXP_NUM, " %d", dnr3_attr.manual_attr.noise_cfg.bls_exp);


	// printf("3dnr auto config value:\n");
	// printf("auto_level: %d\n", dnr3_attr.auto_attr.auto_level);
	// printf("nm_k: %f\n", dnr3_attr.auto_attr.nm_k);
	// printf("nm_p: %f\n", dnr3_attr.auto_attr.nm_p);
	// pr_linear("gains", dnr3_attr.auto_attr.auto_level, " %f", dnr3_attr.auto_attr.gains);
	// pr_linear("fix_curve_start", dnr3_attr.auto_attr.auto_level, " %d", dnr3_attr.auto_attr.fix_curve_start);
	// pr_linear("noisemodel_a", dnr3_attr.auto_attr.auto_level, " %f", dnr3_attr.auto_attr.noisemodel_a);
	// pr_linear("noisemodel_b", dnr3_attr.auto_attr.auto_level, " %f", dnr3_attr.auto_attr.noisemodel_b);
	// pr_linear("tnr_strength", dnr3_attr.auto_attr.auto_level, " %d", dnr3_attr.auto_attr.tnr_strength);
	// pr_linear("tnr_strength2", dnr3_attr.auto_attr.auto_level, " %d", dnr3_attr.auto_attr.tnr_strength2);
	// pr_linear("filter_len", dnr3_attr.auto_attr.auto_level, " %d", dnr3_attr.auto_attr.filter_len);
	// pr_linear("filter_len2", dnr3_attr.auto_attr.auto_level, " %d", dnr3_attr.auto_attr.filter_len2);
	// pr_linear("motion_smooth_factor", dnr3_attr.auto_attr.auto_level, " %f", dnr3_attr.auto_attr.motion_smooth_factor);
	// pr_linear("range_h", dnr3_attr.auto_attr.auto_level, " %d", dnr3_attr.auto_attr.range_h);
	// pr_linear("sad_weight", dnr3_attr.auto_attr.auto_level, " %d", dnr3_attr.auto_attr.sad_weight);
	// pr_linear("sqr_diff_factor", dnr3_attr.auto_attr.auto_level, " %d", dnr3_attr.auto_attr.sqr_diff_factor);
	// pr_linear("motion_smooth_lvl", dnr3_attr.auto_attr.auto_level, " %d", dnr3_attr.auto_attr.motion_smooth_lvl);
	// pr_linear("motion_dilate_en", dnr3_attr.auto_attr.auto_level, " %d", dnr3_attr.auto_attr.motion_dilate_en);
	// pr_linear("dilate_h", dnr3_attr.auto_attr.auto_level, " %d", dnr3_attr.auto_attr.dilate_h);
	// pr_double("bls_exp", dnr3_attr.auto_attr.auto_level, HBN_ISP_3DNR_BLS_EXP_NUM, " %d", dnr3_attr.auto_attr.bls_exp);
	// pr_double("tnr_luma_curve_y", dnr3_attr.auto_attr.auto_level, HBN_ISP_3DNR_THR_LUMA_CURVE_NUM, " %d", dnr3_attr.auto_attr.tnr_luma_curve_y);
	// pr_double("tnr_motion_slop_y", dnr3_attr.auto_attr.auto_level, HBN_ISP_3DNR_THR_LUMA_CURVE_NUM, " %d", dnr3_attr.auto_attr.tnr_motion_slop_y);
}

void tuning_hanle_set_awb_preference_attr(tuning_context_t *ctx)
{
	int32_t illum, level;
	hbn_isp_awb_preference_attr_t awb_pre_attr = {0};

	for (illum = 0; illum < HBN_ISP_ILLUPROFILE_NUM; illum++) {
		awb_pre_attr.gray_preference[illum].enable = 1;
		for (level = 0; level < HBN_ISP_AWB_LIGHT_LEVEL; level++) {
			awb_pre_attr.gray_preference[illum].brightness_level[level] = 1.0f + level;
			awb_pre_attr.gray_preference[illum].gray_rgain[level] = 256 + level;
			awb_pre_attr.gray_preference[illum].gray_bgain[level] = 256 - level;
		}
	}

	ISP_API_EQ(hbn_isp_set_awb_preference_attr(ctx->vnode_fd[1], &awb_pre_attr), return);
}

void tuning_hanle_get_awb_preference_attr(tuning_context_t *ctx)
{
	int32_t illum, level;
	hbn_isp_awb_preference_attr_t awb_pre_attr = {0};

	ISP_API_EQ(hbn_isp_get_awb_preference_attr(ctx->vnode_fd[1], &awb_pre_attr), return);

	for (illum = 0; illum < HBN_ISP_ILLUPROFILE_NUM; illum++) {
		printf("illum: %d, %s\n", illum, (awb_pre_attr.gray_preference[illum].enable ? "enable":"disable"));
		printf("brightness_level:");
		for (level = 0; level < HBN_ISP_AWB_LIGHT_LEVEL; level++) {
			printf(" %.1f", awb_pre_attr.gray_preference[illum].brightness_level[level]);
		}
		printf("\n");
		printf("gray_rgain:");
		for (level = 0; level < HBN_ISP_AWB_LIGHT_LEVEL; level++) {
			printf(" %d", awb_pre_attr.gray_preference[illum].gray_rgain[level]);
		}
		printf("\n");
		printf("gray_bgain:");
		for (level = 0; level < HBN_ISP_AWB_LIGHT_LEVEL; level++) {
			printf(" %d", awb_pre_attr.gray_preference[illum].gray_bgain[level]);
		}
		printf("\n");
	}
}
