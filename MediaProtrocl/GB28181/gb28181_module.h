#ifndef GB28181_MODULE_H
#define GB28181_MODULE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
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
    int content_length;
    const char *body;
} gb28181_sip_message_t;

typedef struct gb28181_context_s* gb28181_handle_t;

gb28181_handle_t gb28181_create(const gb28181_config_t *config);
int gb28181_start(gb28181_handle_t handle);
void gb28181_stop(gb28181_handle_t handle);
void gb28181_destroy(gb28181_handle_t handle);

int gb28181_build_register(const gb28181_config_t *config, char *buf, int buf_size);
int gb28181_build_invite(const gb28181_config_t *config, char *buf, int buf_size);
int gb28181_build_bye(const gb28181_config_t *config, char *buf, int buf_size);
int gb28181_build_sdp(const gb28181_config_t *config, char *buf, int buf_size, const char *ssrc);
int gb28181_parse_sip_message(const char *msg, gb28181_sip_message_t *out);

int gb28181_send_rtp_packet(gb28181_handle_t handle, const void *payload, int payload_size, unsigned int timestamp_inc, int marker);

#ifdef __cplusplus
}
#endif

#endif /* GB28181_MODULE_H */
