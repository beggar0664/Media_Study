#include "gb28181_module.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include "rtpipv4address.h"
#include "rtpsession.h"
#include "rtpsessionparams.h"
#include "rtpudpv4transmitter.h"
#include "rtptcptransmitter.h"
#include "rtptcpaddress.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#endif

using namespace jrtplib;

/*
 * 本文件分四层（按职责读，不必从头到尾）：
 *   1. 配置与生命周期（gb28181_config_t / create/start/stop/destroy）
 *      - start 的 UDP 分支用 RTPUDPv4Transmitter（绑端口+AddDestination）
 *      - start 的 TCP 分支：设备作 client，自建 socket connect + RTPTCPAddress
 *   2. SIP 报文构造（build_register/invite/bye/sdp）
 *      - build_sdp 按 use_tcp 写 RTP/AVP 或 TCP/RTP/AVP
 *   3. MESSAGE + XML 命令（build_xml_message + build_message_*）  ← 理解 GB28181 的核心
 *      - 公共壳 build_xml_message：所有 MESSAGE 外层相同，差别只在 <CmdType>
 *      - Keepalive/Catalog/DeviceInfo/DeviceStatus 只是换字符串；DeviceControl/RecordInfo 带参数
 *   4. RTP/PS/分片（send_rtp_packet/send_rtp_payload_fragmented/send_h264_fu_a/send_h265_fu/build_ps_pack_h264）
 *      - 三层发送原语：单包 < 通用字节切片 < H.264 FU-A / H.265 FU 语义分片
 *
 * 三条主线（对照读，理解演进）：
 *   A. 直线→状态机：gb28181_sip_register_client.cpp(直线) vs gb28181_device_stateful.cpp(状态机)
 *   B. 单包→真实码流：gb28181_minimal_example.cpp(固定帧) vs stateful 的 media_streaming_tick(逐帧)
 *   C. 被动→真平台：mock 最初只回响应 vs 现在主动下发 Query/INVITE/PTZ/RecordInfo
 *
 * 详细函数清单见 gb28181_code_reference.md 第 4 节，状态机导读见第 9 节，学习路径见第 10 节。
 */

typedef struct gb28181_context_s {
    /* 模块运行时上下文：保存配置、RTP 会话和发送状态。 */
    gb28181_config_t config;
    int started;
    unsigned short rtp_seq;
    unsigned int rtp_timestamp;
    unsigned int ssrc;
    RTPSession *rtp_session;
#ifdef _WIN32
    int winsock_started;
#endif
} gb28181_context_t;

static void gb28181_init_default_config(gb28181_config_t *cfg)
{
    /* 给学习示例准备一组可直接跑通的默认值。 */
    memset(cfg, 0, sizeof(*cfg));
    cfg->sip_server_port = 5060;
    cfg->local_sip_port = 5060;
    cfg->local_rtp_port = 10000;
    cfg->media_port = 10000;
    cfg->payload_type = 96;
    cfg->use_tcp = 0;
    cfg->enable_dump = 0;
    cfg->ssrc = 0x12345678;
}

static const char *cfg_local_ip(const gb28181_config_t *cfg)
{
    /* local_ip 和 media_ip 兼容，优先使用明确设置的 local_ip。 */
    if (cfg->local_ip[0] != '\0') {
        return cfg->local_ip;
    }
    return cfg->media_ip;
}

static int cfg_local_sip_port(const gb28181_config_t *cfg)
{
    /* SIP 本地端口，缺省回落到 5060。 */
    return cfg->local_sip_port > 0 ? cfg->local_sip_port : 5060;
}

static int cfg_local_rtp_port(const gb28181_config_t *cfg)
{
    /* RTP 本地端口，兼容 media_port 旧字段。 */
    if (cfg->local_rtp_port > 0) {
        return cfg->local_rtp_port;
    }
    return cfg->media_port > 0 ? cfg->media_port : 10000;
}

static const char *cfg_remote_rtp_ip(const gb28181_config_t *cfg)
{
    /* 远端 RTP 地址，默认用 SIP 服务器地址。 */
    if (cfg->remote_rtp_ip[0] != '\0') {
        return cfg->remote_rtp_ip;
    }
    return cfg->sip_server_ip;
}

static int cfg_remote_rtp_port(const gb28181_config_t *cfg)
{
    /* 远端 RTP 端口，默认跟本地 RTP 端口一致，便于学习演示。 */
    return cfg->remote_rtp_port > 0 ? cfg->remote_rtp_port : cfg_local_rtp_port(cfg);
}

static unsigned int cfg_ssrc(const gb28181_config_t *cfg)
{
    /* SSRC 是 RTP 同步源标识。 */
    return cfg->ssrc != 0 ? cfg->ssrc : 0x12345678;
}

static void copy_config(gb28181_config_t *dst, const gb28181_config_t *src)
{
    /* 先填默认值，再覆盖用户传入值，最后补齐别名字段。 */
    gb28181_init_default_config(dst);
    if (!src) {
        return;
    }
    memcpy(dst, src, sizeof(*dst));
    if (dst->local_ip[0] == '\0' && dst->media_ip[0] != '\0') {
        snprintf(dst->local_ip, sizeof(dst->local_ip), "%s", dst->media_ip);
    }
    if (dst->media_ip[0] == '\0' && dst->local_ip[0] != '\0') {
        snprintf(dst->media_ip, sizeof(dst->media_ip), "%s", dst->local_ip);
    }
    if (dst->local_rtp_port <= 0 && dst->media_port > 0) {
        dst->local_rtp_port = dst->media_port;
    }
    if (dst->media_port <= 0 && dst->local_rtp_port > 0) {
        dst->media_port = dst->local_rtp_port;
    }
    if (dst->local_sip_port <= 0) {
        dst->local_sip_port = 5060;
    }
    if (dst->sip_server_port <= 0) {
        dst->sip_server_port = 5060;
    }
    if (dst->payload_type <= 0) {
        dst->payload_type = 96;
    }
}

gb28181_handle_t gb28181_create(const gb28181_config_t *config)
{
    /* 创建模块上下文，不启动 RTP，只准备配置和状态。 */
    gb28181_context_t *ctx = (gb28181_context_t *)calloc(1, sizeof(gb28181_context_t));
    if (!ctx) {
        return NULL;
    }
    copy_config(&ctx->config, config);
    ctx->rtp_seq = 1;
    ctx->rtp_timestamp = 0;
    ctx->ssrc = cfg_ssrc(&ctx->config);
    return ctx;
}

