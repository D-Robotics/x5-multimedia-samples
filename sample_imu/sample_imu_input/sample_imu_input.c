#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>
#include <string.h>

// ========== Driver unified report format ==========
#define DEFAULT_INPUT_DEV_PATH "/dev/input/event1"
#define BMI088_MSC_DATA 0x04                // MSC code used by the driver (must match kernel)
#define MSC_DATA_COUNT 9                    // MSC fields per frame: 6 axes + timestamp (2 words) + IRQ count
#define ACC_X_IDX 0                         // accelerometer X index
#define ACC_Y_IDX 1                         // accelerometer Y index
#define ACC_Z_IDX 2                         // accelerometer Z index
#define GYRO_X_IDX 3                        // gyroscope X index
#define GYRO_Y_IDX 4                        // gyroscope Y index
#define GYRO_Z_IDX 5                        // gyroscope Z index
#define TS_HIGH_IDX 6                       // timestamp high 32 bits index
#define TS_LOW_IDX 7                        // timestamp low 32 bits index
#define IRQ_COUNT 8                         // IRQ count index


int main(int argc, char **argv) {
    int fd;
    struct input_event ev;
    const char *dev_path = (argc > 1) ? argv[1] : DEFAULT_INPUT_DEV_PATH;
    // Cache MSC values reported by the driver (in driver-defined order)
    int32_t msc_data[MSC_DATA_COUNT] = {0};
    int msc_index = 0;  // Index of the next MSC field in the current frame

    // Parsed frame data
    int16_t ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
    uint64_t raw_timestamp = 0;  // Assembled 64-bit hardware timestamp (ns)
    int irq_count = 0;

    // Open the input device
    fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        perror(dev_path);
        return 1;
    }

    printf("========================================\n");
    printf("BMI088 input reader (unified MSC code)\n");
    printf("Device path: %s\n", dev_path);
    printf("Press Ctrl+C to exit\n");
    printf("========================================\n\n");

    // Packet-loss detection state
    int64_t diff, min_diff = INT64_MAX, max_diff = INT64_MIN;
    uint64_t timestamp_ns_last = 0, lost_count = 0;
    int event_count = 0;

    // Read input events in a loop
    while (1) {
        ssize_t ret = read(fd, &ev, sizeof(ev));
        if (ret != sizeof(ev)) {
            perror("read failed or interrupted");
            continue;
        }
        ++event_count;

        switch (ev.type) {
            // Parse only EV_MSC events with BMI088_MSC_DATA
            case EV_MSC:
                if (ev.code == BMI088_MSC_DATA) {
                    if (msc_index < MSC_DATA_COUNT) {
                        msc_data[msc_index++] = ev.value;  // Store fields in order
                    } else {
                        // Overflow: too many MSC fields in this frame, reset
                        printf("[WARN] MSC data overflow, reset index\n");
                        msc_index = 0;
                    }
                }
                break;

            case EV_SYN:
                if (ev.code == SYN_REPORT) {
                    // End of frame: parse the cached MSC fields
                    if (msc_index == MSC_DATA_COUNT) {
                        // 1. Parse 6-axis raw data (int32 ev.value -> int16 sensor LSB)
                        ax = (int16_t)msc_data[ACC_X_IDX];
                        ay = (int16_t)msc_data[ACC_Y_IDX];
                        az = (int16_t)msc_data[ACC_Z_IDX];
                        gx = (int16_t)msc_data[GYRO_X_IDX];
                        gy = (int16_t)msc_data[GYRO_Y_IDX];
                        gz = (int16_t)msc_data[GYRO_Z_IDX];

                        // 2. Assemble 64-bit timestamp: (high << 32) | low
                        raw_timestamp = ((uint64_t)msc_data[TS_HIGH_IDX] << 32) | (uint32_t)msc_data[TS_LOW_IDX];

                        irq_count = (int16_t)msc_data[IRQ_COUNT];

                        // 3. Print one assembled frame (raw LSB + hardware timestamp)
                        printf("ACC(%d,%d,%d) | GYRO(%d,%d,%d) | TS:%" PRIu64 " ns | Lost: %" PRIu64 " | EventCount: %d | up_count: %d | lower_count: - \n",
                               ax, ay, az, gx, gy, gz, raw_timestamp, lost_count, event_count, irq_count);

                        // 4. Detect packet loss from timestamp gaps
                        if (timestamp_ns_last != 0 && raw_timestamp != 0) {
                            diff = (int64_t)(raw_timestamp - timestamp_ns_last);
                            if (diff > 3500000) {  // Threshold: 3.5 ms
                                lost_count++;
                                printf("[ERROR] IMU data lost: last ts %.6fs, current ts %.6fs, diff %.6fs\n",
                                       timestamp_ns_last * 1e-9, raw_timestamp * 1e-9, diff * 1e-9);
                            }
                            if (diff < min_diff && diff > 0) min_diff = diff;
                            if (diff > max_diff) max_diff = diff;
                        }
                        timestamp_ns_last = raw_timestamp;
                    } else {
                        // Incomplete frame: drop and wait for next SYN_REPORT
                        printf("[WARN] MSC data incomplete (received %d/%d), skip\n", msc_index, MSC_DATA_COUNT);
                    }

                    // Reset cache for the next frame
                    msc_index = 0;
                    memset(msc_data, 0, sizeof(msc_data));
                    event_count = 0;
                }
                break;

            // EV_ABS is not used by this driver
            default:
                // Ignore other event types
                break;
        }
    }

    close(fd);
    // Summary statistics (reached on normal close)
    printf("\n========================================\n");
    printf("Min interval: %" PRId64 " ns | Max interval: %" PRId64 " ns | Total lost: %" PRIu64 "\n",
           min_diff, max_diff, lost_count);
    printf("========================================\n");
    return 0;
}
