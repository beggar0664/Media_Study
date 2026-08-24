#include "gb28181_module.h"

/*
 * GB28181 设备状态机骨架。
 *
 * 对应文档：
 * - ../gb28181_study.md 第 14 节：生产设备状态机设计（状态定义与迁移图）
 * - gb28181_code_reference.md 第 7 节：已实现/待补边界
 *
 * 与 gb28181_sip_register_client.cpp 的区别：
 *   - 后者是"直线走完就退出"的学习 demo
 *   - 本文件是状态机驱动的常驻进程：
 *     显式状态 + select 事件循环 + Keepalive 周期 + 指数退避重连 + BYE 后回注册态
 *
 * 复用 gb28181_module.h 的全部信令构造/解析/RTP 发送函数，不重写信令。
 * 媒体本轮仍用固定 demo PS 包，不接真实编码器。
 */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 常量                                                                */
/* ------------------------------------------------------------------ */

/*
 * 重试与退避参数（单位毫秒）。
 *
 * 这些值都偏小，目的是在 mock 上也能在十几秒内观察到完整的"注册→保活→
 * 点播→BYE→再循环"行为，不用等真实工程的分钟级周期。
 * 生产环境里 KEEPALIVE 通常是 60s、退避更长、STREAMING 不主动 BYE。
 * 改这些宏就能在"学习快"和"生产像"之间切换，不必动逻辑代码。
 */
#define GB_REGISTER_RETRY_MAX 3        /* REGISTERING 内无 auth 重发上限，到上限转退避重连 */
#define GB_REGISTER_TIMEOUT_MS 3000     /* 等 401 / 200 的单步超时；到点判重试或转退避      */
#define GB_INVITE_TIMEOUT_MS 3000       /* 等 INVITE 200+SDP 的超时；到点回 REGISTERED      */
#define GB_BYE_TIMEOUT_MS 3000          /* 等 BYE 200 的超时；到点回 REGISTERED 强行收尾    */
#define GB_BACKOFF_BASE_MS 1000         /* 指数退避初始值，掉线后第一次重连等 1s            */
#define GB_BACKOFF_MAX_MS 60000         /* 指数退避封顶，避免长时间断网后重连风暴          */
#define GB_KEEPALIVE_INTERVAL_MS 2000   /* 学习用保活周期（生产常为 60s），周期发 Keepalive */
#define GB_KEEPALIVE_MISS_MAX 3         /* 连续未收到 200 的上限，到上限判掉线转退避重连   */
#define GB_STREAMING_HOLD_MS 3000       /* STREAMING 停留时长，演示一包媒体后发 BYE         */
#define GB_INVITE_AFTER_MS 1000         /* 注册成功后多久自动发 INVITE，让稳态先跑一会     */

/* 演示用固定媒体数据，和 minimal_example 里的 demo H.264 一致。 */
static const unsigned char g_demo_h264_annexb[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1f,
    0xac, 0xd9, 0x40, 0x78, 0x02, 0x27, 0xe5, 0xc0,
    0x00, 0x00, 0x00, 0x01, 0x68, 0xeb, 0xec, 0xb2,
    0x2c,
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21,
    0xa0, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
    0x17, 0x18
};

/* ------------------------------------------------------------------ */
/* 状态枚举                                                            */
/* ------------------------------------------------------------------ */

/*
 * 状态机的八个状态。每个状态回答"我现在在等什么、下一步期望什么"。
 *
 * 对照直线版 gb28181_sip_register_client.cpp：那里的"状态"是隐含在 main() 的
 * 代码执行位置里的（走到哪一步就是什么状态），没有显式名字。这里抽成枚举，
 * 是为了让事件循环能按状态分发，并且任何报文/超时都按当前状态解释——
 * 例如同一个 200 OK，在 AUTHENTICATING 意味"注册成功"，在 INVITING 意味"会话接受"。
 * 完整迁移图见 ../gb28181_study.md 第 14.2 节。
 */
typedef enum {
    GB_STATE_IDLE = 0,        /* 初始 / 掉线重连入口；退避计时结束后重新注册。直线版无此态，走完即退。 */
    GB_STATE_REGISTERING,     /* 已发 REGISTER(无auth)，等 401 challenge。                 */
    GB_STATE_AUTHENTICATING,  /* 已发 REGISTER(带auth)，等 200；收到 401/403 则鉴权失败转 IDLE。 */
    GB_STATE_REGISTERED,      /* 注册成功稳态：跑 Keepalive 周期 + 自动发起 INVITE。直线版只发一次保活。 */
    GB_STATE_INVITING,        /* 已发 INVITE，等 200+SDP；收到 486/603 被拒回 REGISTERED。   */
    GB_STATE_STREAMING,       /* 已 ACK，媒体会话建立；停留到点后发 BYE。                  */
    GB_STATE_BYE_PENDING,     /* 已发 BYE，等 200；收到后回 REGISTERED，可再 INVITE。      */
    GB_STATE_DEREGISTERING    /* 已发 Expires:0 注销，等 200；收到后回 IDLE。mock 不支持，真实平台会回 200。 */
} gb_device_state_t;

static const char *state_name(gb_device_state_t s)
{
    switch (s) {
    case GB_STATE_IDLE:           return "IDLE";
    case GB_STATE_REGISTERING:    return "REGISTERING";
    case GB_STATE_AUTHENTICATING: return "AUTHENTICATING";
    case GB_STATE_REGISTERED:     return "REGISTERED";
    case GB_STATE_INVITING:       return "INVITING";
    case GB_STATE_STREAMING:      return "STREAMING";
    case GB_STATE_BYE_PENDING:    return "BYE_PENDING";
    case GB_STATE_DEREGISTERING:  return "DEREGISTERING";
    }
    return "?";
}

