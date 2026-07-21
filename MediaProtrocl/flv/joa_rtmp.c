#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>

#include "joa_rtmp.h"
#include "jooan_utils_log.h"
#include "ipc_app_camera_config.h"
#include "Jooan_ShmAvBuffer.h"
#include "ts_avframe_analysis.h"
#include "joa_network.h"
#include "rtmp.h"

#define RTMP_THREAD_NAME        "rtmp_push"
#define RTMP_FLV_HEADER_SIZE    5
#define RTMP_MAX_SPS_SIZE       256
#define RTMP_MAX_PPS_SIZE       256
#define RTMP_RETRY_INTERVAL_DEF 5
#define RTMP_CONNECT_TIMEOUT_DEF 5
#define RTMP_DEFAULT_BUFFER     (512 * 1024)

#define AMF_NUMBER              0x00
#define AMF_BOOLEAN             0x01
#define AMF_STRING              0x02
#define AMF_ECMA_ARRAY          0x08
#define AMF_OBJECT_END          0x09

static int g_push_start = 0;
typedef struct rtmp_context_s {
    rtmp_config_t config;
    pthread_t thread;
    int thread_started;
    int stop;
    pthread_mutex_t lock;
    rtmp_status_t status;
    RTMP *rtmp;
    int video_shm_id;
    int video_codec;
    unsigned char *video_buf;
    int video_buf_size;
    unsigned char *flv_buf;
    int flv_buf_size;
    unsigned char sps[RTMP_MAX_SPS_SIZE];
    int sps_len;
    unsigned char pps[RTMP_MAX_PPS_SIZE];
    int pps_len;
    int sent_config;
    int metadata_stream_sent;
    int metadata_dump_written;
    int reconnect_attempts;
    uint64_t total_video_timestamp;
    uint64_t last_sys_timestamp;
    uint64_t last_video_timestamp;
    int video_ts_initialized;
    int request_idr;
    int video_started;
    FILE *dump_fp;
    int dump_header_written;
    int audio_shm_id;
    int audio_codec;
    unsigned char *audio_buf;
    int audio_buf_size;
    int audio_config_sent;
    int audio_sample_rate;  // 音频采样率（Hz）
    int audio_frame_size;   // AAC帧大小（采样点数，通常是1024）
    uint64_t audio_frame_count;  // 音频帧计数，用于计算时间戳
} rtmp_context_t;

static rtmp_context_t *g_rtmp_ctx = NULL;

static void rtmp_set_status(rtmp_context_t *ctx, rtmp_status_t status)
{
    pthread_mutex_lock(&ctx->lock);
    ctx->status = status;
    pthread_mutex_unlock(&ctx->lock);
}

static void rtmp_disconnect(rtmp_context_t *ctx)
{
    if (ctx->rtmp) {
        RTMP_Close(ctx->rtmp);
        RTMP_Free(ctx->rtmp);
        ctx->rtmp = NULL;
    }
    ctx->sent_config = 0;
    ctx->metadata_stream_sent = 0;
    ctx->metadata_dump_written = 0;
    ctx->total_video_timestamp = 0;
    ctx->last_sys_timestamp = 0;
    ctx->last_video_timestamp = 0;
    ctx->video_ts_initialized = 0;
    ctx->video_started = 0;
    ctx->audio_frame_count = 0;
    rtmp_set_status(ctx, RTMP_STATUS_DISCONNECTED);
}

static int rtmp_prepare_buffers(rtmp_context_t *ctx)
{
    if (ctx->config.video_buffer_size <= 0) {
        ctx->config.video_buffer_size = Jooan_Get_StreamSize(ctx->config.video_profile_no);
        if (ctx->config.video_buffer_size <= 0) {
            ctx->config.video_buffer_size = RTMP_DEFAULT_BUFFER;
        }
    }
	
    ctx->video_buf_size = ctx->config.video_buffer_size;
    ctx->flv_buf_size = ctx->video_buf_size + RTMP_FLV_HEADER_SIZE + 1024;

    ctx->video_buf = (unsigned char *)malloc(ctx->video_buf_size);
    if (!ctx->video_buf) {
        UTIL_ERR("RTMP allocate video buffer failed, size=%d\n", ctx->video_buf_size);
        return -1;
    }

    ctx->flv_buf = (unsigned char *)malloc(ctx->flv_buf_size);
    if (!ctx->flv_buf) {
        UTIL_ERR("RTMP allocate flv buffer failed, size=%d\n", ctx->flv_buf_size);
        free(ctx->video_buf);
        ctx->video_buf = NULL;
        return -1;
    }

    // 分配音频缓冲区
    if (ctx->config.audio_profile_no >= 0) {
        if (ctx->config.audio_buffer_size <= 0) {
            ctx->config.audio_buffer_size = 4096;  // 默认4KB
        }
        ctx->audio_buf_size = ctx->config.audio_buffer_size;
        ctx->audio_buf = (unsigned char *)malloc(ctx->audio_buf_size);
        if (!ctx->audio_buf) {
            UTIL_ERR("RTMP allocate audio buffer failed, size=%d\n", ctx->audio_buf_size);
            free(ctx->video_buf);
            free(ctx->flv_buf);
            ctx->video_buf = NULL;
            ctx->flv_buf = NULL;
            return -1;
        }
    }

    UTIL_INFO("rtmp_prepare_buffers success ctx->flv_buf_size:%d, audio_buf_size:%d\n", 
             ctx->flv_buf_size, ctx->audio_buf_size);
    return 0;
}

static int rtmp_dump_write_header(rtmp_context_t *ctx)
{
    if (!ctx->dump_fp || ctx->dump_header_written) {
        return 0;
    }
    // FLV header: 0x01 = 视频, 0x05 = 视频+音频
    unsigned char flags = 0x01;  // 只有视频
    if (ctx->config.audio_profile_no >= 0) {
        flags = 0x05;  // 视频+音频
    }
    unsigned char header[13] = {
        0x46, 0x4C, 0x56, 0x01, flags,
        0x00, 0x00, 0x00, 0x09,
        0x00, 0x00, 0x00, 0x00
    };
    if (fwrite(header, 1, sizeof(header), ctx->dump_fp) != sizeof(header)) {
        UTIL_ERR("rtmp dump write header failed\n");
        return -1;
    }
    ctx->dump_header_written = 1;
    return 0;
}

