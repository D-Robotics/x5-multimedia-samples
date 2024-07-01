/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#ifndef __TUNING_CMD_H__
#define __TUNING_CMD_H__

#define PARSE_SHORT_OPTS "c:v:d:r:s:f:w:"
#define PARSE_LONG_OPTS {\
		{"cam_path", 1, 0, 'c'},\
		{"vpm_path", 1, 0, 'v'},\
		{"dump_mask", 1, 0, 'd'},\
		{"send_raw", 1, 0, 'r'},\
		{"dump_stream", 1, 0, 's'},\
		{"feedback_path", 1, 0, 'f'},\
		{"work_mode", 1, 0, 'w'},\
		{ NULL, 0, 0, 0 },\
	}
#define PARSE_SHOW_OPTS "-c        camera json path\n"\
			"-v        vpm json path\n"\
			"-r        send raw to hbplayer\n"\
			"-s        dump stream flag\n"\
			"-f        feedback file path\n"\
			"-w        work mode mask\n"\
			"-h        usage help\n"
#define parse_opts_print(prog) do {\
		pr_tuning("Usage: %s\n", prog);\
		printf(PARSE_SHOW_OPTS);\
	} while(0)


#define VALID_CMD_USAGE "s -> dump frame sif raw\n"\
			"y -> dump yuv\n"\
			"e -> set ae attr\n"\
			"E -> get ae attr\n"\
			"b -> get ae statistics\n"\
			"w -> set awb attr\n"\
			"W -> get awb attr\n"\
			"t -> set exp table\n"\
			"T -> get exp table\n"\
			"q -> quit\n"\
			"h -> help\n"
#define valid_cmd_print() do {\
		pr_tuning("Support list:\n");\
		printf(VALID_CMD_USAGE);\
	} while(0)


#endif // __TUNING_CMD_H__