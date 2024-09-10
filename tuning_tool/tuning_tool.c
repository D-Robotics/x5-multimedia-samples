/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include <pthread.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "tuning_tool.h"

static void parse_opts(int argc, char *argv[], tuning_context_t *ctx)
{
	int32_t cmd_ret;
	const char short_options[] = PARSE_SHORT_OPTS;
	const struct option long_options[] = PARSE_LONG_OPTS;

	while (1) {
		cmd_ret = getopt_long(argc, argv, short_options, long_options, NULL);
		if (cmd_ret == -1)
			break;

		switch (cmd_ret) {
		case 'c':
			sprintf(&ctx->cam_json[0], "%s", optarg);
			break;
		case 'v':
			sprintf(&ctx->vpm_json[0], "%s", optarg);
			break;
		case 'd':
			ctx->dump_mask = atoi(optarg);
			break;
		case 'r':
			ctx->send_raw = atoi(optarg);
			break;
		case 's':
			ctx->dump_stream_flag = atoi(optarg);
			break;
		case 'w':
			ctx->work_mode = atoi(optarg);
			break;
		default:
			parse_opts_print(argv[0]);
			break;
		}
	}
}

static int32_t tuning_specify_case(tuning_context_t *ctx)
{
	if (ctx->cam_json[0] == 0)
		sprintf(&ctx->cam_json[0], "%s", DEF_CAM_PATH);

	if (BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK)) {
		pr_tuning("tuning_tool run with feedback\n");
	}

	if (ctx->vpm_json[0] == 0)
		sprintf(&ctx->vpm_json[0], "%s", DEF_VPM_PATH);

	return 0;
}

static int32_t tuning_feeback_prepare_next(tuning_context_t *ctx)
{
	struct stat statbuf;
	FILE *file = NULL;

	stat(ctx->img_path[ctx->cur_img], &statbuf);
	if (!statbuf.st_size) {
		pr_tuning("No such file: %s\n", ctx->img_path[ctx->cur_img]);
		return -1;
	}
#ifdef TUNING_DEBUG
	pr_tuning("feedback file: %s, index %d, size %ld\n", ctx->img_path[ctx->cur_img], ctx->cur_img, statbuf.st_size);
#endif

	file = fopen(ctx->img_path[ctx->cur_img], "r");
	if (!file) {
		pr_tuning("open %s fail\n", ctx->img_path[ctx->cur_img]);
		return -1;
	}

	if ((uint32_t)statbuf.st_size > ctx->src_img.buffer.size[0]) {
		pr_tuning("alloc buffer donot match src file size!\n");
		return -1;
	}

	fread(ctx->src_img.buffer.virt_addr[0], 1, statbuf.st_size, file);
	fclose(file);

	ctx->cur_img = (ctx->cur_img + 1) >= ctx->img_num ? 0 : ctx->cur_img + 1;

	return 0;
}