static int rtmp_dump_write_tag(rtmp_context_t *ctx, uint8_t tag_type, const unsigned char *data, int size, uint32_t timestamp)
{
    if (!ctx->dump_fp) {
        return 0;
    }
    if (rtmp_dump_write_header(ctx) < 0) {
        return -1;
    }

    unsigned char tag_header[11];
    tag_header[0] = tag_type;
    tag_header[1] = (size >> 16) & 0xFF;
    tag_header[2] = (size >> 8) & 0xFF;
    tag_header[3] = size & 0xFF;
    tag_header[4] = (timestamp >> 16) & 0xFF;
    tag_header[5] = (timestamp >> 8) & 0xFF;
    tag_header[6] = timestamp & 0xFF;
    tag_header[7] = (timestamp >> 24) & 0xFF;
    tag_header[8] = tag_header[9] = tag_header[10] = 0x00;

    if (fwrite(tag_header, 1, sizeof(tag_header), ctx->dump_fp) != sizeof(tag_header)) {
        UTIL_ERR("rtmp dump write tag header failed\n");
        return -1;
    }

    if (fwrite(data, 1, size, ctx->dump_fp) != (size_t)size) {
        UTIL_ERR("rtmp dump write tag payload failed\n");
        return -1;
    }

    uint32_t prev_size = size + sizeof(tag_header);
    unsigned char prev[4] = {
        (prev_size >> 24) & 0xFF,
        (prev_size >> 16) & 0xFF,
        (prev_size >> 8) & 0xFF,
        prev_size & 0xFF
    };

    if (fwrite(prev, 1, sizeof(prev), ctx->dump_fp) != sizeof(prev)) {
        UTIL_ERR("rtmp dump write previous tag size failed\n");
        return -1;
    }
    
    // 定期刷新文件缓冲区，确保数据及时写入
    fflush(ctx->dump_fp);
    return 0;
}

static uint8_t* amf_write_string_raw(uint8_t *p, uint8_t *end, const char *str)
{
    size_t len = strlen(str);
    if ((size_t)(end - p) < len + 2) {
        return NULL;
    }
    uint16_t slen = (uint16_t)len;
    *p++ = (slen >> 8) & 0xFF;
    *p++ = slen & 0xFF;
    memcpy(p, str, len);
    return p + len;
}

static uint8_t* amf_write_string(uint8_t *p, uint8_t *end, const char *str)
{
    if (end - p < 1) {
        return NULL;
    }
    *p++ = AMF_STRING;
    return amf_write_string_raw(p, end, str);
}

static uint8_t* amf_write_number(uint8_t *p, uint8_t *end, double value)
{
    if (end - p < 9) {
        return NULL;
    }
    *p++ = AMF_NUMBER;
    union {
        double d;
        uint8_t b[8];
    } u;
    u.d = value;
    for (int i = 7; i >= 0; --i) {
        *p++ = u.b[i];
    }
    return p;
}

static uint8_t* amf_write_u32(uint8_t *p, uint8_t *end, uint32_t value)
{
    if (end - p < 4) {
        return NULL;
    }
    *p++ = (value >> 24) & 0xFF;
    *p++ = (value >> 16) & 0xFF;
    *p++ = (value >> 8) & 0xFF;
    *p++ = value & 0xFF;
    return p;
}

static int rtmp_build_metadata_body(unsigned char *buf, int buf_size)//flv
{
    uint8_t *p = buf;
    uint8_t *end = buf + buf_size;

    p = amf_write_string(p, end, "onMetaData");
    if (!p || end - p < 5) {
        return -1;
    }
    *p++ = AMF_ECMA_ARRAY;
    p = amf_write_u32(p, end, 5);
    if (!p) {
        return -1;
    }
    /* 第二个AMF包 */
    p = amf_write_string_raw(p, end, "duration");
    if (!p || !(p = amf_write_number(p, end, 0.0))) {
        return -1;
    }

    p = amf_write_string_raw(p, end, "width");
    if (!p || !(p = amf_write_number(p, end, 640.0))) {
        return -1;
    }

    p = amf_write_string_raw(p, end, "height");
    if (!p || !(p = amf_write_number(p, end, 360.0))) {
        return -1;
    }

    p = amf_write_string_raw(p, end, "videocodecid");
    if (!p || !(p = amf_write_number(p, end, 7.0))) {
        return -1;
    }

    p = amf_write_string_raw(p, end, "framerate");
    if (!p || !(p = amf_write_number(p, end, 15.0))) {
        return -1;
    }

    // 添加音频信息（如果有音频）
    p = amf_write_string_raw(p, end, "audiocodecid");
    if (!p || !(p = amf_write_number(p, end, 10.0))) {  // 10 = AAC
        return -1;
    }

    p = amf_write_string_raw(p, end, "audiosamplerate");
    if (!p || !(p = amf_write_number(p, end, 16000.0))) {  // 16kHz
        return -1;
    }

    p = amf_write_string_raw(p, end, "stereo");
    if (!p || !(p = amf_write_number(p, end, 0.0))) {  // 0 = mono
        return -1;
    }

    if (end - p < 3) {
        return -1;
    }
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = AMF_OBJECT_END;

    return (int)(p - buf);
}

static int rtmp_write_metadata_dump(rtmp_context_t *ctx)
{
    if (!ctx->config.dump_enable || !ctx->dump_fp || ctx->metadata_dump_written) {
        return 0;
    }

    unsigned char body[256];
    int body_size = rtmp_build_metadata_body(body, sizeof(body));
    if (body_size <= 0) {
        return -1;
    }

    if (rtmp_dump_write_tag(ctx, RTMP_PACKET_TYPE_INFO, body, body_size, 0) < 0) {
        return -1;
    }

    ctx->metadata_dump_written = 1;
    return 0;
}
static int rtmp_send_packet(rtmp_context_t *ctx, const unsigned char *data, int size, uint32_t timestamp, uint8_t packet_type)
{
    if (!ctx->rtmp || !RTMP_IsConnected(ctx->rtmp)) {
        return -1;
    }

    RTMPPacket packet;
    RTMPPacket_Reset(&packet);
    if (!RTMPPacket_Alloc(&packet, size)) {
        UTIL_ERR("RTMP alloc packet failed, size=%d\n", size);
        return -1;
    }

    packet.m_packetType = packet_type;
    packet.m_nChannel = 0x04;
    packet.m_headerType = RTMP_PACKET_SIZE_LARGE;
    packet.m_nTimeStamp = timestamp;
    packet.m_nInfoField2 = ctx->rtmp->m_stream_id;
    packet.m_hasAbsTimestamp = 0;
    packet.m_nBodySize = size;
    memcpy(packet.m_body, data, size);

    int ret = RTMP_SendPacket(ctx->rtmp, &packet, TRUE);
    //int ret = RTMP_SendPacket(ctx->rtmp, &packet, false);
    RTMPPacket_Free(&packet);
    return ret ? 0 : -1;
}

static int rtmp_send_metadata_stream(rtmp_context_t *ctx)
{
    if (ctx->metadata_stream_sent || !ctx->rtmp || !RTMP_IsConnected(ctx->rtmp)) {
        return 0;
    }

    unsigned char body[256];
    int body_size = rtmp_build_metadata_body(body, sizeof(body));
    if (body_size <= 0) {
        return -1;
    }

    if (rtmp_send_packet(ctx, body, body_size, 0, RTMP_PACKET_TYPE_INFO) < 0) {
        return -1;
    }

    ctx->metadata_stream_sent = 1;
    return 0;
}

