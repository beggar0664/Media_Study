#include "gb28181_module.h"

/*
 * 最小 GB28181 SIP mock 平台。
 *
 * 对应文档：
 * - ../gb28181_study.md：REGISTER Digest、MESSAGE、INVITE/SDP 的平台侧响应
 * - ../../current_code_learning_guide.md：如何先启动 mock server 再运行 client
 *
 * 这个程序只用于学习信令闭环：收到 REGISTER/MESSAGE/INVITE/ACK/BYE 后返回最小响应。
 * 同时监听 udp/30000，打印最小 RTP 头和 payload 起始字节，便于把信令和媒体对上。
 */

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int init_winsock(void)
{
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    return 0;
#endif
}

static void cleanup_winsock(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

static int send_reply(int sockfd, const struct sockaddr_in *peer, const char *msg)
{
    int ret = sendto(sockfd, msg, (int)strlen(msg), 0, (const struct sockaddr *)peer, sizeof(*peer));
    if (ret < 0) {
        perror("sendto");
        return -1;
    }
    return 0;
}

static int socket_close(int sockfd)
{
#ifdef _WIN32
    return closesocket(sockfd);
#else
    return close(sockfd);
#endif
}

static int bind_udp_socket(int port)
{
    int sockfd;
    struct sockaddr_in addr;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        socket_close(sockfd);
        return -1;
    }
    return sockfd;
}

static int find_bytes(const unsigned char *data, int size, const unsigned char *pattern, int pattern_size)
{
    int i;
    if (!data || !pattern || size < pattern_size || pattern_size <= 0) {
        return -1;
    }
    for (i = 0; i <= size - pattern_size; ++i) {
        if (memcmp(data + i, pattern, (size_t)pattern_size) == 0) {
            return i;
        }
    }
    return -1;
}

static int is_video_pes_stream_id(unsigned char stream_id)
{
    return stream_id >= 0xE0 && stream_id <= 0xEF;
}

static int find_video_pes_start(const unsigned char *data, int size, unsigned char *stream_id_out)
{
    int i;

    if (!data || size < 4) {
        return -1;
    }

    for (i = 0; i + 3 < size; ++i) {
        unsigned char stream_id;

        if (data[i] != 0x00 || data[i + 1] != 0x00 || data[i + 2] != 0x01) {
            continue;
        }

        stream_id = data[i + 3];
        if (is_video_pes_stream_id(stream_id)) {
            if (stream_id_out) {
                *stream_id_out = stream_id;
            }
            return i;
        }
    }

    return -1;
}

/*
 * 重组完成回调：状态机在末片到达、拼出一个完整 NALU 后调用。
 * nalu / nalu_size 是重组后的裸 NALU；timestamp / ssrc 取自当前 RTP 上下文。
 * user_data 由调用方在注册回调时传入，状态机只原样回传。
 */
typedef void (*fu_a_nalu_output_cb)(const unsigned char *nalu,
                                   int nalu_size,
                                   uint32_t timestamp,
                                   unsigned int ssrc,
                                   void *user_data);

#define FU_A_REORDER_WINDOW_SIZE 8
#define FU_A_REASSEMBLY_TIMEOUT_MS 2000

static unsigned long long fu_a_now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)(ts.tv_nsec / 1000000);
#endif
}

typedef struct {
    int active;
    uint32_t timestamp;
    unsigned int ssrc;
    unsigned int expected_seq;
    unsigned int pending_fragments;
    unsigned char nalu_header;
    unsigned char nalu_header1;   /* H.265 第二字节头（is_h265=1 时有效） */
    int is_h265;                  /* 1=H.265 FU 重组（2字节头），0=H.264 FU-A */
    unsigned char buffer[4096];
    int length;
    fu_a_nalu_output_cb output_cb;
    void *output_user_data;
    /*
     * 乱序重排序窗口：期望序号未到、但后续包先到时，先暂存到 slot，
     * 等期望包补齐后按序追加；窗口满仍未补齐才丢弃当前 NALU。
     * 每个 slot 独立保存 seq、used、data、len、fu_end，避免数据指针失效。
     */
    unsigned int reorder_seq[FU_A_REORDER_WINDOW_SIZE];
    unsigned char reorder_used[FU_A_REORDER_WINDOW_SIZE];
    unsigned char reorder_data[FU_A_REORDER_WINDOW_SIZE][1500];
    int reorder_len[FU_A_REORDER_WINDOW_SIZE];
    unsigned int reorder_fu_end[FU_A_REORDER_WINDOW_SIZE];
    unsigned long long start_tick;
} h264_fu_a_reassembly_t;

static void fu_a_reassembly_reset(h264_fu_a_reassembly_t *ctx)
{
    if (!ctx) {
        return;
    }
    ctx->active = 0;
    ctx->timestamp = 0;
    ctx->ssrc = 0;
    ctx->expected_seq = 0;
    ctx->pending_fragments = 0;
    ctx->nalu_header = 0;
    ctx->nalu_header1 = 0;
    ctx->is_h265 = 0;
    ctx->length = 0;
    {
        int i;
        for (i = 0; i < FU_A_REORDER_WINDOW_SIZE; ++i) {
            ctx->reorder_used[i] = 0;
            ctx->reorder_len[i] = 0;
            ctx->reorder_seq[i] = 0;
            ctx->reorder_fu_end[i] = 0;
        }
    }
    ctx->start_tick = 0;
}

static void fu_a_reassembly_print_nalu(const h264_fu_a_reassembly_t *ctx)
{
    int i;
    int dump_len;

    if (!ctx || ctx->length <= 0) {
        return;
    }

    dump_len = ctx->length < 16 ? ctx->length : 16;
    printf("FU-A reassembled NALU: len=%d header=0x%02X timestamp=%u ssrc=0x%08X head:",
           ctx->length,
           ctx->nalu_header,
           ctx->timestamp,
           ctx->ssrc);
    for (i = 0; i < dump_len; ++i) {
        printf(" %02X", ctx->buffer[i]);
    }
    printf("\n");
}

