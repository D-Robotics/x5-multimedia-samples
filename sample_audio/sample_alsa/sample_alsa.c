/***************************************************************************
 * COPYRIGHT NOTICE
 * Copyright 2024 D-Robotics, Inc.
 * All rights reserved.
 ***************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <linux/soundcard.h>
#include <alsa/asoundlib.h>
#include <stdint.h>
#include <time.h>

// Audio device basic configuration
typedef struct {
    char capture_device[64];
    char playback_device[64];
    unsigned int rate;
    unsigned int bits;
    unsigned int channels;
    unsigned int duration;
    char record_file[64];
} AudioConfig;

// Default configuration
AudioConfig config = {
    // .capture_device = "default", // default capture device
    // .playback_device = "default", // default playback device
    .capture_device = "hw:0,0",
    .playback_device = "hw:0,0",
    .rate = 48000,
    .bits = 16,
    .channels = 2,
    .duration = 5,
    .record_file = "record_test.wav"
};

// Enumerated audio formats for platform support checking
snd_pcm_format_t formats[] = {
    SND_PCM_FORMAT_S8,
    SND_PCM_FORMAT_U8,
    SND_PCM_FORMAT_S16_LE,
    SND_PCM_FORMAT_S16_BE,
    SND_PCM_FORMAT_U16_LE,
    SND_PCM_FORMAT_U16_BE,
    SND_PCM_FORMAT_S24_LE,
    SND_PCM_FORMAT_S24_BE,
    SND_PCM_FORMAT_U24_LE,
    SND_PCM_FORMAT_U24_BE,
    SND_PCM_FORMAT_S32_LE,
    SND_PCM_FORMAT_S32_BE,
    SND_PCM_FORMAT_U32_LE,
    SND_PCM_FORMAT_U32_BE,
    SND_PCM_FORMAT_IEC958_SUBFRAME_LE,
    SND_PCM_FORMAT_IEC958_SUBFRAME_BE,
    SND_PCM_FORMAT_MU_LAW,
    SND_PCM_FORMAT_A_LAW,
    SND_PCM_FORMAT_IMA_ADPCM,
    SND_PCM_FORMAT_MPEG,
    SND_PCM_FORMAT_GSM,
    SND_PCM_FORMAT_UNKNOWN  // sentinel
};
/*
SND_PCM_FORMAT_S8,
SND_PCM_FORMAT_U8,
SND_PCM_FORMAT_S16_LE,
SND_PCM_FORMAT_S16_BE,
SND_PCM_FORMAT_U16_LE,
SND_PCM_FORMAT_U16_BE,
SND_PCM_FORMAT_S24_LE,
SND_PCM_FORMAT_S24_BE,
SND_PCM_FORMAT_U24_LE,
SND_PCM_FORMAT_U24_BE,
SND_PCM_FORMAT_S32_LE,
SND_PCM_FORMAT_S32_BE,
SND_PCM_FORMAT_U32_LE,
SND_PCM_FORMAT_U32_BE,
SND_PCM_FORMAT_F32_LE,
SND_PCM_FORMAT_F32_BE,
SND_PCM_FORMAT_F64_LE,
SND_PCM_FORMAT_F64_BE,
SND_PCM_FORMAT_IEC958_SUBFRAME_LE,
SND_PCM_FORMAT_IEC958_SUBFRAME_BE,
SND_PCM_FORMAT_MU_LAW,
SND_PCM_FORMAT_A_LAW,
SND_PCM_FORMAT_IMA_ADPCM,
SND_PCM_FORMAT_MPEG,
SND_PCM_FORMAT_GSM,
*/


enum {
    OPT_CAPTURE = 1,
    OPT_PLAYBACK,
};