static const unsigned char* rtmp_find_start_code(const unsigned char *p, const unsigned char *end)
{
    while (p + 3 < end) {
        if (p[0] == 0x00 && p[1] == 0x00) {
            if (p[2] == 0x01) {
                return p;
            }
            if (p[2] == 0x00 && p + 4 < end && p[3] == 0x01) {
                return p;
            }
        }
        ++p;
    }
    return NULL;
}

static const unsigned char* rtmp_skip_start_code(const unsigned char *p, const unsigned char *end, int *prefix_len)
{
    if (p + 3 >= end) {
        return end;
    }
    if (p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01) {
        if (prefix_len) {
            *prefix_len = 3;
        }
        return p + 3;
    }
    if (p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x01) {
        if (prefix_len) {
            *prefix_len = 4;
        }
        return p + 4;
    }
    if (prefix_len) {
        *prefix_len = 0;
    }
    return p;
}

static void rtmp_extract_sps_pps(rtmp_context_t *ctx, const unsigned char *data, int len)
{
    const unsigned char *p = data;
    const unsigned char *end = data + len;

    while (p < end) {
        const unsigned char *sc = rtmp_find_start_code(p, end);
        if (!sc) {
            break;
        }
        int prefix = 0;
        const unsigned char *nal = rtmp_skip_start_code(sc, end, &prefix);
        if (nal >= end) {
            break;
        }
        const unsigned char *next = rtmp_find_start_code(nal, end);
        const unsigned char *nal_end = next ? next : end;
        int nal_len = (int)(nal_end - nal);
        if (nal_len <= 0) {
            p = next ? next : end;
            continue;
        }

        int nal_type = nal[0] & 0x1f;
        if (nal_type == 7 && nal_len <= RTMP_MAX_SPS_SIZE) {
            memcpy(ctx->sps, nal, nal_len);
            ctx->sps_len = nal_len;
        } else if (nal_type == 8 && nal_len <= RTMP_MAX_PPS_SIZE) {
            memcpy(ctx->pps, nal, nal_len);
            ctx->pps_len = nal_len;
        }

        p = next ? next : end;
    }
}

static int rtmp_output_video_tag(rtmp_context_t *ctx, const unsigned char *data, int size, uint32_t timestamp)
{
    int ret = 0;
    if (ctx->config.dump_enable && ctx->dump_fp) {
        if (rtmp_write_metadata_dump(ctx) < 0) {
            ret = -1;
        }
        if (ret == 0 && rtmp_dump_write_tag(ctx, RTMP_PACKET_TYPE_VIDEO, data, size, timestamp) < 0) {
            ret = -1;
        }
    }
    if (!ctx->config.dump_only) {
        if (rtmp_send_packet(ctx, data, size, timestamp, RTMP_PACKET_TYPE_VIDEO) < 0) {
            ret = -1;
        }
    }
    return ret;
}

static int rtmp_output_audio_tag(rtmp_context_t *ctx, const unsigned char *data, int size, uint32_t timestamp)
{
    int ret = 0;
    if (ctx->config.dump_enable && ctx->dump_fp) {
        if (rtmp_write_metadata_dump(ctx) < 0) {
            ret = -1;
        }
        if (ret == 0 && rtmp_dump_write_tag(ctx, RTMP_PACKET_TYPE_AUDIO, data, size, timestamp) < 0) {
            ret = -1;
        }
    }
    if (!ctx->config.dump_only) {
        if (rtmp_send_packet(ctx, data, size, timestamp, RTMP_PACKET_TYPE_AUDIO) < 0) {
            ret = -1;
        }
    }
    return ret;
}

// 生成AAC AudioSpecificConfig
// 格式: 5bit objectType + 4bit samplingFrequencyIndex + 4bit channelConfiguration
static void rtmp_build_aac_config(rtmp_context_t *ctx, unsigned char *config)
{
    // Audio Object Type: 2 = AAC-LC
    int objectType = 2;
    
    // 采样率索引
    int samplingFrequencyIndex = 0;
    switch (ctx->audio_sample_rate) {
        case 96000: samplingFrequencyIndex = 0; break;
        case 88200: samplingFrequencyIndex = 1; break;
        case 64000: samplingFrequencyIndex = 2; break;
        case 48000: samplingFrequencyIndex = 3; break;
        case 44100: samplingFrequencyIndex = 4; break;
        case 32000: samplingFrequencyIndex = 5; break;
        case 24000: samplingFrequencyIndex = 6; break;
        case 22050: samplingFrequencyIndex = 7; break;
        case 16000: samplingFrequencyIndex = 8; break;
        case 12000: samplingFrequencyIndex = 9; break;
        case 11025: samplingFrequencyIndex = 10; break;
        case 8000:  samplingFrequencyIndex = 11; break;
        case 7350:  samplingFrequencyIndex = 12; break;
        default:    samplingFrequencyIndex = 8; break;  // 默认16kHz
    }
    
    // 声道配置: 1 = mono
    int channelConfiguration = 1;
    
    // 构建AudioSpecificConfig (2字节)
    config[0] = (objectType << 3) | ((samplingFrequencyIndex >> 1) & 0x07);
    config[1] = ((samplingFrequencyIndex & 0x01) << 7) | (channelConfiguration << 3);
}

static int rtmp_send_aac_config(rtmp_context_t *ctx, uint32_t timestamp)
{
    unsigned char aac_config[2];
    rtmp_build_aac_config(ctx, aac_config);
    
    unsigned char *body = ctx->flv_buf;
    int pos = 0;
    
    // FLV Audio Tag Header
    // SoundFormat(4bit) + SoundRate(2bit) + SoundSize(1bit) + SoundType(1bit)
    unsigned char soundFormat = 10;  // AAC
    unsigned char soundRate = 0;      // 5.5kHz (实际会被AudioSpecificConfig覆盖)
    unsigned char soundSize = 1;      // 16bit
    unsigned char soundType = 0;      // mono
    
    // 根据采样率设置SoundRate
    if (ctx->audio_sample_rate >= 44100) {
        soundRate = 3;  // 44kHz
    } else if (ctx->audio_sample_rate >= 22050) {
        soundRate = 2;  // 22kHz
    } else if (ctx->audio_sample_rate >= 11025) {
        soundRate = 1;  // 11kHz
    } else {
        soundRate = 0;  // 5.5kHz
    }
    
    body[pos++] = (soundFormat << 4) | (soundRate << 2) | (soundSize << 1) | soundType;
    body[pos++] = 0x00;  // AAC sequence header
    body[pos++] = aac_config[0];
    body[pos++] = aac_config[1];
    
    UTIL_INFO("RTMP AAC config: sample_rate=%d, config=0x%02X%02X\n", 
             ctx->audio_sample_rate, aac_config[0], aac_config[1]);
    
    return rtmp_output_audio_tag(ctx, body, pos, timestamp);
}

