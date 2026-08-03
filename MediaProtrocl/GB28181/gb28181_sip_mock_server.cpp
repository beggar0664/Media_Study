#include "gb28181_module.h"

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

int main(void)
{
    /* 最小 SIP mock 平台：处理 REGISTER / MESSAGE / INVITE / ACK / BYE。 */
    int sockfd;
    struct sockaddr_in addr;
    char recv_buf[8192];
    char reply[8192];
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    int registered = 0;
    int invited = 0;
    int acked = 0;

    if (init_winsock() != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        cleanup_winsock();
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5060);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        socket_close(sockfd);
        cleanup_winsock();
        return 1;
    }

    printf("GB28181 SIP mock server listening on udp/5060\n");

    for (;;) {
        int ret = recvfrom(sockfd, recv_buf, sizeof(recv_buf) - 1, 0, (struct sockaddr *)&peer, &peer_len);
        gb28181_sip_message_t msg;
        char from_ip[64];
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
            continue;
        }

        if (registered && strcmp(msg.method, "INVITE") == 0) {
            /* INVITE 返回 SDP，说明平台愿意接受媒体会话。 */
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

    socket_close(sockfd);
    cleanup_winsock();
    return 0;
}
