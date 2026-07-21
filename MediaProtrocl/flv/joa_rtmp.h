#ifndef __JOA_RTMP_H__
#define __JOA_RTMP_H__

#ifdef __cplusplus
extern "C" {
#endif
#define RTMP_URL "rtmp://192.168.124.106/live/livestream"

typedef enum {
    RTMP_STATUS_IDLE = 0,
    RTMP_STATUS_CONNECTING,
    RTMP_STATUS_CONNECTED,
    RTMP_STATUS_PUSHING,
    RTMP_STATUS_DISCONNECTED,
    RTMP_STATUS_ERROR
} rtmp_status_t;

typedef struct {
    char rtmp_url[256];
    int video_profile_no;
    int audio_profile_no;  // 音频profile，-1表示不使用音频
    int reconnect_interval;
    int max_reconnect_times;
    int connect_timeout;
    int video_buffer_size;
    int audio_buffer_size;  // 音频缓冲区大小，0表示使用默认值
    int dump_enable;
    int dump_only;
    char dump_path[256];
} rtmp_config_t;

typedef struct rtmp_context_s* rtmp_handle_t;

rtmp_handle_t rtmp_init(const rtmp_config_t *config);
int rtmp_start(rtmp_handle_t handle);
void rtmp_stop(rtmp_handle_t handle);
void rtmp_destroy(rtmp_handle_t handle);
int rtmp_set_url(rtmp_handle_t handle, const char *url);
rtmp_status_t rtmp_get_status(rtmp_handle_t handle);

int joa_rtmp_start(const rtmp_config_t *config);
void joa_rtmp_stop(void);
rtmp_status_t joa_rtmp_status(void);
int rtmp_push_set(int push_start);

int rtmp_test(void);

#ifdef __cplusplus
}
#endif

#endif /* __JOA_RTMP_H__ */