int gb28181_start(gb28181_handle_t handle)
{
    /* 启动 RTP 会话：创建 jrtplib session、绑定端口、添加远端地址。
     * UDP：jrtplib 自己绑端口 + AddDestination(IP,port)。
     * TCP：jrtplib 的 TCP transmitter 需要一个已连接的 socket fd，
     *      所以本函数自己先 socket()+connect() 到平台，再把 fd 给 jrtplib。
     *      这是"设备作 TCP client 主动连平台"模式（GB28181 被动收流的常见形态）。 */
    gb28181_context_t *ctx = (gb28181_context_t *)handle;
    if (!ctx || ctx->started) {
        return ctx ? 0 : -1;
    }

#ifdef _WIN32
    if (!ctx->winsock_started) {
        WSADATA wsa;
        int wsa_status = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (wsa_status != 0) {
            printf("[gb28181] WSAStartup failed: %d\n", wsa_status);
            return -4;
        }
        ctx->winsock_started = 1;
    }
#endif

    RTPSessionParams session_params;
    session_params.SetOwnTimestampUnit(1.0 / 90000.0);
    session_params.SetAcceptOwnPackets(false);
    session_params.SetUsePredefinedSSRC(true);
    session_params.SetPredefinedSSRC(ctx->ssrc);
    session_params.SetMaximumPacketSize(1400);
    /* RTCP：jrtplib 默认就自动发 SR/RR（默认 5s 间隔）。
     * 学习用调到 1s，让 mock 接收端在 STREAMING 期间每秒能看到一条 SR。
     * 生产环境通常保留 5s。这行只是配置，不手写 RTCP 协议。 */
    session_params.SetMinimumRTCPTransmissionInterval(RTPTime(1.0));

    RTPSession *session = new RTPSession();
    int status;

    if (ctx->config.use_tcp) {
        /* ---- TCP 承载分支 ----
         * 设备作 client，主动 connect 到平台的 RTP TCP 端口。
         * 建好连接后把 socket fd 交给 jrtplib 的 RTPTCPTransmitter。 */
        RTPTCPTransmissionParams trans_params;
        status = session->Create(session_params, &trans_params, RTPTransmitter::TCPProto);
        if (status < 0) {
            printf("[gb28181] RTPSession::Create(TCP) failed: %d\n", status);
            delete session;
            return status;
        }

        /* 自建 TCP socket 并 connect 到平台 remote_rtp_ip:remote_rtp_port。 */
        int tcp_sock = (int)socket(AF_INET, SOCK_STREAM, 0);
        if (tcp_sock < 0) {
            printf("[gb28181] TCP socket creation failed\n");
            session->Destroy();
            delete session;
            return -5;
        }
        struct sockaddr_in tcp_addr;
        memset(&tcp_addr, 0, sizeof(tcp_addr));
        tcp_addr.sin_family = AF_INET;
        tcp_addr.sin_port = htons((u_short)cfg_remote_rtp_port(&ctx->config));
#ifdef _WIN32
        tcp_addr.sin_addr.s_addr = inet_addr(cfg_remote_rtp_ip(&ctx->config));
#else
        inet_pton(AF_INET, cfg_remote_rtp_ip(&ctx->config), &tcp_addr.sin_addr);
#endif
        if (connect(tcp_sock, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr)) < 0) {
            printf("[gb28181] TCP connect to %s:%d failed\n",
                   cfg_remote_rtp_ip(&ctx->config), cfg_remote_rtp_port(&ctx->config));
#ifdef _WIN32
            closesocket(tcp_sock);
#else
            close(tcp_sock);
#endif
            session->Destroy();
            delete session;
            return -6;
        }
        printf("[gb28181] TCP connected to %s:%d\n",
               cfg_remote_rtp_ip(&ctx->config), cfg_remote_rtp_port(&ctx->config));

        /* 把已连接的 socket fd 交给 jrtplib，后续 SendPacket 走这条 TCP。 */
        RTPTCPAddress dest((SocketType)tcp_sock);
        status = session->AddDestination(dest);
        if (status < 0) {
            printf("[gb28181] TCP AddDestination failed: %d\n", status);
#ifdef _WIN32
            closesocket(tcp_sock);
#else
            close(tcp_sock);
#endif
            session->Destroy();
            delete session;
            return status;
        }
    } else {
        /* ---- UDP 承载分支（原逻辑） ---- */
        RTPUDPv4TransmissionParams trans_params;
        trans_params.SetPortbase((uint16_t)cfg_local_rtp_port(&ctx->config));

        uint32_t bind_ip = inet_addr(cfg_local_ip(&ctx->config));
        if (bind_ip != INADDR_NONE) {
            trans_params.SetBindIP(ntohl(bind_ip));
        }

        status = session->Create(session_params, &trans_params, RTPTransmitter::IPv4UDPProto);
        if (status < 0) {
            printf("[gb28181] RTPSession::Create(UDP) failed: %d\n", status);
            delete session;
            return status;
        }

        uint32_t remote_ip = inet_addr(cfg_remote_rtp_ip(&ctx->config));
        if (remote_ip == INADDR_NONE) {
            session->Destroy();
            delete session;
            return -3;
        }
        RTPIPv4Address dest(ntohl(remote_ip), (uint16_t)cfg_remote_rtp_port(&ctx->config));
        status = session->AddDestination(dest);
        if (status < 0) {
            printf("[gb28181] RTPSession::AddDestination failed: %d\n", status);
            session->Destroy();
            delete session;
            return status;
        }
    }

    session->SetDefaultPayloadType((uint8_t)ctx->config.payload_type);

    ctx->rtp_session = session;
    ctx->started = 1;
    return 0;
}

void gb28181_stop(gb28181_handle_t handle)
{
    /* 释放 RTP 会话和平台相关网络资源。 */
    gb28181_context_t *ctx = (gb28181_context_t *)handle;
    if (!ctx) {
        return;
    }
    if (ctx->rtp_session) {
        ctx->rtp_session->Destroy();
        delete ctx->rtp_session;
        ctx->rtp_session = NULL;
    }
#ifdef _WIN32
    if (ctx->winsock_started) {
        WSACleanup();
        ctx->winsock_started = 0;
    }
#endif
    ctx->started = 0;
}

void gb28181_destroy(gb28181_handle_t handle)
{
    gb28181_context_t *ctx = (gb28181_context_t *)handle;
    if (!ctx) {
        return;
    }
    gb28181_stop(handle);
    free(ctx);
}

int gb28181_build_register(const gb28181_config_t *config, char *buf, int buf_size)
{
    /* 生成第一次 REGISTER，请求还没有 Authorization。 */
    if (!config || !buf || buf_size <= 0) {
        return -1;
    }
    return snprintf(buf, buf_size,
        "REGISTER sip:%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-gb28181-register\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s-register\r\n"
        "CSeq: 1 REGISTER\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Max-Forwards: 70\r\n"
        "Expires: 3600\r\n"
        "Content-Length: 0\r\n\r\n",
        config->sip_server_ip,
        cfg_local_ip(config), cfg_local_sip_port(config),
        config->username, config->domain,
        config->local_id, config->domain,
        config->stream_id,
        config->username, cfg_local_ip(config), cfg_local_sip_port(config));
}

/* 生成最小 SDP：告诉对端视频类型、RTP 端口、payload type、方向和 SSRC。 */
int gb28181_build_sdp(const gb28181_config_t *config, char *buf, int buf_size, const char *ssrc)
{
    /*
     * 生成最小 SDP：告诉对端视频类型、RTP 端口、payload type、方向和 SSRC。
     * 这些字段要和后续 RTP 包对应起来看：
     *   m=video <port> RTP/AVP <pt>  -> UDP 目的端口和 RTP payload type
     *   a=rtpmap:<pt> H264/90000     -> pt 的编码语义和 RTP timestamp 时钟
     *   a=ssrc:<ssrc>                -> RTP header 里的 SSRC
     * 注意：GB28181 常见实际负载是 PS over RTP，SDP 说明编码是 H.264，RTP payload 里可以先看到 PS 头 00 00 01 BA。
     */
    if (!config || !buf || buf_size <= 0 || !ssrc) {
        return -1;
    }
    if (config->use_tcp) {
        /* TCP 承载：m= 用 TCP/RTP/AVP，setup:active 表示设备主动连平台，
         * connection:new 表示新建 TCP 连接（GB28181 TCP 被动收流常见写法）。 */
        return snprintf(buf, buf_size,
            "v=0\r\n"
            "o=%s 0 0 IN IP4 %s\r\n"
            "s=Play\r\n"
            "c=IN IP4 %s\r\n"
            "t=0 0\r\n"
            "m=video %d TCP/RTP/AVP %d\r\n"
            "a=sendonly\r\n"
            "a=setup:active\r\n"
            "a=connection:new\r\n"
            "a=rtpmap:%d H264/90000\r\n"
            "a=ssrc:%s\r\n",
            config->local_id,
            cfg_local_ip(config),
            cfg_local_ip(config),
            cfg_local_rtp_port(config),
            config->payload_type,
            config->payload_type,
            ssrc);
    }
    return snprintf(buf, buf_size,
        "v=0\r\n"
        "o=%s 0 0 IN IP4 %s\r\n"
        "s=Play\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=video %d RTP/AVP %d\r\n"
        "a=sendonly\r\n"
        "a=rtpmap:%d H264/90000\r\n"
        "a=ssrc:%s\r\n",
        config->local_id,
        cfg_local_ip(config),
        cfg_local_ip(config),
        cfg_local_rtp_port(config),
        config->payload_type,
        config->payload_type,
        ssrc);
}