static void *tuning_main_worker_thread(void *arg)
{
	tuning_context_t *ctx;
	int ret;
	hbn_vnode_image_t raw_img = {0};
	hbn_vnode_image_t yuv_img = {0};
	static int32_t yuv_stream_cnt = 0;
	char file_name[32] = {0};

	ctx = (tuning_context_t *)arg;
#ifdef TUNING_DEBUG
	pr_tuning("%s run \n", __func__);
#endif

	if (ctx->send_raw &&
		(BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK) || !ctx->is_offline)) {
		pr_tuning("Cannot send raw in feedback mode or ddr disable!\n");
		goto out;
	}

	while (1) {
		if (ctx->send_raw) {
			ret = hbn_vnode_getframe(ctx->vnode_fd[0], 0, 1000, &raw_img);
			if (ret) {
				pr_tuning("get buffer from sif fail\n");
				goto out;
			}

			if (HBPLAYER_EN) {
				ret = tuning_send_raw_to_hbplayer(ctx->hbplayer_event, &raw_img,
					(ctx->vin_format == 0x2A ? RAW_8 : RAW_10), 0);
				if (ret)
					pr_tuning("send to hbplayer failed, skip it\n");
			}
			hbn_vnode_releaseframe(ctx->vnode_fd[0], 0, &raw_img);
		}
		if (BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK)) {
			FUNC_EQ(tuning_feeback_prepare_next(ctx), 0, goto out);
			ret = hbn_vnode_sendframe(ctx->vnode_fd[1], 0, &ctx->src_img);
			if (ret) {
				pr_tuning("isp hbn_vnode_sendframe failed!\n");
				goto out;
			}
		}

		ret = hbn_vnode_getframe(ctx->vnode_fd[1], 0, 1000, &yuv_img);
		if (ret) {
			pr_tuning("get buffer from isp fail\n");
			goto out;
		}

		if (ctx->yuv_dump_cnt) {
			snprintf(file_name, TUNING_PRINT_SIZE_MAX, "%s/stream%d.yuv", DEF_DUMP_PATH, yuv_stream_cnt);
			tuning_dump_file(file_name, &yuv_img);
			ctx->yuv_dump_cnt--;
			if (!ctx->yuv_dump_cnt) {
				yuv_stream_cnt++;
				pr_tuning("Input cmd:");
			}
		}

		if (HBPLAYER_EN) {
			ret = tuning_send_yuv_to_hbplayer(ctx->hbplayer_event, &yuv_img, 0);
			if (ret)
				pr_tuning("send to hbplayer failed, skip it\n");
		}
#ifdef TUNING_DEBUG
		pr_tuning("get buffer size %ld-%ld\n", yuv_img.buffer.size[0], yuv_img.buffer.size[1]);
#endif
		hbn_vnode_releaseframe(ctx->vnode_fd[1], 0, &yuv_img);
	}
out:
	pr_tuning("typing [q] to quit\n");
	sleep(100000);

	return NULL;
}

static void tuning_dump_sif_raw(tuning_context_t *ctx)
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

static void tuning_handle_set_expsoure(tuning_context_t *ctx)
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
		FUNC_EQ(hbn_isp_get_exposure_attr(ctx->vnode_fd[1], &exp_attr), 0, return);
		exp_attr.mode = HBN_ISP_MODE_AUTO;
		read_p("speed_over: ", "%f", &exp_attr.auto_attr.speed_over);
		read_p("speed_under: ", "%f", &exp_attr.auto_attr.speed_under);
		read_p("tolerance: ", "%f", &exp_attr.auto_attr.tolerance);
		read_p("target: ", "%f", &exp_attr.auto_attr.target);
		read_p("flicker_freq: ", "%f", &exp_attr.auto_attr.flicker_freq);
		read_p("anti_flicker_status: ", "%d", &exp_attr.auto_attr.anti_flicker_status);
	} else {
		printf("Unknown mode: %d\n", exp_mode);
		return;
	}

	FUNC_EQ(hbn_isp_set_exposure_attr(ctx->vnode_fd[1], &exp_attr), 0, return);
}

static void tuning_handle_get_expsoure(tuning_context_t *ctx)
{
	hbn_isp_exposure_attr_t exp_attr = {0};

	FUNC_EQ(hbn_isp_get_exposure_attr(ctx->vnode_fd[1], &exp_attr), 0, return);

	printf("Currently AE is in %s mode\n", (exp_attr.mode == HBN_ISP_MODE_MANUAL)?"manual":"auto");

	printf("exp_time: %f\n", exp_attr.manual_attr.exp_time);
	printf("again: %f\n", exp_attr.manual_attr.again);
	printf("dgain: %f\n", exp_attr.manual_attr.dgain);
	printf("ispgain: %f\n", exp_attr.manual_attr.ispgain);
	printf("ae_exp: %f\n",exp_attr.manual_attr.ae_exp);
	printf("mode %d\n", exp_attr.auto_attr.mode);

	printf("exp range %f~%f\n", exp_attr.auto_attr.exp_time_range.min, exp_attr.auto_attr.exp_time_range.max);
	printf("again range %f~%f\n", exp_attr.auto_attr.again_range.min, exp_attr.auto_attr.again_range.max);
	printf("dgain range %f~%f\n", exp_attr.auto_attr.dgain_range.min, exp_attr.auto_attr.dgain_range.max);
	printf("isp dgain range %f~%f\n", exp_attr.auto_attr.isp_dgain_range.min, exp_attr.auto_attr.isp_dgain_range.max);
	printf("speed_over %f\n", exp_attr.auto_attr.speed_over);
	printf("speed_under %f\n", exp_attr.auto_attr.speed_under);
	printf("tolerance %f\n", exp_attr.auto_attr.tolerance);
	printf("target %f\n", exp_attr.auto_attr.target);
	printf("flicker_freq %f\n", exp_attr.auto_attr.flicker_freq);
	printf("anti_flicker_status %d\n", exp_attr.auto_attr.anti_flicker_status);
}

