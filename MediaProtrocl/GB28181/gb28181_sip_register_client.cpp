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

static int send_sip_message(int sockfd, const struct sockaddr_in *addr, const char *msg)
{
    int ret = sendto(sockfd, msg, (int)strlen(msg), 0, (const struct sockaddr *)addr, sizeof(*addr));
    if (ret < 0) {
        perror("sendto");
        return -1;
    }
    return 0;
}

static int recv_sip_message(int sockfd, char *buf, int buf_size, int timeout_ms)
{
    fd_set readfds;
    struct timeval tv;
    int ret;

    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    ret = select(sockfd + 1, &readfds, NULL, NULL, &tv);
    if (ret <= 0) {
        return ret;
    }

    ret = recv(sockfd, buf, buf_size - 1, 0);
    if (ret <= 0) {
        return ret;
    }
    buf[ret] = '\0';
    return ret;
}

static int build_invite_request(const gb28181_config_t *cfg, char *buf, int buf_size)
{
    char sdp[1024];
    char ssrc[16];
    int sdp_len;
    snprintf(ssrc, sizeof(ssrc), "%010u", cfg->ssrc ? cfg->ssrc : 0x12345678u);
    sdp_len = gb28181_build_sdp(cfg, sdp, sizeof(sdp), ssrc);
    if (sdp_len <= 0) {
        return -1;
    }
    return snprintf(buf, buf_size,
        "INVITE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-gb28181-invite\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s-invite\r\n"
        "CSeq: 5 INVITE\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n\r\n"
        "%s",
        cfg->stream_id, cfg->domain,
        cfg->local_ip, cfg->local_sip_port,
        cfg->username, cfg->domain,
        cfg->local_id, cfg->domain,
        cfg->stream_id,
        cfg->username, cfg->local_ip, cfg->local_sip_port,
        sdp_len,
        sdp);
}

static int build_ack_request(const gb28181_config_t *cfg, char *buf, int buf_size)
{
    return snprintf(buf, buf_size,
        "ACK sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-gb28181-ack\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>;tag=mock\r\n"
        "Call-ID: %s-invite\r\n"
        "CSeq: 5 ACK\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Length: 0\r\n\r\n",
        cfg->stream_id, cfg->domain,
        cfg->local_ip, cfg->local_sip_port,
        cfg->username, cfg->domain,
        cfg->local_id, cfg->domain,
        cfg->stream_id,
        cfg->username, cfg->local_ip, cfg->local_sip_port);
}

static int build_bye_request(const gb28181_config_t *cfg, char *buf, int buf_size)
{
    return snprintf(buf, buf_size,
        "BYE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-gb28181-bye\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>;tag=mock\r\n"
        "Call-ID: %s-invite\r\n"
        "CSeq: 6 BYE\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Length: 0\r\n\r\n",
        cfg->stream_id, cfg->domain,
        cfg->local_ip, cfg->local_sip_port,
        cfg->username, cfg->domain,
        cfg->local_id, cfg->domain,
        cfg->stream_id);
}

static void send_message_and_print_response(int sockfd,
                                            const struct sockaddr_in *remote_addr,
                                            const char *title,
                                            const char *request,
                                            char *recv_buf,
                                            int recv_buf_size)
{
    int ret;
    printf("===== %s =====\n%s\n", title, request);
    send_sip_message(sockfd, remote_addr, request);
    ret = recv_sip_message(sockfd, recv_buf, recv_buf_size, 3000);
    if (ret > 0) {
        printf("===== %s RESPONSE =====\n%s\n", title, recv_buf);
    } else {
        printf("No %s response received\n", title);
    }
}

int main(void)
{
    gb28181_config_t cfg;
    gb28181_sip_message_t msg;
    char request[4096];
    char auth_line[1024];
    char uri[256];
    char recv_buf[8192];
    char auth_request[8192];
    gb28181_digest_challenge_t challenge;
    struct sockaddr_in local_addr;
    struct sockaddr_in remote_addr;
    int sockfd;
    int ret;

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.local_id, sizeof(cfg.local_id), "%s", "34020000001320000001");
    snprintf(cfg.domain, sizeof(cfg.domain), "%s", "3402000000");
    snprintf(cfg.username, sizeof(cfg.username), "%s", "34020000001320000001");
    snprintf(cfg.password, sizeof(cfg.password), "%s", "123456");
    snprintf(cfg.sip_server_ip, sizeof(cfg.sip_server_ip), "%s", "127.0.0.1");
    cfg.sip_server_port = 5060;
    snprintf(cfg.local_ip, sizeof(cfg.local_ip), "%s", "127.0.0.1");
    cfg.local_sip_port = 5062;
    snprintf(cfg.stream_id, sizeof(cfg.stream_id), "%s", "34020000001320000001");
    cfg.local_rtp_port = 10000;
    cfg.payload_type = 96;
    cfg.ssrc = 0x12345678;

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

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons((u_short)cfg.local_sip_port);
#ifdef _WIN32
    local_addr.sin_addr.s_addr = inet_addr(cfg.local_ip);
#else
    inet_pton(AF_INET, cfg.local_ip, &local_addr.sin_addr);