/*
 * H.264 FU-A 重组入口。
 *
 * 这里处理的是单个 RTP payload，不是完整 NALU：
 *   - payload[0] 是 FU indicator
 *   - payload[1] 是 FU header，包含 S/E 标志和原始 NALU type
 *   - payload[2...] 是实际片段数据
 *
 * 当前实现只做最小学习闭环：按 timestamp + SSRC 把分片重新拼成原始 NALU，
 * 再按 seq 检查分片是否连续，并在末片到达时打印结果。
 * 它不负责解码；发现丢中间片（超出重排序窗口）或丢末片时直接丢弃当前 NALU。
 *
 * 乱序重排序：落在窗口内的后续包先暂存到 reorder slot，期望包到达后按序刷出。
 * 陈旧上下文清理：如果已经 active 但迟迟收不到末片，墙钟超时或 pending_fragments
 * 超限就判定丢末片并丢弃当前不完整 NALU，避免上一组残留状态一直挂到下一组首片才被覆盖。
 */
#define FU_A_REASSEMBLY_MAX_FRAGMENTS 64
/*
 * 把乱序到达的 FU-A 分片暂存到重排序窗口。
 * 返回 0 表示暂存成功；返回 -1 表示窗口已满或 slot 数据过大，应丢弃当前 NALU。
 */
static int fu_a_reorder_push(h264_fu_a_reassembly_t *ctx,
                             unsigned int seq,
                             const unsigned char *payload,
                             int payload_size,
                             unsigned int fu_end)
{
    int i;

    if (!ctx) {
        return -1;
    }

    for (i = 0; i < FU_A_REORDER_WINDOW_SIZE; ++i) {
        if (!ctx->reorder_used[i]) {
            int header_size = ctx->is_h265 ? 3 : 2;
            int data_len = payload_size > header_size ? payload_size - header_size : 0;
            if (data_len > (int)sizeof(ctx->reorder_data[i])) {
                return -1;
            }
            ctx->reorder_used[i] = 1;
            ctx->reorder_seq[i] = seq;
            ctx->reorder_len[i] = data_len;
            ctx->reorder_fu_end[i] = fu_end;
            if (data_len > 0) {
                int header_size = ctx->is_h265 ? 3 : 2;
                memcpy(ctx->reorder_data[i], payload + header_size, (size_t)data_len);
            }
            return 0;
        }
    }

    return -1;
}

/*
 * 刷出窗口里连续跟随在 expected_seq 后面的暂存包。
 * 返回 1 表示刷出了末片（当前 NALU 已完成）；返回 0 表示仍在继续。
 */
static int fu_a_reorder_drain(h264_fu_a_reassembly_t *ctx)
{
    int i;

    if (!ctx) {
        return 0;
    }

    for (;;) {
        int hit = -1;
        for (i = 0; i < FU_A_REORDER_WINDOW_SIZE; ++i) {
            if (ctx->reorder_used[i] && ctx->reorder_seq[i] == ctx->expected_seq) {
                hit = i;
                break;
            }
        }
        if (hit < 0) {
            return 0;
        }

        if (ctx->reorder_len[hit] > 0) {
            int data_len = ctx->reorder_len[hit];
            if (ctx->length + data_len > (int)sizeof(ctx->buffer)) {
                data_len = (int)sizeof(ctx->buffer) - ctx->length;
            }
            if (data_len > 0) {
                memcpy(ctx->buffer + ctx->length, ctx->reorder_data[hit], (size_t)data_len);
                ctx->length += data_len;
            }
        }
        ctx->expected_seq = (ctx->reorder_seq[hit] + 1) & 0xFFFF;
        ctx->pending_fragments += 1;
        if (ctx->reorder_fu_end[hit]) {
            ctx->reorder_used[hit] = 0;
            ctx->reorder_len[hit] = 0;
            return 1;
        }
        ctx->reorder_used[hit] = 0;
        ctx->reorder_len[hit] = 0;
    }
}