/* Accept hw:card,device only; shorthand card,device also allowed */
static int set_hw_device(char *dest, size_t dest_sz, const char *arg,
                         const char *label)
{
    unsigned int card, dev;
    const char *p = arg;

    if (!arg || !*arg) {
        fprintf(stderr, "%s: empty device string\n", label);
        return -1;
    }

    if (strncmp(arg, "plughw:", 7) == 0 || strncmp(arg, "plug:", 5) == 0) {
        fprintf(stderr,
                "%s: this sample uses hw only; use card,device or hw:card,device\n",
                label);
        fprintf(stderr, "      (plughw adds a plugin layer; not supported here)\n");
        return -1;
    }

    if (strncmp(p, "hw:", 3) == 0)
        p += 3;

    if (sscanf(p, "%u,%u", &card, &dev) != 2) {
        fprintf(stderr,
                "%s: invalid '%s', use hw:card,device (e.g. hw:1,1)\n",
                label, arg);
        return -1;
    }

    snprintf(dest, dest_sz, "hw:%u,%u", card, dev);
    return 0;
}

// Print command-line help
static void print_help() {
    printf("Usage: sample_alsa [OPTIONS]\n");
    printf("Options:\n");
    printf("  --capture <hw:card,device>     Capture PCM device (default hw:0,0)\n");
    printf("  --playback <hw:card,device>    Playback PCM device (default hw:0,0)\n");
    printf("  -r <Sampling rate>            Specify sample rate for record or playback\n");
    printf("  -b <Bit depth>                Specify bit depth for record or playback\n");
    printf("  -c <Number of channels>       Specify channels for record or playback\n");
    printf("  -d <Duration>                 Specify duration for record or playback\n");
    printf("  -f <File name>                Specify file for record or playback\n");
    printf("  -h                            Show this help message\n");
    printf("\n");
    printf("Device: hw:card,device only (direct hw access, not plughw).\n");
    printf("Example: sample_alsa --capture hw:1,1 --playback hw:1,0 -r 16000 -c 8\n");
}

// Print interactive command list
static void command_help() {
    printf("\n***************  Command Lists  ***************\n");
    printf(" q  -- Quit\n");
    printf(" r  -- Start recording\n");
    printf(" p  -- Playback\n");
    printf(" c  -- Check hardware support\n");
    printf(" h  -- Print help message\n");
}


// Write standard 44-byte PCM WAV header
static void write_wav_header(FILE *file, unsigned int rate, unsigned int channels,
                             unsigned int bits, unsigned int data_size)
{
    uint8_t hdr[44];
    uint32_t byte_rate = rate * channels * (bits / 8);
    uint16_t block_align = channels * (bits / 8);

    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "RIFF", 4);
    hdr[4] = (uint8_t)((36 + data_size) & 0xff);
    hdr[5] = (uint8_t)((36 + data_size) >> 8);
    hdr[6] = (uint8_t)((36 + data_size) >> 16);
    hdr[7] = (uint8_t)((36 + data_size) >> 24);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    hdr[16] = 16;
    hdr[20] = 1;
    hdr[22] = (uint8_t)(channels & 0xff);
    hdr[23] = (uint8_t)((channels >> 8) & 0xff);
    hdr[24] = (uint8_t)(rate & 0xff);
    hdr[25] = (uint8_t)((rate >> 8) & 0xff);
    hdr[26] = (uint8_t)((rate >> 16) & 0xff);
    hdr[27] = (uint8_t)((rate >> 24) & 0xff);
    hdr[28] = (uint8_t)(byte_rate & 0xff);
    hdr[29] = (uint8_t)((byte_rate >> 8) & 0xff);
    hdr[30] = (uint8_t)((byte_rate >> 16) & 0xff);
    hdr[31] = (uint8_t)((byte_rate >> 24) & 0xff);
    hdr[32] = (uint8_t)(block_align & 0xff);
    hdr[33] = (uint8_t)((block_align >> 8) & 0xff);
    hdr[34] = (uint8_t)(bits & 0xff);
    hdr[35] = (uint8_t)((bits >> 8) & 0xff);
    memcpy(hdr + 36, "data", 4);
    hdr[40] = (uint8_t)(data_size & 0xff);
    hdr[41] = (uint8_t)((data_size >> 8) & 0xff);
    hdr[42] = (uint8_t)((data_size >> 16) & 0xff);
    hdr[43] = (uint8_t)((data_size >> 24) & 0xff);
    fwrite(hdr, 1, 44, file);
}