#endif

    if (bind(sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind");
#ifdef _WIN32
        closesocket(sockfd);
#else
        close(sockfd);
#endif
        cleanup_winsock();
        return 1;
    }

    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons((u_short)cfg.sip_server_port);
#ifdef _WIN32
    remote_addr.sin_addr.s_addr = inet_addr(cfg.sip_server_ip);
#else
    inet_pton(AF_INET, cfg.sip_server_ip, &remote_addr.sin_addr);
#endif

    gb28181_build_register(&cfg, request, sizeof(request));
    printf("===== First REGISTER =====\n%s\n", request);
    send_sip_message(sockfd, &remote_addr, request);

    ret = recv_sip_message(sockfd, recv_buf, sizeof(recv_buf), 3000);
    if (ret <= 0) {
        printf("No SIP response received\n");
    } else {
        memset(&msg, 0, sizeof(msg));
        if (gb28181_parse_sip_message(recv_buf, &msg) == 0) {
            printf("===== SIP RESPONSE =====\n%s\n", recv_buf);
            printf("status=%d reason=%s\n", msg.status_code, msg.reason);
            if (msg.status_code == 401 && msg.www_authenticate[0] != '\0') {
                if (gb28181_parse_www_authenticate(msg.www_authenticate, &challenge) == 0) {
                    snprintf(uri, sizeof(uri), "sip:%s@%s", cfg.sip_server_ip, cfg.sip_server_ip);
                    if (gb28181_build_digest_authorization(&cfg, "REGISTER", uri, &challenge, auth_line, sizeof(auth_line)) > 0) {
                        snprintf(auth_request, sizeof(auth_request),
                            "REGISTER sip:%s SIP/2.0\r\n"
                            "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-gb28181-register-2\r\n"
                            "From: <sip:%s@%s>;tag=gb28181\r\n"
                            "To: <sip:%s@%s>\r\n"
                            "Call-ID: %s-register\r\n"
                            "CSeq: 2 REGISTER\r\n"
                            "Contact: <sip:%s@%s:%d>\r\n"
                            "%s"
                            "Max-Forwards: 70\r\n"
                            "Expires: 3600\r\n"
                            "Content-Length: 0\r\n\r\n",
                            cfg.sip_server_ip,
                            cfg.local_ip, cfg.local_sip_port,
                            cfg.username, cfg.domain,
                            cfg.local_id, cfg.domain,
                            cfg.stream_id,
                            cfg.username, cfg.local_ip, cfg.local_sip_port,
                            auth_line);
                        printf("===== REGISTER WITH AUTH =====\n%s\n", auth_request);
                        send_sip_message(sockfd, &remote_addr, auth_request);
                        ret = recv_sip_message(sockfd, recv_buf, sizeof(recv_buf), 3000);
                        if (ret > 0) {
                            printf("===== SECOND RESPONSE =====\n%s\n", recv_buf);
                            memset(&msg, 0, sizeof(msg));
                            if (gb28181_parse_sip_message(recv_buf, &msg) == 0 && msg.status_code == 200) {
                                gb28181_build_message_keepalive(&cfg, 3, request, sizeof(request));
                                send_message_and_print_response(sockfd, &remote_addr, "MESSAGE Keepalive", request, recv_buf, sizeof(recv_buf));
                                gb28181_build_message_catalog(&cfg, 4, request, sizeof(request));
                                send_message_and_print_response(sockfd, &remote_addr, "MESSAGE Catalog", request, recv_buf, sizeof(recv_buf));
                                build_invite_request(&cfg, request, sizeof(request));
                                printf("===== INVITE + SDP =====\n%s\n", request);
                                send_sip_message(sockfd, &remote_addr, request);
                                ret = recv_sip_message(sockfd, recv_buf, sizeof(recv_buf), 3000);
                                if (ret > 0) {
                                    printf("===== INVITE RESPONSE =====\n%s\n", recv_buf);
                                    memset(&msg, 0, sizeof(msg));
                                    if (gb28181_parse_sip_message(recv_buf, &msg) == 0 && msg.status_code == 200) {
                                        build_ack_request(&cfg, request, sizeof(request));
                                        printf("===== ACK =====\n%s\n", request);
                                        send_sip_message(sockfd, &remote_addr, request);
                                        build_bye_request(&cfg, request, sizeof(request));
                                        printf("===== BYE =====\n%s\n", request);
                                        send_sip_message(sockfd, &remote_addr, request);
                                        ret = recv_sip_message(sockfd, recv_buf, sizeof(recv_buf), 3000);
                                        if (ret > 0) {
                                            printf("===== BYE RESPONSE =====\n%s\n", recv_buf);
                                        } else {
                                            printf("No BYE response received\n");
                                        }
                                    }
                                } else {
                                    printf("No INVITE response received\n");
                                }
                            }
                        } else {
                            printf("No second SIP response received\n");
                        }
                    }
                }
            }
        }
    }

#ifdef _WIN32
    closesocket(sockfd);
#else
    close(sockfd);
#endif
    cleanup_winsock();
    return 0;
}