static void fu_a_reassembly_handle_packet(h264_fu_a_reassembly_t *ctx,
                                          unsigned int seq,
                                          uint32_t timestamp,
                                          uint32_t ssrc,
                                          const unsigned char *payload,
                                          int payload_size,
                                          unsigned int fu_start,
                                          unsigned int fu_end,
                                          unsigned int fu_type,
                                          int is_h265)
{
    unsigned char nalu_header;
    int data_len;

    if (!ctx || !payload || payload_size < 2) {
        return;
    }

    nalu_header = (unsigned char)((payload[0] & 0xE0) | (fu_type & 0x1F));
    if (fu_start) {
        if (ctx->active) {
            printf("FU drop: start fragment overlaps unfinished NALU, expected_seq=%u pending=%u, restarting\n",
                   ctx->expected_seq,
                   ctx->pending_fragments);
        }
        ctx->active = 1;
        ctx->timestamp = timestamp;
        ctx->ssrc = ssrc;
        ctx->expected_seq = (seq + 1) & 0xFFFF;
        ctx->pending_fragments = 1;
        ctx->start_tick = fu_a_now_ms();
        ctx->is_h265 = is_h265;
        ctx->length = 0;
        if (is_h265) {
            /*
             * H.265 重组：重建 2 字节 NALU 头。
             * 原始 payload header 是 2 字节（type=49），重组时把 type 换回 FuType：
             *   byte0 = (payload[0] & 0x81) | (fu_type << 1)
             *   byte1 = payload[1]（layer_id 低位 + temporal 全保留）
             * 跳过 payload 的 2 字节 header + 1 字节 FU header = 3 字节，数据从 [3] 开始。
             */
            unsigned char h0 = (unsigned char)((payload[0] & 0x81) | ((fu_type & 0x3F) << 1));
            unsigned char h1 = payload[1];
            ctx->nalu_header = h0;
            ctx->nalu_header1 = h1;
            if (ctx->length + 1 < (int)sizeof(ctx->buffer)) {
                ctx->buffer[ctx->length++] = h0;
                ctx->buffer[ctx->length++] = h1;
            }
        } else {
            /* H.264 重组：重建 1 字节 NALU 头 = (fu_indicator & 0xE0) | fu_type。 */
            ctx->nalu_header = nalu_header;
            if (ctx->length < (int)sizeof(ctx->buffer)) {
                ctx->buffer[ctx->length++] = nalu_header;
            }
        }
        if (payload_size > 2) {
            int header_size = is_h265 ? 3 : 2;  /* H.265: payload header(2)+FU header(1); H.264: FU ind(1)+FU header(1) */
            data_len = payload_size - header_size;
            if (data_len > 0) {
                if (ctx->length + data_len > (int)sizeof(ctx->buffer)) {
                    data_len = (int)sizeof(ctx->buffer) - ctx->length;
                }
                memcpy(ctx->buffer + ctx->length, payload + header_size, (size_t)data_len);
                ctx->length += data_len;
            }
        }
        printf("FU reassembly start: %s timestamp=%u ssrc=0x%08X header=0x%02X\n",
               is_h265 ? "H.265" : "H.264", ctx->timestamp, ctx->ssrc, ctx->nalu_header);
        if (fu_end) {
            fu_a_reassembly_print_nalu(ctx);
            if (ctx->output_cb) {
                ctx->output_cb(ctx->buffer, ctx->length, ctx->timestamp, ctx->ssrc, ctx->output_user_data);
            }
            fu_a_reassembly_reset(ctx);
        }
        return;
    }

    if (!ctx->active) {
        printf("FU-A drop: missing start fragment seq=%u timestamp=%u ssrc=0x%08X\n", seq, timestamp, ssrc);
        return;
    }

    /* 墙钟超时：首片后超过 FU_A_REASSEMBLY_TIMEOUT_MS 仍未完成，视为丢末片。 */
    if (fu_a_now_ms() - ctx->start_tick > FU_A_REASSEMBLY_TIMEOUT_MS) {
        printf("FU-A drop: reassembly timeout, expected_seq=%u pending=%u, discard incomplete NALU\n",
               ctx->expected_seq,
               ctx->pending_fragments);
        fu_a_reassembly_reset(ctx);
        return;
    }

    /* 丢末片保护：已 active 但片段数超限仍未收到 E=1，视为不完整 NALU 丢弃。 */
    if (ctx->pending_fragments >= FU_A_REASSEMBLY_MAX_FRAGMENTS) {
        printf("FU-A drop: stale reassembly, expected_seq=%u pending=%u, discard incomplete NALU\n",
               ctx->expected_seq,
               ctx->pending_fragments);
        fu_a_reassembly_reset(ctx);
        return;
    }

    if (ctx->timestamp != timestamp || ctx->ssrc != ssrc) {
        printf("FU-A drop: timestamp/ssrc changed before end, seq=%u timestamp=%u ssrc=0x%08X\n", seq, timestamp, ssrc);
        fu_a_reassembly_reset(ctx);
        return;
    }

    /* 重复包去重：seq 等于上一片已处理序号时，判定为重复并静默丢弃，不影响当前 NALU。 */
    if (seq == ((ctx->expected_seq - 1) & 0xFFFF)) {
        printf("FU-A drop: duplicate fragment seq=%u, ignored\n", seq);
        return;
    }

    if (seq != ctx->expected_seq) {
        /*
         * 乱序重排序：如果 seq 落在 expected_seq 之后的小窗口内，
         * 暂存到 reorder slot，等期望包补齐后按序追加。
         * 窗口满或 slot 数据过大才丢弃当前 NALU。
         */
        int fwd = ((int)seq - (int)ctx->expected_seq) & 0xFFFF;
        if (fwd > 0 && fwd <= FU_A_REORDER_WINDOW_SIZE) {
            if (fu_a_reorder_push(ctx, seq, payload, payload_size, fu_end) == 0) {
                printf("FU-A ooo: expected seq=%u but got seq=%u (ahead %d), buffered\n",
                       ctx->expected_seq, seq, fwd);
                return;
            }
            printf("FU-A drop: reorder window full, expected_seq=%u got seq=%u, discard incomplete NALU\n",
                   ctx->expected_seq, seq);
        } else {
            printf("FU-A drop: expected seq=%u but got seq=%u, discard incomplete NALU\n", ctx->expected_seq, seq);
        }
        fu_a_reassembly_reset(ctx);
        return;
    }

    /* 期望包到达：先追加当前分片，再刷出窗口里连续跟随的暂存包。 */
    ctx->expected_seq = (seq + 1) & 0xFFFF;
    ctx->pending_fragments += 1;

    if (payload_size > 2) {
        int header_size = ctx->is_h265 ? 3 : 2;
        data_len = payload_size - header_size;
        if (ctx->length + data_len > (int)sizeof(ctx->buffer)) {
            data_len = (int)sizeof(ctx->buffer) - ctx->length;
        }
        if (data_len > 0) {
            memcpy(ctx->buffer + ctx->length, payload + header_size, (size_t)data_len);
            ctx->length += data_len;
        }
    }

    if (fu_end) {
        fu_a_reassembly_print_nalu(ctx);
        if (ctx->output_cb) {
            ctx->output_cb(ctx->buffer, ctx->length, ctx->timestamp, ctx->ssrc, ctx->output_user_data);
        }
        fu_a_reassembly_reset(ctx);
        return;
    }

    /*
     * 当前包不是末片时，尝试从重排序窗口刷出连续暂存包。
     * 如果刷出了末片，说明这一组 NALU 已经由窗口里的暂存包完成。
     */
    if (fu_a_reorder_drain(ctx)) {
        fu_a_reassembly_print_nalu(ctx);
        if (ctx->output_cb) {
            ctx->output_cb(ctx->buffer, ctx->length, ctx->timestamp, ctx->ssrc, ctx->output_user_data);
        }
        fu_a_reassembly_reset(ctx);
    }
}

/*
 * PS over RTP payload 的最小拆层摘要。
 *
 * 这个函数只在 RTP payload 看起来像 PS pack 时调用，学习路径是：
 *   PS pack header(00 00 01 BA)
 *     -> video PES(00 00 01 E0-0xEF)
 *     -> PES PTS
 *     -> Annex-B H.264 NALU start code(00 00 00 01)
 *
 * 它不做完整 PS 解复用，只打印足够对照抓包和理解 GB28181 PS over RTP 的关键字段。
 */