int gb28181_build_invite(const gb28181_config_t *config, char *buf, int buf_size)
{
    /* INVITE 携带 SDP，用于发起媒体会话。 */
    char sdp[1024];
    char ssrc[16];
    int sdp_len;

    if (!config || !buf || buf_size <= 0) {
        return -1;
    }
    snprintf(ssrc, sizeof(ssrc), "%010u", cfg_ssrc(config));
    sdp_len = gb28181_build_sdp(config, sdp, sizeof(sdp), ssrc);
    if (sdp_len < 0 || sdp_len >= (int)sizeof(sdp)) {
        return -1;
    }
    return snprintf(buf, buf_size,
        "INVITE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-gb28181-invite\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s-invite\r\n"
        "CSeq: 2 INVITE\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n\r\n"
        "%s",
        config->stream_id, config->domain,
        cfg_local_ip(config), cfg_local_sip_port(config),
        config->username, config->domain,
        config->local_id, config->domain,
        config->stream_id,
        config->username, cfg_local_ip(config), cfg_local_sip_port(config),
        sdp_len,
        sdp);
}

int gb28181_build_bye(const gb28181_config_t *config, char *buf, int buf_size)
{
    /* BYE 用于结束对话。 */
    if (!config || !buf || buf_size <= 0) {
        return -1;
    }
    return snprintf(buf, buf_size,
        "BYE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-gb28181-bye\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s-bye\r\n"
        "CSeq: 3 BYE\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Length: 0\r\n\r\n",
        config->stream_id, config->domain,
        cfg_local_ip(config), cfg_local_sip_port(config),
        config->username, config->domain,
        config->local_id, config->domain,
        config->stream_id);
}

static int build_xml_message(const gb28181_config_t *config,
                             const char *cmd_type,
                             int sn,
                             int cseq,
                             char *buf,
                             int buf_size)
{
    /* 生成 MESSAGE 的 XML body，并把它封装成完整 SIP 报文。 */
    char body[1024];
    int body_len;

    if (!config || !cmd_type || !buf || buf_size <= 0) {
        return -1;
    }

    if (strcmp(cmd_type, "Keepalive") == 0) {
        body_len = snprintf(body, sizeof(body),
            "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
            "<Notify>\r\n"
            "<CmdType>Keepalive</CmdType>\r\n"
            "<SN>%d</SN>\r\n"
            "<DeviceID>%s</DeviceID>\r\n"
            "<Status>OK</Status>\r\n"
            "</Notify>\r\n",
            sn,
            config->local_id);
    } else {
        body_len = snprintf(body, sizeof(body),
            "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
            "<Query>\r\n"
            "<CmdType>%s</CmdType>\r\n"
            "<SN>%d</SN>\r\n"
            "<DeviceID>%s</DeviceID>\r\n"
            "</Query>\r\n",
            cmd_type,
            sn,
            config->local_id);
    }

    if (body_len < 0 || body_len >= (int)sizeof(body)) {
        return -2;
    }

    return snprintf(buf, buf_size,
        "MESSAGE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-gb28181-message-%d\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s-message-%d\r\n"
        "CSeq: %d MESSAGE\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Type: Application/MANSCDP+xml\r\n"
        "Content-Length: %d\r\n\r\n"
        "%s",
        config->sip_server_ip, config->domain,
        cfg_local_ip(config), cfg_local_sip_port(config), cseq,
        config->username, config->domain,
        config->sip_server_ip, config->domain,
        config->stream_id, cseq,
        cseq,
        config->username, cfg_local_ip(config), cfg_local_sip_port(config),
        body_len,
        body);
}

int gb28181_build_message_keepalive(const gb28181_config_t *config, int cseq, char *buf, int buf_size)
{
    /* Keepalive 查询/通知。 */
    return build_xml_message(config, "Keepalive", cseq, cseq, buf, buf_size);
}

int gb28181_build_message_catalog(const gb28181_config_t *config, int cseq, char *buf, int buf_size)
{
    /* 目录查询。 */
    return build_xml_message(config, "Catalog", cseq, cseq, buf, buf_size);
}

int gb28181_build_message_device_info_query(const gb28181_config_t *config, int cseq, char *buf, int buf_size)
{
    /* 设备信息查询。 */
    return build_xml_message(config, "DeviceInfo", cseq, cseq, buf, buf_size);
}

int gb28181_build_message_device_status_query(const gb28181_config_t *config, int cseq, char *buf, int buf_size)
{
    /* 设备状态查询。 */
    return build_xml_message(config, "DeviceStatus", cseq, cseq, buf, buf_size);
}

int gb28181_build_message_catalog_response(const gb28181_config_t *config, int cseq, char *buf, int buf_size)
{
    char body[2048];
    int body_len;

    /* 最小目录响应：返回固定的通道列表，便于学习查询/响应闭环。 */
    if (!config || !buf || buf_size <= 0) {
        return -1;
    }

    /*
     * 这里返回的是“学习用固定目录”，不是动态设备树。
     * 先保留两条通道：一条在线、一条离线，方便后续在 client 里练习
     * Catalog 解析、在线通道筛选和 INVITE 目标选择。
     */
    body_len = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
        "<Response>\r\n"
        "<CmdType>Catalog</CmdType>\r\n"
        "<SN>%d</SN>\r\n"
        "<DeviceID>%s</DeviceID>\r\n"
        "<SumNum>2</SumNum>\r\n"
        "<DeviceList Num=\"2\">\r\n"
        "<Item>\r\n"
        "<DeviceID>34020000001320000001</DeviceID>\r\n"
        "<Name>Camera-01</Name>\r\n"
        "<Manufacturer>MockVendor</Manufacturer>\r\n"
        "<Model>IPC-MOCK-01</Model>\r\n"
        "<Owner>3402000000</Owner>\r\n"
        "<CivilCode>340200</CivilCode>\r\n"
        "<Address>Mock Address</Address>\r\n"
        "<Parental>0</Parental>\r\n"
        "<ParentID>34020000002000000001</ParentID>\r\n"
        "<SafetyWay>0</SafetyWay>\r\n"
        "<RegisterWay>1</RegisterWay>\r\n"
        "<Secrecy>0</Secrecy>\r\n"
        "<Status>ON</Status>\r\n"
        "</Item>\r\n"
        "<Item>\r\n"
        "<DeviceID>34020000001320000002</DeviceID>\r\n"
        "<Name>Camera-02</Name>\r\n"
        "<Manufacturer>MockVendor</Manufacturer>\r\n"
        "<Model>IPC-MOCK-02</Model>\r\n"
        "<Owner>3402000000</Owner>\r\n"
        "<CivilCode>340200</CivilCode>\r\n"
        "<Address>Mock Address 2</Address>\r\n"
        "<Parental>0</Parental>\r\n"
        "<ParentID>34020000002000000001</ParentID>\r\n"
        "<SafetyWay>0</SafetyWay>\r\n"
        "<RegisterWay>1</RegisterWay>\r\n"
        "<Secrecy>0</Secrecy>\r\n"
        "<Status>OFF</Status>\r\n"
        "</Item>\r\n"
        "</DeviceList>\r\n"
        "</Response>\r\n",
        cseq,
        config->local_id);

    if (body_len < 0 || body_len >= (int)sizeof(body)) {
        return -2;
    }

    return snprintf(buf, buf_size,
        "MESSAGE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-gb28181-message-%d\r\n"
        "From: <sip:%s@%s>;tag=mock\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s-message-%d\r\n"
        "CSeq: %d MESSAGE\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Type: Application/MANSCDP+xml\r\n"
        "Content-Length: %d\r\n\r\n"
        "%s",
        config->sip_server_ip, config->domain,
        cfg_local_ip(config), cfg_local_sip_port(config), cseq,
        config->sip_server_ip, config->domain,
        config->username, config->domain,
        config->stream_id, cseq,
        cseq,
        config->sip_server_ip, cfg_local_ip(config), cfg_local_sip_port(config),
        body_len,
        body);
}

int gb28181_build_message_device_info(const gb28181_config_t *config, int cseq, char *buf, int buf_size)
{
    char body[1024];
    int body_len;

    if (!config || !buf || buf_size <= 0) {
        return -1;
    }

    /* 最小设备信息响应：字段固定，便于学习响应格式。 */
    body_len = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
        "<Response>\r\n"
        "<CmdType>DeviceInfo</CmdType>\r\n"
        "<SN>%d</SN>\r\n"
        "<DeviceID>%s</DeviceID>\r\n"
        "<DeviceName>Mock IPC</DeviceName>\r\n"
        "<Manufacturer>MockVendor</Manufacturer>\r\n"
        "<Model>IPC-MOCK-01</Model>\r\n"
        "<Firmware>1.0.0</Firmware>\r\n"
        "<Result>OK</Result>\r\n"
        "</Response>\r\n",
        cseq,
        config->local_id);

    if (body_len < 0 || body_len >= (int)sizeof(body)) {
        return -2;
    }

    return snprintf(buf, buf_size,
        "MESSAGE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-gb28181-message-%d\r\n"
        "From: <sip:%s@%s>;tag=mock\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s-message-%d\r\n"
        "CSeq: %d MESSAGE\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Type: Application/MANSCDP+xml\r\n"
        "Content-Length: %d\r\n\r\n"
        "%s",
        config->sip_server_ip, config->domain,
        cfg_local_ip(config), cfg_local_sip_port(config), cseq,
        config->sip_server_ip, config->domain,
        config->username, config->domain,
        config->stream_id, cseq,
        cseq,
        config->sip_server_ip, cfg_local_ip(config), cfg_local_sip_port(config),
        body_len,
        body);
}