/* Read WAV PCM params; leaves file pointer at start of data chunk */
static int read_wav_pcm_info(FILE *file, unsigned int *rate, unsigned short *channels,
                             unsigned short *bits, unsigned int *data_size)
{
    uint8_t hdr[44];

    if (fread(hdr, 1, 44, file) != 44)
        return -1;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        return -1;
    if (memcmp(hdr + 12, "fmt ", 4) != 0)
        return -1;
    if ((hdr[20] | (hdr[21] << 8)) != 1)
        return -1;
    if (memcmp(hdr + 36, "data", 4) != 0)
        return -1;

    *channels = hdr[22] | (hdr[23] << 8);
    *rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    *bits = hdr[34] | (hdr[35] << 8);
    *data_size = hdr[40] | (hdr[41] << 8) | (hdr[42] << 16) | (hdr[43] << 24);
    return 0;
}

/* Parse card number from hw:card,device */
static int parse_hw_card(const char *hw_device, unsigned int *card)
{
    unsigned int dev;
    const char *p = hw_device;

    if (strncmp(p, "hw:", 3) == 0)
        p += 3;
    if (sscanf(p, "%u,%u", card, &dev) != 2)
        return -1;
    return 0;
}

/* Try setting a mixer control on the given card; returns -1 on failure, does not abort */
static int try_set_mixer_control(const char *hw_device, const char *control_name,
                                 long value, int for_capture)
{
    snd_mixer_t *handle;
    snd_mixer_elem_t *elem;
    snd_mixer_selem_id_t *sid;
    unsigned int card;
    char mixer_name[16];
    int ret;

    if (parse_hw_card(hw_device, &card) < 0)
        return -1;

    if (snd_mixer_open(&handle, 0) < 0)
        return -1;

    snprintf(mixer_name, sizeof(mixer_name), "hw:%u", card);
    if (snd_mixer_attach(handle, mixer_name) < 0) {
        snd_mixer_close(handle);
        return -1;
    }

    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_name(sid, control_name);
    elem = snd_mixer_find_selem(handle, sid);
    if (!elem) {
        snd_mixer_close(handle);
        return -1;
    }

    if (for_capture && snd_mixer_selem_has_capture_volume(elem))
        ret = snd_mixer_selem_set_capture_volume_all(elem, value);
    else if (!for_capture && snd_mixer_selem_has_playback_volume(elem))
        ret = snd_mixer_selem_set_playback_volume_all(elem, value);
    else
        ret = -1;

    snd_mixer_close(handle);
    return ret < 0 ? -1 : 0;
}

static void set_control_optional(const char *hw_device, const char *control_name,
                                 long value, int for_capture)
{
    if (try_set_mixer_control(hw_device, control_name, value, for_capture) == 0)
        return;
    fprintf(stderr, "Warning: mixer control '%s' not set on %s (skipped)\n",
            control_name, hw_device);
    fprintf(stderr, "         Use amixer -c <card> contents to check control names.\n");
}


// Map bit depth to PCM format
snd_pcm_format_t map_format(int bits) {
    switch (bits) {
        case 8:
            return SND_PCM_FORMAT_S8;
        case 16:
            return SND_PCM_FORMAT_S16_LE;
        case 24:
            return SND_PCM_FORMAT_S24_LE;
        case 32:
            return SND_PCM_FORMAT_S32_LE;
        default:
            return SND_PCM_FORMAT_S16_LE;  // default 16-bit
    }
}

// Check if hardware supports the given format
int check_supported_format(snd_pcm_t *pcm_handle, snd_pcm_hw_params_t *params, snd_pcm_format_t format) {
    int ret = snd_pcm_hw_params_test_format(pcm_handle, params, format);
    return (ret == 0) ? 1 : 0;  // 0 means supported
}