static int rtmp_send_aac_frame(rtmp_context_t *ctx, const unsigned char *data, int len, uint32_t timestamp)
{
	//UTIL_INFO("audio rtmp_send_aac_frame\n");
    if (len <= 0) {
		UTIL_ERR("audio len err\n");
        return 0;
    }
    
    unsigned char *body = ctx->flv_buf;
    int pos = 0;
    
    // FLV Audio Tag Header
    // SoundFormat(4bit) + SoundRate(2bit) + SoundSize(1bit) + SoundType(1bit)
    unsigned char soundFormat = 10;  // AAC
    unsigned char soundRate = 0;      // 5.5kHz (实际会被AudioSpecificConfig覆盖)
    unsigned char soundSize = 1;      // 16bit
    unsigned char soundType = 0;      // mono
    
    // 根据采样率设置SoundRate
    if (ctx->audio_sample_rate >= 44100) {
        soundRate = 3;  // 44kHz
    } else if (ctx->audio_sample_rate >= 22050) {
        soundRate = 2;  // 22kHz
    } else if (ctx->audio_sample_rate >= 11025) {
        soundRate = 1;  // 11kHz
    } else {
        soundRate = 0;  // 5.5kHz
    }
    
    body[pos++] = (soundFormat << 4) | (soundRate << 2) | (soundSize << 1) | soundType;
    body[pos++] = 0x01;  // AAC raw data
    
    if (pos + len > ctx->flv_buf_size) {
        UTIL_ERR("RTMP audio frame too large: %d > %d\n", pos + len, ctx->flv_buf_size);
        return -1;
    }
    
    memcpy(body + pos, data, len);
    pos += len;
    
    return rtmp_output_audio_tag(ctx, body, pos, timestamp);
}

static int rtmp_send_avc_config(rtmp_context_t *ctx, uint32_t timestamp)
{
    if (ctx->sps_len <= 0 || ctx->pps_len <= 0) {
        return -1;
    }

    unsigned char *body = ctx->flv_buf;
    int pos = 0;

    body[pos++] = 0x17; // KeyFrame + AVC
    body[pos++] = 0x00; // AVC sequence header
    body[pos++] = 0x00;
    body[pos++] = 0x00;
    body[pos++] = 0x00;

    body[pos++] = 0x01;
    body[pos++] = ctx->sps[1];
    body[pos++] = ctx->sps[2];
    body[pos++] = ctx->sps[3];
    body[pos++] = 0xff;

    body[pos++] = 0xE1;
    body[pos++] = (ctx->sps_len >> 8) & 0xff;
    body[pos++] = ctx->sps_len & 0xff;
    memcpy(body + pos, ctx->sps, ctx->sps_len);
    pos += ctx->sps_len;

    body[pos++] = 0x01;
    body[pos++] = (ctx->pps_len >> 8) & 0xff;
    body[pos++] = ctx->pps_len & 0xff;
    memcpy(body + pos, ctx->pps, ctx->pps_len);
    pos += ctx->pps_len;

    return rtmp_output_video_tag(ctx, body, pos, timestamp);
}

static int rtmp_send_h264_frame(rtmp_context_t *ctx, const unsigned char *data, int len, int keyframe, uint32_t timestamp)
{
    unsigned char *body = ctx->flv_buf;
    int pos = RTMP_FLV_HEADER_SIZE;
    const unsigned char *p = data;
    const unsigned char *end = data + len;

    while (p < end) {
        const unsigned char *sc = rtmp_find_start_code(p, end);
        if (!sc) {
            break;
        }
        int prefix_len = 0;
        const unsigned char *nal = rtmp_skip_start_code(sc, end, &prefix_len);
        if (nal >= end) {
            break;
        }
        const unsigned char *next = rtmp_find_start_code(nal, end);
        const unsigned char *nal_end = next ? next : end;
        int nal_len = (int)(nal_end - nal);
        if (nal_len <= 0) {
            p = next ? next : end;
            continue;
        }

        int nal_type = nal[0] & 0x1f;
        if (nal_type == 7 || nal_type == 8) {
            p = next ? next : end;
            continue;
        }

        if (pos + 4 + nal_len > ctx->flv_buf_size) {
            int new_size = ctx->flv_buf_size + nal_len + 1024;
            unsigned char *new_buf = (unsigned char *)realloc(ctx->flv_buf, new_size);
            if (!new_buf) {
                UTIL_ERR("RTMP realloc flv buf failed, need=%d\n", new_size);
                return -1;
            }
            ctx->flv_buf = new_buf;
            ctx->flv_buf_size = new_size;
            body = ctx->flv_buf;
        }

        body[pos++] = (nal_len >> 24) & 0xff;
        body[pos++] = (nal_len >> 16) & 0xff;
        body[pos++] = (nal_len >> 8) & 0xff;
        body[pos++] = nal_len & 0xff;
        memcpy(body + pos, nal, nal_len);
        pos += nal_len;

        p = next ? next : end;
    }

    if (pos == RTMP_FLV_HEADER_SIZE) {
        return 0;
    }

    body[0] = (keyframe ? 0x17 : 0x27);
    body[1] = 0x01;
    body[2] = 0x00;
    body[3] = 0x00;
    body[4] = 0x00;

    return rtmp_output_video_tag(ctx, body, pos, timestamp);
}

/**
 * 格式化MAC地址：去掉":"和"-"，转换为小写
 * @param mac_in 输入的MAC地址（如 "a0:15:23:d2:89:e8" 或 "a0-15-23-d2-89-e8"）
 * @param mac_out 输出的格式化后的MAC地址（如 "a01523d289e8"）
 * @param out_size 输出缓冲区大小
 * @return 0成功，-1失败
 */
static int rtmp_format_mac_address(const char *mac_in, char *mac_out, int out_size)
{
    if (!mac_in || !mac_out || out_size < 13) {
        return -1;
    }
    
    int i = 0;
    int j = 0;
    while (mac_in[i] != '\0' && j < out_size - 1) {
        char c = mac_in[i];
        if (c == ':' || c == '-') {
            i++;
            continue;
        }
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            mac_out[j++] = (c >= 'A' && c <= 'F') ? (c - 'A' + 'a') : c;
        }
        i++;
    }
    mac_out[j] = '\0';
    
    if (j != 12) {
        UTIL_ERR("Invalid MAC address format: %s (expected 12 hex chars, got %d)\n", mac_in, j);
        return -1;
    }
    
    return 0;
}

/**
 * 根据MAC地址生成RTMP推流URL
 * 格式：rtmp://video11-center.iuoooo.com/openlive/{mac地址}_1
 * @param url_out 输出的URL缓冲区
 * @param url_size URL缓冲区大小
 * @return 0成功，-1失败
 */