static void print_ps_payload_summary(const unsigned char *payload, int payload_size)
{
    const unsigned char ps_pack_start[] = {0x00, 0x00, 0x01, 0xBA};
    /*
     * 当前发送端 gb28181_build_ps_pack_h264() 写出的 video PES stream_id 是 0xE0，
     * 但接收端学习代码不再只写死 E0，而是识别 0xE0-0xEF 这一组 video stream_id。
     *
     * 更完整的工程做法不能在整个 payload 里全局扫描 00 00 01，
     * 因为 PS pack、PES 和 H.264 Annex-B NALU 都可能出现这个前缀。
     * 应该先在 PS 层按结构推进：解析/跳过 pack header、system header、
     * program_stream_map 等，再遇到 PES start code prefix: 00 00 01 时读取 stream_id 并分类：
     *   0xE0-0xEF -> video stream
     *   0xC0-0xDF -> audio stream
     *   0xBD      -> private_stream_1
     *   0xBC      -> program_stream_map
     * 只有进入 PES payload 后，才按 H.264 Annex-B 规则查找 NALU start code。
     */
    const unsigned char annexb_start[] = {0x00, 0x00, 0x00, 0x01};
    unsigned char video_stream_id = 0;
    int ps_offset;
    int pes_offset;
    int nalu_offset;

    ps_offset = find_bytes(payload, payload_size, ps_pack_start, (int)sizeof(ps_pack_start));
    pes_offset = find_video_pes_start(payload, payload_size, &video_stream_id);
    nalu_offset = find_bytes(payload, payload_size, annexb_start, (int)sizeof(annexb_start));

    printf("PS scan: pack_start=%d video_pes=%d video_stream_id=0x%02X annexb_nalu=%d\n",
           ps_offset,
           pes_offset,
           video_stream_id,
           nalu_offset);
    if (pes_offset >= 0 && pes_offset + 9 < payload_size) {
        unsigned int pes_len = (unsigned int)((payload[pes_offset + 4] << 8) | payload[pes_offset + 5]);
        unsigned int pes_flags = payload[pes_offset + 7];
        unsigned int pes_header_len = payload[pes_offset + 8];
        printf("PES detail: stream_id=0x%02X pes_len=%u flags=0x%02X header_len=%u\n",
               payload[pes_offset + 3], pes_len, pes_flags, pes_header_len);
        if (pes_header_len >= 5 && pes_offset + 14 <= payload_size) {
            const unsigned char *pts = payload + pes_offset + 9;
            unsigned long long pts_90khz;
            pts_90khz = ((unsigned long long)((pts[0] >> 1) & 0x07) << 30) |
                        ((unsigned long long)pts[1] << 22) |
                        ((unsigned long long)((pts[2] >> 1) & 0x7F) << 15) |
                        ((unsigned long long)pts[3] << 7) |
                        ((unsigned long long)((pts[4] >> 1) & 0x7F));
            printf("PTS detail: bytes=%02X %02X %02X %02X %02X value=%llu (90kHz)\n",
                   pts[0], pts[1], pts[2], pts[3], pts[4], pts_90khz);
        }
    }
    if (nalu_offset >= 0 && nalu_offset + 4 < payload_size) {
        unsigned int nalu_type = payload[nalu_offset + 4] & 0x1F;
        printf("NALU detail: first_byte=0x%02X h264_type=%u\n", payload[nalu_offset + 4], nalu_type);
    }
}

/*
 * 打印 RTP 包摘要，并对 payload 做学习式识别。
 *
 * 这个函数关注的是“RTP 这一层到底收到了什么”：
 *   - 先解析固定头里的 V / M / PT / Sequence Number / Timestamp / SSRC
 *   - 再根据 payload 头判断是 PS over RTP、裸 H.264 IDR，还是 H.264 FU-A
 *   - 若是 FU-A，就把分片交给 fu_a_reassembly_handle_packet() 演示重组
 *
 * 接收解析学习路径是：UDP -> RTP header -> payload 类型判断 -> PS/PES/NALU 或 FU-A 重组。
 *
 * 它不是完整 RTP 协议栈，也不是解码器，只是为了把抓包结果和 GB28181 学习链路
 * 对齐，方便理解 SDP 协商、RTP 发送、FU-A 重组和 PS 解析之间的关系。
 */
/*
 * 默认 NALU 输出回调：把重组后的裸 NALU 加上 Annex-B start code 追加写到文件。
 * 这样可以用 ffplay/ffmpeg 直接打开 gb28181_rx.h264 验证整条接收链路是否正确。
 */
static void fu_a_default_output_cb(const unsigned char *nalu,
                                   int nalu_size,
                                   uint32_t timestamp,
                                   unsigned int ssrc,
                                   void *user_data)
{
    static FILE *fp = NULL;
    const unsigned char annexb_start[] = {0x00, 0x00, 0x00, 0x01};

    (void)timestamp;
    (void)ssrc;
    (void)user_data;

    if (!nalu || nalu_size <= 0) {
        return;
    }

    if (!fp) {
        fp = fopen("gb28181_rx.h264", "wb");
        if (!fp) {
            printf("FU-A output: cannot open gb28181_rx.h264\n");
            return;
        }
        printf("FU-A output: writing reassembled NALU to gb28181_rx.h264\n");
    }

    fwrite(annexb_start, 1, sizeof(annexb_start), fp);
    fwrite(nalu, 1, (size_t)nalu_size, fp);
    fflush(fp);
    printf("FU-A output: wrote nalu len=%d timestamp=%u ssrc=0x%08X\n", nalu_size, timestamp, ssrc);
}

