/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include <pthread.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "tuning_tool.h"

static void print_usage(const char *prog)
{
	pr_tuning("Usage: %s ", prog);
	printf("-c        camera json path\n"
		"-v        vpm json path\n"
		"-r        send raw to hbplayer\n"
		"-s        dump stream flag\n"
		"-h        usage help\n");
}

static void print_support_list(void)
{
	pr_tuning("Support list:\n");
	printf("s -> dump frame sif raw\n"
		"y -> dump yuv\n"
		"e -> set ae attr\n"
		"E -> get ae attr\n"
		"b -> get ae statistics\n"
		"w -> set awb attr\n"
		"W -> get awb attr\n"
		"t -> set exp table\n"
		"T -> get exp table\n"
		"q -> quit\n"
		"h -> help\n");
}

void parse_opts(int argc, char *argv[], tuning_context_t *ctx)
{
	int32_t cmd_ret;

	while (1) {
		static const char short_options[] =
			"c:v:d:r:s:";
		static const struct option long_options[] = {
			{"cam_path", 1, 0, 'c'},
			{"vpm_path", 1, 0, 'v'},
			{"dump_mask", 1, 0, 'd'},
			{"send_raw", 1, 0, 'r'},
			{"dump_stream", 1, 0, 's'},
			{ NULL, 0, 0, 0 },
		};
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
		default:
			print_usage(argv[0]);
			break;
		}
	}
}

#if 0
static void tuning_get_char(char *des, char *def)
{
	char temp;
	int32_t len = 0;

	while ((temp = getchar()) != '\n')
	{
		snprintf(&des[len], TUNING_PRINT_SIZE_MAX, "%s", &temp);
		len += 1;
	}

	if (des[0] == 0)
		sprintf(&des[0], "%s", def);
}

static void tuning_param_dbg(tuning_context_t *ctx)
{
	if (ctx->cam_json && ctx->cam_json[0] != ' ')
		pr_tuning("camera json path: %s\n", ctx->cam_json);
	pr_tuning("vpm json path: %s\n", ctx->vpm_json);
}
#endif

