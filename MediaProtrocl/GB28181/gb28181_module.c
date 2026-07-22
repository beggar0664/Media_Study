#include "gb28181_module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct gb28181_context_s {
    gb28181_config_t config;
    int started;
    unsigned short rtp_seq;
    unsigned int rtp_timestamp;
    unsigned int ssrc;
} gb28181_context_t;

static void gb28181_init_default_config(gb28181_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->sip_server_port = 5060;
    cfg->media_port = 10000;
    cfg->payload_type = 96;
    cfg->use_tcp = 0;
    cfg->enable_dump = 0;
}

gb28181_handle_t gb28181_create(const gb28181_config_t *config)
{
    gb28181_context_t *ctx = (gb28181_context_t *)calloc(1, sizeof(gb28181_context_t));
    if (!ctx) {
        return NULL;
    }
    gb28181_init_default_config(&ctx->config);
    if (config) {
        memcpy(&ctx->config, config, sizeof(gb28181_config_t));
    }
    ctx->rtp_seq = 1;
    ctx->rtp_timestamp = 0;
    ctx->ssrc = 0x12345678;
    return ctx;
}

int gb28181_start(gb28181_handle_t handle)
{
    gb28181_context_t *ctx = (gb28181_context_t *)handle;
    if (!ctx) {
        return -1;
    }
    ctx->started = 1;
    return 0;
}

void gb28181_stop(gb28181_handle_t handle)
{
    gb28181_context_t *ctx = (gb28181_context_t *)handle;
    if (!ctx) {
        return;
    }
    ctx->started = 0;
}

void gb28181_destroy(gb28181_handle_t handle)
{
    gb28181_context_t *ctx = (gb28181_context_t *)handle;
    if (!ctx) {
        return;
    }
    free(ctx);
}

int gb28181_build_register(const gb28181_config_t *config, char *buf, int buf_size)
{
    if (!config || !buf || buf_size <= 0) {
        return -1;
    }
    return snprintf(buf, buf_size,
        "REGISTER sip:%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d\r\n"
        "From: <sip:%s@%s>\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: 1 REGISTER\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Content-Length: 0\r\n\r\n",
        config->sip_server_ip,
        config->media_ip, config->media_port,
        config->username, config->domain,
        config->local_id, config->domain,
        config->stream_id,
        config->username, config->media_ip, config->media_port);
}

int gb28181_build_invite(const gb28181_config_t *config, char *buf, int buf_size)
{
    if (!config || !buf || buf_size <= 0) {
        return -1;
    }
    return snprintf(buf, buf_size,
        "INVITE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d\r\n"
        "From: <sip:%s@%s>\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: 2 INVITE\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: 0\r\n\r\n",
        config->stream_id, config->domain,
        config->media_ip, config->media_port,
        config->username, config->domain,
        config->local_id, config->domain,
        config->stream_id,
        config->username, config->media_ip, config->media_port);
}

int gb28181_build_bye(const gb28181_config_t *config, char *buf, int buf_size)
{
    if (!config || !buf || buf_size <= 0) {
        return -1;
    }
    return snprintf(buf, buf_size,
        "BYE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d\r\n"
        "From: <sip:%s@%s>\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: 3 BYE\r\n"
        "Content-Length: 0\r\n\r\n",
        config->stream_id, config->domain,
        config->media_ip, config->media_port,
        config->username, config->domain,
        config->local_id, config->domain,
        config->stream_id);
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
        "a=recvonly\r\n"
        "a=rtpmap:%d H264/90000\r\n"
        "a=ssrc:%s\r\n",
        config->local_id,
        config->media_ip,
        config->media_ip,
        config->media_port,
        config->payload_type,
        config->payload_type,
        ssrc);
}

int gb28181_send_rtp_packet(gb28181_handle_t handle, const void *payload, int payload_size, unsigned int timestamp, int marker)
{
    gb28181_context_t *ctx = (gb28181_context_t *)handle;
    if (!ctx || !ctx->started || !payload || payload_size <= 0) {
        return -1;
    }
    ctx->rtp_seq++;
    ctx->rtp_timestamp = timestamp;
    (void)marker;
    return payload_size;
}
