/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024-2025, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include <ctype.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imu_manager.h"

#define INPUT_BUFFER_SIZE 64

static void print_help(const char *default_imu_name)
{
	printf("Usage: sample_imu [OPTIONS]\n");
	printf("Options:\n");
	printf("  -n <imu_name>         Specify IMU name");
	if (default_imu_name) {
		printf(" (default: %s)", default_imu_name);
	}
	printf("\n");
	printf("  -h                    Show this help message\n");
	list_available_sensors();
}

static void print_command_help(void)
{
	printf("\n");
	printf("***************  Command Lists  ***************\n");
	printf(" g    -- Get a single frame of imu data\n");
	printf(" l    -- Get multiple frames of imu data\n");
	printf(" q    -- Quit the program\n");
	printf(" h    -- Print this help message\n");
}

static int read_input_line(char *buffer, size_t buffer_size)
{
	if (!fgets(buffer, (int)buffer_size, stdin)) {
		return -1;
	}

	buffer[strcspn(buffer, "\r\n")] = '\0';
	return 0;
}

static char read_command(void)
{
	char input[INPUT_BUFFER_SIZE];

	if (read_input_line(input, sizeof(input)) != 0) {
		return 'q';
	}

	for (size_t i = 0; input[i] != '\0'; ++i) {
		if (!isspace((unsigned char)input[i])) {
			return (char)tolower((unsigned char)input[i]);
		}
	}

	return '\0';
}

static int read_frame_count(int *num_frames)
{
	char input[INPUT_BUFFER_SIZE];
	char *end_ptr = NULL;
	long value = 0;

	if (!num_frames) {
		return -1;
	}

	if (read_input_line(input, sizeof(input)) != 0) {
		return -1;
	}

	value = strtol(input, &end_ptr, 10);
	if (end_ptr == input || *end_ptr != '\0' || value <= 0) {
		return -1;
	}

	*num_frames = (int)value;
	return 0;
}

static void read_and_print_frames(SensorHandle handle, int num_frames)
{
	for (int i = 0; i < num_frames; ++i) {
		ImuData data;

		if (read_sensor_data(handle, &data) == 0) {
			printf("Data received (Frame %d):\n", i + 1);
			print_imu_data(&data);
			continue;
		}

		printf("Error: Failed to read data from IMU (Frame %d)\n", i + 1);
		break;
	}
}

int main(int argc, char **argv)
{
	int opt_index = 0;
	int option = 0;
	const char *imu_name = NULL;
	const char *default_imu_name = NULL;
	SensorHandle handle = NULL;
	char command = '\0';

	static struct option long_options[] = {
		{"imu_name", required_argument, 0, 'n'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};

	default_imu_name = get_default_imu_name();

	while ((option = getopt_long(argc, argv, "n:h", long_options, &opt_index)) != -1) {
		switch (option) {
		case 'n':
			imu_name = optarg;
			break;
		case 'h':
		default:
			print_help(default_imu_name);
			return 0;
		}
	}

	if (optind < argc) {
		printf("Unknown arguments found. Please check your input.\n");
		print_help(default_imu_name);
		return 0;
	}

	if (!imu_name) {
		imu_name = default_imu_name;
		if (!imu_name) {
			printf("Error: no supported IMU device was detected.\n");
			list_available_sensors();
			return 1;
		}
		printf("No IMU specified, using detected default: %s\n", imu_name);
	}

	printf("Using IMU: %s\n", imu_name);

	handle = init_sensor(imu_name, "");
	if (!handle) {
		printf("Error: init IMU '%s' failed !!! Quit Now\n", imu_name);
		return 1;
	}

	do {
		print_command_help();
		printf("Enter command: ");
		command = read_command();

		switch (command) {
		case 'g':
			read_and_print_frames(handle, 1);
			break;
		case 'l': {
			int num_frames = 0;

			printf("Enter number of frames to read: ");
			if (read_frame_count(&num_frames) != 0) {
				printf("Invalid frame count. Please enter a positive integer.\n");
				break;
			}

			read_and_print_frames(handle, num_frames);
			break;
		}
		case 'h':
			print_help(default_imu_name);
			break;
		case 'q':
			break;
		case '\0':
			printf("Empty command. Please try again.\n");
			break;
		default:
			printf("Unknown command. Please try again.\n");
			break;
		}
	} while (command != 'q');

	release_sensor(handle);
	printf("IMU resources released\n");
	return 0;
}