static void print_rtp_packet_summary(const unsigned char *data, int size)
{
    static h264_fu_a_reassembly_t fu_a_ctx;
    static int fu_a_ctx_initialized = 0;
    if (!fu_a_ctx_initialized) {
        fu_a_ctx.output_cb = fu_a_default_output_cb;
        fu_a_ctx.output_user_data = NULL;
        fu_a_ctx_initialized = 1;
    }
    unsigned int version;
    unsigned int marker;
    unsigned int payload_type;
    unsigned int seq;
    uint32_t timestamp;
    uint32_t ssrc;
    /* 当前 demo 只处理最小 RTP 固定头，长度是 12 字节。 */
    int payload_offset = 12;
    int payload_size;
    int i;

    if (!data || size < 12) {
        printf("===== RTP RX invalid packet len=%d =====\n", size);
        return;
    }

    /*
     * RTP fixed header 的最小 12 字节布局：
     *   data[0]    : V / P / X / CC，其中高 2 位是 version
     *   data[1]    : M / PT，其中最高位是 marker，低 7 位是 payload type
     *   data[2..3] : sequence number，网络字节序，大端
     *   data[4..7] : timestamp，网络字节序，大端
     *   data[8..11]: SSRC，网络字节序，大端
     *   data[12..] : RTP payload
     *
     * 真实工程里如果 CC > 0、X = 1 或 P = 1，payload_offset 不能固定为 12，
     * 还要跳过 CSRC、扩展头或处理尾部 padding。当前学习 demo 暂不覆盖这些扩展情况。
     */
    version = (unsigned int)((data[0] >> 6) & 0x03);
    marker = (unsigned int)((data[1] >> 7) & 0x01);
    payload_type = (unsigned int)(data[1] & 0x7F);
    seq = (unsigned int)((data[2] << 8) | data[3]);
    timestamp = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | data[7];
    ssrc = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) | ((uint32_t)data[10] << 8) | data[11];
    payload_size = size - payload_offset;

    /*
     * RTP 头字段要回到 SDP 里解释：
     *   pt        -> SDP 的 m= / a=rtpmap，例如 PT 96 表示 H264/90000
     *   timestamp  -> 按 a=rtpmap 的 90000 Hz 时钟解释
     *   ssrc       -> SDP 的 a=ssrc，日志里用十六进制打印
     * payload 可能是裸 H.264，也可能是 GB28181 常见的 PS over RTP。
     */
    printf("===== RTP RX udp/30000 =====\n");
    printf("version=%u pt=%u marker=%u seq=%u timestamp=%u ssrc=0x%08X payload_len=%d\n",
           version, payload_type, marker, seq, timestamp, ssrc, payload_size);
    printf("payload head:");
    for (i = 0; i < payload_size && i < 16; ++i) {
        printf(" %02X", data[payload_offset + i]);
    }
    printf("\n");

    /*
     * PS over RTP 判断：PS pack header 固定以 00 00 01 BA 开头。
     * GB28181 常见媒体负载就是把 H.264/H.265 先封进 PS/PES，再放进 RTP payload。
     */
    if (payload_size >= 4 && data[payload_offset] == 0x00 && data[payload_offset + 1] == 0x00 &&
        data[payload_offset + 2] == 0x01 && data[payload_offset + 3] == 0xBA) {
        printf("payload type guess: PS pack header 00 00 01 BA\n");
        print_ps_payload_summary(data + payload_offset, payload_size);
    } else if (payload_size > 0 && (data[payload_offset] & 0x1F) == 5) {
        /* 裸 H.264 单包 NALU：NALU header 低 5 位 type=5，表示 IDR slice。 */
        printf("payload type guess: raw H.264 IDR NALU\n");
    } else if (payload_size >= 2 && (data[payload_offset] & 0x1F) == 28) {
        /*
         * H.264 FU-A 分包规则：
         *   RTP payload[0] = FU indicator
         *     - F/NRI 继承原始 NALU header
         *     - 低 5 位 type=28，表示当前 payload 不是完整 NALU，而是 FU-A 分片
         *
         *   RTP payload[1] = FU header
         *     - bit7 S=1 表示首片
         *     - bit6 E=1 表示末片
         *     - 低 5 位保留原始 NALU type，例如 5 表示 IDR
         *
         *   RTP payload[2...] = 当前分片承载的原始 NALU 数据片段。
         * 接收端要按 seq 排序、按 timestamp/SSRC 归组，再用 S/E 位判断 NALU 起止；
         * marker 通常只在访问单元最后一个 RTP 包上置 1，不能单独代替 FU-A 的 S/E 判断。
         */
        unsigned int fu_start = (unsigned int)((data[payload_offset + 1] >> 7) & 0x01);
        unsigned int fu_end = (unsigned int)((data[payload_offset + 1] >> 6) & 0x01);
        unsigned int fu_type = (unsigned int)(data[payload_offset + 1] & 0x1F);
        const char *fu_role = fu_start ? "first" : (fu_end ? "last" : "middle");
        printf("payload type guess: H.264 FU-A\n");
        printf("FU-A detail: indicator=0x%02X header=0x%02X S=%u E=%u type=%u role=%s\n",
               data[payload_offset],
               data[payload_offset + 1],
               fu_start,
               fu_end,
               fu_type,
               fu_role);
        fu_a_reassembly_handle_packet(&fu_a_ctx,
                                      seq,
                                      timestamp,
                                      ssrc,
                                      data + payload_offset,
                                      payload_size,
                                      fu_start,
                                      fu_end,
                                      fu_type,
                                      0);
    } else if (payload_size >= 3 && ((data[payload_offset] >> 1) & 0x3F) == 49) {
        /*
         * H.265 FU 分包规则（RFC 7798）：
         *   RTP payload[0..1] = 2 字节 payload header（NALU 头，type=49）
         *     - forbidden(1) | nal_unit_type(6)=49 | nuh_layer_id_high(1)
         *     - nuh_layer_id_low(5) | nuh_temporal_zero(1) | temporal_id_plus1(3)
         *
         *   RTP payload[2] = FU header
         *     - bit7 S=1 表示首片
         *     - bit6 E=1 表示末片
         *     - 低 6 位 FuType = 原始 NALU type
         *
         *   RTP payload[3...] = 当前分片承载的原始 NALU 数据片段。
         * 与 H.264 FU-A 的差异：头 2 字节、type=49（非 28）、FU header 的 FuType 是 6 位。
         */
        unsigned int fu_start = (unsigned int)((data[payload_offset + 2] >> 7) & 0x01);
        unsigned int fu_end = (unsigned int)((data[payload_offset + 2] >> 6) & 0x01);
        unsigned int fu_type = (unsigned int)(data[payload_offset + 2] & 0x3F);
        const char *fu_role = fu_start ? "first" : (fu_end ? "last" : "middle");
        printf("payload type guess: H.265 FU\n");
        printf("FU detail: payload_header=0x%02X%02X fu_header=0x%02X S=%u E=%u FuType=%u role=%s\n",
               data[payload_offset],
               data[payload_offset + 1],
               data[payload_offset + 2],
               fu_start,
               fu_end,
               fu_type,
               fu_role);
        fu_a_reassembly_handle_packet(&fu_a_ctx,
                                      seq,
                                      timestamp,
                                      ssrc,
                                      data + payload_offset,
                                      payload_size,
                                      fu_start,
                                      fu_end,
                                      fu_type,
                                      1);
    } else {
        /* 当前 demo 没覆盖的 payload 类型先只打印头部，便于后续按抓包继续扩展解析。 */
        printf("payload type guess: unknown/demo payload\n");
    }
}