static void tuning_handle_set_white_balance(tuning_context_t *ctx)
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
		FUNC_EQ(hbn_isp_get_awb_attr(ctx->vnode_fd[1], &awb_attr), 0, return);
		awb_attr.mode = HBN_ISP_MODE_AUTO;
	} else {
		printf("Unknown mode: %d\n", awb_mode);
		return;
	}

	FUNC_EQ(hbn_isp_set_awb_attr(ctx->vnode_fd[1], &awb_attr), 0, return);
}

static void tuning_handle_get_white_balance(tuning_context_t *ctx)
{
	hbn_isp_awb_attr_t awb_attr = {0};

	FUNC_EQ(hbn_isp_get_awb_attr(ctx->vnode_fd[1], &awb_attr), 0, return);

	printf("Currently AWB is in %s mode\n", (awb_attr.mode == HBN_ISP_MODE_MANUAL)?"manual":"auto");

	printf("temper %d\n", awb_attr.auto_attr.temper);
	printf("rgain %f\n", awb_attr.auto_attr.gain.rgain);
	printf("grgain %f\n", awb_attr.auto_attr.gain.grgain);
	printf("gbgain %f\n", awb_attr.auto_attr.gain.gbgain);
	printf("bgain %f\n", awb_attr.auto_attr.gain.bgain);
}

static void tuning_hanle_set_ae_table(tuning_context_t *ctx)
{
	int32_t i;
	hbn_isp_exposure_table_t ae_table_attr = {0};

	for (i = 0; i < CAMDEV_AE_EXP_TABLE_NUM; i++) {
		printf("input table %d, typing [0] to finish:\n", i);
		read_p("exposure_time: ", "%f", &ae_table_attr.exp_table[i].exposure_time);
		if (ae_table_attr.exp_table[i].exposure_time == 0)
			break;

		read_p("again: ", "%f", &ae_table_attr.exp_table[i].again);
		read_p("dgain: ", "%f", &ae_table_attr.exp_table[i].dgain);
		read_p("isp_gain: ", "%f", &ae_table_attr.exp_table[i].isp_gain);
	}
	ae_table_attr.valid_num = i == CAMDEV_AE_EXP_TABLE_NUM - 1 ? CAMDEV_AE_EXP_TABLE_NUM : i;

	FUNC_EQ(hbn_isp_set_exposure_table(ctx->vnode_fd[1], &ae_table_attr), 0, return);
}

static void tuning_hanle_get_ae_table(tuning_context_t *ctx)
{
	int32_t i;
	hbn_isp_exposure_table_t ae_table_attr = {0};

	FUNC_EQ(hbn_isp_get_exposure_table(ctx->vnode_fd[1], &ae_table_attr), 0, return);

	for (i = 0; i < ae_table_attr.valid_num; i++) {
		printf("---------table %d---------\n", i);
		printf("exposure_time: %f\n", ae_table_attr.exp_table[i].exposure_time);
		printf("again: %f\n", ae_table_attr.exp_table[i].again);
		printf("dgain: %f\n", ae_table_attr.exp_table[i].dgain);
		printf("isp_gain: %f\n", ae_table_attr.exp_table[i].isp_gain);
	}
}

static void tuning_dump_yuv(tuning_context_t *ctx)
{
	read_p("typing the number to dump: ", "%d", &ctx->yuv_dump_cnt);
}

