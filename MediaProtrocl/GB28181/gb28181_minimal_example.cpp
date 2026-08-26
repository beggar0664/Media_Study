#include "gb28181_module.h"

/*
 * 最小 RTP / PS over RTP 学习示例。
 *
 * 对应文档：
 * - ../gb28181_study.md：RTP、PS over RTP、RTP 分片章节
 * - ../../current_code_learning_guide.md：Wireshark 过滤 udp.dstport == 30000 的抓包方法
 *
 * 这个程序单独演示媒体承载：裸 H.264 over RTP、PS over RTP、强制小包分片和 1200 字节分片。
 * 运行时重点看 RTP 头字段、payload 起始字节、以及分片时 seq / timestamp / marker 的变化。
 * SIP/SDP 会话控制由 gb28181_sip_register_client.cpp / gb28181_sip_mock_server.cpp 单独演示。
 */

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <stdio.h>
#include <string.h>

int main(void)
{
    gb28181_config_t cfg;
    char register_msg[2048];
    char invite_msg[4096];
    char bye_msg[2048];
    /*
     * 下面三段是裸 H.264 NALU，不带 Annex-B start code。
     *
     * 裸 NALU 的第一个字节就是 NALU header：
     *   0x67 -> type=7，SPS
     *   0x68 -> type=8，PPS
     *   0x65 -> type=5，IDR slice
     *
     * 这些数据很小，示例里直接通过 gb28181_send_rtp_packet() 单包发送，
     * 用来先观察裸 H.264 over RTP 的最小访问单元：SPS -> PPS -> IDR。
     */
    const unsigned char demo_h264_sps[] = {
        0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40, 0x78,
        0x02, 0x27, 0xe5, 0xc0
    };
    const unsigned char demo_h264_pps[] = {
        0x68, 0xeb, 0xec, 0xb2, 0x2c
    };
    const unsigned char demo_h264_idr[] = {
        0x65, 0x88, 0x84, 0x21, 0xa0, 0x10, 0x11, 0x12,
        0x13, 0x14, 0x15, 0x16, 0x17, 0x18
    };
    /*
     * 这段是 Annex-B 形式的 H.264 数据，每个 NALU 前面都有 00 00 00 01 起始码。
     * 它不是给 FU-A 用的，而是给 gb28181_build_ps_pack_h264() 打包成 PS/PES 用的。
     */
    const unsigned char demo_h264_annexb[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1f,
        0xac, 0xd9, 0x40, 0x78, 0x02, 0x27, 0xe5, 0xc0,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xeb, 0xec, 0xb2,
        0x2c,
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21,
        0xa0, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
        0x17, 0x18
    };
    unsigned char demo_ps_pack[512];
    unsigned char large_idr_nalu[256];
    unsigned char large_h265_nalu[256];  /* 用于 H.265 FU 分片演示 */
    unsigned char normal_h264_annexb[2048];
    unsigned char normal_ps_pack[4096];
    int demo_ps_len;
    int normal_h264_len = 0;
    int normal_ps_len;
    gb28181_handle_t handle;
    int ret;
    int local_rtp_port = 0;
    unsigned int ssrc = 0;

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.local_id, sizeof(cfg.local_id), "%s", "34020000001320000001");
    snprintf(cfg.domain, sizeof(cfg.domain), "%s", "3402000000");
    snprintf(cfg.username, sizeof(cfg.username), "%s", "34020000001320000001");
    snprintf(cfg.sip_server_ip, sizeof(cfg.sip_server_ip), "%s", "127.0.0.1");
    cfg.sip_server_port = 5060;
    snprintf(cfg.local_ip, sizeof(cfg.local_ip), "%s", "127.0.0.1");
    cfg.local_sip_port = 5060;
    cfg.local_rtp_port = 10000;
    snprintf(cfg.remote_rtp_ip, sizeof(cfg.remote_rtp_ip), "%s", "127.0.0.1");
    cfg.remote_rtp_port = 30000;
    snprintf(cfg.stream_id, sizeof(cfg.stream_id), "%s", "34020000001320000001");
    cfg.payload_type = 96;
    cfg.ssrc = 0x12345678;

    /*
     * 构造一个较大的裸 IDR NALU，专门用来触发 H.264 FU-A 分片。
     *
     * large_idr_nalu[0] = 0x65，表示原始 NALU 是 IDR(type=5)。
     * 后面 255 字节只是演示填充数据，不代表真实编码器输出。
     *
     * 后面调用 gb28181_send_h264_fu_a(..., max_payload_size=24, ...)，
     * 每个 RTP payload 里还要留 2 字节给 FU indicator / FU header，
     * 所以每片最多只装 22 字节原始 NALU 数据。256 字节的 NALU 放不进一包，
     * 因此一定会被拆成多片 FU-A。
     */
    large_idr_nalu[0] = 0x65;
    for (normal_h264_len = 1; normal_h264_len < (int)sizeof(large_idr_nalu); ++normal_h264_len) {
        large_idr_nalu[normal_h264_len] = (unsigned char)(0x80 + (normal_h264_len & 0x3F));
    }
    normal_h264_len = 0;

    /*
     * 构造一个较大的 H.265 NALU，专门用来触发 H.265 FU 分片。
     *
     * H.265 NALU 头是 2 字节：
     *   byte0 = forbidden(1) | nal_unit_type(6) | nuh_layer_id_high(1)
     *   byte1 = nuh_layer_id_low(5) | nuh_temporal_zero(1) | temporal_id_plus1(3)
     *
     * 这里用 type=19(IDR_W_RADL)：byte0 = (19 << 1) | 0 = 0x26，byte1 = 0x01(temporal=1)。
     * 后面 254 字节是演示填充，不代表真实编码器输出。
     *
     * 后面调用 gb28181_send_h265_fu(..., max_payload_size=24, ...)，每片要留 3 字节
     * 给 payload header(2) + FU header(1)，所以每片最多装 21 字节原始 NALU 数据，
     * 256 字节的 NALU 一定会被拆成多片 H.265 FU。
     */
    large_h265_nalu[0] = 0x26;  /* type=19 IDR_W_RADL */
    large_h265_nalu[1] = 0x01; /* layer_id=0, temporal_id_plus1=1 */
    {
        int i;
        for (i = 2; i < (int)sizeof(large_h265_nalu); ++i) {
            large_h265_nalu[i] = (unsigned char)(0x80 + (i & 0x3F));
        }
    }

    memcpy(normal_h264_annexb + normal_h264_len, demo_h264_annexb, sizeof(demo_h264_annexb));
    normal_h264_len += (int)sizeof(demo_h264_annexb);
    for (; normal_h264_len < 1800; ++normal_h264_len) {
        normal_h264_annexb[normal_h264_len] = (unsigned char)(normal_h264_len & 0xFF);
    }

    gb28181_build_register(&cfg, register_msg, sizeof(register_msg));
    gb28181_build_invite(&cfg, invite_msg, sizeof(invite_msg));
    gb28181_build_bye(&cfg, bye_msg, sizeof(bye_msg));
    demo_ps_len = gb28181_build_ps_pack_h264(demo_h264_annexb, (int)sizeof(demo_h264_annexb), 9000, 9000, demo_ps_pack, (int)sizeof(demo_ps_pack));
    normal_ps_len = gb28181_build_ps_pack_h264(normal_h264_annexb, normal_h264_len, 18000, 18000, normal_ps_pack, (int)sizeof(normal_ps_pack));

    printf("===== REGISTER =====\n%s\n", register_msg);
    printf("===== INVITE + SDP =====\n%s\n", invite_msg);
    printf("===== BYE =====\n%s\n", bye_msg);
    if (demo_ps_len > 0) {
        int i;
        /* 这段输出是给 WinHex / 十六进制比对用的，先看 PS pack header 再看 PES。 */
        printf("===== PS PACK (H.264) len=%d =====\n", demo_ps_len);
        for (i = 0; i < demo_ps_len; ++i) {
            printf("%02X%c", demo_ps_pack[i], ((i + 1) % 16 == 0 || i + 1 == demo_ps_len) ? '\n' : ' ');
        }
    }
    if (normal_ps_len > 0) {
        /* 这段更接近工程分包尺寸，便于观察 1200 字节左右的常见发送方式。 */
        printf("===== NORMAL PS PACK len=%d, normal max_payload=1200 =====\n", normal_ps_len);
    }

    printf("[1/4] create context\n");
    handle = gb28181_create(&cfg);
    if (!handle) {
        printf("create failed\n");
        return 1;
    }

    printf("[2/4] start RTP session\n");
    if (gb28181_start(handle) == 0) {
        int i;
        printf("RTP session started\n");
        if (gb28181_get_local_rtp_port(handle, &local_rtp_port) == 0) {
            printf("local RTP port: %d\n", local_rtp_port);
        } else {
            printf("local RTP port: unknown\n");
        }
        if (gb28181_get_ssrc(handle, &ssrc) == 0) {
            printf("SSRC: %010u (0x%08X)\n", ssrc, ssrc);
        }
        /* 先发一组最小访问单元，便于确认裸 H.264 over RTP 的基础行为。 */
        printf("sending one H.264 access unit: SPS -> PPS -> IDR\n");
        ret = gb28181_send_rtp_packet(handle, demo_h264_sps, sizeof(demo_h264_sps), 0, 0);
        printf("[3/4] send SPS ret=%d len=%u timestamp_inc=%u marker=0\n", ret, (unsigned)sizeof(demo_h264_sps), 0u);
        if (ret >= 0) {
            ret = gb28181_send_rtp_packet(handle, demo_h264_pps, sizeof(demo_h264_pps), 0, 0);
            printf("[3/4] send PPS ret=%d len=%u timestamp_inc=%u marker=0\n", ret, (unsigned)sizeof(demo_h264_pps), 0u);
        }
        if (ret >= 0) {
            ret = gb28181_send_rtp_packet(handle, demo_h264_idr, sizeof(demo_h264_idr), 9000, 1);
            printf("[3/4] send IDR ret=%d len=%u timestamp_inc=%u marker=1\n", ret, (unsigned)sizeof(demo_h264_idr), 9000u);
        }
        if (ret >= 0 && demo_ps_len > 0) {
            /* 同一个 PS pack 直接作为 RTP payload 发出去，适合先看整体字节布局。 */
            printf("sending one PS-over-RTP packet: PS pack -> PES -> H.264 IDR\n");
            ret = gb28181_send_rtp_packet(handle, demo_ps_pack, demo_ps_len, 9000, 1);
            printf("[3/4] send PS ret=%d len=%u timestamp_inc=%u marker=1\n", ret, (unsigned)demo_ps_len, 9000u);
        }
        if (ret >= 0 && demo_ps_len > 0) {
            int repeat_idx;
            /* 连续发送相同的 PS payload，观察 seq / timestamp / marker 的变化。 */
            printf("sending repeated PS-over-RTP packets for seq/timestamp inspection\n");
            for (repeat_idx = 0; repeat_idx < 3; ++repeat_idx) {
                ret = gb28181_send_rtp_packet(handle, demo_ps_pack, demo_ps_len, 9000, 1);
                printf("[3/4] send repeated PS %d ret=%d len=%u timestamp_inc=%u marker=1\n",
                       repeat_idx + 1, ret, (unsigned)demo_ps_len, 9000u);
                if (ret < 0) {
                    break;
                }
            }
        }
        if (ret >= 0 && demo_ps_len > 0) {
            /* 故意把 payload 压得很小，逼出多个 RTP 包，方便看 seq / timestamp / marker。 */
            printf("sending fragmented PS-over-RTP packets: max_payload=24\n");
            ret = gb28181_send_rtp_payload_fragmented(handle, demo_ps_pack, demo_ps_len, 24, 9000);
            printf("[3/4] send fragmented PS total=%d len=%u timestamp_inc=%u marker(last)=1\n", ret, (unsigned)demo_ps_len, 9000u);
        }
        if (ret >= 0) {
            /*
             * H.264 FU-A 分片发送演示。
             *
             * 输入 large_idr_nalu 是裸 IDR NALU，首字节是 0x65。
             * 进入 gb28181_send_h264_fu_a() 后，0x65 不会原样出现在每个 RTP payload 开头，
             * 而是被拆成：
             *   FU indicator = 0x7C，表示这是 FU-A(type=28)，并保留原 NRI
             *   FU header    = 0x85 / 0x05 / 0x45，分别表示首片 / 中间片 / 末片
             *
             * max_payload=24 是故意取很小，用来强制 256 字节 IDR NALU 触发多包分片。
             * 如果 max_payload 足够大，裸 NALU 可以单包发出，就不需要 FU-A。
             */
            printf("sending H.264 FU-A fragmented IDR: max_payload=24\n");
            ret = gb28181_send_h264_fu_a(handle, large_idr_nalu, (int)sizeof(large_idr_nalu), 24, 9000);
            printf("[3/4] send H264 FU-A total=%d nalu_len=%u timestamp_inc=%u marker(last)=1\n", ret, (unsigned)sizeof(large_idr_nalu), 9000u);
        }
        if (ret >= 0) {
            /*
             * H.265 FU 分片发送演示（RFC 7798），与上面 H.264 FU-A 对照。
             *
             * 输入 large_h265_nalu 是裸 H.265 NALU，前 2 字节是 NALU 头(0x26 0x01，type=19)。
             * 进入 gb28181_send_h265_fu() 后，2 字节头不会原样出现在每个 RTP payload 开头，
             * 而是被拆成：
             *   payload header(2 字节) = 原始头但 type 改成 49：0x62 0x01
             *   FU header(1 字节)      = S/E 位 + 原始 FuType(19)
             *     首片 0x93 (S=1, type=19)，中间片 0x13，末片 0x53 (E=1)
             *
             * 与 H.264 FU-A 的差异：头是 2 字节（非 1）、type=49（非 28）、FU header 的 FuType 是 6 位。
             */
            printf("sending H.265 FU fragmented IDR: max_payload=24\n");
            ret = gb28181_send_h265_fu(handle, large_h265_nalu, (int)sizeof(large_h265_nalu), 24, 9000);
            printf("[3/4] send H265 FU total=%d nalu_len=%u timestamp_inc=%u marker(last)=1\n", ret, (unsigned)sizeof(large_h265_nalu), 9000u);
        }
        if (ret >= 0 && normal_ps_len > 0) {
            /* 这个分片尺寸更接近实际工程，通常会让单包接近 MTU 上限但不超过。 */
            printf("sending normal PS-over-RTP packets: max_payload=1200\n");
            ret = gb28181_send_rtp_payload_fragmented(handle, normal_ps_pack, normal_ps_len, 1200, 9000);
            printf("[3/4] send normal fragmented PS total=%d len=%u timestamp_inc=%u marker(last)=1\n", ret, (unsigned)normal_ps_len, 9000u);
        }
#ifdef _WIN32
        Sleep(5000);
#else
        sleep(5);
#endif
        /* 重复发送同一 IDR，便于在抓包里对比连续包的时间戳和序号。 */
        printf("repeat IDR packets for packet inspection\n");
        for (i = 0; i < 4; ++i) {
            ret = gb28181_send_rtp_packet(handle, demo_h264_idr, sizeof(demo_h264_idr), 9000, 1);
            printf("[3/4] send repeat IDR %d ret=%d len=%u timestamp_inc=%u marker=1\n", i + 1, ret, (unsigned)sizeof(demo_h264_idr), 9000u);
            if (ret < 0) {
                break;
            }
        }
#ifdef _WIN32
        Sleep(5000);
#else
        sleep(5);
#endif
        printf("[4/4] stop RTP session\n");
        gb28181_stop(handle);
    } else {
        printf("start failed\n");
    }
    gb28181_destroy(handle);
    return 0;
}