/* ------------------------------------------------------------------ */
/* 设备上下文                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    gb28181_config_t cfg;            /* 本机/平台/鉴权等静态配置，贯穿整个生命周期     */

    int sip_sock;                    /* SIP UDP socket；掉线重连时会被关闭重建         */
    struct sockaddr_in remote_addr;  /* 平台 SIP 地址，sendto 目标                       */

    gb_device_state_t state;         /* 当前状态；事件循环靠它分发 handle_incoming/timeout */

    /* SIP 事务标识：状态机里 CSeq 单调递增（直线版写死），branch/Call-ID 每个事务
     * 独立，便于对端把请求和响应配对。 */
    unsigned int cseq;               /* 单调递增 CSeq，每次发新请求都 +1                */
    char call_id_register[64];      /* 注册事务 Call-ID，每次注册/注销事务独立         */
    char call_id_invite[64];        /* INVITE 事务 Call-ID，标识一条点播会话 dialog    */

    /* 平台在 200 OK(INVITE) 的 To 头里回的 tag，后续 ACK/BYE 必须带上。
     * 直线版写死 tag=mock，状态机要从真实响应里提取。 */
    char to_tag[64];

    /* 401 challenge 缓存：收到 401 后解析出 realm/nonce 等，供下一步发带 auth
     * 的 REGISTER 复用。跨"收 401 → 发 auth"两步保存。 */
    gb28181_digest_challenge_t challenge;
    int have_challenge;              /* 是否已缓存可用 challenge，控制能否发 auth REGISTER */

    /* 重试 / 退避 / 定时：状态机所有定时都基于"墙钟毫秒 + deadline 点"，
     * 不依赖多线程。每个状态设自己的 deadline，select 用最近 deadline 作超时。 */
    int register_retries;            /* REGISTERING 状态内无 auth 重试计数，到上限转退避 */
    int backoff_ms;                  /* 当前指数退避值，掉线后 1s->2s->4s 翻倍封顶 60s   */
    unsigned long long state_deadline_ms;  /* 当前状态超时点（0=不限时），到点跑 timeout */
    unsigned long long next_keepalive_ms;  /* 下次发 Keepalive 的墙钟点，保活周期定时器    */
    unsigned long long invite_after_ms;    /* 注册成功后多久自动发 INVITE，让稳态先跑一会  */
    int keepalive_misses;            /* 连续未收到 200 的次数，到 KEEPALIVE_MISS_MAX 判掉线 */

    /* INVITE dialog 媒体参数：协商出的媒体通道参数。 */
    unsigned int ssrc;               /* RTP 同步源标识，INVITE 的 SDP 和 RTP 头要用同一个 */
    char invite_target[64];          /* 从 Catalog 选的通道号，INVITE 的 Request-URI 目标 */

    /* 媒体句柄：STREAMING 期间持有 jrtplib RTP 会话，BYE 时必须释放。
     * 把资源清理和信令动作绑定在一起，避免会话泄漏。 */
    gb28181_handle_t media_handle;
    unsigned long long streaming_until_ms;  /* STREAMING 停留到何时，到点发 BYE 结束会话      */

    /* 演示控制：多少次 INVITE/BYE 循环后停止。0=不限，Ctrl+C 退出。
     * 生产环境这里通常常驻不退出。 */
    int invite_cycles_done;
    int invite_cycles_max;

    /* 退避中是否要重新初始化 socket（掉线重连路径）。
     * IDLE 退避到期后 start_registering 会据此重建 socket。 */
    int need_resocket;
} gb_device_ctx_t;

/* ------------------------------------------------------------------ */
/* 时间工具（跨平台，毫秒墙钟）                                         */
/* ------------------------------------------------------------------ */

static unsigned long long now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)(ts.tv_nsec / 1000000);
#endif
}

static unsigned long long min_u64(unsigned long long a, unsigned long long b)
{
    return a < b ? a : b;
}

/* ------------------------------------------------------------------ */
/* 网络小工具                                                           */
/* ------------------------------------------------------------------ */

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

static int socket_close(int sockfd)
{
#ifdef _WIN32
    return closesocket(sockfd);
#else
    return close(sockfd);
#endif
}

/* 创建并绑定本地 SIP UDP socket。失败返回 -1。 */
static int open_sip_socket(const char *local_ip, int local_port)
{
    int sockfd;
    struct sockaddr_in addr;

    sockfd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)local_port);
#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr(local_ip);
#else
    inet_pton(AF_INET, local_ip, &addr.sin_addr);
#endif

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        socket_close(sockfd);
        return -1;
    }
    return sockfd;
}