static int rtmp_generate_url_from_mac(char *url_out, int url_size)
{
    if (!url_out || url_size < 64) {
        return -1;
    }
    
    char mac_addr[32] = {0};
    if (NW_GetIfMac("wlan0", mac_addr, sizeof(mac_addr)) < 0 || strlen(mac_addr) == 0) {
        UTIL_ERR("Failed to get MAC address from wlan0\n");
        return -1;
    }
    
    char formatted_mac[16] = {0};
    if (rtmp_format_mac_address(mac_addr, formatted_mac, sizeof(formatted_mac)) < 0) {
        UTIL_ERR("Failed to format MAC address: %s\n", mac_addr);
        return -1;
    }
    
    int ret = snprintf(url_out, url_size, "rtmp://video11-center.iuoooo.com/openlive/%s_1", formatted_mac);
    if (ret < 0 || ret >= url_size) {
        UTIL_ERR("Failed to generate RTMP URL, buffer too small\n");
        return -1;
    }
    
    UTIL_INFO("Generated RTMP URL from MAC: %s -> %s\n", mac_addr, url_out);
    return 0;
}

static int rtmp_connect_server(rtmp_context_t *ctx)
{
    if (ctx->config.dump_only) {
        return 0;
    }

    if (ctx->rtmp) {
        rtmp_disconnect(ctx);
    }

    ctx->rtmp = RTMP_Alloc();
    if (!ctx->rtmp) {
        UTIL_ERR("RTMP alloc failed\n");
        return -1;
    }

    RTMP_Init(ctx->rtmp);
    ctx->rtmp->Link.timeout = ctx->config.connect_timeout > 0 ? ctx->config.connect_timeout : RTMP_CONNECT_TIMEOUT_DEF;
	// 根据MAC地址自动生成
    if (access("/mnt/sd_card/rtmp_debug", F_OK) == 0) {
		strncpy(ctx->config.rtmp_url, RTMP_URL, sizeof(ctx->config.rtmp_url) - 1);
	} else {
        if (rtmp_generate_url_from_mac(ctx->config.rtmp_url, sizeof(ctx->config.rtmp_url)) < 0) {
            UTIL_ERR("Failed to auto-generate RTMP URL from MAC address\n");
            // 如果自动生成失败，继续使用空URL，让用户稍后设置
        } else {
            UTIL_INFO("Auto-generated RTMP URL: %s\n", ctx->config.rtmp_url);
        }
    }
    if (!RTMP_SetupURL(ctx->rtmp, ctx->config.rtmp_url)) {
        UTIL_ERR("RTMP_SetupURL failed url=%s\n", ctx->config.rtmp_url);
        rtmp_disconnect(ctx);
        return -1;
    }

    RTMP_EnableWrite(ctx->rtmp);

    rtmp_set_status(ctx, RTMP_STATUS_CONNECTING);
    if (!RTMP_Connect(ctx->rtmp, NULL)) {
        UTIL_ERR("RTMP_Connect failed url=%s\n", ctx->config.rtmp_url);
        rtmp_disconnect(ctx);
        return -1;
    }

    if (!RTMP_ConnectStream(ctx->rtmp, 0)) {
        UTIL_ERR("RTMP_ConnectStream failed\n");
        rtmp_disconnect(ctx);
        return -1;
    }

    rtmp_set_status(ctx, RTMP_STATUS_CONNECTED);
    if (rtmp_send_metadata_stream(ctx) < 0) {//个别播放器 需要提前渲染metadata
        UTIL_ERR("RTMP send metadata failed\n");
    }
    ctx->request_idr = 1;
	UTIL_INFO("RTMP URL: %s\n", ctx->config.rtmp_url);
    return 0;
}

static int rtmp_init_video_stream(rtmp_context_t *ctx)
{
    if (ctx->video_shm_id >= 0) {
        return 0;
    }

    if (Jooan_VideoInitID(ctx->config.video_profile_no, &ctx->video_shm_id) < 0) {
        UTIL_ERR("RTMP init video shm failed, profile=%d\n", ctx->config.video_profile_no);
        ctx->video_shm_id = -1;
        return -1;
    }

    ShmVideoHeaderInfo header;
    memset(&header, 0, sizeof(header));
    if (Jooan_VideoGetHeaderInfoWithID(ctx->video_shm_id, (char *)&header) < 0) {
        UTIL_ERR("RTMP get video header failed\n");
        Jooan_VideoRemoveID(ctx->video_shm_id);
        ctx->video_shm_id = -1;
        return -1;
    }

    ctx->video_codec = header.ucCodec;
    if (ctx->video_codec != SHM_ENUM_VIDEO_CODEC_TYPE_H264) {
        UTIL_ERR("RTMP only supports H264, current codec=%d\n", ctx->video_codec);
        Jooan_VideoRemoveID(ctx->video_shm_id);
        ctx->video_shm_id = -1;
        return -1;
    }

    Jooan_VideoRefresh(ctx->video_shm_id, 1);
    ctx->request_idr = 1;
    return 0;
}

static int rtmp_init_audio_stream(rtmp_context_t *ctx)
{
    if (ctx->config.audio_profile_no < 0) {
        return 0;  // 不使用音频
    }

    if (ctx->audio_shm_id >= 0) {
        return 0;
    }
	ctx->audio_shm_id = ctx->video_shm_id;
	ctx->audio_codec = SHM_ENUM_AUDIO_CODEC_TYPE_AAC;//header.ucCodec;
	ctx->audio_sample_rate = 16000;
    /*if (Jooan_AudioInitID(ctx->config.audio_profile_no, &ctx->audio_shm_id) < 0) {
        UTIL_ERR("RTMP init audio shm failed, profile=%d\n", ctx->config.audio_profile_no);
        ctx->audio_shm_id = -1;
        return -1;
    }
	
    ShmAudioHeaderInfo header;
    memset(&header, 0, sizeof(header));
    if (Jooan_AudioGetHeaderInfoWithID(ctx->audio_shm_id, (char *)&header) < 0) {
        UTIL_ERR("RTMP get audio header failed\n");
        //Jooan_AudioRemoveID(ctx->audio_shm_id);
        ctx->audio_shm_id = -1;
        return -1;
    }
	*/
    
    /*if (ctx->audio_codec != SHM_ENUM_AUDIO_CODEC_TYPE_AAC) {
        UTIL_ERR("RTMP only supports AAC audio, current codec=%d\n", ctx->audio_codec);
        //Jooan_AudioRemoveID(ctx->audio_shm_id);
        ctx->audio_shm_id = -1;
        return -1;
    }

    // 根据 ucSamplesPerSec 获取实际采样率
    switch (header.ucSamplesPerSec) {
        case SHM_ENUM_AUDIO_8K_PER_SEC:
            ctx->audio_sample_rate = 8000;
            break;
        case SHM_ENUM_AUDIO_16K_PER_SEC:
            ctx->audio_sample_rate = 16000;
            break;
        case SHM_ENUM_AUDIO_44K_PER_SEC:
            ctx->audio_sample_rate = 44100;
            break;
        case SHM_ENUM_AUDIO_48K_PER_SEC:
            ctx->audio_sample_rate = 48000;
            break;
        default:
            ctx->audio_sample_rate = 16000;  // 默认16kHz
            break;
    }
	*/
    // AAC帧大小通常是1024个采样点（参考joa_audio.c中的u32PeriodSize = 1024）
    ctx->audio_frame_size = 1024;
    ctx->audio_frame_count = 0;

    UTIL_INFO("RTMP audio stream initialized, codec=%d, sample_rate=%d, frame_size=%d\n", 
             ctx->audio_codec, ctx->audio_sample_rate, ctx->audio_frame_size);
    return 0;
}