int gb28181_build_message_device_status(const gb28181_config_t *config, int cseq, char *buf, int buf_size)
{
    char body[1024];
    int body_len;

    if (!config || !buf || buf_size <= 0) {
        return -1;
    }

    /* 最小设备状态响应：字段固定，便于学习响应格式。 */
    body_len = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
        "<Response>\r\n"
        "<CmdType>DeviceStatus</CmdType>\r\n"
        "<SN>%d</SN>\r\n"
        "<DeviceID>%s</DeviceID>\r\n"
        "<Online>ONLINE</Online>\r\n"
        "<Status>OK</Status>\r\n"
        "<Encode>H264</Encode>\r\n"
        "<Record>ON</Record>\r\n"
        "</Response>\r\n",
        cseq,
        config->local_id);

    if (body_len < 0 || body_len >= (int)sizeof(body)) {
        return -2;
    }

    return snprintf(buf, buf_size,
        "MESSAGE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-gb28181-message-%d\r\n"
        "From: <sip:%s@%s>;tag=mock\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s-message-%d\r\n"
        "CSeq: %d MESSAGE\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Type: Application/MANSCDP+xml\r\n"
        "Content-Length: %d\r\n\r\n"
        "%s",
        config->sip_server_ip, config->domain,
        cfg_local_ip(config), cfg_local_sip_port(config), cseq,
        config->sip_server_ip, config->domain,
        config->username, config->domain,
        config->stream_id, cseq,
        cseq,
        config->sip_server_ip, cfg_local_ip(config), cfg_local_sip_port(config),
        body_len,
        body);
}

/*
 * DeviceControl 云台控制构造（GB/T 28181 MANSCDP）。
 *
 * 与前面的 Query/Response 不同，DeviceControl 根元素是 <Control>，且 body 带
 * <PTZCmd> 子命令。PTZCmd 是 8 字节十六进制字符串（标准规定布局）：
 *   byte0=0xA5(校验位) byte1=0x0F(地址) byte2=指令(方向+镜头)
 *   byte3=水平速 byte4=垂直速 byte5=变倍速 byte6=0x00 byte7=校验和
 * 本函数只把传入的 8 字节拼成十六进制串填进去，不解析语义（学习用）。
 */
int gb28181_build_message_device_control_ptz(const gb28181_config_t *config,
                                              int cseq,
                                              const char *device_id,
                                              const unsigned char ptz_cmd[8],
                                              char *buf,
                                              int buf_size)
{
    char body[1024];
    int body_len;

    if (!config || !device_id || !ptz_cmd || !buf || buf_size <= 0) {
        return -1;
    }
    body_len = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
        "<Control>\r\n"
        "<CmdType>DeviceControl</CmdType>\r\n"
        "<SN>%d</SN>\r\n"
        "<DeviceID>%s</DeviceID>\r\n"
        "<PTZCmd>%02X%02X%02X%02X%02X%02X%02X%02X</PTZCmd>\r\n"
        "</Control>\r\n",
        cseq, device_id,
        ptz_cmd[0], ptz_cmd[1], ptz_cmd[2], ptz_cmd[3],
        ptz_cmd[4], ptz_cmd[5], ptz_cmd[6], ptz_cmd[7]);
    if (body_len < 0 || body_len >= (int)sizeof(body)) {
        return -2;
    }
    return snprintf(buf, buf_size,
        "MESSAGE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-gb28181-message-%d\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s-message-%d\r\n"
        "CSeq: %d MESSAGE\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Type: Application/MANSCDP+xml\r\n"
        "Content-Length: %d\r\n\r\n"
        "%s",
        config->sip_server_ip, config->domain,
        cfg_local_ip(config), cfg_local_sip_port(config), cseq,
        config->username, config->domain,
        config->sip_server_ip, config->domain,
        config->stream_id, cseq,
        cseq,
        config->username, cfg_local_ip(config), cfg_local_sip_port(config),
        body_len, body);
}

/*
 * DeviceControl 录像控制：根元素 <Control>，带 <RecordCmd>。
 * is_start=1 -> Record（开录像），0 -> StopRecord（停录像）。
 */
int gb28181_build_message_device_control_record(const gb28181_config_t *config,
                                                int cseq,
                                                const char *device_id,
                                                int is_start,
                                                char *buf,
                                                int buf_size)
{
    char body[1024];
    int body_len;

    if (!config || !device_id || !buf || buf_size <= 0) {
        return -1;
    }
    body_len = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
        "<Control>\r\n"
        "<CmdType>DeviceControl</CmdType>\r\n"
        "<SN>%d</SN>\r\n"
        "<DeviceID>%s</DeviceID>\r\n"
        "<RecordCmd>%s</RecordCmd>\r\n"
        "</Control>\r\n",
        cseq, device_id, is_start ? "Record" : "StopRecord");
    if (body_len < 0 || body_len >= (int)sizeof(body)) {
        return -2;
    }
    return snprintf(buf, buf_size,
        "MESSAGE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-gb28181-message-%d\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s-message-%d\r\n"
        "CSeq: %d MESSAGE\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Type: Application/MANSCDP+xml\r\n"
        "Content-Length: %d\r\n\r\n"
        "%s",
        config->sip_server_ip, config->domain,
        cfg_local_ip(config), cfg_local_sip_port(config), cseq,
        config->username, config->domain,
        config->sip_server_ip, config->domain,
        config->stream_id, cseq,
        cseq,
        config->username, cfg_local_ip(config), cfg_local_sip_port(config),
        body_len, body);
}

/*
 * RecordInfo 查询：根元素 <Query>，带 ISO8601 起止时间和查询类型。
 * start_time/end_time 格式 YYYY-MM-DDTHH:MM:SS。type 常用 "all"/"event"/"alarm"。
 * 用于查某设备某时段的历史录像列表，设备应回 RecordInfo Response 带 <RecordList>。
 */
int gb28181_build_message_record_info_query(const gb28181_config_t *config,
                                             int cseq,
                                             const char *device_id,
                                             const char *start_time,
                                             const char *end_time,
                                             const char *type,
                                             char *buf,
                                             int buf_size)
{
    char body[1024];
    int body_len;

    if (!config || !device_id || !start_time || !end_time || !buf || buf_size <= 0) {
        return -1;
    }
    if (!type || type[0] == '\0') {
        type = "all";
    }
    body_len = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
        "<Query>\r\n"
        "<CmdType>RecordInfo</CmdType>\r\n"
        "<SN>%d</SN>\r\n"
        "<DeviceID>%s</DeviceID>\r\n"
        "<StartTime>%s</StartTime>\r\n"
        "<EndTime>%s</EndTime>\r\n"
        "<Type>%s</Type>\r\n"
        "<Secrecy>0</Secrecy>\r\n"
        "<IndistinctQuery>0</IndistinctQuery>\r\n"
        "</Query>\r\n",
        cseq, device_id, start_time, end_time, type);
    if (body_len < 0 || body_len >= (int)sizeof(body)) {
        return -2;
    }
    return snprintf(buf, buf_size,
        "MESSAGE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-gb28181-message-%d\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s-message-%d\r\n"
        "CSeq: %d MESSAGE\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Type: Application/MANSCDP+xml\r\n"
        "Content-Length: %d\r\n\r\n"
        "%s",
        config->sip_server_ip, config->domain,
        cfg_local_ip(config), cfg_local_sip_port(config), cseq,
        config->username, config->domain,
        config->sip_server_ip, config->domain,
        config->stream_id, cseq,
        cseq,
        config->username, cfg_local_ip(config), cfg_local_sip_port(config),
        body_len, body);
}

