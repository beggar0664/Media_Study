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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace jrtplib;

typedef struct gb28181_context_s {
    gb28181_config_t config;
    int started;
    unsigned short rtp_seq;
    unsigned int rtp_timestamp;
    unsigned int ssrc;
    RTPSession *rtp_session;
} gb28181_context_t;

static void gb28181_init_default_config(gb28181_config_t *cfg)
{
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
    if (cfg->local_ip[0] != '\0') {
        return cfg->local_ip;
    }
    return cfg->media_ip;
}

static int cfg_local_sip_port(const gb28181_config_t *cfg)
{
    return cfg->local_sip_port > 0 ? cfg->local_sip_port : 5060;
}

static int cfg_local_rtp_port(const gb28181_config_t *cfg)
{
    if (cfg->local_rtp_port > 0) {
        return cfg->local_rtp_port;
    }
    return cfg->media_port > 0 ? cfg->media_port : 10000;
}

static const char *cfg_remote_rtp_ip(const gb28181_config_t *cfg)
{
    if (cfg->remote_rtp_ip[0] != '\0') {
        return cfg->remote_rtp_ip;
    }
    return cfg->sip_server_ip;
}

static int cfg_remote_rtp_port(const gb28181_config_t *cfg)
{
    return cfg->remote_rtp_port > 0 ? cfg->remote_rtp_port : cfg_local_rtp_port(cfg);
}

static unsigned int cfg_ssrc(const gb28181_config_t *cfg)
{
    return cfg->ssrc != 0 ? cfg->ssrc : 0x12345678;
}

static void copy_config(gb28181_config_t *dst, const gb28181_config_t *src)
{
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
    gb28181_context_t *ctx = (gb28181_context_t *)handle;
    if (!ctx || ctx->started) {
        return ctx ? 0 : -1;
    }
    if (ctx->config.use_tcp) {
        return -2;
    }

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
    gb28181_context_t *ctx = (gb28181_context_t *)handle;
    if (!ctx) {
        return;
    }
    if (ctx->rtp_session) {
        ctx->rtp_session->Destroy();
        delete ctx->rtp_session;
        ctx->rtp_session = NULL;
    }
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

static int ascii_case_equal_n(const char *a, const char *b, size_t n)
{
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
    } else if (name_len == 14 && ascii_case_equal_n(name, "Content-Length", name_len)) {
        char lenbuf[32];
        copy_trimmed(lenbuf, sizeof(lenbuf), value_begin, value_end);
        out->content_length = atoi(lenbuf);
    }
}

int gb28181_parse_sip_message(const char *msg, gb28181_sip_message_t *out)
{
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

int gb28181_send_rtp_packet(gb28181_handle_t handle, const void *payload, int payload_size, unsigned int timestamp_inc, int marker)
{
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