/*
 * RTCP 报文最小解析。
 *
 * 和 RTP 一样只做学习式拆头，不做完整 RTCP 协议栈。RTCP 固定头 4 字节：
 *   byte0: V(2) P(1) RC/SC(5)
 *   byte1: PT(8)      200=SR 201=RR 202=SDES 203=BYE 204=APP
 *   byte2-3: length(16)  这是以 32 位字为单位的长度-1
 *
 * SR(200) 在头之后还有 24 字节 sender info：
 *   SSRC(4) NTP_ts(8: 高32秒+低32小数) RTP_ts(4) packet_count(4) octet_count(4)
 * RR(201) 在头之后是 report blocks：SSRC(4) + 每个 block 24 字节
 *   fraction_lost(1) lost(24 紧缩) ext_high_seq(4) jitter(4) LSR(4) DLSR(4)
 *
 * 这里 SR 提取 NTP/RTP ts/packets/octet；RR 提取 fraction_lost/lost/jitter；
 * SDES/BYE/APP 只报类型。复合 RTCP 可能含多个 packet，按 length 推进循环。
 */
static void print_rtcp_packet_summary(const unsigned char *data, int size)
{
    int offset = 0;
    int reported = 0;

    if (!data || size < 4) {
        printf("===== RTCP RX udp/30001 invalid len=%d =====\n", size);
        return;
    }

    printf("===== RTCP RX udp/30001 len=%d =====\n", size);

    /* RTCP compound packet 可能含多个子报文，按 length 字段推进。 */
    while (offset + 4 <= size) {
        unsigned int version = (data[offset] >> 6) & 0x03;
        unsigned int rc_sc = data[offset] & 0x1F;
        unsigned int pt = data[offset + 1];
        unsigned int word_len = ((unsigned int)data[offset + 2] << 8) | data[offset + 3];
        int byte_len = (int)((word_len + 1) * 4);  /* length 字段是不含自己的字数-1 */
        const char *pt_name = "unknown";

        if (version != 2) {
            /* 不是 RTCP v2，可能是别的 UDP 报文，停止解析。 */
            printf("  [sub %d] version=%u (not RTCP v2), stop\n", offset, version);
            break;
        }

        switch (pt) {
        case 200: pt_name = "SR"; break;
        case 201: pt_name = "RR"; break;
        case 202: pt_name = "SDES"; break;
        case 203: pt_name = "BYE"; break;
        case 204: pt_name = "APP"; break;
        default: break;
        }

        printf("  [sub %d] V=2 RC=%u PT=%u(%s) length=%u bytes=%d\n",
               offset, rc_sc, pt, pt_name, word_len, byte_len);

        /* SR sender info（头 4 字节之后 24 字节）。 */
        if (pt == 200 && offset + 28 <= size) {
            const unsigned char *p = data + offset;
            unsigned int ssrc = ((unsigned int)p[4] << 24) | ((unsigned int)p[5] << 16) |
                               ((unsigned int)p[6] << 8) | p[7];
            unsigned long long ntp_sec = ((unsigned long long)p[8] << 24) | ((unsigned long long)p[9] << 16) |
                                        ((unsigned long long)p[10] << 8) | p[11];
            unsigned long long ntp_frac = ((unsigned long long)p[12] << 24) | ((unsigned long long)p[13] << 16) |
                                         ((unsigned long long)p[14] << 8) | p[15];
            unsigned int rtp_ts = ((unsigned int)p[16] << 24) | ((unsigned int)p[17] << 16) |
                                 ((unsigned int)p[18] << 8) | p[19];
            unsigned int pkt_count = ((unsigned int)p[20] << 24) | ((unsigned int)p[21] << 16) |
                                    ((unsigned int)p[22] << 8) | p[23];
            unsigned int oct_count = ((unsigned int)p[24] << 24) | ((unsigned int)p[25] << 16) |
                                    ((unsigned int)p[26] << 8) | p[27];
            printf("    SR sender: ssrc=0x%08X ntp_sec=%llu ntp_frac=%llu rtp_ts=%u packets=%u octets=%u\n",
                   ssrc, ntp_sec, ntp_frac, rtp_ts, pkt_count, oct_count);
        }

        /* RR report block（头 4 + SSRC 4 之后，每 block 24 字节）。rc_sc 是 block 数。 */
        if (pt == 201 && offset + 8 <= size) {
            unsigned int reporter_ssrc = ((unsigned int)data[offset+4] << 24) | ((unsigned int)data[offset+5] << 16) |
                                        ((unsigned int)data[offset+6] << 8) | data[offset+7];
            printf("    RR reporter: ssrc=0x%08X blocks=%u\n", reporter_ssrc, rc_sc);
            if (rc_sc > 0 && offset + 32 <= size) {
                const unsigned char *b = data + offset + 8;
                unsigned int fraction = b[0];
                int lost = (int)(((unsigned int)b[1] << 16) | ((unsigned int)b[2] << 8) | b[3]);
                if (lost & 0x00800000) {
                    lost |= ~0x00FFFFFF;  /* 24 位有符号扩展 */
                }
                unsigned int jitter = ((unsigned int)b[4] << 8) | b[5];
                printf("    RR block 0: fraction_lost=%u/256 packets_lost=%d jitter=%u\n",
                       fraction, lost, jitter);
            }
        }

        reported++;
        /* 推进到下一个子报文。byte_len 是含头的总字节数。 */
        if (byte_len <= 0) {
            break;  /* 防御：异常 length */
        }
        offset += byte_len;
        if (reported > 8) {
            printf("  (more than 8 sub-packets, stop)\n");
            break;
        }
    }
}

static void build_www_auth(char *buf, int buf_size)
{
    /* 用于 401 响应的最小 Digest challenge。 */
    snprintf(buf, buf_size,
        "WWW-Authenticate: Digest realm=\"3402000000\", nonce=\"1234567890abcdef\", algorithm=MD5, qop=auth\r\n");
}

static void build_play_sdp(char *buf, int buf_size)
{
    /* INVITE 返回的最小 SDP，用于说明媒体端口和编码。 */
    snprintf(buf, buf_size,
        "v=0\r\n"
        "o=34020000002000000001 0 0 IN IP4 127.0.0.1\r\n"
        "s=Play\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=video 30000 RTP/AVP 96\r\n"
        "a=recvonly\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=ssrc:0305419896\r\n");
}

static void build_catalog_response_message(char *buf, int buf_size, int cseq)
{
    gb28181_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.local_id, sizeof(cfg.local_id), "%s", "34020000002000000001");
    snprintf(cfg.domain, sizeof(cfg.domain), "%s", "3402000000");
    snprintf(cfg.username, sizeof(cfg.username), "%s", "34020000002000000001");
    snprintf(cfg.sip_server_ip, sizeof(cfg.sip_server_ip), "%s", "127.0.0.1");
    cfg.local_sip_port = 5060;
    snprintf(cfg.local_ip, sizeof(cfg.local_ip), "%s", "127.0.0.1");
    snprintf(cfg.stream_id, sizeof(cfg.stream_id), "%s", "34020000001320000001");
    gb28181_build_message_catalog_response(&cfg, cseq, buf, buf_size);
}

