#include "codec_helper.h"
#define TAG "[MP4_DECODE]"
// 写入H.264头信息（SPS和PPS）
int write_h264_header(AVCodecParameters *codec_params, FILE *fp)
{
    if (!codec_params->extradata || codec_params->extradata_size <= 0)
    {
        printf("%s Warning: No extradata found for H.264\n", TAG);
        return -1;
    }

    // 查找SPS和PPS
    uint8_t *data = codec_params->extradata;
    int size = codec_params->extradata_size;
    int pos = 0;

    while (pos < size - 4)
    {
        // 查找NALU起始码 (0x00 0x00 0x01 或 0x00 0x00 0x00 0x01)
        if ((pos + 3 < size && data[pos] == 0x00 && data[pos + 1] == 0x00 &&
             data[pos + 2] == 0x01) ||
            (pos + 4 < size && data[pos] == 0x00 && data[pos + 1] == 0x00 &&
             data[pos + 2] == 0x00 && data[pos + 3] == 0x01))
        {

            int nalu_start = pos;
            int nalu_type = 0;

            // 跳过起始码
            if (data[pos + 2] == 0x01)
            {
                pos += 3;
                nalu_type = data[pos] & 0x1F;
            }
            else
            {
                pos += 4;
                nalu_type = data[pos] & 0x1F;
            }

            // 查找下一个NALU起始码
            int nalu_end = size;
            for (int i = pos; i < size - 3; i++)
            {
                if ((data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) ||
                    (i + 3 < size && data[i] == 0x00 && data[i + 1] == 0x00 &&
                     data[i + 2] == 0x00 && data[i + 3] == 0x01))
                {
                    nalu_end = i;
                    break;
                }
            }

            // 写入SPS (NALU type 7) 或 PPS (NALU type 8)
            if (nalu_type == 7 || nalu_type == 8)
            {
                printf("%s Writing %s (NALU type %d), size: %d\n",
                       TAG, nalu_type == 7 ? "SPS" : "PPS", nalu_type, nalu_end - nalu_start);

                // 写入起始码
                if (data[nalu_start + 2] == 0x01)
                {
                    fwrite("\x00\x00\x01", 1, 3, fp);
                }
                else
                {
                    fwrite("\x00\x00\x00\x01", 1, 4, fp);
                }

                // 写入NALU数据
                fwrite(data + nalu_start + (data[nalu_start + 2] == 0x01 ? 3 : 4),
                       1, nalu_end - nalu_start - (data[nalu_start + 2] == 0x01 ? 3 : 4), fp);
                fflush(fp);
            }

            pos = nalu_end;
        }
        else
        {
            pos++;
        }
    }

    return 0;
}

// 写入H.265头信息（VPS, SPS和PPS）
int write_h265_header(AVCodecParameters *codec_params, FILE *fp)
{
    if (!codec_params->extradata || codec_params->extradata_size <= 0)
    {
        printf("%s Warning: No extradata found for H.265\n", TAG);
        return -1;
    }

    // 查找VPS, SPS和PPS
    uint8_t *data = codec_params->extradata;
    int size = codec_params->extradata_size;
    int pos = 0;

    while (pos < size - 4)
    {
        // 查找NALU起始码
        if ((pos + 3 < size && data[pos] == 0x00 && data[pos + 1] == 0x00 &&
             data[pos + 2] == 0x01) ||
            (pos + 4 < size && data[pos] == 0x00 && data[pos + 1] == 0x00 &&
             data[pos + 2] == 0x00 && data[pos + 3] == 0x01))
        {

            int nalu_start = pos;
            int nalu_type = 0;

            // 跳过起始码
            if (data[pos + 2] == 0x01)
            {
                pos += 3;
                nalu_type = (data[pos] >> 1) & 0x3F;
            }
            else
            {
                pos += 4;
                nalu_type = (data[pos] >> 1) & 0x3F;
            }

            // 查找下一个NALU起始码
            int nalu_end = size;
            for (int i = pos; i < size - 3; i++)
            {
                if ((data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) ||
                    (i + 3 < size && data[i] == 0x00 && data[i + 1] == 0x00 &&
                     data[i + 2] == 0x00 && data[i + 3] == 0x01))
                {
                    nalu_end = i;
                    break;
                }
            }

            // 写入VPS (NALU type 32), SPS (NALU type 33) 或 PPS (NALU type 34)
            if (nalu_type == 32 || nalu_type == 33 || nalu_type == 34)
            {
                const char *nalu_name = (nalu_type == 32) ? "VPS" : (nalu_type == 33) ? "SPS"
                                                                                      : "PPS";
                printf("%s Writing %s (NALU type %d), size: %d\n",
                       TAG, nalu_name, nalu_type, nalu_end - nalu_start);

                // 写入起始码
                if (data[nalu_start + 2] == 0x01)
                {
                    fwrite("\x00\x00\x01", 1, 3, fp);
                }
                else
                {
                    fwrite("\x00\x00\x00\x01", 1, 4, fp);
                }

                // 写入NALU数据
                fwrite(data + nalu_start + (data[nalu_start + 2] == 0x01 ? 3 : 4),
                       1, nalu_end - nalu_start - (data[nalu_start + 2] == 0x01 ? 3 : 4), fp);
                fflush(fp);
            }

            pos = nalu_end;
        }
        else
        {
            pos++;
        }
    }

    return 0;
}
