# RTMP 代码学习摘录

这份文档把 RTMP 当作一层独立模块来学：它不是 FLV 本身，但它会承载 FLV 风格的音视频消息。

当前参考代码来自：

- `D:\wechat_company\WXWork\1688857297759642\Cache\File\2025-12\joa_rtmp.c`
- `D:\wechat_company\WXWork\1688857297759642\Cache\File\2025-12\joa_rtmp.h`

## 1. 先看边界

RTMP 这一层关心的不是“帧里是什么编码细节”，而是：

```text
1. 怎么连上服务器
2. 怎么发连接/发布消息
3. 怎么把视频和音频消息送出去
4. 怎么控制时间戳、重连、状态机
```

视频和音频的真正编码格式仍然是下层内容：

```text
视频：H.264 / H.265 等
音频：AAC 等
```

但当前这份代码实际只支持：

```text
视频：H.264
音频：AAC
```

## 2. 公开接口

```c
typedef enum {
    RTMP_STATUS_IDLE = 0,
    RTMP_STATUS_CONNECTING,
    RTMP_STATUS_CONNECTED,
    RTMP_STATUS_PUSHING,
    RTMP_STATUS_DISCONNECTED,
    RTMP_STATUS_ERROR
} rtmp_status_t;

typedef struct {
    char rtmp_url[256];
    int video_profile_no;
    int audio_profile_no;
    int reconnect_interval;
    int max_reconnect_times;
    int connect_timeout;
    int video_buffer_size;
    int audio_buffer_size;
    int dump_enable;
    int dump_only;
    char dump_path[256];
} rtmp_config_t;

typedef struct rtmp_context_s* rtmp_handle_t;

rtmp_handle_t rtmp_init(const rtmp_config_t *config);
int rtmp_start(rtmp_handle_t handle);
void rtmp_stop(rtmp_handle_t handle);
void rtmp_destroy(rtmp_handle_t handle);
int rtmp_set_url(rtmp_handle_t handle, const char *url);
rtmp_status_t rtmp_get_status(rtmp_handle_t handle);
```

学习时先抓住一句话：

```text
RTMP 模块 = 连接管理 + 消息发送 + 推流状态机 + 可选本地 FLV dump
```

## 3. RTMP 头部和 dump 写法

### 3.1 FLV dump header

这份代码会把 RTMP 发送内容同时 dump 成一个 FLV 文件，便于调试。

```c
static int rtmp_dump_write_header(rtmp_context_t *ctx)
{
    if (!ctx->dump_fp || ctx->dump_header_written) {
        return 0;
    }
    unsigned char flags = 0x01;
    if (ctx->config.audio_profile_no >= 0) {
        flags = 0x05;
    }
    unsigned char header[13] = {
        0x46, 0x4C, 0x56, 0x01, flags,
        0x00, 0x00, 0x00, 0x09,
        0x00, 0x00, 0x00, 0x00
    };
    ...
}
```

这里的重点不是 FLV 格式本身，而是：

```text
RTMP 发送的数据流，可以被顺手落成 FLV 文件做对照。
```

### 3.2 FLV Tag 写法

```c
static int rtmp_dump_write_tag(rtmp_context_t *ctx, uint8_t tag_type,
                               const unsigned char *data, int size, uint32_t timestamp)
{
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
    ...
    uint32_t prev_size = size + sizeof(tag_header);
    ...
}
```

这说明 dump 出来的内容本质上就是：

```text
FLV Header + PreviousTagSize + Tag Header + Tag Payload + PreviousTagSize
```

### 3.2.1 `dump_enable` 和 `dump_only`

这两个配置决定 dump 路径是否开启，以及是否只落盘不发网：

```c
typedef struct {
    ...
    int dump_enable;
    int dump_only;
    char dump_path[256];
} rtmp_config_t;
```

在初始化时：

```c
if (ctx->config.dump_enable) {
    const char *path = ctx->config.dump_path[0] ? ctx->config.dump_path : "/mnt/sd_card/rtmp_dump.flv";
    ctx->dump_fp = fopen(path, "wb");
    if (!ctx->dump_fp) {
        ctx->config.dump_enable = 0;
    } else {
        ctx->dump_header_written = 0;
    }
}
```

这说明：

```text
dump_enable = 1  -> 尝试打开本地 FLV 文件
dump_only = 1    -> 只写本地文件，不发送 RTMP
```

### 3.2.2 `rtmp_output_video_tag()` / `rtmp_output_audio_tag()`

这两个函数把“发网”和“落盘”统一起来：

```c
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
```

音频对应函数结构一样：

```c
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
```

这两段是整个 dump 路径真正的出口。没有它们，前面的 `rtmp_dump_write_*` 只是局部工具函数。

### 3.3 `rtmp_write_metadata_dump()`：把 metadata 先写进 dump 文件

```c
static int rtmp_write_metadata_dump(rtmp_context_t *ctx)
{
    if (ctx->metadata_dump_written || !ctx->dump_fp) {
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
```

这一段的作用很直接：

```text
1. 第一次 dump 时，先写 onMetaData。
2. 后面的音视频 Tag 再继续追加。
3. 避免 metadata 重复写多次。
```