// Return a supported sample format
snd_pcm_format_t set_supported_format(snd_pcm_t *pcm_handle, snd_pcm_hw_params_t *params, int bits) {
    snd_pcm_format_t format = map_format(bits);
    if (check_supported_format(pcm_handle, params, format)) {
        return format;
    } else {
        printf("Format with %d bits is not supported, setting to SND_PCM_FORMAT_S16_LE\n", bits);
        return SND_PCM_FORMAT_S16_LE;
    }
}

// Check if hardware supports the given sample rate
int check_supported_rate(snd_pcm_t *pcm_handle, snd_pcm_hw_params_t *params, unsigned int rate) {
    int ret = snd_pcm_hw_params_test_rate(pcm_handle, params, rate, 0);
    return (ret == 0) ? 1 : 0;  // 0 means supported
}

// Return a supported sample rate
unsigned int set_supported_rate(snd_pcm_t *pcm_handle, snd_pcm_hw_params_t *params, unsigned int rate) {
    if (check_supported_rate(pcm_handle, params, rate)) {
        return rate;
    } else {
        printf("Rate %d is not supported, setting to 44100\n", rate);
        return 44100;
    }
}


// Record audio
void record_audio(const AudioConfig *config) {
    snd_pcm_t *pcm_handle;
    snd_pcm_hw_params_t *params;
    snd_pcm_uframes_t frames;
    int ret;
    char *buffer;
    float gain = 10;  // digital gain applied to samples; adjust or remove as needed

    const char *control_name = "ADC PGA Gain";  // mixer control name; adjust per board
    int value_adc_pga_gain = 8;                // ADC PGA gain value; adjust per board
    unsigned int rate = config->rate;
    size_t frame_bytes = config->channels * (config->bits / 8);

    set_control_optional(config->capture_device, control_name, value_adc_pga_gain, 1);

    // Open PCM device
    ret = snd_pcm_open(&pcm_handle, config->capture_device, SND_PCM_STREAM_CAPTURE, 0);
    if (ret < 0) {
        fprintf(stderr, "Unable to open PCM device: %s\n", snd_strerror(ret));
        exit(1);
    }

    // Configure hardware parameters
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm_handle, params);
    snd_pcm_hw_params_set_access(pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);  // interleaved is standard

    // Ensure supported sample format
    snd_pcm_format_t format = set_supported_format(pcm_handle, params, config->bits);
    snd_pcm_hw_params_set_format(pcm_handle, params, format);
    // Ensure supported sample rate
    rate = set_supported_rate(pcm_handle, params, rate);
    snd_pcm_hw_params_set_rate_near(pcm_handle, params, &rate, 0);
    // Set channel count
    snd_pcm_hw_params_set_channels(pcm_handle, params, config->channels);

    // Apply hardware parameters
    ret = snd_pcm_hw_params(pcm_handle, params);
    if (ret < 0) {
        fprintf(stderr, "Unable to set hardware parameters: %s\n", snd_strerror(ret));
        exit(1);
    }

    // Get buffer size
    snd_pcm_hw_params_get_period_size(params, &frames, 0);
    buffer = (char *) malloc(frames * frame_bytes);

    // Open file for recording
    FILE *file = fopen(config->record_file, "wb");
    if (!file) {
        fprintf(stderr, "Unable to open file %s for writing.\n", config->record_file);
        exit(1);
    }

    // Record and save to file
    printf("Start recording: %s (%u sec, %u Hz, %u ch, %u bit, device %s)\n",
           config->record_file, config->duration, rate, config->channels,
           config->bits, config->capture_device);
    unsigned int data_size = 0;
    snd_pcm_uframes_t total_frames = (snd_pcm_uframes_t)rate * config->duration;
    snd_pcm_uframes_t captured = 0;

    while (captured < total_frames) {
        snd_pcm_uframes_t chunk = frames;
        if (captured + chunk > total_frames)
            chunk = total_frames - captured;

        ret = snd_pcm_readi(pcm_handle, buffer, chunk);
        if (ret == -EPIPE) {
            fprintf(stderr, "Buffer overflow error occurred!\n");
            snd_pcm_prepare(pcm_handle);
            continue;
        } else if (ret < 0) {
            fprintf(stderr, "Recording read error: %s\n", snd_strerror(ret));
            break;
        } else if (ret > 0) {
            int16_t *samples = (int16_t *)buffer;
            size_t sample_count = ret * config->channels;
            for (size_t j = 0; j < sample_count; j++) {
                int sample = samples[j] * gain;
                if (sample > 32767) sample = 32767;
                if (sample < -32768) sample = -32768;
                samples[j] = (int16_t)sample;
            }
            size_t bytes = ret * frame_bytes;
            fwrite(buffer, 1, bytes, file);
            data_size += bytes;
            captured += ret;
        }
    }

    // Write WAV header
    fseek(file, 0, SEEK_SET);
    write_wav_header(file, rate, config->channels, config->bits, data_size);

    // Close PCM device and file
    snd_pcm_close(pcm_handle);
    fclose(file);
    free(buffer);

    printf("Recording finished: %s (%u bytes, %.2f sec, %u Hz, %u ch, %u bit)\n",
           config->record_file, data_size,
           rate > 0 ? (double)data_size / (rate * frame_bytes) : 0.0,
           rate, config->channels, config->bits);
}