static void build_device_info_response_message(char *buf, int buf_size, int cseq)
{
    gb28181_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.local_id, sizeof(cfg.local_id), "%s", "34020000002000000001");
    snprintf(cfg.domain, sizeof(cfg.domain), "%s", "3402000000");
    snprintf(cfg.username, sizeof(cfg.username), "%s", "34020000002000000001");
    snprintf(cfg.sip_server_ip, sizeof(cfg.sip_server_ip), "%s", "127.0.0.1");
    cfg.local_sip_port = 5060;
    snprintf(cfg.local_ip, sizeof(cfg.local_ip), "%s", "127.0.0.1");
    snprintf(cfg.stream_id, sizeof(cfg.stream_id), "%s", "34020000001320000001");
    gb28181_build_message_device_info(&cfg, cseq, buf, buf_size);
}

static void build_device_status_response_message(char *buf, int buf_size, int cseq)
{
    gb28181_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.local_id, sizeof(cfg.local_id), "%s", "34020000002000000001");
    snprintf(cfg.domain, sizeof(cfg.domain), "%s", "3402000000");
    snprintf(cfg.username, sizeof(cfg.username), "%s", "34020000002000000001");
    snprintf(cfg.sip_server_ip, sizeof(cfg.sip_server_ip), "%s", "127.0.0.1");
    cfg.local_sip_port = 5060;
    snprintf(cfg.local_ip, sizeof(cfg.local_ip), "%s", "127.0.0.1");
    snprintf(cfg.stream_id, sizeof(cfg.stream_id), "%s", "34020000001320000001");
    gb28181_build_message_device_status(&cfg, cseq, buf, buf_size);
}