/*
 * RecordInfo 响应：根元素 <Response>，带 SumNum + RecordList。
 * 学习用固定 2 条录像 Item（不读真实录像文件），便于验证"查录像→回列表"闭环。
 * 真实平台对接时这里应填真实录像索引。
 */
int gb28181_build_message_record_info_response(const gb28181_config_t *config,
                                                int cseq,
                                                const char *device_id,
                                                char *buf,
                                                int buf_size)
{
    char body[2048];
    int body_len;

    if (!config || !device_id || !buf || buf_size <= 0) {
        return -1;
    }
    body_len = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
        "<Response>\r\n"
        "<CmdType>RecordInfo</CmdType>\r\n"
        "<SN>%d</SN>\r\n"
        "<DeviceID>%s</DeviceID>\r\n"
        "<Name>MockRecord</Name>\r\n"
        "<SumNum>2</SumNum>\r\n"
        "<RecordList Num=\"2\">\r\n"
        "<Item>\r\n"
        "<DeviceID>%s</DeviceID>\r\n"
        "<StartTime>2026-08-01T08:00:00</StartTime>\r\n"
        "<EndTime>2026-08-01T08:30:00</EndTime>\r\n"
        "<Name>record-01</Name>\r\n"
        "</Item>\r\n"
        "<Item>\r\n"
        "<DeviceID>%s</DeviceID>\r\n"
        "<StartTime>2026-08-01T09:00:00</StartTime>\r\n"
        "<EndTime>2026-08-01T09:15:00</EndTime>\r\n"
        "<Name>record-02</Name>\r\n"
        "</Item>\r\n"
        "</RecordList>\r\n"
        "</Response>\r\n",
        cseq, device_id, device_id, device_id);
    if (body_len < 0 || body_len >= (int)sizeof(body)) {
        return -2;
    }
    return snprintf(buf, buf_size,
        "MESSAGE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-gb28181-message-%d\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s-message-%d\r\n"
        "CSeq: %d MESSAGE\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Type: Application/MANSCDP+xml\r\n"
        "Content-Length: %d\r\n\r\n"
        "%s",
        config->sip_server_ip, config->domain,
        cfg_local_ip(config), cfg_local_sip_port(config), cseq,
        config->username, config->domain,
        config->sip_server_ip, config->domain,
        config->stream_id, cseq,
        cseq,
        config->username, cfg_local_ip(config), cfg_local_sip_port(config),
        body_len, body);
}

int gb28181_extract_xml_tag(const char *xml, const char *tag, char *buf, int buf_size)
{
    /* 只做最小标签提取，便于 mock server 学习命令解析。 */
    char open_tag[64];
    char close_tag[64];
    const char *begin;
    const char *end;
    size_t len;

    if (!xml || !tag || !buf || buf_size <= 0) {
        return -1;
    }
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
    begin = strstr(xml, open_tag);
    if (!begin) {
        return -2;
    }
    begin += strlen(open_tag);
    end = strstr(begin, close_tag);
    if (!end) {
        return -3;
    }
    len = (size_t)(end - begin);
    if (len >= (size_t)buf_size) {
        len = (size_t)buf_size - 1;
    }
    memcpy(buf, begin, len);
    buf[len] = '\0';
    return (int)len;
}

