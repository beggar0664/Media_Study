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
    /* 启动 RTP 会话：创建 jrtplib session、绑定端口、添加远端地址。 */
    gb28181_context_t *ctx = (gb28181_context_t *)handle;
    if (!ctx || ctx->started) {
        return ctx ? 0 : -1;
    }
    if (ctx->config.use_tcp) {
        return -2;
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

    RTPUDPv4TransmissionParams trans_params;
    trans_params.SetPortbase((uint16_t)cfg_local_rtp_port(&ctx->config));

    uint32_t bind_ip = inet_addr(cfg_local_ip(&ctx->config));
    if (bind_ip != INADDR_NONE) {
        trans_params.SetBindIP(ntohl(bind_ip));
    }

    RTPSession *session = new RTPSession();
    int status = session->Create(session_params, &trans_params, RTPTransmitter::IPv4UDPProto);
    if (status < 0) {
        printf("[gb28181] RTPSession::Create failed: %d\n", status);
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

int gb28181_build_sdp(const gb28181_config_t *config, char *buf, int buf_size, const char *ssrc)
{
    /* 生成最小 SDP：告诉对端视频类型、端口、payload type、SSRC。 */
    if (!config || !buf || buf_size <= 0 || !ssrc) {
        return -1;
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

    body_len = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
        "<Response>\r\n"
        "<CmdType>Catalog</CmdType>\r\n"
        "<SN>%d</SN>\r\n"
        "<DeviceID>%s</DeviceID>\r\n"
        "<SumNum>1</SumNum>\r\n"
        "<DeviceList Num=\"1\">\r\n"
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

    pes_header_data_len = 5; /* PTS only */
    pes_len = 3 + 1 + 2 + 3 + pes_header_data_len + es_payload_len;
    payload_len = 4 + 3 + 1 + 2 + 3 + pes_header_data_len + es_payload_len;
    if (payload_len > out_buf_size) {
        return -4;
    }

    p = out_buf;
    write_u32_be(p, 0x000001BA);
    p += 4;
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
    write_u16_be(p, (unsigned short)(pes_len - 6));
    p += 2;
    *p++ = 0x80;
    *p++ = 0x80;
    *p++ = (unsigned char)pes_header_data_len;
    build_pts(p, 0x02, pts_90khz);
    p += 5;

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

int gb28181_send_rtp_packet(gb28181_handle_t handle, const void *payload, int payload_size, unsigned int timestamp_inc, int marker)
{
    /* 发送单个 RTP 包；timestamp_inc 是时间戳增量，不是绝对值。 */
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

int gb28181_send_rtp_payload_fragmented(gb28181_handle_t handle,
                                        const void *payload,
                                        int payload_size,
                                        int max_payload_size,
                                        unsigned int timestamp_inc)
{
    /* 简单按字节切片：前面分片 marker=0，最后一片 marker=1。 */
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
