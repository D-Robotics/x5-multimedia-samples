/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#ifndef __TUNING_CMD_H__
#define __TUNING_CMD_H__

/* app cmd */
#define PARSE_SHORT_OPTS "c:v:d:r:s:f:w:"
#define PARSE_LONG_OPTS {\
		{"cam_path", 1, 0, 'c'},\
		{"vpm_path", 1, 0, 'v'},\
		{"dump_mask", 1, 0, 'd'},\
		{"send_raw", 1, 0, 'r'},\
		{"dump_stream", 1, 0, 's'},\
		{"work_mode", 1, 0, 'w'},\
		{ NULL, 0, 0, 0 },\
	}
#define PARSE_SHOW_OPTS "-c        camera json path\n"\
			"-v        vpm json path\n"\
			"-r        send raw to hbplayer\n"\
			"-s        dump stream flag\n"\
			"-w        work mode mask\n"\
			"-h        usage help\n"
#define parse_opts_print(prog) do {\
		pr_tuning("Usage: %s\n", prog);\
		printf(PARSE_SHOW_OPTS);\
	} while(0)

/* api cmd */
#define VALID_CMD_USAGE "s -> dump frame sif raw\n"\
			"y -> dump yuv\n"\
			"e -> set ae attr\n"\
			"E -> get ae attr\n"\
			"b -> get ae statistics\n"\
			"w -> set awb attr\n"\
			"W -> get awb attr\n"\
			"t -> set exp table\n"\
			"T -> get exp table\n"\
			"m -> set module control\n"\
			"M -> get module control\n"\
			"q -> quit\n"\
			"h -> help\n"
#define valid_cmd_print() do {\
		pr_tuning("Support list:\n");\
		printf(VALID_CMD_USAGE);\
	} while(0)
#define TUNING_CMD_FUNC_LIST {\
	{'s',	tuning_dump_sif_raw},\
	{'e',	tuning_handle_set_expsoure},\
	{'E',	tuning_handle_get_expsoure},\
	{'w',	tuning_handle_set_white_balance},\
	{'W',	tuning_handle_get_white_balance},\
	{'t',	tuning_hanle_set_ae_table},\
	{'T',	tuning_hanle_get_ae_table},\
	{'y',	tuning_dump_yuv},\
	{'b',	tuning_get_ae_statistics},\
	{'m',	tuning_set_module_control},\
	{'M',	tuning_get_module_control},\
}

#endif // __TUNING_CMD_H__