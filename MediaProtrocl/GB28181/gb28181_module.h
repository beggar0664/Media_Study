#ifndef GB28181_MODULE_H
#define GB28181_MODULE_H

/*
 * GB28181 学习模块公共接口。
 *
 * 这套代码是协议学习用最小实现，不是完整 GB28181 SDK。
 * 建议先看文档再读代码：
 * - ../gb28181_study.md：GB28181 信令、SDP、RTP/PS 的分层和时序图
 * - ../../current_code_learning_guide.md：当前代码的运行、抓包和阅读顺序
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 国标设备/平台/媒体协商所需的最小配置。 */
    char local_id[64];
    char domain[64];
    char username[64];
    char password[64];
    char sip_server_ip[64];
    int sip_server_port;
    char local_ip[64];
    int local_sip_port;
    int local_rtp_port;
    char remote_rtp_ip[64];
    int remote_rtp_port;
    char media_ip[64];       /* Backward-compatible alias for local_ip. */
    int media_port;          /* Backward-compatible alias for local_rtp_port. */
    char stream_id[64];
    int payload_type;
    int use_tcp;
    int enable_dump;
    unsigned int ssrc;
} gb28181_config_t;

typedef struct {
    /* 解析 SIP 报文后的最小结果集，够学习和做 mock 验证。 */
    int is_response;
    int status_code;
    char reason[64];
    char method[32];
    char request_uri[128];
    char via[256];
    char from[256];
    char to[256];
    char call_id[128];
    int cseq;
    char cseq_method[32];
    char contact[256];
    char content_type[64];
    char www_authenticate[512];
    char authorization[512];
    int content_length;
    const char *body;
} gb28181_sip_message_t;

typedef struct {
    /* Digest 鉴权挑战参数。 */
    char realm[128];
    char nonce[256];
    char qop[64];
    char opaque[128];
    char algorithm[32];
} gb28181_digest_challenge_t;

typedef struct gb28181_context_s* gb28181_handle_t;

/* 生命周期接口：创建、启动 RTP、停止、销毁。 */
gb28181_handle_t gb28181_create(const gb28181_config_t *config);
int gb28181_start(gb28181_handle_t handle);
void gb28181_stop(gb28181_handle_t handle);
void gb28181_destroy(gb28181_handle_t handle);

/* SIP 信令文本构造。详见 ../gb28181_study.md 的 REGISTER / INVITE / BYE 章节。 */
/* 构造第一次 REGISTER：不带 Authorization，用于触发平台返回 401 challenge。 */
int gb28181_build_register(const gb28181_config_t *config, char *buf, int buf_size);
/* 构造 INVITE + SDP：用于发起媒体会话协商，不负责发送 RTP。 */
int gb28181_build_invite(const gb28181_config_t *config, char *buf, int buf_size);
/* 构造 BYE：用于结束 SIP 媒体会话。 */
int gb28181_build_bye(const gb28181_config_t *config, char *buf, int buf_size);
/* SIP MESSAGE：用于 Keepalive / Catalog 这类 XML 控制消息。 */
int gb28181_build_message_keepalive(const gb28181_config_t *config, int cseq, char *buf, int buf_size);
int gb28181_build_message_catalog(const gb28181_config_t *config, int cseq, char *buf, int buf_size);
int gb28181_build_message_catalog_response(const gb28181_config_t *config, int cseq, char *buf, int buf_size);
/* 学习用 XML 片段提取。 */
int gb28181_extract_xml_tag(const char *xml, const char *tag, char *buf, int buf_size);
/* SDP / PS / RTP 相关构造与发送。详见 ../gb28181_study.md 的 SDP、RTP、PS over RTP 章节。 */
int gb28181_build_sdp(const gb28181_config_t *config, char *buf, int buf_size, const char *ssrc);
/* 把 Annex-B H.264 数据封成最小 PS pack：H.264 NALU -> PES -> PS。 */
int gb28181_build_ps_pack_h264(const unsigned char *annexb_data,
                               int annexb_size,
                               unsigned int pts_90khz,
                               unsigned int dts_90khz,
                               unsigned char *out_buf,
                               int out_buf_size);
int gb28181_parse_sip_message(const char *msg, gb28181_sip_message_t *out);
int gb28181_parse_www_authenticate(const char *header_value, gb28181_digest_challenge_t *out);
/* 根据 401 返回的 challenge 生成 Authorization。 */
int gb28181_build_digest_authorization(const gb28181_config_t *config,
                                       const char *method,
                                       const char *uri,
                                       const gb28181_digest_challenge_t *challenge,
                                       char *buf,
                                       int buf_size);
int gb28181_get_local_rtp_port(gb28181_handle_t handle, int *port_out);
int gb28181_get_ssrc(gb28181_handle_t handle, unsigned int *ssrc_out);

/* 直接发送一包 RTP payload。 */
int gb28181_send_rtp_packet(gb28181_handle_t handle, const void *payload, int payload_size, unsigned int timestamp_inc, int marker);
/* 按 max_payload_size 做简单分片发送。 */
int gb28181_send_rtp_payload_fragmented(gb28181_handle_t handle,
                                        const void *payload,
                                        int payload_size,
                                        int max_payload_size,
                                        unsigned int timestamp_inc);

#ifdef __cplusplus
}
#endif

#endif /* GB28181_MODULE_H */
