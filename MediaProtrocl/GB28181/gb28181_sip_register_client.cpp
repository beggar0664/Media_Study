#include "gb28181_module.h"

/*
 * 最小 GB28181 SIP 客户端学习示例。
 *
 * 对应文档：
 * - ../gb28181_study.md：REGISTER Digest、MESSAGE、INVITE/SDP 时序图
 * - ../../current_code_learning_guide.md：运行 mock server/client 和 Wireshark 抓包方法
 *
 * 当前流程：
 * REGISTER(无 Authorization) -> 401 -> REGISTER + Authorization -> 200 OK
 * -> MESSAGE Keepalive -> MESSAGE Catalog -> MESSAGE DeviceInfo -> MESSAGE DeviceStatus
 * -> INVITE + SDP -> ACK -> BYE
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
    /* INVITE 携带 SDP，告诉平台我要开哪一路媒体会话。 */
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
    /* ACK 确认 INVITE 事务，表示双方对会话参数已确认。 */
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
    /* BYE 结束会话。 */
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
    /* MESSAGE 走同样的 SIP 收发逻辑，只是 body 内容换成 XML。 */
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

static void print_xml_tag_if_present(const char *xml, const char *tag)
{
    char value[128];
    value[0] = '\0';
    if (gb28181_extract_xml_tag(xml, tag, value, sizeof(value)) >= 0 && value[0] != '\0') {
        printf("  %-12s: %s\n", tag, value);
    }
}

static void print_catalog_items(const char *xml);

static void print_message_xml_summary(const char *title, const gb28181_sip_message_t *msg)
{
    /*
     * 学习用 XML 摘要：原始 SIP/MESSAGE 已经完整打印，这里只把 body 里的业务字段再列一遍。
     * 对应文档：../gb28181_study.md 的 MESSAGE / Catalog / DeviceInfo / DeviceStatus 章节。
     */
    char cmd_type[64];

    if (!msg || !msg->body || msg->body[0] == '\0') {
        return;
    }

    cmd_type[0] = '\0';
    gb28181_extract_xml_tag(msg->body, "CmdType", cmd_type, sizeof(cmd_type));
    if (cmd_type[0] == '\0') {
        return;
    }

    printf("===== %s XML SUMMARY =====\n", title);
    print_xml_tag_if_present(msg->body, "CmdType");
    print_xml_tag_if_present(msg->body, "SN");
    print_xml_tag_if_present(msg->body, "DeviceID");

    if (strcmp(cmd_type, "Catalog") == 0) {
        print_xml_tag_if_present(msg->body, "SumNum");
        print_xml_tag_if_present(msg->body, "Name");
        print_xml_tag_if_present(msg->body, "Manufacturer");
        print_xml_tag_if_present(msg->body, "Model");
        print_xml_tag_if_present(msg->body, "Owner");
        print_xml_tag_if_present(msg->body, "CivilCode");
        print_xml_tag_if_present(msg->body, "Parental");
        print_xml_tag_if_present(msg->body, "ParentID");
        print_xml_tag_if_present(msg->body, "Status");
        print_catalog_items(msg->body);
    } else if (strcmp(cmd_type, "DeviceInfo") == 0) {
        print_xml_tag_if_present(msg->body, "DeviceName");
        print_xml_tag_if_present(msg->body, "Manufacturer");
        print_xml_tag_if_present(msg->body, "Model");
        print_xml_tag_if_present(msg->body, "Firmware");
        print_xml_tag_if_present(msg->body, "Result");
    } else if (strcmp(cmd_type, "DeviceStatus") == 0) {
        print_xml_tag_if_present(msg->body, "Online");
        print_xml_tag_if_present(msg->body, "Status");
        print_xml_tag_if_present(msg->body, "Encode");
        print_xml_tag_if_present(msg->body, "Record");
    }
}

static void print_catalog_items(const char *xml)
{
    const char *cursor;
    int index = 0;

    if (!xml) {
        return;
    }

    cursor = xml;
    printf("===== CATALOG ITEMS =====\n");
    while ((cursor = strstr(cursor, "<Item>")) != NULL) {
        const char *end = strstr(cursor, "</Item>");
        char item_xml[2048];
        char value[128];

        if (!end) {
            break;
        }
        if (end <= cursor) {
            cursor += 6;
            continue;
        }
        if ((size_t)(end - cursor) >= sizeof(item_xml)) {
            cursor = end + 7;
            continue;
        }

        memcpy(item_xml, cursor, (size_t)(end - cursor));
        item_xml[end - cursor] = '\0';

        printf("  Item #%d\n", ++index);
        if (gb28181_extract_xml_tag(item_xml, "DeviceID", value, sizeof(value)) >= 0 && value[0] != '\0') {
            printf("    DeviceID    : %s\n", value);
        }
        if (gb28181_extract_xml_tag(item_xml, "Name", value, sizeof(value)) >= 0 && value[0] != '\0') {
            printf("    Name        : %s\n", value);
        }
        if (gb28181_extract_xml_tag(item_xml, "Manufacturer", value, sizeof(value)) >= 0 && value[0] != '\0') {
            printf("    Manufacturer: %s\n", value);
        }
        if (gb28181_extract_xml_tag(item_xml, "Model", value, sizeof(value)) >= 0 && value[0] != '\0') {
            printf("    Model       : %s\n", value);
        }
        if (gb28181_extract_xml_tag(item_xml, "ParentID", value, sizeof(value)) >= 0 && value[0] != '\0') {
            printf("    ParentID    : %s\n", value);
        }
        if (gb28181_extract_xml_tag(item_xml, "Status", value, sizeof(value)) >= 0 && value[0] != '\0') {
            printf("    Status      : %s\n", value);
        }
        cursor = end + 7;
    }

    if (index == 0) {
        printf("  <no item found>\n");
    }
}

