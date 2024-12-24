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
#include "tuning_cmd.h"
#include "tuning_tool.h"

tuning_context_t *global_ctx;

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
		case 'f':
			ctx->feedback_times = atoi(optarg);
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
		ctx->feedback_times = ctx->feedback_times == 0 ? 0xFFFF : ctx->feedback_times;
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
	pr_tuning("feedback file: %s, index %d, size %ld\n",
		ctx->img_path[ctx->cur_img], ctx->cur_img, statbuf.st_size);
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

	if ((ctx->feedback_times <= 1) &&
		((ctx->cur_img + 1) >= ctx->img_num)) {
		pr_tuning("feedback raw list done!\n");
		return -1;
	}

	if ((ctx->cur_img + 1) >= ctx->img_num) {
		ctx->cur_img = 0;
		ctx->feedback_times -= 1;
	} else {
		ctx->cur_img += 1;
	}

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
	printf("Input cmd:");

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
			ret = tuning_feeback_prepare_next(ctx);
			if (ret) goto out;
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
		// pr_tuning("get buffer size %ld-%ld\n", yuv_img.buffer.size[0], yuv_img.buffer.size[1]);
#endif
		hbn_vnode_releaseframe(ctx->vnode_fd[1], 0, &yuv_img);
		if (BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK)) {
			usleep(20*1000);
		}
	}
out:
	pr_tuning("typing [q] to quit\n");
	sleep(100000);

	return NULL;
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

	if (cmd != 'h')
		printf("Unknown cmd: %c!\n", (char)cmd);
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
#ifdef TUNING_DEBUG
	pr_tuning("stop transfer done\n");
#endif

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

	global_ctx = &ctx;
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