static int32_t tuning_specify_case(tuning_context_t *ctx)
{
#if 0
	printf("camera config json path(/app/testcase/S08_VPS/testsuite/res/cfg/tuning_cfg/cam_x5_config.json):");
	tuning_get_char(ctx->cam_json, "/app/testcase/S08_VPS/testsuite/res/cfg/tuning_cfg/cam_x5_config.json");

	printf("vpm config json path(/app/testcase/S08_VPS/testsuite/res/cfg/tuning_cfg/vpm_x5_config.json):");
	tuning_get_char(ctx->vpm_json, "/app/testcase/S08_VPS/testsuite/res/cfg/tuning_cfg/vpm_x5_config.json");
#endif
	if (ctx->cam_json[0] == 0)
		sprintf(&ctx->cam_json[0], "%s", DEF_CAM_PATH);
	if (ctx->vpm_json[0] == 0)
		sprintf(&ctx->vpm_json[0], "%s", DEF_VPM_PATH);

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
	pr_tuning("%s run \n", __func__);

	while (1) {
		if (ctx->send_raw) {
			ret = hbn_vnode_getframe(ctx->vnode_fd[0], 0, 1000, &raw_img);
			if (ret) {
				pr_tuning("get buffer from sif fail\n");
				goto out;
			}

			if (HBPLAYER_EN) {
				// todo: get bitwidth from sif ichn
				ret = tuning_send_raw_to_hbplayer(ctx->hbplayer_event, &raw_img, RAW_16, 0);
				if (ret)
					pr_tuning("send to hbplayer failed, skip it\n");
			}
			hbn_vnode_releaseframe(ctx->vnode_fd[0], 0, &raw_img);
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

		// pr_tuning("get buffer size %ld-%ld\n", yuv_img.buffer.size[0], yuv_img.buffer.size[1]);
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

	// if (!ctx->is_offline) {
	// 	pr_tuning("cannot dump raw when sif otf isp\n");
	// 	return ;
	// }

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

	read_p("typing the expsoure mode, manual_mode(0)/auto_mode(1): ", "%d", &exp_mode);

	if (exp_mode == 0) {
		exp_attr.mode = HBN_ISP_MODE_MANUAL;
		read_p("expsoure time(units: s): ", "%f", &exp_attr.manual_attr.exp_time);
		read_p("again: ", "%f", &exp_attr.manual_attr.again);
		read_p("dgain: ", "%f", &exp_attr.manual_attr.dgain);
		read_p("ispgain: ", "%f", &exp_attr.manual_attr.ispgain);
	} else {
		pr_tuning("temp enable ae auto directly!\n");
		VIO_ASSERT_FUNC_EQ(hbn_isp_get_exposure_attr(ctx->vnode_fd[1], &exp_attr), 0, return);
		exp_attr.mode = HBN_ISP_MODE_AUTO;
		read_p("speed_over: ", "%f", &exp_attr.auto_attr.speed_over);
		read_p("speed_under: ", "%f", &exp_attr.auto_attr.speed_under);
		read_p("tolerance: ", "%f", &exp_attr.auto_attr.tolerance);
		read_p("target: ", "%f", &exp_attr.auto_attr.target);
		read_p("flicker_freq: ", "%f", &exp_attr.auto_attr.flicker_freq);
		read_p("anti_flicker_status: ", "%d", &exp_attr.auto_attr.anti_flicker_status);
	}

	VIO_ASSERT_FUNC_EQ(hbn_isp_set_exposure_attr(ctx->vnode_fd[1], &exp_attr), 0, return);
}

static void tuning_handle_get_expsoure(tuning_context_t *ctx)
{
	hbn_isp_exposure_attr_t exp_attr = {0};

	VIO_ASSERT_FUNC_EQ(hbn_isp_get_exposure_attr(ctx->vnode_fd[1], &exp_attr), 0, return);

	pr_tuning("exp_time: %f\n", exp_attr.manual_attr.exp_time);
	pr_tuning("again: %f\n", exp_attr.manual_attr.again);
	pr_tuning("dgain: %f\n", exp_attr.manual_attr.dgain);
	pr_tuning("ispgain: %f\n", exp_attr.manual_attr.ispgain);
	pr_tuning("ae_exp: %d\n",exp_attr.manual_attr.ae_exp);
	pr_tuning("mode %d\n", exp_attr.auto_attr.mode);

	pr_tuning("exp range %f %f\n", exp_attr.auto_attr.exp_time_range.min, exp_attr.auto_attr.exp_time_range.max);
	pr_tuning("again range %f %f\n", exp_attr.auto_attr.again_range.min, exp_attr.auto_attr.again_range.max);
	pr_tuning("dgain range %f %f\n", exp_attr.auto_attr.dgain_range.min, exp_attr.auto_attr.dgain_range.max);
	pr_tuning("isp dgain range %f %f\n", exp_attr.auto_attr.isp_dgain_range.min, exp_attr.auto_attr.isp_dgain_range.max);
	pr_tuning("speed_over %f\n", exp_attr.auto_attr.speed_over);
	pr_tuning("speed_under %f\n", exp_attr.auto_attr.speed_under);
	pr_tuning("tolerance %f\n", exp_attr.auto_attr.tolerance);
	pr_tuning("target %f\n", exp_attr.auto_attr.target);
	pr_tuning("flicker_freq %f\n", exp_attr.auto_attr.flicker_freq);
	pr_tuning("anti_flicker_status %d\n", exp_attr.auto_attr.anti_flicker_status);
}

static void tuning_handle_set_white_balance(tuning_context_t *ctx)
{
	hbn_isp_awb_attr_t awb_attr = {0};
	uint32_t awb_mode;

	read_p("typing the awb mode, manual_mode(0)/auto_mode(1): ", "%d", &awb_mode);

	if (awb_mode == 0) {
		awb_attr.mode = HBN_ISP_MODE_MANUAL;

		read_p("rgain: ", "%f", &awb_attr.manual_attr.gain.rgain);
		read_p("grgain: ", "%f", &awb_attr.manual_attr.gain.grgain);
		read_p("gbgain: ", "%f", &awb_attr.manual_attr.gain.gbgain);
		read_p("bgain: ", "%f", &awb_attr.manual_attr.gain.bgain);
	} else {
		pr_tuning("temp enable awb auto directly!\n");
		VIO_ASSERT_FUNC_EQ(hbn_isp_get_awb_attr(ctx->vnode_fd[1], &awb_attr), 0, return);
		awb_attr.mode = HBN_ISP_MODE_AUTO;
	}

	VIO_ASSERT_FUNC_EQ(hbn_isp_set_awb_attr(ctx->vnode_fd[1], &awb_attr), 0, return);
}

static void tuning_handle_get_white_balance(tuning_context_t *ctx)
{
	hbn_isp_awb_attr_t awb_attr = {0};

	VIO_ASSERT_FUNC_EQ(hbn_isp_get_awb_attr(ctx->vnode_fd[1], &awb_attr), 0, return);

	pr_tuning("rgain %f\n", awb_attr.auto_attr.gain.rgain);
	pr_tuning("grgain %f\n", awb_attr.auto_attr.gain.grgain);
	pr_tuning("gbgain %f\n", awb_attr.auto_attr.gain.gbgain);
	pr_tuning("bgain %f\n", awb_attr.auto_attr.gain.bgain);
}

static void tuning_hanle_set_ae_table(tuning_context_t *ctx)
{
	int32_t i;
	hbn_isp_exposure_table_t ae_table_attr = {0};

	for (i = 0; i < CAMDEV_AE_EXP_TABLE_NUM; i++) {
		pr_tuning("input table %d, type [0] to finish:\n", i);
		read_p("exposure_time: ", "%f", &ae_table_attr.exp_table[i].exposure_time);
		if (ae_table_attr.exp_table[i].exposure_time == 0) {
			ae_table_attr.exp_table[i].exposure_time = 0;
			break;
		}
		read_p("again: ", "%f", &ae_table_attr.exp_table[i].again);
		read_p("dgain: ", "%f", &ae_table_attr.exp_table[i].dgain);
		read_p("isp_gain: ", "%f", &ae_table_attr.exp_table[i].isp_gain);
	}
	ae_table_attr.valid_num = i == CAMDEV_AE_EXP_TABLE_NUM - 1 ? CAMDEV_AE_EXP_TABLE_NUM : i;

	VIO_ASSERT_FUNC_EQ(hbn_isp_set_exposure_table(ctx->vnode_fd[1], &ae_table_attr), 0, return);
}

static void tuning_hanle_get_ae_table(tuning_context_t *ctx)
{
	int32_t i;
	hbn_isp_exposure_table_t ae_table_attr = {0};

	VIO_ASSERT_FUNC_EQ(hbn_isp_get_exposure_table(ctx->vnode_fd[1], &ae_table_attr), 0, return);

	for (i = 0; i < ae_table_attr.valid_num; i++) {
		pr_tuning("---------table %d---------\n", i);
		pr_tuning("exposure_time: %f\n", ae_table_attr.exp_table[i].exposure_time);
		pr_tuning("again: %f\n", ae_table_attr.exp_table[i].again);
		pr_tuning("dgain: %f\n", ae_table_attr.exp_table[i].dgain);
		pr_tuning("isp_gain: %f\n", ae_table_attr.exp_table[i].isp_gain);
	}
}

static void tuning_dump_yuv(tuning_context_t *ctx)
{
	read_p("typing the number to dump: ", "%d", &ctx->yuv_dump_cnt);
}

static void tuning_get_ae_statistics(tuning_context_t *ctx)
{
	// int32_t chn, col, row;
	// hbn_isp_ae_statistics_t ae_statistics = {0};

	// VIO_ASSERT_FUNC_EQ(hbn_isp_get_ae_statistics(ctx->vnode_fd[1], &ae_statistics), 0, return);

	// pr_tuning("Ae statistics current frameid: %d, timestamps: %ld\n", ae_statistics.frame_id, ae_statistics.timestamps);
	// pr_tuning("Datatype: %d\n", ae_statistics.datatype);

	// for (chn = 0; chn < HBN_ISP_PIXEL_CHANNEL; chn++) {
	// 	printf("channel index: %d\n", chn);
	// 	for (row = 0; row < HBN_ISP_GRID_NUM; row++) {
	// 		printf("%d: ", row);
	// 		for (col = 0; col < HBN_ISP_GRID_NUM; col++) {
	// 			printf(" %d", ae_statistics.expStat[HBN_ISP_GRID_ITEMS*chn + HBN_ISP_GRID_NUM*row + col]);
	// 		}
	// 		printf("\n");
	// 	}
	// }
}

static int32_t tuning_api_func(int32_t cmd, tuning_context_t *ctx)
{
	int32_t ret = 0;

	switch (cmd)
	{
	case 'a':
		pr_tuning("xxxx\n");
		break;
	case 's':
		tuning_dump_sif_raw(ctx);
		break;
	case 'e':
		tuning_handle_set_expsoure(ctx);
		break;
	case 'E':
		tuning_handle_get_expsoure(ctx);
		break;
	case 'w':
		tuning_handle_set_white_balance(ctx);
		break;
	case 'W':
		tuning_handle_get_white_balance(ctx);
		break;
	case 't':
		tuning_hanle_set_ae_table(ctx);
		break;
	case 'T':
		tuning_hanle_get_ae_table(ctx);
		break;
	case 'y':
		tuning_dump_yuv(ctx);
		return ret;
	case 'b':
		tuning_get_ae_statistics(ctx);
		break;
	case 'h':
		print_support_list();
		break;
	default:
		pr_tuning("Unknown cmd: %d\n", cmd);
		print_support_list();
		break;
	}
	pr_tuning("Input cmd:");

	return ret;
}

static void *tuning_api_worker_thread(void *arg)
{
	tuning_context_t *ctx = (tuning_context_t *)arg;

	main_while_func_run(tuning_api_func, ctx)

	return NULL;
}

static int32_t tuning_case_run(tuning_context_t *ctx)
{
	int32_t ret = 0;
	hbn_vflow_handle_t vflow_fd = 0;
	isp_attr_t isp_attr;

	if (!ctx->vpm_json) {
		pr_tuning("vpm cfg json null!\n");
		return -1;
	}

	VIO_ASSERT_FUNC_EQ(hbn_vflow_create_cfg(ctx->vpm_json, &vflow_fd), 0, return -1);
	if (ctx->cam_json && ctx->cam_json[0] != ' ') {
		pr_tuning("camera run in this case.\n");
		VIO_ASSERT_FUNC_EQ(hbn_camera_init_cfg(ctx->cam_json), 0, goto destroy);
	}
	ctx->vflow_fd = vflow_fd;

	ctx->vnode_fd[0] = hbn_vflow_get_vnode_handle(ctx->vflow_fd, HB_VIN, 0);
	if (ctx->vnode_fd[0] < 0) {
		pr_tuning("isp hbn_vflow_get_vnode_hanle failed!\n");
		ret = -1;
		goto destroy;
	}

	ctx->vnode_fd[1] = hbn_vflow_get_vnode_handle(ctx->vflow_fd, HB_ISP, 0);
	if (ctx->vnode_fd[1] < 0) {
		pr_tuning("isp hbn_vflow_get_vnode_hanle failed!\n");
		ret = -1;
		goto destroy;
	}

	ret = hbn_vnode_get_attr(ctx->vnode_fd[1], &isp_attr);
	if (ret < 0) {
		pr_tuning("isp hbn_vnode_get_attr failed!\n");
		goto destroy;
	}
	ctx->is_offline = isp_attr.input_mode == 2 ? 1 : 0;

	if (HBPLAYER_EN) {
		ctx->hbplayer_event = hb_tool_start_transfer(0);
		hb_tool_event_setcb(ctx->hbplayer_event, NULL, NULL, NULL, NULL, NULL);
	}
	hbn_vflow_start(vflow_fd);

	VIO_ASSERT_FUNC_EQ(pthread_create(&ctx->main_thid, NULL, tuning_main_worker_thread, (void *)(ctx)), 0, goto destroy);
	VIO_ASSERT_FUNC_EQ(pthread_create(&ctx->api_thid, NULL, tuning_api_worker_thread, (void *)(ctx)), 0, goto destroy);

	pthread_join(ctx->api_thid, NULL);
	pr_tuning("api thread join done\n");

	pthread_cancel(ctx->main_thid);
	pr_tuning("main thread cancel done\n");

	if (HBPLAYER_EN) {
		hb_tool_stop_transfer(ctx->hbplayer_event);
	}

	hbn_vflow_stop(vflow_fd);

destroy:
	if (ctx->cam_json && ctx->cam_json[0] != ' ')
		VIO_ASSERT_FUNC_EQ(hbn_camera_init_cfg(NULL), 0, return -1);

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

	tuning_specify_case(&ctx);

	VIO_ASSERT_FUNC_EQ(tuning_case_run(&ctx), 0, return -1);

	return ret;
}