static int ascii_case_equal_n(const char *a, const char *b, size_t n)
{
    /* 不区分大小写的前缀比较。 */
    size_t i;
    for (i = 0; i < n; ++i) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

static void copy_trimmed(char *dst, size_t dst_size, const char *begin, const char *end)
{
    /* 去掉首尾空白后拷贝，适合解析 SIP 头字段。 */
    size_t len;
    while (begin < end && isspace((unsigned char)*begin)) {
        begin++;
    }
    while (end > begin && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    len = (size_t)(end - begin);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, begin, len);
    dst[len] = '\0';
}

static void lower_copy(char *dst, size_t dst_size, const char *src)
{
    size_t i;
    if (!dst || dst_size == 0) {
        return;
    }
    for (i = 0; i + 1 < dst_size && src[i] != '\0'; ++i) {
        dst[i] = (char)tolower((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static int ascii_ncasecmp(const char *a, const char *b, size_t n)
{
#ifdef _WIN32
    return _strnicmp(a, b, n);
#else
    return strncasecmp(a, b, n);
#endif
}

static void strip_quotes(char *s)
{
    size_t len;
    if (!s) {
        return;
    }
    len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
}

static void md5_hex(const unsigned char *data, size_t len, char out_hex[33])
{
    /* Windows 下用 CryptoAPI 计算 MD5。 */
#ifdef _WIN32
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    unsigned char digest[16];
    DWORD digest_len = sizeof(digest);
    DWORD i;

    out_hex[0] = '\0';
    if (!CryptAcquireContext(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        return;
    }
    if (!CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
        CryptReleaseContext(prov, 0);
        return;
    }
    CryptHashData(hash, data, (DWORD)len, 0);
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_len, 0)) {
        CryptDestroyHash(hash);
        CryptReleaseContext(prov, 0);
        return;
    }
    for (i = 0; i < digest_len; ++i) {
        sprintf(out_hex + i * 2, "%02x", digest[i]);
    }
    out_hex[32] = '\0';
    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
#else
    (void)data;
    (void)len;
    strcpy(out_hex, "00000000000000000000000000000000");
#endif
}

static void md5_concat_hex(char out_hex[33], const char *a, const char *b, const char *c)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s%s%s", a, b, c);
    md5_hex((const unsigned char *)buf, strlen(buf), out_hex);
}

static void write_u16_be(unsigned char *dst, unsigned short value)
{
    dst[0] = (unsigned char)((value >> 8) & 0xFF);
    dst[1] = (unsigned char)(value & 0xFF);
}

static void write_u32_be(unsigned char *dst, unsigned int value)
{
    dst[0] = (unsigned char)((value >> 24) & 0xFF);
    dst[1] = (unsigned char)((value >> 16) & 0xFF);
    dst[2] = (unsigned char)((value >> 8) & 0xFF);
    dst[3] = (unsigned char)(value & 0xFF);
}

static int find_nal_start_code(const unsigned char *data, int size, int offset)
{
    int i;
    for (i = offset; i + 3 <= size; ++i) {
        if (i + 4 <= size && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
            return i;
        }
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
            return i;
        }
    }
    return -1;
}

static int find_next_nal_start_code(const unsigned char *data, int size, int offset)
{
    int i;
    for (i = offset; i + 3 <= size; ++i) {
        if (i + 4 <= size && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
            return i;
        }
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
            return i;
        }
    }
    return size;
}

static int build_pts(unsigned char *dst, unsigned char fb, unsigned long long ts)
{
    unsigned long long val = ts & 0x1FFFFFFFFULL;
    dst[0] = (unsigned char)((fb << 4) | (((val >> 30) & 0x07) << 1) | 1);
    dst[1] = (unsigned char)((val >> 22) & 0xFF);
    dst[2] = (unsigned char)((((val >> 15) & 0x7F) << 1) | 1);
    dst[3] = (unsigned char)((val >> 7) & 0xFF);
    dst[4] = (unsigned char)((((val & 0x7F) << 1) | 1));
    return 5;
}

/*
 * 将 Annex-B H.264 数据封装成最小 PS over RTP 所需的 PS/PES 字节流。
 *
 * 处理层次：H.264 Annex-B -> 视频 PES -> PS pack。
 * 这个函数只负责生成内存中的 PS 数据，不创建 RTP 头，也不发送 UDP；
 * 生成结果还需要交给 gb28181_send_rtp_packet() 或
 * gb28181_send_rtp_payload_fragmented() 发送。
 *
 * annexb_data 必须包含 00 00 00 01 或 00 00 01 起始码；pts_90khz 和
 * dts_90khz 使用 90 kHz 时间基；返回值是输出 PS 字节数，失败返回负值。
 */
int gb28181_build_ps_pack_h264(const unsigned char *annexb_data,
                               int annexb_size,
                               unsigned int pts_90khz,
                               unsigned int dts_90khz,
                               unsigned char *out_buf,
                               int out_buf_size)
{
    int es_start;
    int es_payload_len;
    int pes_len;
    int pes_header_data_len;
    int payload_len;
    unsigned char *p;

    (void)dts_90khz;

    if (!annexb_data || annexb_size <= 0 || !out_buf || out_buf_size <= 0) {
        return -1;
    }

    es_start = find_nal_start_code(annexb_data, annexb_size, 0);
    if (es_start < 0) {
        return -2;
    }

    es_payload_len = annexb_size - es_start;
    if (es_payload_len <= 0) {
        return -3;
    }

    /*
     * 当前最小实现只封一个视频 PES，再外包一层 PS pack。
     * 结构可以按 WinHex 直接去找：
     *   00 00 01 BA  -> PS pack header
     *   00 00 01 E0  -> 视频 PES start code
     *   80 80 05     -> PES header + PTS only
     *   00 00 00 01  -> Annex-B NALU 起始码
     */
    pes_header_data_len = 5; /* PTS only */
    pes_len = 3 + 1 + 2 + 3 + pes_header_data_len + es_payload_len;
    payload_len = 4 + 3 + 1 + 2 + 3 + pes_header_data_len + es_payload_len;
    if (payload_len > out_buf_size) {
        return -4;
    }

    p = out_buf;
    write_u32_be(p, 0x000001BA);
    p += 4;
    /* pack_start_code 后面是固定形态的 pack header，便于播放器识别这是 PS。 */
    *p++ = 0x44;
    *p++ = 0x00;
    *p++ = 0x04;
    *p++ = 0x00;
    *p++ = 0x04;
    *p++ = 0x01;
    *p++ = 0x89;
    *p++ = 0xC3;

    write_u32_be(p, 0x000001E0);
    p += 4;
    /* PES_packet_length 不包含 start code 和 length 字段本身，所以这里减 6。 */
    write_u16_be(p, (unsigned short)(pes_len - 6));
    p += 2;
    /* PES 标志：'10' + PTS only。 */
    *p++ = 0x80;
    *p++ = 0x80;
    *p++ = (unsigned char)pes_header_data_len;
    /* PTS 用 90kHz 时钟写入，便于和 RTP timestamp 对齐。 */
    build_pts(p, 0x02, pts_90khz);
    p += 5;

    /* Annex-B 码流直接跟在 PES payload 后，保留原始 NALU 起始码。 */
    memcpy(p, annexb_data + es_start, (size_t)es_payload_len);
    p += es_payload_len;

    return (int)(p - out_buf);
}

static void parse_header(gb28181_sip_message_t *out, const char *name, size_t name_len, const char *value_begin, const char *value_end)
{
    if (name_len == 3 && ascii_case_equal_n(name, "Via", name_len)) {
        copy_trimmed(out->via, sizeof(out->via), value_begin, value_end);
    } else if (name_len == 4 && ascii_case_equal_n(name, "From", name_len)) {
        copy_trimmed(out->from, sizeof(out->from), value_begin, value_end);
    } else if (name_len == 2 && ascii_case_equal_n(name, "To", name_len)) {
        copy_trimmed(out->to, sizeof(out->to), value_begin, value_end);
    } else if (name_len == 7 && ascii_case_equal_n(name, "Call-ID", name_len)) {
        copy_trimmed(out->call_id, sizeof(out->call_id), value_begin, value_end);
    } else if (name_len == 4 && ascii_case_equal_n(name, "CSeq", name_len)) {
        char cseq[64];
        copy_trimmed(cseq, sizeof(cseq), value_begin, value_end);
        sscanf(cseq, "%d %31s", &out->cseq, out->cseq_method);
    } else if (name_len == 7 && ascii_case_equal_n(name, "Contact", name_len)) {
        copy_trimmed(out->contact, sizeof(out->contact), value_begin, value_end);
    } else if (name_len == 12 && ascii_case_equal_n(name, "Content-Type", name_len)) {
        copy_trimmed(out->content_type, sizeof(out->content_type), value_begin, value_end);
    } else if (name_len == 16 && ascii_case_equal_n(name, "WWW-Authenticate", name_len)) {
        copy_trimmed(out->www_authenticate, sizeof(out->www_authenticate), value_begin, value_end);
    } else if (name_len == 13 && ascii_case_equal_n(name, "Authorization", name_len)) {
        copy_trimmed(out->authorization, sizeof(out->authorization), value_begin, value_end);
    } else if (name_len == 14 && ascii_case_equal_n(name, "Content-Length", name_len)) {
        char lenbuf[32];
        copy_trimmed(lenbuf, sizeof(lenbuf), value_begin, value_end);
        out->content_length = atoi(lenbuf);
    }
}

static void parse_digest_param(const char *value_begin, const char *value_end, const char *key, char *dst, size_t dst_size)
{
    size_t key_len = strlen(key);
    const char *p = value_begin;
    while (p < value_end) {
        const char *kbeg = p;
        const char *keq = NULL;
        const char *vend = p;
        while (vend < value_end && *vend != ',') {
            ++vend;
        }
        keq = (const char *)memchr(kbeg, '=', (size_t)(vend - kbeg));
        if (keq) {
            const char *vbeg = keq + 1;
            while (kbeg < keq && isspace((unsigned char)*kbeg)) {
                ++kbeg;
            }
            while (keq > kbeg && isspace((unsigned char)*(keq - 1))) {
                --keq;
            }
            if ((size_t)(keq - kbeg) == key_len && ascii_case_equal_n(kbeg, key, key_len)) {
                copy_trimmed(dst, dst_size, vbeg, vend);
                strip_quotes(dst);
                return;
            }
        }
        p = vend + 1;
    }
}

int gb28181_parse_sip_message(const char *msg, gb28181_sip_message_t *out)
{
    /* 把 SIP 文本拆成起始行、头字段和 body 指针。 */
    const char *line_begin;
    const char *line_end;
    const char *body;

    if (!msg || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    line_begin = msg;
    line_end = strstr(line_begin, "\r\n");
    if (!line_end) {
        return -1;
    }

    if ((size_t)(line_end - line_begin) >= 7 && strncmp(line_begin, "SIP/2.0", 7) == 0) {
        out->is_response = 1;
        sscanf(line_begin, "SIP/2.0 %d %63[^\r\n]", &out->status_code, out->reason);
    } else {
        sscanf(line_begin, "%31s %127s", out->method, out->request_uri);
    }

    line_begin = line_end + 2;
    while (*line_begin) {
        const char *colon;
        line_end = strstr(line_begin, "\r\n");
        if (!line_end) {
            return -1;
        }
        if (line_end == line_begin) {
            body = line_end + 2;
            out->body = body;
            if (out->content_length == 0 && *body != '\0') {
                out->content_length = (int)strlen(body);
            }
            return 0;
        }
        colon = (const char *)memchr(line_begin, ':', (size_t)(line_end - line_begin));
        if (colon) {
            parse_header(out, line_begin, (size_t)(colon - line_begin), colon + 1, line_end);
        }
        line_begin = line_end + 2;
    }

    return 0;
}

int gb28181_parse_www_authenticate(const char *header_value, gb28181_digest_challenge_t *out)
{
    /* 从 401 的 WWW-Authenticate 中提取 Digest challenge 参数。 */
    const char *p;
    const char *end;

    if (!header_value || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->algorithm, sizeof(out->algorithm), "%s", "MD5");

    p = header_value;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    if (ascii_ncasecmp(p, "Digest", 6) == 0) {
        p += 6;
    }
    end = p + strlen(p);
    parse_digest_param(p, end, "realm", out->realm, sizeof(out->realm));
    parse_digest_param(p, end, "nonce", out->nonce, sizeof(out->nonce));
    parse_digest_param(p, end, "qop", out->qop, sizeof(out->qop));
    parse_digest_param(p, end, "opaque", out->opaque, sizeof(out->opaque));
    parse_digest_param(p, end, "algorithm", out->algorithm, sizeof(out->algorithm));
    if (out->realm[0] == '\0' || out->nonce[0] == '\0') {
        return -2;
    }
    return 0;
}

int gb28181_build_digest_authorization(const gb28181_config_t *config,
                                       const char *method,
                                       const char *uri,
                                       const gb28181_digest_challenge_t *challenge,
                                       char *buf,
                                       int buf_size)
{
    /* 按 Digest 规则计算响应值，生成 Authorization 头。 */
    char ha1[33];
    char ha2[33];
    char response[33];
    char nc[9] = "00000001";
    const char *qop_value = NULL;
    char a1[512];
    char a2[512];
    char rsp_input[1024];

    if (!config || !method || !uri || !challenge || !buf || buf_size <= 0) {
        return -1;
    }
    if (challenge->qop[0] != '\0') {
        qop_value = "auth";
    }

    snprintf(a1, sizeof(a1), "%s:%s:%s", config->username, challenge->realm, config->password);
    md5_hex((const unsigned char *)a1, strlen(a1), ha1);
    snprintf(a2, sizeof(a2), "%s:%s", method, uri);
    md5_hex((const unsigned char *)a2, strlen(a2), ha2);

    if (qop_value != NULL) {
        snprintf(rsp_input, sizeof(rsp_input), "%s:%s:%s:%s:%s:%s", ha1, challenge->nonce, nc, "gb28181", qop_value, ha2);
    } else {
        snprintf(rsp_input, sizeof(rsp_input), "%s:%s:%s", ha1, challenge->nonce, ha2);
    }
    md5_hex((const unsigned char *)rsp_input, strlen(rsp_input), response);

    if (qop_value != NULL) {
        return snprintf(buf, buf_size,
            "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", algorithm=%s, qop=%s, nc=%s, cnonce=\"gb28181\"\r\n",
            config->username,
            challenge->realm,
            challenge->nonce,
            uri,
            response,
            challenge->algorithm[0] ? challenge->algorithm : "MD5",
            qop_value,
            nc);
    }

    return snprintf(buf, buf_size,
        "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", algorithm=%s\r\n",
        config->username,
        challenge->realm,
        challenge->nonce,
        uri,
        response,
        challenge->algorithm[0] ? challenge->algorithm : "MD5");
}

int gb28181_get_local_rtp_port(gb28181_handle_t handle, int *port_out)
{
    /* 读取 jrtplib 当前实际绑定的 RTP 端口。 */
    gb28181_context_t *ctx = (gb28181_context_t *)handle;
    RTPTransmissionInfo *info;
    int port;

    if (!ctx || !port_out || !ctx->rtp_session || !ctx->started) {
        return -1;
    }

    info = ctx->rtp_session->GetTransmissionInfo();
    if (!info) {
        return -2;
    }

    port = -3;
    if (info->GetTransmissionProtocol() == RTPTransmitter::IPv4UDPProto) {
        RTPUDPv4TransmissionInfo *udpinfo = static_cast<RTPUDPv4TransmissionInfo *>(info);
        port = (int)udpinfo->GetRTPPort();
    }
    ctx->rtp_session->DeleteTransmissionInfo(info);

    if (port <= 0) {
        return -3;
    }
    *port_out = port;
    return 0;
}

int gb28181_get_ssrc(gb28181_handle_t handle, unsigned int *ssrc_out)
{
    /* 读取当前上下文使用的 SSRC。 */
    gb28181_context_t *ctx = (gb28181_context_t *)handle;
    if (!ctx || !ssrc_out) {
        return -1;
    }
    *ssrc_out = ctx->ssrc;
    return 0;
}

int gb28181_send_rtcp_app(gb28181_handle_t handle,
                          unsigned char subtype,
                          const unsigned char name[4],
                          const void *appdata,
                          int appdata_len)
{
    /* 显式发一个 RTCP APP 报文，验证 RTCP 通路。
     * jrtplib 的 SendRTCPAPPPacket 会把它封进 RTCP compound packet 发出。 */
    gb28181_context_t *ctx = (gb28181_context_t *)handle;
    uint8_t nm[4] = {0, 0, 0, 0};
    int status;

    if (!ctx || !ctx->started || !ctx->rtp_session || !name) {
        return -1;
    }
    if (appdata_len < 0) {
        return -1;
    }
    memcpy(nm, name, 4);
    status = ctx->rtp_session->SendRTCPAPPPacket((uint8_t)subtype, nm,
                                                 appdata, (size_t)appdata_len);
    if (status < 0) {
        printf("[gb28181] SendRTCPAPPPacket failed: %d\n", status);
        return status;
    }
    return 0;
}

/*
 * RTP 单包发送原语。
 *
 * 把一段已经准备好的 payload 封装并发送为一个 RTP 包；函数不理解 payload
 * 是 PS、裸 H.264 还是 FU-A，只负责使用当前会话的 PT、SSRC、序号和时间戳发送。
 * timestamp_inc 是本次发送后时间戳的相对增量，不是绝对时间戳；marker 通常只在
 * 一个媒体单元的最后一个 RTP 包上置 1。返回实际发送的 payload 字节数，失败返回负值。
 */
int gb28181_send_rtp_packet(gb28181_handle_t handle, const void *payload, int payload_size, unsigned int timestamp_inc, int marker)
{
    /*
     * 最底层的 RTP 单包发送原语。
     *
     * 这个函数不理解 payload 里面装的是什么，只负责把一段字节发成一个 RTP 包。
     * 上层只需要决定三件事：
     *   1. payload 内容是什么
     *   2. 本包的 marker 要不要置 1
     *   3. 这一包发送后，RTP timestamp 应该前进多少
     *
     * 这里的 timestamp_inc 不是绝对时间戳，而是“相对增量”。
     * marker 通常只在某个媒体单元的最后一包置 1。
     */
    gb28181_context_t *ctx = (gb28181_context_t *)handle;
    int status;

    if (!ctx || !ctx->started || !ctx->rtp_session || !payload || payload_size <= 0) {
        return -1;
    }

    ctx->rtp_timestamp += timestamp_inc;
    status = ctx->rtp_session->SendPacket(payload, (size_t)payload_size, (uint8_t)ctx->config.payload_type, marker != 0, timestamp_inc);
    if (status < 0) {
        return status;
    }
    ctx->rtp_seq++;
    return payload_size;
}

/*
 * 通用 RTP payload 字节分片发送。
 *
 * 将一个已经准备好的 payload 按 max_payload_size 机械切块，并多次调用
 * gb28181_send_rtp_packet()；不解析、不改写 payload 内部结构。当前主要用于
 * PS over RTP：前面的分片 marker=0，最后一片 marker=1，只有最后一片推进 timestamp。
 * 它不是 H.264 FU-A 分片器。返回所有已发送 payload 字节数，失败返回负值。
 */
int gb28181_send_rtp_payload_fragmented(gb28181_handle_t handle,
                                        const void *payload,
                                        int payload_size,
                                        int max_payload_size,
                                        unsigned int timestamp_inc)
{
    /*
     * 通用的 payload 字节切片发送。
     *
     * 和 H.264 语义分片的区别：
     *   - 这里不理解 payload 的内容，只按字节大小切块
     *   - 不会重写 payload 的内部结构
     *   - 在当前仓库里，主要用于 PS pack 继续切成 RTP，也就是 PS over RTP
     *
     * 这个函数不关心 payload 是 PS、ES 还是别的容器/数据，只做机械切片：
     *   - 前面的分片 marker=0
     *   - 最后一片 marker=1
     *   - 只有最后一片才推进 timestamp_inc
     *
     * 所以它适合“已经有一个完整 payload，需要按大小拆进多个 RTP 包”的场景。
     * 例如：PS pack 太大时，可以先直接按字节切给 RTP。
     *
     * 它不做 H.264 FU-A 那种语义化分片，也不重写 payload 内容本身。
     */
    const unsigned char *data = (const unsigned char *)payload;
    int offset = 0;
    int total_sent = 0;

    if (!handle || !payload || payload_size <= 0 || max_payload_size <= 0) {
        return -1;
    }

    while (offset < payload_size) {
        int remaining = payload_size - offset;
        int chunk = remaining > max_payload_size ? max_payload_size : remaining;
        int is_last = (offset + chunk) >= payload_size;
        int ret = gb28181_send_rtp_packet(handle, data + offset, chunk, is_last ? timestamp_inc : 0, is_last ? 1 : 0);
        if (ret < 0) {
            return ret;
        }
        total_sent += ret;
        offset += chunk;
    }

    return total_sent;
}

/*
 * H.264 裸 NALU 的 FU-A 语义分片发送。
 *
 * 输入必须是带原始 NALU 头的裸 NALU，例如 0x65 开头的 IDR，不带 Annex-B
 * start code。函数跳过原始 NALU 头，为每个 RTP payload 写入 FU indicator 和
 * FU header，并按首片/中间片/末片设置 S/E 位；前 2 字节占用 FU-A 头，因此每片
 * 实际最多承载 max_payload_size - 2 字节 NALU 数据。内部仍通过
 * gb28181_send_rtp_packet() 完成真正发送。返回所有已发送 payload 字节数，失败返回负值。
 */
int gb28181_send_h264_fu_a(gb28181_handle_t handle,
                           const unsigned char *nalu,
                           int nalu_size,
                           int max_payload_size,
                           unsigned int timestamp_inc)
{
    /*
     * H.264 FU-A 标准分片。
     *
     * 和通用字节切片的区别：
     *   - 这里理解输入是 H.264 裸 NALU
     *   - 会写入 FU indicator / FU header
     *   - 会根据首片/末片设置 S/E 位
     *   - 适合裸 H.264 over RTP 的标准分片
     *
     * 参数解释：
     *   handle           -> 已启动的 RTP 会话句柄，函数通过它发包。
     *   nalu             -> 裸 H.264 NALU 起始地址，必须包含原始 NALU 头字节。
     *   nalu_size        -> nalu 的总字节数。
     *   max_payload_size -> 单个 RTP payload 允许的最大大小，
     *                       其中前 2 字节要留给 FU-A 头。
     *                       工程里常见会取 1200~1300 左右；
     *                       本示例用 24 是为了强制看分片，用 1200 是为了接近常见工程值。
     *   timestamp_inc    -> 这一整个 NALU 完成后，RTP timestamp 前进的步长。
     *
     * 这个函数与 gb28181_send_rtp_payload_fragmented() 的区别在于：
     *   - 后者只会切 payload，不理解内容
     *   - 这里会理解 H.264 NALU，并写出 FU-A 头
     *
     * 也就是说：
     *   gb28181_send_rtp_packet()             -> 单包发送原语
     *   gb28181_send_rtp_payload_fragmented() -> 通用字节切片
     *   gb28181_send_h264_fu_a()              -> H.264 语义分片
     *
     * 输入必须是裸 NALU，不带 Annex-B start code。
     * 例如：
     *   65 xx xx ...   -> 一个 IDR NALU
     *   67 xx xx ...   -> 一个 SPS NALU
     *
     * FU-A 的 RTP payload 结构是：
     *   [FU indicator][FU header][NALU slice...]
     *
     * 其中：
     *   FU indicator = 原始 NALU 的 NRI | 28
     *   FU header    = S/E 位 + 原始 NALU type
     *
     * 这里的 S/E 位含义：
     *   S=1 -> 首片
     *   S=0 -> 非首片
     *   E=1 -> 末片
     *   E=0 -> 非末片
     *
     * 这个函数只负责“拆分并发送”，不负责解码。
     */
    unsigned char packet[1500];
    unsigned char nalu_header;
    unsigned char fu_indicator;
    unsigned char nalu_type;
    int offset;
    int total_sent = 0;

    if (!handle || !nalu || nalu_size <= 1 || max_payload_size <= 2) {
        return -1;
    }
    if (max_payload_size > (int)sizeof(packet)) {
        return -2;
    }

    nalu_header = nalu[0];
    nalu_type = (unsigned char)(nalu_header & 0x1F);
    fu_indicator = (unsigned char)((nalu_header & 0xE0) | 28);
    /* 跳过原始 NALU 头字节，后面只发送真正的 NALU 负载。 */
    offset = 1;

    while (offset < nalu_size) {
        /* 每个 RTP 包最多容纳 max_payload_size 字节，其中 2 字节留给 FU-A 头。 */
        int remaining = nalu_size - offset;
        int chunk = remaining > (max_payload_size - 2) ? (max_payload_size - 2) : remaining;
        /* 第一片就是 offset 刚进入负载区域的那一包。 */
        int is_first = (offset == 1);
        /* 最后一片就是当前 chunk 已经覆盖到原始 NALU 末尾的那一包。 */
        int is_last = (offset + chunk >= nalu_size);
        int ret;

        /* FU indicator 保留原始 NALU 的 NRI，只把 type 改成 28。 */
        packet[0] = fu_indicator;
        /* FU header 先写入原始 NALU type，再按首片/末片位置补 S/E 位。 */
        packet[1] = nalu_type;
        if (is_first) {
            packet[1] = (unsigned char)(packet[1] | 0x80);
        }
        if (is_last) {
            packet[1] = (unsigned char)(packet[1] | 0x40);
        }
        /* 后面跟的就是当前片段的原始 NALU 数据，不再带原始 NALU 头字节。 */
        memcpy(packet + 2, nalu + offset, (size_t)chunk);

        /* 只有最后一片才让 timestamp_inc 生效，并把 marker 置 1。 */
        ret = gb28181_send_rtp_packet(handle, packet, chunk + 2, is_last ? timestamp_inc : 0, is_last ? 1 : 0);
        if (ret < 0) {
            return ret;
        }
        total_sent += chunk;
        offset += chunk;
    }

    return total_sent;
}

/*
 * H.265 FU 分片发送（RFC 7798）。
 *
 * 和 H.264 FU-A 的关键差异（这是学习重点）：
 *   - H.264 NALU 头 1 字节，FU-A 用 type=28，FU 头是 S/E/type
 *   - H.265 NALU 头 2 字节，FU 用 NAL_UNIT_TYPE=49，FU 头是 S/E/FuType
 *
 * H.265 NALU 头 2 字节布局：
 *   byte0: forbidden(1) | nal_unit_type(6) | nuh_layer_id_high(1)
 *   byte1: nuh_layer_id_low(5) | nuh_temporal_zero(1) | temporal_id_plus1(3)
 *   nal_unit_type = (byte0 >> 1) & 0x3F
 *
 * FU 的 RTP payload 结构：
 *   [2 字节 payload header][1 字节 FU header][NALU slice...]
 * 其中：
 *   payload header = 原始 NALU 头 2 字节，但 nal_unit_type 改成 49
 *   FU header = S(1) | E(1) | FuType(6)，FuType = 原始 nal_unit_type
 *
 * 跳过原始 2 字节头，每片 payload 前加 payload header(2) + FU header(1) = 3 字节开销。
 */
int gb28181_send_h265_fu(gb28181_handle_t handle,
                         const unsigned char *nalu,
                         int nalu_size,
                         int max_payload_size,
                         unsigned int timestamp_inc)
{
    unsigned char packet[1500];
    unsigned char ph0, ph1;        /* payload header 2 字节 */
    unsigned char fu_type;
    int offset;
    int total_sent = 0;

    if (!handle || !nalu || nalu_size <= 2 || max_payload_size <= 3) {
        return -1;
    }
    if (max_payload_size > (int)sizeof(packet)) {
        return -2;
    }

    /* 提取原始 nal_unit_type，并构造 payload header：type 改成 49。 */
    fu_type = (unsigned char)((nalu[0] >> 1) & 0x3F);
    ph0 = (unsigned char)((nalu[0] & 0x81) | (49 << 1));  /* forbidden+layer_id 高位保留，type=49 */
    ph1 = nalu[1];                                          /* layer_id 低位 + temporal 全保留 */

    /* 跳过原始 2 字节 NALU 头。 */
    offset = 2;

    while (offset < nalu_size) {
        /* 每片 payload 前有 3 字节开销（payload header 2 + FU header 1）。 */
        int remaining = nalu_size - offset;
        int chunk = remaining > (max_payload_size - 3) ? (max_payload_size - 3) : remaining;
        int is_first = (offset == 2);
        int is_last = (offset + chunk >= nalu_size);
        int ret;

        packet[0] = ph0;
        packet[1] = ph1;
        /* FU header：S/E 位 + 原始 FuType。 */
        packet[2] = (unsigned char)(fu_type & 0x3F);
        if (is_first) {
            packet[2] = (unsigned char)(packet[2] | 0x80);  /* S=1 */
        }
        if (is_last) {
            packet[2] = (unsigned char)(packet[2] | 0x40);  /* E=1 */
        }
        memcpy(packet + 3, nalu + offset, (size_t)chunk);

        ret = gb28181_send_rtp_packet(handle, packet, chunk + 3, is_last ? timestamp_inc : 0, is_last ? 1 : 0);
        if (ret < 0) {
            return ret;
        }
        total_sent += chunk;
        offset += chunk;
    }

    return total_sent;
}