static uint64_t rtmp_get_timestamp_ms(const ShmVideoExtraData *extra)
{
    if (!extra) {
        return 0;
    }
    uint64_t ts = extra->now_timestamp;
    if (ts == 0) {
        ts = extra->ullTimeStamp;
    }
    return ts;
}

static uint64_t rtmp_get_system_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
}

static uint32_t rtmp_frame_interval_ms(uint8_t framerate)
{
    uint32_t fps = framerate;
    if (fps == 0) {
        fps = 15;
    }
    uint32_t interval = fps ? (1000U / fps) : 66U;
    if (interval == 0) {
        interval = 1;
    }
    return interval;
}

static uint32_t rtmp_next_video_timestamp(rtmp_context_t *ctx, const ShmVideoExtraData *extra)
{
    uint64_t sys_now = rtmp_get_system_time_ms();
    uint64_t capture_ts = rtmp_get_timestamp_ms(extra);
    uint32_t interval = rtmp_frame_interval_ms(extra ? extra->ucframerate : 0);

    if (!ctx->video_ts_initialized) {
        ctx->video_ts_initialized = 1;
        ctx->total_video_timestamp = 0;
        ctx->last_sys_timestamp = sys_now;
        ctx->last_video_timestamp = capture_ts;
        return 0;
    }

    uint64_t sys_delta = 0;
    if (ctx->last_sys_timestamp > 0 && sys_now >= ctx->last_sys_timestamp) {
        sys_delta = sys_now - ctx->last_sys_timestamp;
    }
    ctx->last_sys_timestamp = sys_now;
    if (sys_delta == 0 || sys_delta >= 1000) {
        sys_delta = interval;
    }

    uint64_t video_delta = 0;
    if (capture_ts > 0 && ctx->last_video_timestamp > 0 && capture_ts >= ctx->last_video_timestamp) {
        video_delta = capture_ts - ctx->last_video_timestamp;
    }
    if (capture_ts > 0) {
        ctx->last_video_timestamp = capture_ts;
    }
    if (video_delta == 0 || video_delta >= 1000) {
        video_delta = interval;
    }

    ctx->total_video_timestamp += (sys_delta + video_delta) / 2;
    // RTMP时间戳溢出处理：使用回绕而不是截断，确保时间戳可以继续递增
    // RTMP时间戳是32位无符号整数，最大值是4294967295ms（约49.7天）
    if (ctx->total_video_timestamp > UINT32_MAX) {
        // 回绕处理：取模运算，保持时间戳在32位范围内继续递增
        uint64_t old_ts = ctx->total_video_timestamp;
        ctx->total_video_timestamp = ctx->total_video_timestamp % (UINT32_MAX + 1ULL);
        UTIL_INFO("RTMP video timestamp wrapped: %llu -> %llu\n", old_ts, ctx->total_video_timestamp);
    }
	if(access("/tmp/rtmp", F_OK) == 0)
		UTIL_INFO("video send ts: video ctx->total_video_timestamp:%llu\n", ctx->total_video_timestamp);
	
    return (uint32_t)ctx->total_video_timestamp;
}