static void send_query_and_print_responses(int sockfd,
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
        printf("===== %s RESPONSE #1 =====\n%s\n", title, recv_buf);
    } else {
        printf("No %s response #1 received\n", title);
        return;
    }

    ret = recv_sip_message(sockfd, recv_buf, recv_buf_size, 3000);
    if (ret > 0) {
        gb28181_sip_message_t msg;
        printf("===== %s RESPONSE #2 =====\n%s\n", title, recv_buf);
        memset(&msg, 0, sizeof(msg));
        if (gb28181_parse_sip_message(recv_buf, &msg) == 0 && strcmp(msg.method, "MESSAGE") == 0) {
            char ok[2048];
            print_message_xml_summary(title, &msg);
            snprintf(ok, sizeof(ok),
                "SIP/2.0 200 OK\r\n"
                "Via: %s\r\n"
                "From: %s\r\n"
                "To: %s\r\n"
                "Call-ID: %s\r\n"
                "CSeq: %d MESSAGE\r\n"
                "Content-Length: 0\r\n\r\n",
                msg.via, msg.from, msg.to, msg.call_id, msg.cseq);
            printf("===== %s RESPONSE #2 ACK =====\n%s\n", title, ok);
            send_sip_message(sockfd, remote_addr, ok);
        }
    } else {
        printf("No %s response #2 received\n", title);
    }
}

int main(void)
{
    /* 最小 SIP 客户端演示：REGISTER -> MESSAGE -> INVITE -> ACK -> BYE。 */
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
    /* 第一次 REGISTER：没有 Authorization，故意触发 401。 */
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
                /* 根据 401 的 challenge 再构造带 Authorization 的第二次 REGISTER。 */
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
                                /* 注册成功后，先发两个 MESSAGE，学保活和目录查询。 */
                                gb28181_build_message_keepalive(&cfg, 3, request, sizeof(request));
                                send_message_and_print_response(sockfd, &remote_addr, "MESSAGE Keepalive", request, recv_buf, sizeof(recv_buf));
                                gb28181_build_message_catalog(&cfg, 4, request, sizeof(request));
                                send_query_and_print_responses(sockfd, &remote_addr, "MESSAGE Catalog", request, recv_buf, sizeof(recv_buf));
                                gb28181_build_message_device_info_query(&cfg, 6, request, sizeof(request));
                                send_query_and_print_responses(sockfd, &remote_addr, "MESSAGE DeviceInfo", request, recv_buf, sizeof(recv_buf));
                                gb28181_build_message_device_status_query(&cfg, 8, request, sizeof(request));
                                send_query_and_print_responses(sockfd, &remote_addr, "MESSAGE DeviceStatus", request, recv_buf, sizeof(recv_buf));
                                /* 再发 INVITE，开始媒体会话协商；真正发媒体前还要等 200 OK + SDP。 */
                                build_invite_request(&cfg, request, sizeof(request));
                                printf("===== INVITE + SDP =====\n%s\n", request);
                                send_sip_message(sockfd, &remote_addr, request);
                                ret = recv_sip_message(sockfd, recv_buf, sizeof(recv_buf), 3000);
                                if (ret > 0) {
                                    printf("===== INVITE RESPONSE =====\n%s\n", recv_buf);
                                    memset(&msg, 0, sizeof(msg));
                                    if (gb28181_parse_sip_message(recv_buf, &msg) == 0 && msg.status_code == 200) {
                                        /* 200 OK 后发 ACK，这一步表示双方都接受了 SDP 参数，媒体会话正式建立。 */
                                        build_ack_request(&cfg, request, sizeof(request));
                                        printf("===== ACK =====\n%s\n", request);
                                        send_sip_message(sockfd, &remote_addr, request);
                                        /* ACK 之后才进入真正的媒体阶段；这里为了演示，马上用 BYE 结束会话。 */
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