// Play audio
void play_audio(const AudioConfig *config) {
    snd_pcm_t *pcm_handle;
    snd_pcm_hw_params_t *params;
    snd_pcm_uframes_t frames;
    FILE *file;
    char *buffer;
    int ret;
    unsigned int rate;
    unsigned short wav_channels, wav_bits;
    unsigned int wav_data_size;
    size_t frame_bytes;
    unsigned int bytes_played = 0;

    const char *control_name = "DAC";
    int value_dac = 191;

    file = fopen(config->record_file, "rb");
    if (!file) {
        fprintf(stderr, "Unable to open file: %s\n", config->record_file);
        exit(1);
    }

    if (read_wav_pcm_info(file, &rate, &wav_channels, &wav_bits, &wav_data_size) < 0) {
        fprintf(stderr, "Invalid or unsupported WAV file: %s\n", config->record_file);
        fclose(file);
        exit(1);
    }

    frame_bytes = wav_channels * (wav_bits / 8);
    if (frame_bytes == 0) {
        fprintf(stderr, "Invalid WAV format in %s\n", config->record_file);
        fclose(file);
        exit(1);
    }

    printf("Playing file: %s (%u Hz, %u ch, %u bit, %.2f sec, device %s)\n",
         config->record_file, rate, wav_channels, wav_bits,
         rate > 0 ? (double)wav_data_size / (rate * frame_bytes) : 0.0,
         config->playback_device);

    if (wav_channels != config->channels || rate != config->rate ||
        wav_bits != config->bits) {
        printf("Note: WAV format differs from current CLI settings "
               "(%u Hz, %u ch, %u bit); playback follows WAV header.\n",
               config->rate, config->channels, config->bits);
    }

    set_control_optional(config->playback_device, control_name, value_dac, 0);

    ret = snd_pcm_open(&pcm_handle, config->playback_device, SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        fprintf(stderr, "Unable to open PCM device:%s\n", snd_strerror(ret));
        fclose(file);
        exit(1);
    }

    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm_handle, params);
    snd_pcm_hw_params_set_access(pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);

    snd_pcm_format_t format = set_supported_format(pcm_handle, params, wav_bits);
    snd_pcm_hw_params_set_format(pcm_handle, params, format);
    rate = set_supported_rate(pcm_handle, params, rate);
    snd_pcm_hw_params_set_rate_near(pcm_handle, params, &rate, 0);
    snd_pcm_hw_params_set_channels(pcm_handle, params, wav_channels);

    ret = snd_pcm_hw_params(pcm_handle, params);
    if (ret < 0) {
        fprintf(stderr, "Unable to set hardware parameters: %s\n", snd_strerror(ret));
        snd_pcm_close(pcm_handle);
        fclose(file);
        exit(1);
    }

    snd_pcm_hw_params_get_period_size(params, &frames, 0);
    buffer = (char *) malloc(frames * frame_bytes);

    while (bytes_played < wav_data_size) {
        size_t to_read = frames * frame_bytes;
        if (bytes_played + to_read > wav_data_size)
            to_read = wav_data_size - bytes_played;

        size_t read_size = fread(buffer, 1, to_read, file);
        if (read_size == 0)
            break;

        snd_pcm_uframes_t pcm_frames = read_size / frame_bytes;
        snd_pcm_uframes_t offset = 0;

        while (offset < pcm_frames) {
            ret = snd_pcm_writei(pcm_handle, buffer + offset * frame_bytes,
                                 pcm_frames - offset);
            if (ret == -EPIPE) {
                fprintf(stderr, "Buffer overflow error occurred!\n");
                snd_pcm_prepare(pcm_handle);
            } else if (ret < 0) {
                fprintf(stderr, "Error playing audio: %s\n", snd_strerror(ret));
                break;
            } else if (ret > 0) {
                offset += ret;
            }
        }
        bytes_played += read_size;
    }

    snd_pcm_drain(pcm_handle);
    snd_pcm_close(pcm_handle);
    fclose(file);
    free(buffer);

    printf("Playback finished: %s\n", config->record_file);
}