static void tuning_get_ae_statistics(tuning_context_t *ctx)
{
	int32_t col, row;
	uint32_t *luma;
	hbn_isp_ae_statistics_t ae_statistics = {0};

	FUNC_EQ(hbn_isp_get_ae_statistics(ctx->vnode_fd[1], &ae_statistics), 0, return);

	printf("Ae statistics current frameid: %d, timestamps: %ld\n", ae_statistics.frame_id, ae_statistics.timestamps);
	printf("Datatype: %d\n", ae_statistics.datatype);

	for (col = 0; col < HBN_ISP_GRID_NUM * HBN_ISP_PIXEL_CHANNEL; col += HBN_ISP_PIXEL_CHANNEL) {
		for (row = 0; row < HBN_ISP_GRID_NUM; row++) {
			luma = &ae_statistics.expStat[HBN_ISP_GRID_NUM * HBN_ISP_PIXEL_CHANNEL * row + col];
			printf("(%d, %d, %d, %d) ", *luma, *(luma+1), *(luma+2), *(luma+3));
		}
		printf("\n");
	}
}

static void tuning_set_module_control(tuning_context_t *ctx)
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

	FUNC_EQ(hbn_isp_set_module_control(ctx->vnode_fd[1], &module_ctrl), 0, return);
}

static void tuning_get_module_control(tuning_context_t *ctx)
{
	hbn_isp_module_ctrl_t module_ctrl = {0};
	FUNC_EQ(hbn_isp_get_module_control(ctx->vnode_fd[1], &module_ctrl), 0, return);

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

tuning_cmd_func_t cmd_funcs[] = TUNING_CMD_FUNC_LIST;
static void tuning_api_func(int32_t cmd, tuning_context_t *ctx)
{
	int32_t i;

	for (i = 0; i < ARRAY_SIZE(cmd_funcs); i++) {
		if (cmd_funcs[i].cmd == cmd) {
			cmd_funcs[i].api_func(ctx);
			printf("Input cmd:");
			return;
		}
	}

	valid_cmd_print();
	printf("Input cmd:");
}

static void *tuning_api_worker_thread(void *arg)
{
	tuning_context_t *ctx = (tuning_context_t *)arg;

	main_while_func_run(tuning_api_func, ctx)

	return NULL;
}

static int32_t tuning_feeback_init(tuning_context_t *ctx)
{
	isp_ichn_attr_t ichn_attr = {0};
	char file_path[255] = {0};

	if (getcwd(file_path, sizeof(file_path)) != NULL) {
#ifdef TUNING_DEBUG
		pr_tuning("Current working directory: %s\n", file_path);
#endif
	} else {
		pr_tuning("getcwd fail\n");
		return -1;
	}

	FUNC_EQ(tuning_get_raw_list(file_path, ctx->img_path, ctx->img_name, &ctx->img_num), 0, return -1);
	if (ctx->img_num < 1) {
		pr_tuning("no raw file found in working directory\n");
		return -1;
	}
	FUNC_EQ(hbn_vnode_get_ichn_attr(ctx->vnode_fd[1], 0, &ichn_attr), 0, return -1);
	FUNC_EQ(tuning_alloc_feedback_buffer(&ctx->src_img.buffer, ichn_attr.height, ichn_attr.width, 0), 0, return -1);

	return 0;
}

static int32_t tuning_case_run(tuning_context_t *ctx)
{
	int32_t ret = 0;
	hbn_vflow_handle_t vflow_fd = 0;
	// isp_attr_t isp_attr;
	vin_ochn_attr_t vin_oattr;

	if (!ctx->vpm_json) {
		pr_tuning("vpm cfg json null!\n");
		return -1;
	}

	FUNC_EQ(hbn_vflow_create_cfg(ctx->vpm_json, &vflow_fd), 0, return -1);
	if (ctx->cam_json && ctx->cam_json[0] != ' ') {
#ifdef TUNING_DEBUG
		pr_tuning("camera run in this case.\n");
#endif
		FUNC_EQ(hbn_camera_init_cfg(ctx->cam_json), 0, goto destroy_vflow);
	}
	ctx->vflow_fd = vflow_fd;

	if (!BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK)) {
		ctx->vnode_fd[0] = hbn_vflow_get_vnode_handle(ctx->vflow_fd, HB_VIN, 0);
		if (ctx->vnode_fd[0] < 0) {
			pr_tuning("hbn_vflow_get_vnode_hanle vin failed!\n");
			ret = ctx->vnode_fd[0];
			goto destroy_cam;
		}

		ret = hbn_vnode_get_ochn_attr(ctx->vnode_fd[0], 0, &vin_oattr);
		if (ret < 0) {
			pr_tuning("hbn_vnode_get_ochn_attr vin failed!\n");
			goto destroy;
		}
		ctx->is_offline = vin_oattr.ddr_en ? 1 : 0;
		ctx->vin_format = vin_oattr.vin_basic_attr.format;
	}

	ctx->vnode_fd[1] = hbn_vflow_get_vnode_handle(ctx->vflow_fd, HB_ISP, 0);
	if (ctx->vnode_fd[1] < 0) {
		pr_tuning("hbn_vflow_get_vnode_hanle isp failed!\n");
		ret = ctx->vnode_fd[1];
		goto destroy_cam;
	}

	// ret = hbn_vnode_get_attr(ctx->vnode_fd[1], &isp_attr);
	// if (ret < 0) {
	// 	pr_tuning("isp hbn_vnode_get_attr failed!\n");
	// 	goto destroy;
	// }
	if (BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK))
		FUNC_EQ(tuning_feeback_init(ctx), 0, goto destroy_cam);

	if (HBPLAYER_EN) {
		ctx->hbplayer_event = hb_tool_start_transfer(0);
		hb_tool_event_setcb(ctx->hbplayer_event, NULL, NULL, NULL, NULL, NULL);
	}
	hbn_vflow_start(vflow_fd);

	if (BIT_ENABLE(ctx->work_mode, START_DUMP_MASK))
		ctx->yuv_dump_cnt = 20;

	FUNC_EQ(pthread_create(&ctx->main_thid, NULL, tuning_main_worker_thread, (void *)(ctx)), 0, goto destroy);
	FUNC_EQ(pthread_create(&ctx->api_thid, NULL, tuning_api_worker_thread, (void *)(ctx)), 0, goto destroy);

	pthread_join(ctx->api_thid, NULL);