它和前面的 `rtmp_send_metadata_stream()` 很像，只是一个发 RTMP 包，一个写 FLV dump 文件。

你可以把这两个函数对照着看：

```text
rtmp_send_metadata_stream()  -> 发到网络里的 RTMP metadata
rtmp_write_metadata_dump()   -> 落到本地 FLV 文件里的 metadata
```

### 3.4 `librtmp` 角色说明

这份代码使用的就是 `rtmpdump` 体系里的 `librtmp` 风格接口。最直接的判断依据是这些 API：

```c
RTMP_Alloc
RTMP_Init
RTMP_SetupURL
RTMP_EnableWrite
RTMP_Connect
RTMP_ConnectStream
RTMP_SendPacket
RTMPPacket_Alloc
RTMPPacket_Reset
RTMPPacket_Free
RTMP_Close
RTMP_Free
```

这些符号不是 FFmpeg 的 RTMP API，而是 `librtmp` 的典型接口。

在这套代码里，`librtmp` 做的事可以概括成：

```text
1. 建立 RTMP 连接。
2. 把业务层构造好的音视频消息发出去。
3. 提供 RTMPPacket 作为消息容器。
```

`RTMPPacket` 的角色很关键，它不是 FLV Tag 本身，而是 RTMP 协议层的 message body 容器。代码会把音视频或 metadata 的 payload 放进 `RTMPPacket`，然后交给 `RTMP_SendPacket()` 发到服务器。

可以这样理解三层关系：

```text
业务帧 / 编码数据
  -> 组装成 FLV 风格 payload
  -> 放进 RTMPPacket
  -> 交给 librtmp 发送
```

如果开启了 `dump_enable`，同一份 payload 还会被写成本地 FLV 文件，用来做对照验证。

## 4. RTMP 连接和发布

### 4.1 建立连接

```c
static int rtmp_connect_server(rtmp_context_t *ctx)
{
    if (ctx->rtmp) {
        rtmp_disconnect(ctx);
    }

    ctx->rtmp = RTMP_Alloc();
    RTMP_Init(ctx->rtmp);
    ctx->rtmp->Link.timeout = ctx->config.connect_timeout > 0 ? ctx->config.connect_timeout : RTMP_CONNECT_TIMEOUT_DEF;

    if (!RTMP_SetupURL(ctx->rtmp, ctx->config.rtmp_url)) {
        ...
    }
    RTMP_EnableWrite(ctx->rtmp);

    if (!RTMP_Connect(ctx->rtmp, NULL)) {
        ...
    }

    if (!RTMP_ConnectStream(ctx->rtmp, 0)) {
        ...
    }

    rtmp_set_status(ctx, RTMP_STATUS_CONNECTED);
    if (rtmp_send_metadata_stream(ctx) < 0) {
        ...
    }
    return 0;
}
```

这里的层次是：

```text
RTMP_Alloc / Init -> SetupURL -> Connect -> ConnectStream -> Send metadata
```

### 4.2 状态机

```c
static void rtmp_set_status(rtmp_context_t *ctx, rtmp_status_t status)
{
    pthread_mutex_lock(&ctx->lock);
    ctx->status = status;
    pthread_mutex_unlock(&ctx->lock);
}
```

状态枚举：

```text
IDLE -> CONNECTING -> CONNECTED -> PUSHING
                     -> DISCONNECTED / ERROR
```

这部分是 RTMP 模块的控制面，不是媒体面。

## 5. metadata 消息

```c
static int rtmp_build_metadata_body(unsigned char *buf, int buf_size)
{
    p = amf_write_string(p, end, "onMetaData");
    *p++ = AMF_ECMA_ARRAY;
    p = amf_write_u32(p, end, 5);

    p = amf_write_string_raw(p, end, "duration");
    p = amf_write_number(p, end, 0.0);
    p = amf_write_string_raw(p, end, "width");
    p = amf_write_number(p, end, 640.0);
    p = amf_write_string_raw(p, end, "height");
    p = amf_write_number(p, end, 360.0);
    p = amf_write_string_raw(p, end, "videocodecid");
    p = amf_write_number(p, end, 7.0);
    p = amf_write_string_raw(p, end, "framerate");
    p = amf_write_number(p, end, 15.0);
    p = amf_write_string_raw(p, end, "audiocodecid");
    p = amf_write_number(p, end, 10.0);
    ...
}
```

这里的关键是 `videocodecid=7` 和 `audiocodecid=10`。这就是标准 FLV/RTMP 常见组合。

```text
videocodecid = 7  -> H.264/AVC
audiocodecid = 10 -> AAC
```

## 6. 音视频发送

### 6.1 发送 RTMP packet

```c
static int rtmp_send_packet(rtmp_context_t *ctx, const unsigned char *data,
                            int size, uint32_t timestamp, uint8_t packet_type)
{
    RTMPPacket packet;
    RTMPPacket_Reset(&packet);
    if (!RTMPPacket_Alloc(&packet, size)) {
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
    RTMPPacket_Free(&packet);
    return ret ? 0 : -1;
}
```

