#include "gb28181_module.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    gb28181_config_t cfg;
    char register_msg[2048];
    char invite_msg[4096];
    char bye_msg[2048];
    const unsigned char demo_h264_nalu[] = {
        0x65, 0x88, 0x84, 0x21, 0xa0
    };
    gb28181_handle_t handle;

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.local_id, sizeof(cfg.local_id), "%s", "34020000001320000001");
    snprintf(cfg.domain, sizeof(cfg.domain), "%s", "3402000000");
    snprintf(cfg.username, sizeof(cfg.username), "%s", "34020000001320000001");
    snprintf(cfg.sip_server_ip, sizeof(cfg.sip_server_ip), "%s", "192.168.1.100");
    cfg.sip_server_port = 5060;
    snprintf(cfg.local_ip, sizeof(cfg.local_ip), "%s", "192.168.1.10");
    cfg.local_sip_port = 5060;
    cfg.local_rtp_port = 10000;
    snprintf(cfg.remote_rtp_ip, sizeof(cfg.remote_rtp_ip), "%s", "192.168.1.100");
    cfg.remote_rtp_port = 30000;
    snprintf(cfg.stream_id, sizeof(cfg.stream_id), "%s", "34020000001320000001");
    cfg.payload_type = 96;
    cfg.ssrc = 0x12345678;

    gb28181_build_register(&cfg, register_msg, sizeof(register_msg));
    gb28181_build_invite(&cfg, invite_msg, sizeof(invite_msg));
    gb28181_build_bye(&cfg, bye_msg, sizeof(bye_msg));

    printf("===== REGISTER =====\n%s\n", register_msg);
    printf("===== INVITE + SDP =====\n%s\n", invite_msg);
    printf("===== BYE =====\n%s\n", bye_msg);

    handle = gb28181_create(&cfg);
    if (!handle) {
        return 1;
    }

    if (gb28181_start(handle) == 0) {
        gb28181_send_rtp_packet(handle, demo_h264_nalu, sizeof(demo_h264_nalu), 9000, 1);
        gb28181_stop(handle);
    }
    gb28181_destroy(handle);
    return 0;
}