// Pretty-print supported formats for readability
void print_supported_formats(snd_pcm_t *pcm_handle, snd_pcm_hw_params_t *params) {
    printf("Supported formats:\n");
    printf("%-30s%-50s%-15s\n", "Format", "Support", "Description");
    printf("----------------------------------------------------------------------------------------------\n");

    for (int i = 0; formats[i] != SND_PCM_FORMAT_UNKNOWN; i++) {
        const char *format_name = snd_pcm_format_name(formats[i]);
        const char *format_desc = snd_pcm_format_description(formats[i]);
        int ret = snd_pcm_hw_params_test_format(pcm_handle, params, formats[i]);

        printf("%-30s%-50s%-15s\n",
                format_name ? format_name : "Unknown",
                format_desc ? format_desc : "No description",
                ret == 0 ? "Supported" : "Not Supported");
    }
}

void print_supported_channels(snd_pcm_t *pcm_handle, snd_pcm_hw_params_t *params) {
    unsigned int min_channels, max_channels;
    int ret;

    // Get min/max channel count
    ret = snd_pcm_hw_params_get_channels_min(params, &min_channels);
    if (ret < 0) {
        fprintf(stderr, "Error getting minimum channels: %s\n", snd_strerror(ret));
        return;
    }

    ret = snd_pcm_hw_params_get_channels_max(params, &max_channels);
    if (ret < 0) {
        fprintf(stderr, "Error getting maximum channels: %s\n", snd_strerror(ret));
        return;
    }

    printf("\nChannels                      SupportNum\n");
    printf("--------------------------------------------------------\n");
    printf("Max                              %u\n", max_channels);
    printf("Min                              %u\n", min_channels);
}


void print_supported_sampling_rates(snd_pcm_t *pcm_handle, snd_pcm_hw_params_t *params) {
    unsigned int rates[] = {8000, 16000, 22050, 44100, 48000, 96000, 192000};
    size_t num_rates = sizeof(rates) / sizeof(rates[0]);

    printf("\nSampling Rate (Hz)            Support\n");
    printf("--------------------------------------------------------\n");
    for (size_t i = 0; i < num_rates; i++) {
        unsigned int rate = rates[i];
        int ret = snd_pcm_hw_params_test_rate(pcm_handle, params, rate, 0);
        printf("%-30u %-15s\n", rate, ret == 0 ? "Supported" : "Not Supported");
    }
}


