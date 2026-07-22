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
    char media_ip[64];
    int media_port;
    char stream_id[64];
    int payload_type;
    int use_tcp;
    int enable_dump;
} gb28181_config_t;

typedef struct gb28181_context_s* gb28181_handle_t;

gb28181_handle_t gb28181_create(const gb28181_config_t *config);
int gb28181_start(gb28181_handle_t handle);
void gb28181_stop(gb28181_handle_t handle);
void gb28181_destroy(gb28181_handle_t handle);

int gb28181_build_register(const gb28181_config_t *config, char *buf, int buf_size);
int gb28181_build_invite(const gb28181_config_t *config, char *buf, int buf_size);
int gb28181_build_bye(const gb28181_config_t *config, char *buf, int buf_size);
int gb28181_build_sdp(const gb28181_config_t *config, char *buf, int buf_size, const char *ssrc);

int gb28181_send_rtp_packet(gb28181_handle_t handle, const void *payload, int payload_size, unsigned int timestamp, int marker);

#ifdef __cplusplus
}
#endif

#endif /* GB28181_MODULE_H */