这是 RTMP 的核心发送路径：把已有 payload 作为一个 RTMP message 发出去。

### 6.2 发送 metadata stream

```c
static int rtmp_send_metadata_stream(rtmp_context_t *ctx)
{
    unsigned char body[256];
    int body_size = rtmp_build_metadata_body(body, sizeof(body));
    if (body_size <= 0) {
        return -1;
    }
    return rtmp_send_packet(ctx, body, body_size, 0, RTMP_PACKET_TYPE_INFO);
}
```

### 6.3 发送 H.264 sequence header 和帧

```c
static int rtmp_send_avc_config(rtmp_context_t *ctx, uint32_t timestamp)
{
    body[pos++] = 0x17; // KeyFrame + AVC
    body[pos++] = 0x00; // AVC sequence header
    ...
    return rtmp_output_video_tag(ctx, body, pos, timestamp);
}

static int rtmp_send_h264_frame(rtmp_context_t *ctx, const unsigned char *data,
                                int len, int keyframe, uint32_t timestamp)
{
    ...
    int nal_type = nal[0] & 0x1f;
    if (nal_type == 7 || nal_type == 8) {
        p = next ? next : end;
        continue;
    }
    ...
    body[0] = (keyframe ? 0x17 : 0x27);
    body[1] = 0x01;
    body[2] = 0x00;
    body[3] = 0x00;
    body[4] = 0x00;
    ...
}
```

这说明它把 H.264 裸流转成了 FLV/RTMP 要的 AVC 格式。

### 6.4 发送 AAC sequence header 和 raw frame

```c
static int rtmp_send_aac_config(rtmp_context_t *ctx, uint32_t timestamp)
{
    unsigned char soundFormat = 10;  // AAC
    ...
    body[pos++] = (soundFormat << 4) | (soundRate << 2) | (soundSize << 1) | soundType;
    body[pos++] = 0x00;  // AAC sequence header
    body[pos++] = aac_config[0];
    body[pos++] = aac_config[1];
    ...
}

static int rtmp_send_aac_frame(rtmp_context_t *ctx, const unsigned char *data,
                               int len, uint32_t timestamp)
{
    body[pos++] = (soundFormat << 4) | (soundRate << 2) | (soundSize << 1) | soundType;
    body[pos++] = 0x01;  // AAC raw data
    memcpy(body + pos, data, len);
    ...
}
```

## 7. 视频和音频流初始化

### 7.1 视频只支持 H.264

```c
ctx->video_codec = header.ucCodec;
if (ctx->video_codec != SHM_ENUM_VIDEO_CODEC_TYPE_H264) {
    UTIL_ERR("RTMP only supports H264, current codec=%d\n", ctx->video_codec);
    return -1;
}
```

### 7.2 音频只支持 AAC

```c
ctx->audio_codec = SHM_ENUM_AUDIO_CODEC_TYPE_AAC;
...
if (ctx->audio_codec != SHM_ENUM_AUDIO_CODEC_TYPE_AAC) {
    UTIL_ERR("RTMP only supports AAC audio, current codec=%d\n", ctx->audio_codec);
    return -1;
}
```

这两句把这份代码的定位定得很清楚：

```text
它是标准 RTMP/H.264/AAC 推流样例，不是 HEVC 版本。
```

## 8. 推流线程

```c
static void* rtmp_push_thread(void *arg)
{
    if (rtmp_init_video_stream(ctx) < 0) {
        ...
    }
    if (rtmp_init_audio_stream(ctx) < 0) {
        ...
    }

    while (!ctx->stop) {
        if (!g_push_start) {
            ...
            continue;
        }

        if (!ctx->rtmp || !RTMP_IsConnected(ctx->rtmp)) {
            if (rtmp_connect_server(ctx) < 0) {
                ...
                continue;
            }
        }

        int frame_len = ctx->video_buf_size;
        int ret = Jooan_VideoReadFrameWithExtras(...);
        if (ret >= SHM_ENUM_GET_A_NEW_FRAME && frame_len > 0) {
            if (keyframe && !ctx->sent_config) {
                if (rtmp_send_avc_config(ctx, 0) == 0) {
                    ctx->sent_config = 1;
                }
            }

            uint32_t timestamp = rtmp_next_video_timestamp(ctx, &extra);
            if (rtmp_send_h264_frame(ctx, ctx->video_buf, frame_len, keyframe, timestamp) < 0) {
                ...
            }
        }
    }
    ...
}
```

这部分是运行时控制面最值得看的地方：

```text
没连上 -> 重连
首次关键帧 -> 先发 AVC config
拿到新帧 -> 算 timestamp -> 发视频包
```

## 9. 学习顺序建议

1. 先看 `rtmp_config_t` 和 `rtmp_status_t`。
2. 再看 `rtmp_connect_server()`，理解连接流程。
3. 再看 `rtmp_build_metadata_body()`，理解 metadata。
4. 再看 `rtmp_send_avc_config()` / `rtmp_send_h264_frame()`。
5. 最后看 `rtmp_push_thread()`，把整个状态机串起来。
