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
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

static void print_ps_payload_summary(const unsigned char *payload, int payload_size)
{
    const unsigned char ps_pack_start[] = {0x00, 0x00, 0x01, 0xBA};
    const unsigned char video_pes_start[] = {0x00, 0x00, 0x01, 0xE0};
    const unsigned char annexb_start[] = {0x00, 0x00, 0x00, 0x01};
    int ps_offset;
    int pes_offset;
    int nalu_offset;

    ps_offset = find_bytes(payload, payload_size, ps_pack_start, (int)sizeof(ps_pack_start));
    pes_offset = find_bytes(payload, payload_size, video_pes_start, (int)sizeof(video_pes_start));
    nalu_offset = find_bytes(payload, payload_size, annexb_start, (int)sizeof(annexb_start));

    printf("PS scan: pack_start=%d video_pes=%d annexb_nalu=%d\n", ps_offset, pes_offset, nalu_offset);
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

static void print_rtp_packet_summary(const unsigned char *data, int size)
{
    unsigned int version;
    unsigned int marker;
    unsigned int payload_type;
    unsigned int seq;
    uint32_t timestamp;
    uint32_t ssrc;
    int payload_offset = 12;
    int payload_size;
    int i;

    if (!data || size < 12) {
        printf("===== RTP RX invalid packet len=%d =====\n", size);
        return;
    }

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
    if (payload_size >= 4 && data[payload_offset] == 0x00 && data[payload_offset + 1] == 0x00 &&
        data[payload_offset + 2] == 0x01 && data[payload_offset + 3] == 0xBA) {
        printf("payload type guess: PS pack header 00 00 01 BA\n");
        print_ps_payload_summary(data + payload_offset, payload_size);
    } else if (payload_size > 0 && (data[payload_offset] & 0x1F) == 5) {
        printf("payload type guess: raw H.264 IDR NALU\n");
    } else if (payload_size >= 2 && (data[payload_offset] & 0x1F) == 28) {
        printf("payload type guess: H.264 FU-A\n");
        printf("FU-A detail: indicator=0x%02X header=0x%02X S=%u E=%u type=%u\n",
               data[payload_offset],
               data[payload_offset + 1],
               (unsigned)((data[payload_offset + 1] >> 7) & 0x01),
               (unsigned)((data[payload_offset + 1] >> 6) & 0x01),
               (unsigned)(data[payload_offset + 1] & 0x1F));
    } else {
        printf("payload type guess: unknown/demo payload\n");
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
    char recv_buf[8192];
    unsigned char rtp_buf[2048];
    char reply[8192];
    struct sockaddr_in peer;
    struct sockaddr_in rtp_peer;
    socklen_t peer_len = sizeof(peer);
    socklen_t rtp_peer_len = sizeof(rtp_peer);
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

    printf("GB28181 SIP mock server listening on udp/5060\n");
    printf("GB28181 RTP mock receiver listening on udp/30000\n");

    for (;;) {
        fd_set readfds;
        int maxfd;
        int ret;
        gb28181_sip_message_t msg;
        char from_ip[64];

        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        FD_SET(rtp_sockfd, &readfds);
        maxfd = sockfd > rtp_sockfd ? sockfd : rtp_sockfd;
        ret = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ret <= 0) {
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

    socket_close(rtp_sockfd);
    socket_close(sockfd);
    cleanup_winsock();
    return 0;
}