int main(void)
{
    /* 最小 SIP mock 平台：处理 REGISTER / MESSAGE / INVITE / ACK / BYE。 */
    int sockfd;
    int rtp_sockfd;
    int rtcp_sockfd;
    char recv_buf[8192];
    unsigned char rtp_buf[2048];
    unsigned char rtcp_buf[2048];
    char reply[8192];
    struct sockaddr_in peer;
    struct sockaddr_in rtp_peer;
    struct sockaddr_in rtcp_peer;
    socklen_t peer_len = sizeof(peer);
    socklen_t rtp_peer_len = sizeof(rtp_peer);
    socklen_t rtcp_peer_len = sizeof(rtcp_peer);
    int registered = 0;
    int invited = 0;
    int acked = 0;

    if (init_winsock() != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }

    sockfd = bind_udp_socket(5060);
    if (sockfd < 0) {
        cleanup_winsock();
        return 1;
    }
    rtp_sockfd = bind_udp_socket(30000);
    if (rtp_sockfd < 0) {
        socket_close(sockfd);
        cleanup_winsock();
        return 1;
    }
    /* RTCP 端口 = RTP 端口 + 1。jrtplib 发送端默认会在 30001 发 SR/RR。 */
    rtcp_sockfd = bind_udp_socket(30001);
    if (rtcp_sockfd < 0) {
        socket_close(rtp_sockfd);
        socket_close(sockfd);
        cleanup_winsock();
        return 1;
    }

    printf("GB28181 SIP mock server listening on udp/5060\n");
    printf("GB28181 RTP mock receiver listening on udp/30000\n");
    printf("GB28181 RTCP mock receiver listening on udp/30001\n");

    for (;;) {
        fd_set readfds;
        int maxfd;
        int ret;
        gb28181_sip_message_t msg;
        char from_ip[64];

        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        FD_SET(rtp_sockfd, &readfds);
        FD_SET(rtcp_sockfd, &readfds);
        maxfd = sockfd;
        if (rtp_sockfd > maxfd) maxfd = rtp_sockfd;
        if (rtcp_sockfd > maxfd) maxfd = rtcp_sockfd;
        ret = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ret <= 0) {
            continue;
        }

        if (FD_ISSET(rtcp_sockfd, &readfds)) {
            ret = recvfrom(rtcp_sockfd, (char *)rtcp_buf, sizeof(rtcp_buf), 0,
                           (struct sockaddr *)&rtcp_peer, &rtcp_peer_len);
            if (ret > 0) {
                print_rtcp_packet_summary(rtcp_buf, ret);
            }
            continue;
        }

        if (FD_ISSET(rtp_sockfd, &readfds)) {
            ret = recvfrom(rtp_sockfd, (char *)rtp_buf, sizeof(rtp_buf), 0, (struct sockaddr *)&rtp_peer, &rtp_peer_len);
            if (ret > 0) {
                print_rtp_packet_summary(rtp_buf, ret);
            }
            continue;
        }

        ret = recvfrom(sockfd, recv_buf, sizeof(recv_buf) - 1, 0, (struct sockaddr *)&peer, &peer_len);
        memset(&msg, 0, sizeof(msg));
        if (ret <= 0) {
            continue;
        }
        recv_buf[ret] = '\0';
        if (gb28181_parse_sip_message(recv_buf, &msg) != 0) {
            printf("Invalid SIP message:\n%s\n", recv_buf);
            continue;
        }

        inet_ntop(AF_INET, &peer.sin_addr, from_ip, sizeof(from_ip));
        printf("===== RX from %s:%d =====\n%s\n", from_ip, ntohs(peer.sin_port), recv_buf);

        if (msg.is_response) {
            printf("SIP response status=%d reason=%s, ignored by mock server\n",
                   msg.status_code,
                   msg.reason[0] ? msg.reason : "<none>");
            continue;
        }

        printf("method=%s cseq=%d auth=%s\n", msg.method, msg.cseq, msg.authorization[0] ? msg.authorization : "<none>");

        /* 第一次 REGISTER 没有 Authorization，故意回 401 让客户端走鉴权。 */
        if (!registered && strcmp(msg.method, "REGISTER") == 0 && msg.authorization[0] == '\0') {
            char www_auth[256];
            build_www_auth(www_auth, sizeof(www_auth));
            snprintf(reply, sizeof(reply),
                "SIP/2.0 401 Unauthorized\r\n"
                "Via: %s\r\n"
                "From: %s\r\n"
                "To: %s\r\n"
                "Call-ID: %s\r\n"
                "CSeq: %d REGISTER\r\n"
                "%s"
                "Content-Length: 0\r\n\r\n",
                msg.via, msg.from, msg.to, msg.call_id, msg.cseq, www_auth);
            printf("===== TX 401 =====\n%s\n", reply);
            send_reply(sockfd, &peer, reply);
            continue;
        }

        if (strcmp(msg.method, "REGISTER") == 0 && msg.authorization[0] != '\0') {
            /* 第二次 REGISTER 带 Authorization，认为注册成功。 */
            registered = 1;
            snprintf(reply, sizeof(reply),
                "SIP/2.0 200 OK\r\n"
                "Via: %s\r\n"
                "From: %s\r\n"
                "To: %s\r\n"
                "Call-ID: %s\r\n"
                "CSeq: %d REGISTER\r\n"
                "Content-Length: 0\r\n\r\n",
                msg.via, msg.from, msg.to, msg.call_id, msg.cseq);
            printf("===== TX 200 =====\n%s\n", reply);
            send_reply(sockfd, &peer, reply);
            continue;
        }

        if (registered && strcmp(msg.method, "MESSAGE") == 0) {
            /* MESSAGE 是 SIP 信令，body 中一般放 XML 控制消息。 */
            char cmd_type[64];
            char sn[64];
            char device_id[64];
            cmd_type[0] = '\0';
            sn[0] = '\0';
            device_id[0] = '\0';
            if (msg.body) {
                gb28181_extract_xml_tag(msg.body, "CmdType", cmd_type, sizeof(cmd_type));
                gb28181_extract_xml_tag(msg.body, "SN", sn, sizeof(sn));
                gb28181_extract_xml_tag(msg.body, "DeviceID", device_id, sizeof(device_id));
            }
            printf("MESSAGE xml CmdType=%s SN=%s DeviceID=%s\n",
                   cmd_type[0] ? cmd_type : "<none>",
                   sn[0] ? sn : "<none>",
                   device_id[0] ? device_id : "<none>");
            snprintf(reply, sizeof(reply),
                "SIP/2.0 200 OK\r\n"
                "Via: %s\r\n"
                "From: %s\r\n"
                "To: %s\r\n"
                "Call-ID: %s\r\n"
                "CSeq: %d MESSAGE\r\n"
                "Content-Length: 0\r\n\r\n",
                msg.via, msg.from, msg.to, msg.call_id, msg.cseq);
            printf("===== TX MESSAGE 200 =====\n%s\n", reply);
            send_reply(sockfd, &peer, reply);

            if (strcmp(cmd_type, "Catalog") == 0) {
                char catalog_msg[4096];
                build_catalog_response_message(catalog_msg, sizeof(catalog_msg), msg.cseq + 1);
                printf("===== TX CATALOG MESSAGE =====\n%s\n", catalog_msg);
                send_reply(sockfd, &peer, catalog_msg);
            } else if (strcmp(cmd_type, "DeviceInfo") == 0) {
                char info_msg[2048];
                build_device_info_response_message(info_msg, sizeof(info_msg), msg.cseq + 1);
                printf("===== TX DEVICEINFO MESSAGE =====\n%s\n", info_msg);
                send_reply(sockfd, &peer, info_msg);
            } else if (strcmp(cmd_type, "DeviceStatus") == 0) {
                char status_msg[2048];
                build_device_status_response_message(status_msg, sizeof(status_msg), msg.cseq + 1);
                printf("===== TX DEVICESTATUS MESSAGE =====\n%s\n", status_msg);
                send_reply(sockfd, &peer, status_msg);
            }
            continue;
        }

        if (registered && strcmp(msg.method, "INVITE") == 0) {
            /* INVITE 返回 SDP，说明平台愿意接受媒体会话，并声明接收侧 RTP 参数。 */
            char sdp[1024];
            int sdp_len;
            build_play_sdp(sdp, sizeof(sdp));
            sdp_len = (int)strlen(sdp);
            invited = 1;
            snprintf(reply, sizeof(reply),
                "SIP/2.0 200 OK\r\n"
                "Via: %s\r\n"
                "From: %s\r\n"
                "To: %s;tag=mock\r\n"
                "Call-ID: %s\r\n"
                "CSeq: %d INVITE\r\n"
                "Contact: <sip:34020000002000000001@127.0.0.1:5060>\r\n"
                "Content-Type: application/sdp\r\n"
                "Content-Length: %d\r\n\r\n"
                "%s",
                msg.via, msg.from, msg.to, msg.call_id, msg.cseq, sdp_len, sdp);
            printf("===== TX INVITE 200 + SDP =====\n%s\n", reply);
            send_reply(sockfd, &peer, reply);
            continue;
        }

        if (invited && strcmp(msg.method, "ACK") == 0) {
            /* ACK 到达后，认为会话正式建立。 */
            acked = 1;
            printf("===== ACK RECEIVED, media session established in mock =====\n");
            continue;
        }

        if ((invited || acked) && strcmp(msg.method, "BYE") == 0) {
            /* BYE 结束会话并清理状态。 */
            snprintf(reply, sizeof(reply),
                "SIP/2.0 200 OK\r\n"
                "Via: %s\r\n"
                "From: %s\r\n"
                "To: %s\r\n"
                "Call-ID: %s\r\n"
                "CSeq: %d BYE\r\n"
                "Content-Length: 0\r\n\r\n",
                msg.via, msg.from, msg.to, msg.call_id, msg.cseq);
            printf("===== TX BYE 200 =====\n%s\n", reply);
            send_reply(sockfd, &peer, reply);
            invited = 0;
            acked = 0;
            continue;
        }

        snprintf(reply, sizeof(reply),
            "SIP/2.0 501 Not Implemented\r\n"
            "Via: %s\r\n"
            "From: %s\r\n"
            "To: %s\r\n"
            "Call-ID: %s\r\n"
            "CSeq: %d %s\r\n"
            "Content-Length: 0\r\n\r\n",
            msg.via, msg.from, msg.to, msg.call_id, msg.cseq, msg.method);
        printf("===== TX 501 =====\n%s\n", reply);
        send_reply(sockfd, &peer, reply);
    }

    socket_close(rtcp_sockfd);
    socket_close(rtp_sockfd);
    socket_close(sockfd);
    cleanup_winsock();
    return 0;
}