#ifdef TUNING_DEBUG
	pr_tuning("api thread join done\n");
#endif

	pthread_cancel(ctx->main_thid);
#ifdef TUNING_DEBUG
	pr_tuning("main thread cancel done\n");
#endif

	if (HBPLAYER_EN) {
		hb_tool_stop_transfer(ctx->hbplayer_event);
	}

	hbn_vflow_stop(vflow_fd);

destroy:
	if (BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK))
		tuning_free_feedback_buffer(&ctx->src_img.buffer);

destroy_cam:
	if (ctx->cam_json && ctx->cam_json[0] != ' ')
		FUNC_EQ(hbn_camera_init_cfg(NULL), 0, return -1);

destroy_vflow:
	hbn_vflow_destroy(vflow_fd);

	pr_tuning("vflow destory done\n");

	if (ctx->err_cnt) {
		pr_tuning("some error: %d occurred\n", ctx->err_cnt);
		return -1;
	}

	return ret;
}

int32_t main(int argc, char *argv[])
{
	int32_t ret = 0;
	tuning_context_t ctx = {0};

	parse_opts(argc, argv, &ctx);

	if (access(DEF_DUMP_PATH, 0)) {
		ret = mkdir(DEF_DUMP_PATH, 0777);
		if (ret < 0) {
			pr_tuning("mkdir %s for dump failed !\n", DEF_DUMP_PATH);
			return -1;
		}
	}

	FUNC_EQ(tuning_specify_case(&ctx), 0, return -1);

	FUNC_EQ(tuning_case_run(&ctx), 0, return -1);

	return ret;
}