static void* rtmp_push_thread(void *arg)
{
    UTIL_INFO("rtmp_push_thread start\n");
    rtmp_context_t *ctx = (rtmp_context_t *)arg;
    prctl(PR_SET_NAME, RTMP_THREAD_NAME);
    ShmVideoExtraData extra;
    //ShmVideoExtraData audio_extra;
    
	 if (rtmp_init_video_stream(ctx) < 0) {
        usleep(200 * 1000);
        //continue;
    }
    if (rtmp_init_audio_stream(ctx) < 0) {
        UTIL_ERR("RTMP init audio stream failed, continue without audio\n");
    }
    while (!ctx->stop) {
		
		if (NW_Get_Network_Status() <= ENUM_NETWORK_STATE_CONNECTED) {
	  		usleep(10*1000);
			continue;
      	}
		if(!g_push_start) {
			if(ctx->rtmp) {
				rtmp_disconnect(ctx);
	           	rtmp_set_status(ctx, RTMP_STATUS_IDLE);
			}
			usleep(500*1000);
			continue;
		}
        if (!ctx->config.dump_only) {
            if (!ctx->rtmp || !RTMP_IsConnected(ctx->rtmp)) {
                if (rtmp_connect_server(ctx) < 0) {
                    ctx->reconnect_attempts++;
                    if (ctx->config.max_reconnect_times >= 0 &&
                        ctx->reconnect_attempts > ctx->config.max_reconnect_times) {
                        UTIL_ERR("RTMP reach max reconnect times\n");
                        rtmp_set_status(ctx, RTMP_STATUS_ERROR);
                        break;
                    }
                    sleep(ctx->config.reconnect_interval > 0 ? ctx->config.reconnect_interval : RTMP_RETRY_INTERVAL_DEF);
                    UTIL_INFO("RTMP sleep connect ctx->config.reconnect_interval:%d\n", ctx->config.reconnect_interval);
                    continue;
                }
                ctx->reconnect_attempts = 0;
            }
        }

        // 循环读取帧，直到没有新帧或遇到错误
        int frame_processed = 0;
        int consecutive_errors = 0;
        const int MAX_CONSECUTIVE_ERRORS = 10;
        
        while (!ctx->stop && consecutive_errors < MAX_CONSECUTIVE_ERRORS) {
            int frame_len = ctx->video_buf_size;
            memset(&extra, 0, sizeof(extra));
            int ret = Jooan_VideoReadFrameWithExtras(ctx->video_shm_id, (char *)ctx->video_buf, &frame_len, (char *)&extra);
            
            if (ret >= SHM_ENUM_GET_A_NEW_FRAME && frame_len > 0) {
                consecutive_errors = 0;
                frame_processed = 1;
                
                // 处理视频帧
                if(extra.ucKeyFrameFlag == SHM_ENUM_VIDEO_FRAME_TYPE_KEY || 
                    extra.ucKeyFrameFlag == SHM_ENUM_VIDEO_FRAME_TYPE_NORMAL) {
                    unsigned long long now_time = rtmp_get_system_time_ms();
                    int  intervel = now_time - extra.now_timestamp;
					if(access("/tmp/rtmp", F_OK) == 0)
	                    UTIL_INFO("ljb after:video: video now_timestamp:%llu, xtra.now_timestamp:%llu, extra.ullTimeStamp:%llu, interval:%d\n", 
                        	now_time, extra.now_timestamp, extra.ullTimeStamp, intervel);
					
                    int keyframe = (extra.ucKeyFrameFlag == SHM_ENUM_VIDEO_FRAME_TYPE_KEY);
                    
                    if (keyframe) {
                        int sps_len = h264_get_sps(ctx->video_buf, frame_len, ctx->sps, sizeof(ctx->sps));
                        int pps_len = h264_get_pps(ctx->video_buf, frame_len, ctx->pps, sizeof(ctx->pps));
                        if (sps_len > 0 && pps_len > 0) {
                            ctx->sps_len = sps_len;
                            ctx->pps_len = pps_len;
                        } else {
                            ctx->sps_len = 0;
                            ctx->pps_len = 0;
                            UTIL_ERR("failed to parse SPS/PPS from key frame (sps=%d, pps=%d)\n", sps_len, pps_len);
                        }
                    }
                    
                    if (!ctx->sent_config) {
                        if (ctx->sps_len > 0 && ctx->pps_len > 0) {
                            if (rtmp_send_avc_config(ctx, 0) == 0) {
                                ctx->sent_config = 1;
                                UTIL_INFO("RTMP sent AVC config (SPS %d bytes, PPS %d bytes)\n", ctx->sps_len, ctx->pps_len);
                            } else {
                                UTIL_ERR("RTMP send AVC config failed\n");
                                rtmp_disconnect(ctx);
                                break;
                            }
                        } else {
                            if (ctx->request_idr) {
                                UTIL_INFO("rtmp need ikey\n");
                                Jooan_VideoRefresh(ctx->video_shm_id, 1);
                                ctx->request_idr = 0;
                            }
                            break;  // 等待下一帧
                        }
                    }

                    if (!ctx->video_started) {
                        if (!keyframe) {
                            if (ctx->request_idr) {
                                UTIL_INFO("waiting first keyframe, force refresh\n");
                                Jooan_VideoRefresh(ctx->video_shm_id, 1);
                                ctx->request_idr = 0;
                            }
                            break;  // 等待关键帧
                        }
                        ctx->video_started = 1;
                        ctx->total_video_timestamp = 0;
                        ctx->last_sys_timestamp = 0;
                        ctx->last_video_timestamp = 0;
                        ctx->video_ts_initialized = 0;
                        ctx->request_idr = 1;
                    }

                    if (keyframe) {
                        ctx->request_idr = 1;
                    }

                    uint32_t timestamp = rtmp_next_video_timestamp(ctx, &extra);
                    if (rtmp_send_h264_frame(ctx, ctx->video_buf, frame_len, keyframe, timestamp) < 0) {//发送视频帧 h264编码
                        UTIL_ERR("RTMP send video frame failed, reconnecting\n");
                        if (!ctx->config.dump_only) {
                            rtmp_disconnect(ctx);
                            break;
                        }
                    }
                    rtmp_set_status(ctx, RTMP_STATUS_PUSHING);
                }  
                // 处理音频帧
                #if 0
                else if(extra.ucKeyFrameFlag == SHM_ENUM_AAC_FRAME_TYPE_KEY) {
                    if (!ctx->audio_config_sent) {
                        // 发送AAC配置，时间戳为0
                        if (rtmp_send_aac_config(ctx, 0) == 0) {
                            ctx->audio_config_sent = 1;
                            ctx->audio_frame_count = 0;
                            UTIL_INFO("RTMP sent AAC config\n");
                        }
                    }
                    
                    if (ctx->audio_config_sent) {
                        // 根据采样率和帧大小计算音频时间戳（毫秒）
                        // 时间戳 = (帧数 * 帧大小 / 采样率) * 1000
                        uint64_t audio_ts_64 = (ctx->audio_frame_count * ctx->audio_frame_size * 1000ULL) / ctx->audio_sample_rate;
                        uint32_t audio_timestamp;
                        
                        // RTMP时间戳溢出处理：使用回绕而不是截断
                        // RTMP时间戳是32位无符号整数，最大值是4294967295ms（约49.7天）
                        if (audio_ts_64 > UINT32_MAX) {
                            // 回绕处理：取模运算，保持时间戳在32位范围内继续递增
                            audio_timestamp = (uint32_t)(audio_ts_64 % (UINT32_MAX + 1ULL));
                            UTIL_INFO("RTMP audio timestamp wrapped: %llu -> %u\n", audio_ts_64, audio_timestamp);
                        } else {
                            audio_timestamp = (uint32_t)audio_ts_64;
                        }
                        
                        ctx->audio_frame_count++;
						if(access("/tmp/rtmp", F_OK) == 0)
	                   		UTIL_INFO("ljb audio send ts:%llu\n", audio_timestamp);
						
                        if (rtmp_send_aac_frame(ctx, ctx->video_buf, frame_len, audio_timestamp) < 0) {
                            UTIL_ERR("RTMP send audio frame failed\n");
                        }
                    }
                }
				#endif
                // 继续读取下一帧
                continue;
            } else if (ret == SHM_ENUM_NO_NEW_FRAME || ret == SHM_ENUM_NO_FRAME) {
                // 没有新帧，退出循环
                break;
            } else if (ret == SHM_ENUM_FRAME_TOO_LARGE) {
                UTIL_ERR("RTMP frame too large: %d, buffer=%d\n", frame_len, ctx->video_buf_size);
                Jooan_VideoRefresh(ctx->video_shm_id, 1);
                consecutive_errors++;
                usleep(10 * 1000);
                break;
            } else {
                // 其他错误
                consecutive_errors++;
                if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                    UTIL_ERR("RTMP read frame failed too many times, ret=%d\n", ret);
                }
                break;
            }
        }
        
        // 如果没有处理任何帧，适当休眠避免CPU占用过高
        if (!frame_processed) {
            usleep(10 * 1000); 
        }
    }

    if (ctx->video_shm_id >= 0) {
        UTIL_INFO("Jooan_VideoRemoveID\n");
        Jooan_VideoRemoveID(ctx->video_shm_id);
        ctx->video_shm_id = -1;
    }
    UTIL_INFO("rtmp_push_thread exit\n");
    rtmp_disconnect(ctx);
    rtmp_set_status(ctx, RTMP_STATUS_IDLE);
    return NULL;
}