static int send_sip(int sockfd, const struct sockaddr_in *addr, const char *msg)
{
    int ret = sendto(sockfd, msg, (int)strlen(msg), 0,
                     (const struct sockaddr *)addr, sizeof(*addr));
    if (ret < 0) {
        perror("sendto");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* SIP 报文构造（带参版本，CSeq/branch/Call-ID 由状态机控制）           */
/* ------------------------------------------------------------------ */

/*
 * gb28181_build_register() 内部 CSeq/branch 固定，学习用。
 * 状态机需要递增 CSeq 和独立 branch，所以这里自行拼装，
 * 但 Authorization 的计算仍复用 gb28181_build_digest_authorization()。
 */

static void make_branch(char *buf, size_t buf_size, const char *tag)
{
    /* branch 需要唯一性，学习用简化：tag + 当前 cseq + 当前毫秒。 */
    snprintf(buf, buf_size, "z9hG4bK-gb-%s-%u-%llu", tag, 0u, now_ms());
}

/* 发第一次 REGISTER（无 Authorization），触发 401。 */
static int build_register_no_auth(const gb_device_ctx_t *ctx, char *buf, size_t buf_size)
{
    char branch[128];
    const gb28181_config_t *c = &ctx->cfg;
    make_branch(branch, sizeof(branch), "reg");
    return snprintf(buf, buf_size,
        "REGISTER sip:%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u REGISTER\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Max-Forwards: 70\r\n"
        "Expires: 3600\r\n"
        "Content-Length: 0\r\n\r\n",
        c->sip_server_ip,
        c->local_ip, c->local_sip_port, branch,
        c->username, c->domain,
        c->local_id, c->domain,
        ctx->call_id_register,
        ctx->cseq,
        c->username, c->local_ip, c->local_sip_port);
}

/* 用缓存的 challenge 发带 Authorization 的 REGISTER。 */
static int build_register_with_auth(const gb_device_ctx_t *ctx, char *buf, size_t buf_size)
{
    char branch[128];
    char auth_line[1024];
    char uri[256];
    const gb28181_config_t *c = &ctx->cfg;

    make_branch(branch, sizeof(branch), "reg2");
    snprintf(uri, sizeof(uri), "sip:%s", c->sip_server_ip);
    if (gb28181_build_digest_authorization(c, "REGISTER", uri, &ctx->challenge,
                                           auth_line, sizeof(auth_line)) <= 0) {
        return -1;
    }
    return snprintf(buf, buf_size,
        "REGISTER sip:%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u REGISTER\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "%s"
        "Max-Forwards: 70\r\n"
        "Expires: 3600\r\n"
        "Content-Length: 0\r\n\r\n",
        c->sip_server_ip,
        c->local_ip, c->local_sip_port, branch,
        c->username, c->domain,
        c->local_id, c->domain,
        ctx->call_id_register,
        ctx->cseq,
        c->username, c->local_ip, c->local_sip_port,
        auth_line);
}

/* 发 INVITE+SDP，目标通道由状态机决定。 */
static int build_invite_request(const gb_device_ctx_t *ctx, char *buf, size_t buf_size)
{
    char sdp[1024];
    char ssrc_str[16];
    char branch[128];
    int sdp_len;
    const gb28181_config_t *c = &ctx->cfg;

    snprintf(ssrc_str, sizeof(ssrc_str), "%010u", ctx->ssrc);
    sdp_len = gb28181_build_sdp(c, sdp, sizeof(sdp), ssrc_str);
    if (sdp_len <= 0) {
        return -1;
    }
    make_branch(branch, sizeof(branch), "inv");
    return snprintf(buf, buf_size,
        "INVITE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u INVITE\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n\r\n"
        "%s",
        ctx->invite_target, c->domain,
        c->local_ip, c->local_sip_port, branch,
        c->username, c->domain,
        c->local_id, c->domain,
        ctx->call_id_invite,
        ctx->cseq,
        c->username, c->local_ip, c->local_sip_port,
        sdp_len, sdp);
}

static int build_ack_request(const gb_device_ctx_t *ctx, char *buf, size_t buf_size)
{
    char branch[128];
    const gb28181_config_t *c = &ctx->cfg;
    make_branch(branch, sizeof(branch), "ack");
    return snprintf(buf, buf_size,
        "ACK sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>;tag=%s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u ACK\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Length: 0\r\n\r\n",
        ctx->invite_target, c->domain,
        c->local_ip, c->local_sip_port, branch,
        c->username, c->domain,
        c->local_id, c->domain, ctx->to_tag,
        ctx->call_id_invite,
        ctx->cseq,
        c->username, c->local_ip, c->local_sip_port);
}

static int build_bye_request(const gb_device_ctx_t *ctx, char *buf, size_t buf_size)
{
    char branch[128];
    const gb28181_config_t *c = &ctx->cfg;
    make_branch(branch, sizeof(branch), "bye");
    return snprintf(buf, buf_size,
        "BYE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "From: <sip:%s@%s>;tag=gb28181\r\n"
        "To: <sip:%s@%s>;tag=%s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u BYE\r\n"
        "Max-Forwards: 70\r\n"
        "Content-Length: 0\r\n\r\n",
        ctx->invite_target, c->domain,
        c->local_ip, c->local_sip_port, branch,
        c->username, c->domain,
        c->local_id, c->domain, ctx->to_tag,
        ctx->call_id_invite,
        ctx->cseq);
}

/* ------------------------------------------------------------------ */
/* 状态迁移 + 动作                                                      */
/* ------------------------------------------------------------------ */

/* 切换状态，并打印迁移日志。动作由调用方在切换后自行设置 deadline 等。 */
static void enter_state(gb_device_ctx_t *ctx, gb_device_state_t next)
{
    printf("[state] %s -> %s\n", state_name(ctx->state), state_name(next));
    ctx->state = next;
}

/* 进入 IDLE 后启动退避重连：关 socket，到时间后重新 open + REGISTER。 */
static void enter_idle_with_backoff(gb_device_ctx_t *ctx)
{
    if (ctx->sip_sock >= 0) {
        socket_close(ctx->sip_sock);
        ctx->sip_sock = -1;
    }
    ctx->need_resocket = 1;
    ctx->have_challenge = 0;
    ctx->register_retries = 0;
    ctx->keepalive_misses = 0;
    /* 退避：base * 2^k，封顶。 */
    if (ctx->backoff_ms == 0) {
        ctx->backoff_ms = GB_BACKOFF_BASE_MS;
    } else {
        ctx->backoff_ms <<= 1;
        if (ctx->backoff_ms > GB_BACKOFF_MAX_MS) {
            ctx->backoff_ms = GB_BACKOFF_MAX_MS;
        }
    }
    ctx->state_deadline_ms = now_ms() + (unsigned long long)ctx->backoff_ms;
    enter_state(ctx, GB_STATE_IDLE);
    printf("[backoff] %dms before reconnect\n", ctx->backoff_ms);
}

/* 真正打开 socket 并进入 REGISTERING。由 IDLE 退避到期或首次启动调用。 */
static int start_registering(gb_device_ctx_t *ctx)
{
    char buf[2048];
    int len;

    if (ctx->need_resocket || ctx->sip_sock < 0) {
        ctx->sip_sock = open_sip_socket(ctx->cfg.local_ip, ctx->cfg.local_sip_port);
        if (ctx->sip_sock < 0) {
            printf("[error] open SIP socket failed, backoff retry\n");
            enter_idle_with_backoff(ctx);
            return -1;
        }
        ctx->need_resocket = 0;
    }

    /* 新 Call-ID + CSeq 标识本次注册事务。 */
    snprintf(ctx->call_id_register, sizeof(ctx->call_id_register),
             "%s-reg-%llu", ctx->cfg.stream_id, now_ms());
    ctx->cseq = 1;
    ctx->register_retries = 0;

    len = build_register_no_auth(ctx, buf, sizeof(buf));
    if (len <= 0) {
        return -1;
    }
    printf("===== TX REGISTER (no auth) =====\n%s\n", buf);
    if (send_sip(ctx->sip_sock, &ctx->remote_addr, buf) != 0) {
        enter_idle_with_backoff(ctx);
        return -1;
    }
    ctx->state_deadline_ms = now_ms() + GB_REGISTER_TIMEOUT_MS;
    enter_state(ctx, GB_STATE_REGISTERING);
    return 0;
}

/* 收到 401 后，用 challenge 发带 auth 的 REGISTER，进入 AUTHENTICATING。 */
static int send_register_with_auth(gb_device_ctx_t *ctx)
{
    char buf[4096];
    int len;

    ctx->cseq++;
    len = build_register_with_auth(ctx, buf, sizeof(buf));
    if (len <= 0) {
        printf("[error] build auth REGISTER failed\n");
        enter_idle_with_backoff(ctx);
        return -1;
    }
    printf("===== TX REGISTER (with auth) =====\n%s\n", buf);
    if (send_sip(ctx->sip_sock, &ctx->remote_addr, buf) != 0) {
        enter_idle_with_backoff(ctx);
        return -1;
    }
    ctx->state_deadline_ms = now_ms() + GB_REGISTER_TIMEOUT_MS;
    enter_state(ctx, GB_STATE_AUTHENTICATING);
    return 0;
}

/* 进入 REGISTERED：重置退避，启动 Keepalive 周期，并计划稍后发 INVITE。 */
static void enter_registered(gb_device_ctx_t *ctx)
{
    ctx->backoff_ms = 0;
    ctx->keepalive_misses = 0;
    ctx->next_keepalive_ms = now_ms() + GB_KEEPALIVE_INTERVAL_MS;
    ctx->invite_after_ms = now_ms() + GB_INVITE_AFTER_MS;
    /* deadline 取 keepalive 和 invite 的较早者，先到先触发。 */
    ctx->state_deadline_ms = min_u64(ctx->next_keepalive_ms, ctx->invite_after_ms);
    enter_state(ctx, GB_STATE_REGISTERED);
}

/* 发一包 Keepalive。失败计数到上限则判掉线重连。 */
static int send_keepalive(gb_device_ctx_t *ctx)
{
    char buf[2048];
    int len;

    ctx->cseq++;
    len = gb28181_build_message_keepalive(&ctx->cfg, (int)ctx->cseq, buf, (int)sizeof(buf));
    if (len <= 0) {
        return -1;
    }
    printf("===== TX Keepalive SN=%u =====\n", ctx->cseq);
    if (send_sip(ctx->sip_sock, &ctx->remote_addr, buf) != 0) {
        return -1;
    }
    ctx->keepalive_misses++;
    ctx->next_keepalive_ms = now_ms() + GB_KEEPALIVE_INTERVAL_MS;
    ctx->state_deadline_ms = min_u64(ctx->next_keepalive_ms, ctx->invite_after_ms);
    if (ctx->keepalive_misses >= GB_KEEPALIVE_MISS_MAX) {
        printf("[keepalive] %d consecutive misses, reconnect\n", ctx->keepalive_misses);
        enter_idle_with_backoff(ctx);
        return -1;
    }
    return 0;
}

/* 发 INVITE，进入 INVITING。 */
static int send_invite(gb_device_ctx_t *ctx)
{
    char buf[4096];
    int len;

    snprintf(ctx->call_id_invite, sizeof(ctx->call_id_invite),
             "%s-inv-%llu", ctx->cfg.stream_id, now_ms());
    ctx->to_tag[0] = '\0';
    ctx->cseq++;
    len = build_invite_request(ctx, buf, sizeof(buf));
    if (len <= 0) {
        return -1;
    }
    printf("===== TX INVITE target=%s =====\n%s\n", ctx->invite_target, buf);
    if (send_sip(ctx->sip_sock, &ctx->remote_addr, buf) != 0) {
        enter_idle_with_backoff(ctx);
        return -1;
    }
    ctx->state_deadline_ms = now_ms() + GB_INVITE_TIMEOUT_MS;
    enter_state(ctx, GB_STATE_INVITING);
    return 0;
}

/* 收到 INVITE 200+SDP 后发 ACK，进入 STREAMING，发一包 demo PS。 */
static int send_ack_and_start_media(gb_device_ctx_t *ctx)
{
    char buf[2048];
    int len;

    len = build_ack_request(ctx, buf, sizeof(buf));
    if (len <= 0) {
        return -1;
    }
    printf("===== TX ACK =====\n%s\n", buf);
    if (send_sip(ctx->sip_sock, &ctx->remote_addr, buf) != 0) {
        enter_idle_with_backoff(ctx);
        return -1;
    }

    /* 发一包 demo PS over RTP。媒体目标端口写死成 mock 的 30000。 */
    {
        gb28181_config_t media_cfg = ctx->cfg;
        unsigned char ps_pack[512];
        int ps_len;
        int ret;

        snprintf(media_cfg.remote_rtp_ip, sizeof(media_cfg.remote_rtp_ip), "%s", "127.0.0.1");
        media_cfg.remote_rtp_port = 30000;
        media_cfg.local_rtp_port = 10000;
        media_cfg.ssrc = ctx->ssrc;

        ps_len = gb28181_build_ps_pack_h264(g_demo_h264_annexb, (int)sizeof(g_demo_h264_annexb),
                                            9000, 9000, ps_pack, (int)sizeof(ps_pack));
        if (ps_len <= 0) {
            printf("[media] build PS failed ret=%d\n", ps_len);
        } else {
            ctx->media_handle = gb28181_create(&media_cfg);
            if (ctx->media_handle && gb28181_start(ctx->media_handle) == 0) {
                ret = gb28181_send_rtp_packet(ctx->media_handle, ps_pack, ps_len, 9000, 1);
                printf("[media] send PS over RTP ret=%d len=%d marker=1 timestamp_inc=9000\n", ret, ps_len);
            } else if (ctx->media_handle) {
                printf("[media] start RTP failed\n");
                gb28181_destroy(ctx->media_handle);
                ctx->media_handle = NULL;
            }
        }
    }

    ctx->streaming_until_ms = now_ms() + GB_STREAMING_HOLD_MS;
    ctx->state_deadline_ms = ctx->streaming_until_ms;
    enter_state(ctx, GB_STATE_STREAMING);
    return 0;
}

/* 发 BYE，进入 BYE_PENDING。 */
static int send_bye(gb_device_ctx_t *ctx)
{
    char buf[2048];
    int len;

    if (ctx->media_handle) {
        gb28181_stop(ctx->media_handle);
        gb28181_destroy(ctx->media_handle);
        ctx->media_handle = NULL;
    }

    ctx->cseq++;
    len = build_bye_request(ctx, buf, sizeof(buf));
    if (len <= 0) {
        return -1;
    }
    printf("===== TX BYE =====\n%s\n", buf);
    if (send_sip(ctx->sip_sock, &ctx->remote_addr, buf) != 0) {
        enter_idle_with_backoff(ctx);
        return -1;
    }
    ctx->state_deadline_ms = now_ms() + GB_BYE_TIMEOUT_MS;
    enter_state(ctx, GB_STATE_BYE_PENDING);
    return 0;
}

/* 从 200 OK 的 To 头里取 tag=xxx，存到 ctx->to_tag。 */
static void extract_to_tag(const gb28181_sip_message_t *msg, char *out, size_t out_size)
{
    const char *p;
    const char *tag_kw;
    size_t kw_len;

    if (!msg || !out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!msg->to) {
        return;
    }
    tag_kw = "tag=";
    kw_len = strlen(tag_kw);
    p = msg->to;
    /* 不区分大小写找 tag= */
    for (;;) {
        while (*p && *p != 't' && *p != 'T') {
            p++;
        }
        if (!*p) {
            return;
        }
#ifdef _WIN32
        if (_strnicmp(p, tag_kw, kw_len) == 0)
#else
        if (strncasecmp(p, tag_kw, kw_len) == 0)
#endif
        {
            break;
        }
        p++;
    }
    p += kw_len;
    snprintf(out, out_size, "%s", p);
    /* tag 值到右尖括号或分号或行尾结束。 */
    {
        size_t i;
        for (i = 0; i < out_size - 1 && out[i]; ++i) {
            if (out[i] == ';' || out[i] == '>' || out[i] == '\r' || out[i] == '\n') {
                out[i] = '\0';
                break;
            }
        }
        out[out_size - 1] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* 报文分发：按当前状态解释收到的 SIP 报文                             */
/* ------------------------------------------------------------------ */

/*
 * 状态机的"大脑"。收到一条 SIP 报文后，按 ctx->state 分发处理。
 *
 * 关键点：报文的语义由"当前状态 + 报文内容"共同决定，不由收到顺序决定。
 * 最典型的例子是同一个 200 OK：
 *   - AUTHENTICATING 收到 200 -> 注册成功，进 REGISTERED
 *   - REGISTERED    收到 200 -> Keepalive 的回执，清零未达计数
 *   - INVITING      收到 200 -> 会话被接受，发 ACK + 开媒体
 *   - BYE_PENDING   收到 200 -> 会话结束，回 REGISTERED（可再 INVITE）
 *
 * 这正是状态机比直线流程强的本质：直线版只能按"发完等响应"的固定顺序走，
 * 状态机能根据上下文解释同一条报文，并处理乱序/延迟/平台主动下发等情形。
 *
 * 除了按状态处理响应，还要应对平台主动下发的请求（MESSAGE/BYE），
 * 在对应状态里回 200，不让它们破坏当前事务链。
 */
static void handle_incoming(gb_device_ctx_t *ctx, const char *msg_text, int msg_len)
{
    gb28181_sip_message_t msg;
    char reply_show[512];

    if (gb28181_parse_sip_message(msg_text, &msg) != 0) {
        printf("[rx] parse failed, ignored\n");
        return;
    }
    (void)msg_len;

    snprintf(reply_show, sizeof(reply_show), "status=%d reason=%s method=%s cseq=%d",
             msg.status_code, msg.reason[0] ? msg.reason : "<none>",
             msg.method[0] ? msg.method : "<none>", msg.cseq);

    switch (ctx->state) {
    case GB_STATE_REGISTERING:
        if (msg.is_response && msg.status_code == 401 && msg.www_authenticate[0]) {
            printf("[rx] %s in REGISTERING\n", reply_show);
            if (gb28181_parse_www_authenticate(msg.www_authenticate, &ctx->challenge) == 0) {
                ctx->have_challenge = 1;
                ctx->cseq++;
                send_register_with_auth(ctx);
            } else {
                printf("[error] parse WWW-Authenticate failed, reconnect\n");
                enter_idle_with_backoff(ctx);
            }
        } else if (msg.is_response && msg.status_code == 200) {
            /* 某些平台直接回 200（不要求 Digest）。 */
            printf("[rx] %s in REGISTERING (no-auth 200), treat as registered\n", reply_show);
            enter_registered(ctx);
        } else if (msg.is_response && (msg.status_code == 401 || msg.status_code == 403)) {
            printf("[rx] %s in REGISTERING, auth rejected, reconnect\n", reply_show);
            enter_idle_with_backoff(ctx);
        } else {
            printf("[rx] %s in REGISTERING, ignored\n", reply_show);
        }
        break;

    case GB_STATE_AUTHENTICATING:
        if (msg.is_response && msg.status_code == 200) {
            printf("[rx] %s in AUTHENTICATING\n", reply_show);
            enter_registered(ctx);
        } else if (msg.is_response && (msg.status_code == 401 || msg.status_code == 403)) {
            printf("[rx] %s in AUTHENTICATING, auth failed, reconnect\n", reply_show);
            enter_idle_with_backoff(ctx);
        } else {
            printf("[rx] %s in AUTHENTICATING, ignored\n", reply_show);
        }
        break;

    case GB_STATE_REGISTERED:
        if (msg.is_response && msg.status_code == 200) {
            /* Keepalive 的 200 OK：清零未达计数。 */
            printf("[rx] %s in REGISTERED (keepalive ack)\n", reply_show);
            ctx->keepalive_misses = 0;
        } else if (!msg.is_response && strcmp(msg.method, "MESSAGE") == 0) {
            /* 平台主动下发的 MESSAGE（如 Catalog/DeviceInfo Response）。回 200。 */
            char ok[2048];
            printf("[rx] MESSAGE in REGISTERED, reply 200\n");
            snprintf(ok, sizeof(ok),
                "SIP/2.0 200 OK\r\n"
                "Via: %s\r\n"
                "From: %s\r\n"
                "To: %s\r\n"
                "Call-ID: %s\r\n"
                "CSeq: %d MESSAGE\r\n"
                "Content-Length: 0\r\n\r\n",
                msg.via, msg.from, msg.to, msg.call_id, msg.cseq);
            send_sip(ctx->sip_sock, &ctx->remote_addr, ok);
        } else if (!msg.is_response && strcmp(msg.method, "BYE") == 0) {
            /* 平台主动 BYE，回 200 并回注册态。 */
            char ok[2048];
            printf("[rx] platform BYE in REGISTERED, reply 200\n");
            snprintf(ok, sizeof(ok),
                "SIP/2.0 200 OK\r\n"
                "Via: %s\r\n"
                "From: %s\r\n"
                "To: %s\r\n"
                "Call-ID: %s\r\n"
                "CSeq: %d BYE\r\n"
                "Content-Length: 0\r\n\r\n",
                msg.via, msg.from, msg.to, msg.call_id, msg.cseq);
            send_sip(ctx->sip_sock, &ctx->remote_addr, ok);
        } else {
            printf("[rx] %s in REGISTERED, ignored\n", reply_show);
        }
        break;

    case GB_STATE_INVITING:
        if (msg.is_response && msg.status_code == 200) {
            printf("[rx] %s in INVITING, send ACK + start media\n", reply_show);
            extract_to_tag(&msg, ctx->to_tag, sizeof(ctx->to_tag));
            if (ctx->to_tag[0]) {
                printf("[dialog] platform to_tag=%s\n", ctx->to_tag);
            }
            send_ack_and_start_media(ctx);
        } else if (msg.is_response &&
                   (msg.status_code == 486 || msg.status_code == 603 ||
                    msg.status_code == 487 || msg.status_code == 488)) {
            printf("[rx] %s in INVITING, invite rejected, back to REGISTERED\n", reply_show);
            enter_registered(ctx);
        } else {
            printf("[rx] %s in INVITING, ignored\n", reply_show);
        }
        break;

    case GB_STATE_STREAMING:
        /* STREAMING 期间收到 BYE（平台主动结束），回 200 回注册态。 */
        if (!msg.is_response && strcmp(msg.method, "BYE") == 0) {
            char ok[2048];
            printf("[rx] platform BYE in STREAMING, reply 200\n");
            if (ctx->media_handle) {
                gb28181_stop(ctx->media_handle);
                gb28181_destroy(ctx->media_handle);
                ctx->media_handle = NULL;
            }
            snprintf(ok, sizeof(ok),
                "SIP/2.0 200 OK\r\n"
                "Via: %s\r\n"
                "From: %s\r\n"
                "To: %s\r\n"
                "Call-ID: %s\r\n"
                "CSeq: %d BYE\r\n"
                "Content-Length: 0\r\n\r\n",
                msg.via, msg.from, msg.to, msg.call_id, msg.cseq);
            send_sip(ctx->sip_sock, &ctx->remote_addr, ok);
            enter_registered(ctx);
        } else {
            printf("[rx] %s in STREAMING, ignored\n", reply_show);
        }
        break;

    case GB_STATE_BYE_PENDING:
        if (msg.is_response && msg.status_code == 200) {
            printf("[rx] %s in BYE_PENDING, back to REGISTERED\n", reply_show);
            ctx->invite_cycles_done++;
            if (ctx->invite_cycles_max > 0 && ctx->invite_cycles_done >= ctx->invite_cycles_max) {
                printf("[demo] %d invite cycles done, stopping\n", ctx->invite_cycles_done);
                /* 演示结束：进 DEREGISTERING 走注销。生产环境这里通常继续常驻。 */
                ctx->cseq++;
                /* 用 Expires:0 的 REGISTER 注销。 */
                {
                    char buf[4096];
                    char branch[128];
                    const gb28181_config_t *c = &ctx->cfg;
                    make_branch(branch, sizeof(branch), "dereg");
                    snprintf(ctx->call_id_register, sizeof(ctx->call_id_register),
                             "%s-dereg-%llu", ctx->cfg.stream_id, now_ms());
                    snprintf(buf, sizeof(buf),
                        "REGISTER sip:%s SIP/2.0\r\n"
                        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
                        "From: <sip:%s@%s>;tag=gb28181\r\n"
                        "To: <sip:%s@%s>\r\n"
                        "Call-ID: %s\r\n"
                        "CSeq: %u REGISTER\r\n"
                        "Contact: <sip:%s@%s:%d>\r\n"
                        "Max-Forwards: 70\r\n"
                        "Expires: 0\r\n"
                        "Content-Length: 0\r\n\r\n",
                        c->sip_server_ip,
                        c->local_ip, c->local_sip_port, branch,
                        c->username, c->domain,
                        c->local_id, c->domain,
                        ctx->call_id_register,
                        ctx->cseq,
                        c->username, c->local_ip, c->local_sip_port);
                    printf("===== TX REGISTER (deregister Expires:0) =====\n%s\n", buf);
                    send_sip(ctx->sip_sock, &ctx->remote_addr, buf);
                    ctx->state_deadline_ms = now_ms() + GB_REGISTER_TIMEOUT_MS;
                    enter_state(ctx, GB_STATE_DEREGISTERING);
                }
            } else {
                enter_registered(ctx);
            }
        } else {
            printf("[rx] %s in BYE_PENDING, ignored\n", reply_show);
        }
        break;

    case GB_STATE_DEREGISTERING:
        if (msg.is_response && msg.status_code == 200) {
            printf("[rx] %s in DEREGISTERING, done\n", reply_show);
            if (ctx->sip_sock >= 0) {
                socket_close(ctx->sip_sock);
                ctx->sip_sock = -1;
            }
            enter_state(ctx, GB_STATE_IDLE);
            ctx->state_deadline_ms = 0;
            /* 让主循环退出。 */
            ctx->invite_cycles_max = -1;
        } else {
            printf("[rx] %s in DEREGISTERING, ignored\n", reply_show);
        }
        break;

    default:
        printf("[rx] %s in %s, ignored\n", reply_show, state_name(ctx->state));
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 状态超时处理                                                         */
/* ------------------------------------------------------------------ */

/*
 * 和 handle_incoming 对称：当 select 超时、当前状态的 deadline 到点时调用。
 * 每个状态有自己的超时动作，和该状态"在等什么"对应：
 *   - REGISTERING 超时 -> 还没收到 401，重发无 auth，到上限转退避
 *   - AUTHENTICATING 超时 -> 还没收到 200，退回重发无 auth
 *   - REGISTERED 超时 -> keepalive 周期到或 invite_after 到，分别触发
 *   - STREAMING 超时 -> 停留时长到，发 BYE 主动收尾
 *
 * 直线版没有超时处理——recv 不到响应就打印错误继续往下走，是生产不能接受的。
 * 状态机靠 deadline + 本函数实现"等不到就重试/退避/判掉线"。
 *
 * REGISTERED 那段尤其值得看：一个状态同时挂两个定时器（keepalive +
 * invite_after），用 min_u64 取较早者作 deadline，到点在本函数里再区分
 * 是哪个触发的，决定发保活还是发 INVITE。
 */
static void handle_state_timeout(gb_device_ctx_t *ctx)
{
    switch (ctx->state) {
    case GB_STATE_IDLE:
        /* 退避到期，开始重连。 */
        printf("[timeout] IDLE backoff elapsed, start registering\n");
        start_registering(ctx);
        break;

    case GB_STATE_REGISTERING:
        /* 等 401 超时：重发无 auth，到上限转退避重连。 */
        ctx->register_retries++;
        if (ctx->register_retries >= GB_REGISTER_RETRY_MAX) {
            printf("[timeout] REGISTERING no 401 after %d retries, reconnect\n",
                   ctx->register_retries);
            enter_idle_with_backoff(ctx);
        } else {
            printf("[timeout] REGISTERING retry %d\n", ctx->register_retries);
            start_registering(ctx);
        }
        break;

    case GB_STATE_AUTHENTICATING:
        /* 等 200 超时：退回重发无 auth。 */
        printf("[timeout] AUTHENTICATING no 200, back to REGISTERING\n");
        start_registering(ctx);
        break;

    case GB_STATE_REGISTERED: {
        /* REGISTERED 有两类定时：keepalive 周期 / 自动发起 INVITE。 */
        unsigned long long now = now_ms();
        int do_keepalive = (ctx->next_keepalive_ms && now >= ctx->next_keepalive_ms);
        int do_invite = (ctx->invite_after_ms && now >= ctx->invite_after_ms);
        if (do_invite) {
            /* 优先发起 INVITE，进入点播流程；keepalive 暂停。 */
            ctx->invite_after_ms = 0;
            printf("[timeout] REGISTERED auto-invite trigger\n");
            send_invite(ctx);
        } else if (do_keepalive) {
            printf("[timeout] REGISTERED keepalive tick\n");
            send_keepalive(ctx);
        } else if (ctx->state_deadline_ms) {
            /* 还没到点，只更新 deadline。 */
            ctx->state_deadline_ms = min_u64(ctx->next_keepalive_ms, ctx->invite_after_ms);
        }
        break;
    }

    case GB_STATE_INVITING:
        printf("[timeout] INVITING no 200, back to REGISTERED\n");
        enter_registered(ctx);
        break;

    case GB_STATE_STREAMING:
        /* 停留时长到点，发 BYE。 */
        printf("[timeout] STREAMING hold elapsed, send BYE\n");
        send_bye(ctx);
        break;

    case GB_STATE_BYE_PENDING:
        printf("[timeout] BYE_PENDING no 200, back to REGISTERED\n");
        if (ctx->media_handle) {
            gb28181_stop(ctx->media_handle);
            gb28181_destroy(ctx->media_handle);
            ctx->media_handle = NULL;
        }
        enter_registered(ctx);
        break;

    case GB_STATE_DEREGISTERING:
        printf("[timeout] DEREGISTERING no 200, force idle\n");
        if (ctx->sip_sock >= 0) {
            socket_close(ctx->sip_sock);
            ctx->sip_sock = -1;
        }
        enter_state(ctx, GB_STATE_IDLE);
        ctx->state_deadline_ms = 0;
        ctx->invite_cycles_max = -1;
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 主循环                                                              */
/* ------------------------------------------------------------------ */

/*
 * 事件循环主入口。
 *
 * 骨架是单线程 select：用"距最近 deadline 的剩余时间"作 select 超时，
 * 一个线程同时守着 SIP socket 可读 和 各状态定时器。不引入多线程就能
 * "同时"收信令和守时——这是替代直线版"发完同步 recv"的关键。
 *
 * 循环两件事：
 *   - select 返回可读 -> recvfrom -> handle_incoming（按状态解释报文）
 *   - select 返回超时 -> handle_state_timeout（按状态处理 deadline）
 *
 * 唯一退出路径：DEREGISTERING 完成后置 invite_cycles_max=-1，主循环检测到退出。
 * 生产环境通常常驻不退出，这里给循环上限是为了 demo 能自动停。
 *
 * 命令行参数：max_cycles，0=常驻到 Ctrl+C，>0=N 次 INVITE/BYE 循环后注销。
 */
int main(int argc, char **argv)
{
    gb_device_ctx_t ctx;
    int max_cycles = 2;  /* 默认演示 2 次 INVITE/BYE 循环后注销退出 */

    if (argc > 1) {
        max_cycles = atoi(argv[1]);
        if (max_cycles <= 0) {
            max_cycles = 0;  /* 0=不限，常驻到 Ctrl+C */
        }
    }

    if (init_winsock() != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.sip_sock = -1;
    ctx.invite_cycles_max = max_cycles;

    /* 配置：与 gb28181_sip_register_client.cpp 对齐，指向本机 mock server。 */
    snprintf(ctx.cfg.local_id, sizeof(ctx.cfg.local_id), "%s", "34020000001320000001");
    snprintf(ctx.cfg.domain, sizeof(ctx.cfg.domain), "%s", "3402000000");
    snprintf(ctx.cfg.username, sizeof(ctx.cfg.username), "%s", "34020000001320000001");
    snprintf(ctx.cfg.password, sizeof(ctx.cfg.password), "%s", "123456");
    snprintf(ctx.cfg.sip_server_ip, sizeof(ctx.cfg.sip_server_ip), "%s", "127.0.0.1");
    ctx.cfg.sip_server_port = 5060;
    snprintf(ctx.cfg.local_ip, sizeof(ctx.cfg.local_ip), "%s", "127.0.0.1");
    ctx.cfg.local_sip_port = 5062;
    snprintf(ctx.cfg.stream_id, sizeof(ctx.cfg.stream_id), "%s", "34020000001320000001");
    /* 演示用固定 INVITE 目标（mock Catalog 里的在线通道）。 */
    snprintf(ctx.invite_target, sizeof(ctx.invite_target), "%s", "34020000001320000001");
    ctx.cfg.local_rtp_port = 10000;
    ctx.cfg.payload_type = 96;
    ctx.cfg.ssrc = 0x12345678;
    ctx.ssrc = 0x12345678;

    memset(&ctx.remote_addr, 0, sizeof(ctx.remote_addr));
    ctx.remote_addr.sin_family = AF_INET;
    ctx.remote_addr.sin_port = htons((u_short)ctx.cfg.sip_server_port);
#ifdef _WIN32
    ctx.remote_addr.sin_addr.s_addr = inet_addr(ctx.cfg.sip_server_ip);
#else
    inet_pton(AF_INET, ctx.cfg.sip_server_ip, &ctx.remote_addr.sin_addr);
#endif

    printf("===== GB28181 stateful device starting =====\n");
    printf("local=%s:%d server=%s:%d target=%s cycles=%d\n",
           ctx.cfg.local_ip, ctx.cfg.local_sip_port,
           ctx.cfg.sip_server_ip, ctx.cfg.sip_server_port,
           ctx.invite_target, ctx.invite_cycles_max);
    printf("Ctrl+C to stop\n");

    /* 首次启动：直接进入注册（无退避）。 */
    start_registering(&ctx);

    for (;;) {
        fd_set readfds;
        struct timeval tv;
        unsigned long long now;
        unsigned long long wait_deadline;
        long wait_ms;
        int ret;
        char recv_buf[8192];

        if (ctx.state == GB_STATE_IDLE && ctx.invite_cycles_max == -1) {
            /* DEREGISTERING 完成后主循环退出。 */
            break;
        }

        now = now_ms();

        /* 计算最近的需要等待的 deadline，select 用它作超时。 */
        wait_deadline = ctx.state_deadline_ms;
        if (ctx.state == GB_STATE_REGISTERED && ctx.next_keepalive_ms) {
            wait_deadline = min_u64(wait_deadline, ctx.next_keepalive_ms);
        }
        if (wait_deadline == 0) {
            wait_ms = 1000;  /* 无限期时给 1s 让循环呼吸 */
        } else if (wait_deadline > now) {
            wait_ms = (long)(wait_deadline - now);
            if (wait_ms > 1000) {
                wait_ms = 1000;  /* 上限 1s，避免长时间无响应卡死 */
            }
        } else {
            wait_ms = 0;  /* 已过期，立即处理超时 */
        }

        if (ctx.sip_sock >= 0) {
            FD_ZERO(&readfds);
            FD_SET(ctx.sip_sock, &readfds);
            tv.tv_sec = wait_ms / 1000;
            tv.tv_usec = (wait_ms % 1000) * 1000;
            ret = select(ctx.sip_sock + 1, &readfds, NULL, NULL, &tv);
        } else {
            /* IDLE 退避中，无 socket，只睡。 */
#ifdef _WIN32
            Sleep((DWORD)wait_ms);
#else
            {
                struct timespec ts;
                ts.tv_sec = wait_ms / 1000;
                ts.tv_nsec = (long)(wait_ms % 1000) * 1000000L;
                nanosleep(&ts, NULL);
            }
#endif
            ret = 0;
        }

        if (ret < 0) {
            /* select 出错，通常是被信号打断；短暂等待后继续。 */
            continue;
        }

        if (ret > 0 && ctx.sip_sock >= 0 && FD_ISSET(ctx.sip_sock, &readfds)) {
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            int n = recvfrom(ctx.sip_sock, recv_buf, (int)sizeof(recv_buf) - 1, 0,
                             (struct sockaddr *)&peer, &peer_len);
            if (n > 0) {
                recv_buf[n] = '\0';
                handle_incoming(&ctx, recv_buf, n);
            } else if (n < 0) {
                printf("[error] recvfrom failed, reconnect\n");
                enter_idle_with_backoff(&ctx);
            }
            continue;
        }

        /* select 超时，检查状态 deadline 是否到点。 */
        if (ctx.state_deadline_ms && now_ms() >= ctx.state_deadline_ms) {
            handle_state_timeout(&ctx);
        } else if (ctx.state == GB_STATE_REGISTERED &&
                   ctx.next_keepalive_ms && now_ms() >= ctx.next_keepalive_ms) {
            handle_state_timeout(&ctx);
        }
    }

    /* 清理。 */
    if (ctx.media_handle) {
        gb28181_stop(ctx.media_handle);
        gb28181_destroy(ctx.media_handle);
        ctx.media_handle = NULL;
    }
    if (ctx.sip_sock >= 0) {
        socket_close(ctx.sip_sock);
        ctx.sip_sock = -1;
    }
    cleanup_winsock();
    printf("===== device stopped =====\n");
    return 0;
}