// Query and print device capabilities
void get_pcm_format(const AudioConfig *config) {
    snd_pcm_t *pcm_handle;
    snd_pcm_hw_params_t *params;
    int ret;

    // Open capture PCM device
    printf("capture_device:\n");
    ret = snd_pcm_open(&pcm_handle, config->capture_device, SND_PCM_STREAM_CAPTURE, 0);
    if (ret < 0) {
        fprintf(stderr, "Unable to open PCM device: %s\n", snd_strerror(ret));
        exit(1);
    }

    // Allocate hardware params object
    snd_pcm_hw_params_alloca(&params);

    // Initialize hardware params object
    ret = snd_pcm_hw_params_any(pcm_handle, params);
    if (ret < 0) {
        fprintf(stderr, "Error initializing hardware parameters: %s\n", snd_strerror(ret));
        snd_pcm_close(pcm_handle);
        exit(1);
    }

    // Print supported formats
    print_supported_formats(pcm_handle, params);

    // Print supported channel range
    print_supported_channels(pcm_handle, params);

    // Print supported sample rates
    print_supported_sampling_rates(pcm_handle, params);

    snd_pcm_close(pcm_handle);  // close device, release resources

    // Open playback PCM device
    printf("playback_device:\n");
    ret = snd_pcm_open(&pcm_handle, config->playback_device, SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        fprintf(stderr, "Unable to open PCM device: %s\n", snd_strerror(ret));
        exit(1);
    }

    // Allocate hardware params object
    snd_pcm_hw_params_alloca(&params);

    // Initialize hardware params object
    ret = snd_pcm_hw_params_any(pcm_handle, params);
    if (ret < 0) {
        fprintf(stderr, "Error initializing hardware parameters: %s\n", snd_strerror(ret));
        snd_pcm_close(pcm_handle);
        exit(1);
    }

    // Print supported formats
    print_supported_formats(pcm_handle, params);

    // Print supported channel range
    print_supported_channels(pcm_handle, params);

    // Print supported sample rates
    print_supported_sampling_rates(pcm_handle, params);

    snd_pcm_close(pcm_handle);  // close device, release resources

    command_help();

}

// Handle user interactive commands
static void handle_user_command(const AudioConfig *config) {
    char option;
    int running = 1;

    command_help();
    printf("\nCommand: ");
    while (running && ((option = getchar()) != EOF)) {
        switch (option) {
            case 'q':
                printf("Quit\n");
                running = 0;
                break;
            case 'r':
                record_audio(config);
                break;
            case 'p':
                play_audio(config);
                break;
            case 'c':
                // Check hardware info before setting parameters to avoid invalid configs
                get_pcm_format(config);
                break;
            case 'h':
                print_help();
                break;
            case '\n':
            case '\r':
                continue;
            default:
                printf("Command not supported!\n");
                command_help();
                break;
        }
        printf("\nCommand: ");
    }
}


int main(int argc, char **argv) {
    int opt;
    static const struct option long_opts[] = {
        {"capture", required_argument, NULL, OPT_CAPTURE},
        {"playback", required_argument, NULL, OPT_PLAYBACK},
        {NULL, 0, NULL, 0}
    };

    while ((opt = getopt_long(argc, argv, "r:b:c:d:f:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case OPT_CAPTURE:
                if (set_hw_device(config.capture_device,
                                  sizeof(config.capture_device),
                                  optarg, "--capture") < 0)
                    return 1;
                break;
            case OPT_PLAYBACK:
                if (set_hw_device(config.playback_device,
                                  sizeof(config.playback_device),
                                  optarg, "--playback") < 0)
                    return 1;
                break;
            case 'r':
                config.rate = atoi(optarg);
                break;
            case 'b':
                config.bits = atoi(optarg);
                break;
            case 'c':
                config.channels = atoi(optarg);
                break;
            case 'd':
                config.duration = atoi(optarg);
                break;
            case 'f':
                strncpy(config.record_file, optarg, sizeof(config.record_file) - 1);
                break;
            case 'h':
            default:
                print_help();
                return 0;
        }
    }

    printf("Audio Recording and Playback Program\n");
    printf("Settings:\n");
    printf("  Capture Device        : %s\n", config.capture_device);
    printf("  Playback Device       : %s\n", config.playback_device);
    printf("  Sampling Rate         : %d Hz\n", config.rate);
    printf("  Bit Depth             : %d bit\n", config.bits);
    printf("  Channels              : %d\n", config.channels);
    printf("  Duration              : %d seconds\n", config.duration);
    printf("  File Name             : %s\n", config.record_file);

    handle_user_command(&config);
    return 0;
}