rtmp_handle_t rtmp_init(const rtmp_config_t *config)
{
    if (!config) {
        return NULL;
    }

    rtmp_context_t *ctx = (rtmp_context_t *)calloc(1, sizeof(rtmp_context_t));
    if (!ctx) {
        return NULL;
    }

    memcpy(&ctx->config, config, sizeof(rtmp_config_t));
    if (ctx->config.video_profile_no < 0) {
        ctx->config.video_profile_no = SHM_ENUM_VIDEO_STREAM0_PROFILE;
    }
    if (ctx->config.reconnect_interval <= 0) {
        ctx->config.reconnect_interval = RTMP_RETRY_INTERVAL_DEF;
    }
    if (ctx->config.max_reconnect_times == 0) {
        ctx->config.max_reconnect_times = -1;
    }
    if (ctx->config.connect_timeout <= 0) {
        ctx->config.connect_timeout = RTMP_CONNECT_TIMEOUT_DEF;
    }

    pthread_mutex_init(&ctx->lock, NULL);
    ctx->status = RTMP_STATUS_IDLE;
    ctx->video_shm_id = -1;
    ctx->video_codec = -1;
    ctx->audio_shm_id = -1;
    ctx->audio_codec = -1;
    ctx->audio_buf = NULL;
    ctx->audio_buf_size = 0;
    ctx->audio_config_sent = 0;
    ctx->audio_sample_rate = 0;
    ctx->audio_frame_size = 0;
    ctx->audio_frame_count = 0;
    ctx->sps_len = 0;
    ctx->pps_len = 0;
    ctx->metadata_stream_sent = 0;
    ctx->metadata_dump_written = 0;
    ctx->request_idr = 1;
    ctx->video_started = 0;

    if (rtmp_prepare_buffers(ctx) < 0) {
        pthread_mutex_destroy(&ctx->lock);
        free(ctx);
        UTIL_INFO("rtmp_init error\n");
        return NULL;
    }

    if (ctx->config.dump_enable) {
        const char *path = ctx->config.dump_path[0] ? ctx->config.dump_path : "/mnt/sd_card/rtmp_dump.flv";
        ctx->dump_fp = fopen(path, "wb");
        if (!ctx->dump_fp) {
            UTIL_ERR("open dump file %s failed: %s\n", path, strerror(errno));
            // 如果文件打开失败，禁用 dump 功能
            ctx->config.dump_enable = 0;
        } else {
            ctx->dump_header_written = 0;
            UTIL_INFO("rtmp dump to %s (dump_only=%d)\n", path, ctx->config.dump_only);
        }
    }

    UTIL_INFO("rtmp_init success\n");
    return ctx;
}

int rtmp_start(rtmp_handle_t handle)
{
    rtmp_context_t *ctx = (rtmp_context_t *)handle;
    if (!ctx) {
        return -1;
    }

    if (ctx->thread_started) {
        return 0;
    }

    ctx->stop = 0;
    UTIL_INFO("rtmp_start thread\n");
    ctx->thread_started = (pthread_create(&ctx->thread, NULL, rtmp_push_thread, ctx) == 0);
    if (!ctx->thread_started) {
        UTIL_ERR("RTMP start thread failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

void rtmp_stop(rtmp_handle_t handle)
{
    rtmp_context_t *ctx = (rtmp_context_t *)handle;
    if (!ctx || !ctx->thread_started) {
        return;
    }

    ctx->stop = 1;
    pthread_join(ctx->thread, NULL);
    ctx->thread_started = 0;
}

void rtmp_destroy(rtmp_handle_t handle)
{
    rtmp_context_t *ctx = (rtmp_context_t *)handle;
    if (!ctx) {
        return;
    }

    rtmp_stop(handle);
    if (ctx->video_shm_id >= 0) {
        Jooan_VideoRemoveID(ctx->video_shm_id);
        ctx->video_shm_id = -1;
    }
    if (ctx->audio_shm_id >= 0) {
        Jooan_AudioRemoveID(ctx->audio_shm_id);
        ctx->audio_shm_id = -1;
    }
    rtmp_disconnect(ctx);

    free(ctx->video_buf);
    free(ctx->flv_buf);
    if (ctx->audio_buf) {
        free(ctx->audio_buf);
        ctx->audio_buf = NULL;
    }
    if (ctx->dump_fp) {
        fflush(ctx->dump_fp);
        fclose(ctx->dump_fp);
        ctx->dump_fp = NULL;
    }
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

int rtmp_set_url(rtmp_handle_t handle, const char *url)
{
    rtmp_context_t *ctx = (rtmp_context_t *)handle;
    if (!ctx || !url) {
        return -1;
    }

    pthread_mutex_lock(&ctx->lock);
    strncpy(ctx->config.rtmp_url, url, sizeof(ctx->config.rtmp_url) - 1);
    ctx->config.rtmp_url[sizeof(ctx->config.rtmp_url) - 1] = '\0';
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

rtmp_status_t rtmp_get_status(rtmp_handle_t handle)
{
    rtmp_context_t *ctx = (rtmp_context_t *)handle;
    if (!ctx) {
        return RTMP_STATUS_ERROR;
    }

    pthread_mutex_lock(&ctx->lock);
    rtmp_status_t status = ctx->status;
    pthread_mutex_unlock(&ctx->lock);
    return status;
}

int joa_rtmp_start(const rtmp_config_t *config)
{
    if (g_rtmp_ctx) {
        UTIL_INFO("RTMP already started\n");
        return 0;
    }

    rtmp_handle_t handle = rtmp_init(config);
    if (!handle) {
        UTIL_ERR("RTMP init failed\n");
        return -1;
    }
    UTIL_INFO("joa_rtmp_start...\n");
    if (rtmp_start(handle) < 0) {
        UTIL_ERR("joa_rtmp_start err\n");
        rtmp_destroy(handle);
        return -1;
    }

    g_rtmp_ctx = (rtmp_context_t *)handle;
    UTIL_INFO("RTMP push started\n");
    return 0;
}

void joa_rtmp_stop(void)
{
    if (!g_rtmp_ctx) {
        return;
    }

    rtmp_destroy(g_rtmp_ctx);
    g_rtmp_ctx = NULL;
    UTIL_INFO("RTMP push stopped\n");
}

rtmp_status_t joa_rtmp_status(void)
{
    if (!g_rtmp_ctx) {
        return RTMP_STATUS_IDLE;
    }
    return rtmp_get_status(g_rtmp_ctx);
}

int rtmp_push_set(int push_start)
{
	g_push_start = push_start;
	UTIL_INFO("RTMP push enable:%d\n", g_push_start);
	return 0;
}
int rtmp_test(void)
{
    rtmp_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.rtmp_url, "rtmp://192.168.124.106/live/livestream", sizeof(cfg.rtmp_url) - 1);
    cfg.video_profile_no = SHM_ENUM_VIDEO_STREAM0_PROFILE;
    cfg.reconnect_interval = 5;
    cfg.max_reconnect_times = -1;
    cfg.connect_timeout = 5;
    cfg.video_buffer_size = 0;

    return joa_rtmp_start(&cfg);
}

