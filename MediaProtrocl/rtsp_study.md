# RTSP 学习笔记

RTSP 不是容器格式，也不是编解码格式。它是实时流媒体的会话控制协议。学习 RTSP 链路时建议拆成四块：

```text
RTSP：会话控制
SDP：媒体描述 / 协商内容
RTP：媒体承载
RTCP：质量反馈 / 同步辅助
```

这里要注意：SDP 适合作为学习模块单独拆出来，但它不是和 RTP/RTCP 同类的传输协议。SDP 是媒体描述格式，通常放在 RTSP 的 `DESCRIBE` 响应 body 里，用来告诉客户端后续 RTP/RTCP 应该怎么解释和建立。

它们和编解码层、容器层的关系是：

```text
RTSP
  -> 建立会话，协商 SDP，控制播放/暂停/停止

SDP
  -> 描述媒体参数：编码、payload type、clock rate、控制地址、端口等

RTP
  -> 根据 SDP 协商结果承载 H.264/H.265/AAC payload

RTCP
  -> 给 RTP 做统计、反馈、同步辅助

H.264 / H.265 / AAC
  -> 真正的音视频编码数据
```

一句话记忆：

```text
RTSP 说：我要看这个流，怎么传？
SDP 说：这路流是什么编码、什么 PT、什么 clock、怎么 setup。
RTP 说：我开始一包一包送媒体 payload。
RTCP 说：我报告丢包、抖动、发送统计和时间同步。
```

也可以用角色关系记：

```text
RTSP：控制面
SDP：描述层 / 协商内容
RTP：媒体数据面
RTCP：媒体反馈面
```

## 1. 分层边界

RTSP 所在位置：

```text
容器层：PS / TS / FLV / MP4
协议传输层：RTSP / RTP / RTCP / RTMP / GB28181
编解码层：H.264 / H.265 / AAC
```

RTSP 不负责：

```text
1. 不负责 H.264/H.265 怎么解码
2. 不负责把媒体封成 MP4/FLV/TS 文件
3. 不负责每个 RTP 包的 sequence number 和 timestamp
4. 不负责统计丢包和 jitter
```

RTSP 负责：

```text
1. 发现服务端支持哪些方法
2. 获取媒体描述 SDP
3. 协商 RTP/RTCP 传输方式
4. 控制 PLAY / PAUSE / TEARDOWN
5. 维护 Session
6. 指定 Range、Scale 等播放控制参数
```

快递类比：

```text
H.264 / H.265 / AAC
  -> 包裹里的真正内容

RTP
  -> 快递小包裹的运单 + 包序号 + 时间戳

RTCP
  -> 物流状态回执 / 丢包统计 / 时间同步报告

RTSP
  -> 下单和调度系统：告诉双方什么时候开始、走哪种传输、什么时候结束

SDP
  -> 订单详情 / 货物说明书：说明编码、PT、clock rate、track、fmtp 等
```

## 2. RTSP / SDP / RTP / RTCP 四块

学习上可以拆成四块：

```text
1. RTSP：OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN
2. SDP：描述媒体流，解释 codec、PT、clock rate、control track、fmtp
3. RTP：真正传 H.264/H.265/AAC payload
4. RTCP：统计、反馈、同步
```

协议角色上要区分：RTSP、RTP、RTCP 是协议；SDP 是描述格式。SDP 不负责传输媒体包，它负责描述后续媒体包应该按什么参数接收和解释。

### RTSP

RTSP 是控制面。常见方法：

| 方法 | 作用 |
|---|---|
| `OPTIONS` | 查询服务端支持哪些 RTSP 方法 |
| `DESCRIBE` | 获取媒体描述，响应 body 通常是 SDP |
| `SETUP` | 为某一路 media 建立 RTP/RTCP 传输参数 |
| `PLAY` | 开始播放 |
| `PAUSE` | 暂停播放 |
| `TEARDOWN` | 结束会话并释放资源 |
| `GET_PARAMETER` | 保活或查询参数，很多设备用于 keepalive |
| `SET_PARAMETER` | 设置参数，设备兼容性差异较多 |
| `ANNOUNCE` | 推流或发布 SDP 时常见 |
| `RECORD` | 配合 ANNOUNCE 做推流/录制方向 |

### RTP

RTP 是数据面，负责真正承载媒体 payload。

| 字段 | 作用 |
|---|---|
| `Payload Type` | 对应 SDP 中的 `rtpmap`，例如 `96 -> H264/90000` |
| `Sequence Number` | 包序号，检测丢包和乱序 |
| `Timestamp` | 媒体时间戳，视频常用 90000 Hz |
| `Marker` | 视频里常用于标记访问单元结束 |
| `SSRC` | 一路 RTP 流的同步源身份 |

RTSP 不会替 RTP 生成这些字段。RTSP 只通过 SDP 和 `Transport` 头告诉双方 RTP 应该怎么传。

### RTCP

RTCP 是控制反馈面。

| 类型 | 全称 | 作用 |
|---|---|---|
| `SR` | Sender Report | 发送端报告，包含 NTP 时间、RTP timestamp、发送包数、字节数 |
| `RR` | Receiver Report | 接收端报告，包含丢包率、累计丢包、最高序号、jitter、LSR/DLSR |
| `SDES` | Source Description | 描述 SSRC，例如 CNAME |
| `BYE` | Goodbye | 说明某个 SSRC 离开会话 |
| `APP` | Application-defined | 应用自定义控制信息 |

RTCP 的价值主要在：统计丢包、计算 jitter、辅助音视频同步、把 RTP timestamp 和 NTP wall clock 建立关系。

## 3. 最小 RTSP 拉流链路

典型拉流顺序：

```text
Client -> Server: OPTIONS
Server -> Client: 200 OK, Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN

Client -> Server: DESCRIBE
Server -> Client: 200 OK + SDP

Client -> Server: SETUP video track
Server -> Client: 200 OK + Transport + Session

Client -> Server: PLAY
Server -> Client: 200 OK

Server -> Client: RTP video packets
Server <-> Client: RTCP reports

Client -> Server: TEARDOWN
Server -> Client: 200 OK
```

如果有音频和视频两路，一般会对每个 media track 各发一次 `SETUP`，最后一次 `PLAY` 开始整个 session。

## 4. 交互逻辑与时序

RTSP 的交互可以按“控制面先建立规则，数据面再开始传输”来理解。

最小拉流时序：

```text
Client                                      Server
  |                                           |
  | OPTIONS rtsp://... RTSP/1.0              |
  | CSeq: 1                                  |
  |------------------------------------------>|
  |                                           |
  | RTSP/1.0 200 OK                          |
  | CSeq: 1                                  |
  | Public: OPTIONS, DESCRIBE, SETUP, PLAY...|
  |<------------------------------------------|
  |                                           |
  | DESCRIBE rtsp://... RTSP/1.0             |
  | CSeq: 2                                  |
  | Accept: application/sdp                  |
  |------------------------------------------>|
  |                                           |
  | RTSP/1.0 200 OK                          |
  | CSeq: 2                                  |
  | Content-Type: application/sdp            |
  |                                          |
  | v=0                                      |
  | m=video 0 RTP/AVP 96                     |
  | a=rtpmap:96 H264/90000                   |
  | a=control:trackID=0                      |
  |<------------------------------------------|
  |                                           |
  | SETUP rtsp://.../trackID=0 RTSP/1.0      |
  | CSeq: 3                                  |
  | Transport: RTP/AVP;unicast;              |
  |            client_port=10000-10001       |
  |------------------------------------------>|
  |                                           |
  | RTSP/1.0 200 OK                          |
  | CSeq: 3                                  |
  | Session: 12345678                        |
  | Transport: RTP/AVP;unicast;              |
  |            client_port=10000-10001;      |
  |            server_port=50000-50001       |
  |<------------------------------------------|
  |                                           |
  | PLAY rtsp://... RTSP/1.0                 |
  | CSeq: 4                                  |
  | Session: 12345678                        |
  | Range: npt=0.000-                        |
  |------------------------------------------>|
  |                                           |
  | RTSP/1.0 200 OK                          |
  | CSeq: 4                                  |
  | Session: 12345678                        |
  | RTP-Info: url=...trackID=0;seq=...       |
  |<------------------------------------------|
  |                                           |
  |        RTP video packets                  |
  |<==========================================|
  |                                           |
  |        RTCP SR/RR                         |
  |<=========================================>|
  |                                           |
  | TEARDOWN rtsp://... RTSP/1.0             |
  | CSeq: 5                                  |
  | Session: 12345678                        |
  |------------------------------------------>|
  |                                           |
  | RTSP/1.0 200 OK                          |
  | CSeq: 5                                  |
  |<------------------------------------------|
  |                                           |
```

这里有两个方向要分清：

```text
RTSP 控制面
  -> 客户端发请求，服务端回响应
  -> 走 RTSP TCP 连接，默认端口 554

RTP/RTCP 数据面
  -> PLAY 成功后才开始传媒体和反馈
  -> UDP 模式下走 Transport 协商出来的端口
  -> TCP interleaved 模式下塞回 RTSP TCP 连接
```

### 状态变化

可以把 RTSP client 的状态粗略拆成：

```text
Init
  -> 还不知道服务端能力和媒体信息

OptionsDone
  -> 已知道服务端支持哪些方法

Described
  -> 已拿到 SDP，知道有哪些 track、codec、PT、clock rate、control URL

SetupDone
  -> 已为每个 track 建好 RTP/RTCP 通道，拿到 Session

Playing
  -> PLAY 已成功，开始接收 RTP/RTCP

Paused
  -> PAUSE 后媒体暂停，但 Session 可能仍存在

Teardown
  -> TEARDOWN 后会话释放
```

状态和方法的关系：

| 当前状态 | 动作 | 成功后状态 | 重点产物 |
|---|---|---|---|
| `Init` | `OPTIONS` | `OptionsDone` | `Public` 方法列表 |
| `OptionsDone` | `DESCRIBE` | `Described` | SDP |
| `Described` | `SETUP` | `SetupDone` | `Transport`、`Session` |
| `SetupDone` | `PLAY` | `Playing` | `RTP-Info`，开始 RTP/RTCP |
| `Playing` | `PAUSE` | `Paused` | 暂停媒体发送 |
| `Playing/Paused` | `TEARDOWN` | `Teardown` | 释放会话 |

### 多 track 时序

如果 SDP 里同时有 video 和 audio，一般是：

```text
DESCRIBE
  -> 得到 video trackID=0，audio trackID=1

SETUP trackID=0
  -> 建立 video RTP/RTCP 通道

SETUP trackID=1
  -> 建立 audio RTP/RTCP 通道

PLAY aggregate URL
  -> 同一个 Session 下开始播放所有已 SETUP 的 track
```

每一路 track 通常有独立的 RTP/RTCP 端口或 interleaved channel：

```text
video: client_port=10000-10001 或 interleaved=0-1
audio: client_port=10002-10003 或 interleaved=2-3
```

### 交互中的关键约束

学习和写代码时要盯住这些规则：

```text
1. 每个请求 CSeq 递增，响应必须带相同 CSeq。
2. DESCRIBE 成功后才能知道 track control URL。
3. SETUP 成功后必须保存 Session，后续 PLAY/PAUSE/TEARDOWN 都要带。
4. UDP 模式下，RTP/RTCP 端口来自 Transport 头。
5. TCP interleaved 模式下，RTP/RTCP 用 $ + channel + length 封在 RTSP TCP 流里。
6. PLAY 成功前不应该期待稳定 RTP 数据。
7. TEARDOWN 后 Session 失效，RTP/RTCP 通道应释放。
```

### RTSP / SDP / RTP / RTCP 协同时序

更完整地看，四块不是顺序孤立发生的，而是在一次拉流里相互衔接：

```text
Client                                                Server
  |                                                     |
  | 1. RTSP OPTIONS                                    |
  |    问服务端支持哪些控制方法                         |
  |---------------------------------------------------->|
  |                                                     |
  | 2. RTSP 200 OK + Public                            |
  |    返回可用方法列表                                 |
  |<----------------------------------------------------|
  |                                                     |
  | 3. RTSP DESCRIBE                                   |
  |    请求媒体描述，Accept: application/sdp             |
  |---------------------------------------------------->|
  |                                                     |
  | 4. RTSP 200 OK + SDP                               |
  |    SDP 描述媒体轨道、编码、PT、clock、control URL    |
  |<----------------------------------------------------|
  |                                                     |
  |    SDP 示例信息：                                   |
  |      m=video 0 RTP/AVP 96                           |
  |      a=rtpmap:96 H264/90000                         |
  |      a=control:trackID=0                            |
  |                                                     |
  | 5. RTSP SETUP trackID=0                             |
  |    根据 SDP 的 control URL 设置 video RTP/RTCP 通道  |
  |    Transport: RTP/AVP;client_port=10000-10001        |
  |---------------------------------------------------->|
  |                                                     |
  | 6. RTSP 200 OK + Transport + Session                |
  |    确认 RTP/RTCP 端口、SSRC、Session                |
  |<----------------------------------------------------|
  |                                                     |
  |    此时 RTP/RTCP 通道参数已确定，但通常还未发媒体     |
  |                                                     |
  | 7. RTSP PLAY                                       |
  |    带 Session，请求开始播放                         |
  |---------------------------------------------------->|
  |                                                     |
  | 8. RTSP 200 OK + RTP-Info                          |
  |    可返回初始 seq/rtptime                           |
  |<----------------------------------------------------|
  |                                                     |
  | 9. RTP                                             |
  |    服务器开始按 SDP/Transport 结果发送媒体 payload   |
  |<====================================================|
  |                                                     |
  | 10. RTCP                                           |
  |     SR/RR/SDES 周期性交互，做统计、反馈、同步        |
  |<===================================================>|
  |                                                     |
  | 11. RTSP TEARDOWN                                  |
  |     结束 Session                                   |
  |---------------------------------------------------->|
  |                                                     |
  | 12. RTSP 200 OK                                    |
  |     RTP/RTCP 通道释放                              |
  |<----------------------------------------------------|
```

这条链路里每一块的“输入/输出”可以这样看：

| 阶段 | 输入 | 输出 | 后续影响 |
|---|---|---|---|
| `OPTIONS` | RTSP URL | `Public` 方法列表 | 客户端知道服务端支持哪些控制动作 |
| `DESCRIBE` | RTSP URL、`Accept: application/sdp` | SDP | 客户端知道 media track、codec、PT、clock、control URL |
| `SETUP` | SDP 中的 `a=control`、客户端端口 | `Transport`、`Session` | 确定 RTP/RTCP 是 UDP 端口还是 TCP channel |
| `PLAY` | `Session`、可选 `Range` | `RTP-Info` | 服务端开始发送 RTP，RTCP 开始反馈 |
| `RTP` | SDP 的 PT/clock、SETUP 的 Transport | RTP 包头 + payload | 客户端按 Seq/Timestamp/Marker/SSRC 重组媒体 |
| `RTCP` | RTP 收发状态 | SR/RR/SDES/BYE | 统计丢包、jitter、同步 RTP timestamp 和 NTP |
| `TEARDOWN` | `Session` | 释放确认 | 停止 RTP/RTCP，释放服务端资源 |

### SDP 在时序中的位置

SDP 只在控制面里出现，但它决定数据面如何解释。

```text
DESCRIBE 响应 body 中的 SDP
  -> 告诉客户端有哪些媒体 track
  -> 告诉客户端每路 track 的 payload type 和 codec
  -> 告诉客户端 RTP clock rate
  -> 告诉客户端后续 SETUP 应该使用哪个 control URL
```

例如：

```text
m=video 0 RTP/AVP 96
a=rtpmap:96 H264/90000
a=control:trackID=0
```

它在后续时序中的作用：

```text
a=control:trackID=0
  -> SETUP rtsp://.../trackID=0

PT 96 + H264/90000
  -> 收到 RTP 包时，把 rtp.p_type == 96 的 payload 按 H.264 和 90000 Hz 时钟解释

m=video
  -> 这路 RTP 属于 video track
```

SDP 不会逐包出现。真正每包变化的是 RTP 头里的 `sequence number`、`timestamp`、`marker`。

### RTP 在时序中的位置

RTP 通常从 `PLAY 200 OK` 之后开始稳定出现。

UDP 模式下：

```text
SETUP Transport: client_port=10000-10001

Server RTP  -> Client UDP 10000
Server RTCP -> Client UDP 10001
Client RTCP -> Server RTCP port
```

RTP 每包携带：

```text
Payload Type
Sequence Number
Timestamp
Marker
SSRC
Payload(H.264/H.265/AAC/PS/TS...)
```

这些字段和 SDP/SETUP 的关系：

| RTP 字段 | 来自哪里解释 | 说明 |
|---|---|---|
| `PT` | SDP `rtpmap` | 例如 `96 -> H264/90000` |
| `Timestamp` | SDP clock rate | H.264/H.265 通常 90000 Hz |
| `SSRC` | SETUP `Transport` 可能声明，也可能只在 RTP 包里出现 | 标识一路 RTP 流 |
| `Marker` | payload 格式规则 | 视频里常标记访问单元结束 |
| `Sequence Number` | RTP 自身维护 | 每个 RTP 包递增，用于丢包/乱序检测 |

### RTCP 在时序中的位置

RTCP 和 RTP 是一对通道。它不是 `DESCRIBE/SETUP/PLAY` 这种 RTSP 文本响应，也不是 SDP body。

RTCP 通常在 `PLAY` 后周期性出现：

```text
Server -> Client: RTCP Sender Report
Client -> Server: RTCP Receiver Report
Server -> Client: RTCP SDES
Client/Server -> Peer: RTCP BYE
```

关键用途：

```text
SR：发送端报告，把 RTP timestamp 和 NTP 时间关联起来
RR：接收端报告丢包、jitter、最高序号等
SDES：描述 SSRC，例如 CNAME
BYE：通知某个 SSRC 离开
```

RTCP 和 RTSP 的关系是：

```text
RTSP SETUP 决定 RTCP 走哪个端口或哪个 interleaved channel。
RTSP PLAY 之后，RTCP 才有持续反馈意义。
RTSP TEARDOWN 后，RTCP 通道也应该结束。
```

### TCP interleaved 下的协同时序

如果 `SETUP` 使用：

```text
Transport: RTP/AVP/TCP;unicast;interleaved=0-1
```

那么 RTSP、RTP、RTCP 都走同一条 TCP 连接：

```text
RTSP text request/response
  -> 普通文本，以 RTSP/1.0、CSeq 等解析

RTP/RTCP interleaved frame
  -> 以 '$' 开头
  -> $ + channel + length + RTP/RTCP payload
```

时序上：

```text
Client -> Server: RTSP SETUP, interleaved=0-1
Server -> Client: RTSP 200 OK
Client -> Server: RTSP PLAY
Server -> Client: RTSP 200 OK
Server -> Client: $ channel 0 RTP packet
Server -> Client: $ channel 1 RTCP packet
Client -> Server: $ channel 1 RTCP Receiver Report
Client -> Server: RTSP TEARDOWN
```

这时抓包时不会看到独立 UDP RTP 包，而是在 TCP stream 里看到 `$` 帧。Wireshark 会把它解析成 `rtsp:rtp` 或 `rtsp:rtcp`。

## 5. RTSP 请求/响应格式

RTSP 文本格式和 HTTP 很像，但语义不同。

请求格式：

```text
METHOD rtsp://host/path RTSP/1.0
CSeq: 1
Header: value

optional body
```

响应格式：

```text
RTSP/1.0 200 OK
CSeq: 1
Header: value

optional body
```

必须关注的头字段：

| 字段 | 作用 |
|---|---|
| `CSeq` | 请求序号。每个 RTSP 请求递增，响应必须带相同 CSeq |
| `Session` | `SETUP` 后服务端返回的会话 ID，后续 PLAY/PAUSE/TEARDOWN 必须带 |
| `Transport` | `SETUP` 里最关键的头，协商 RTP/RTCP 走 UDP 还是 TCP |
| `Content-Type` | `DESCRIBE` 响应里常见为 `application/sdp` |
| `Content-Length` | body 长度 |
| `Range` | 播放范围，例如 `npt=0.000-` |
| `RTP-Info` | `PLAY` 响应中可能返回起始 RTP seq/rtptime |
| `Public` | `OPTIONS` 响应里列出服务端支持的方法 |

## 6. OPTIONS

`OPTIONS` 用来问服务端支持哪些 RTSP 方法。

请求：

```text
OPTIONS rtsp://127.0.0.1/live/stream RTSP/1.0
CSeq: 1
```

响应：

```text
RTSP/1.0 200 OK
CSeq: 1
Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, GET_PARAMETER
```

学习重点：`CSeq` 必须对应，`Public` 告诉客户端可用方法，`OPTIONS` 不建立媒体链路。

## 7. DESCRIBE 和 SDP

`DESCRIBE` 用来获取媒体描述。响应 body 通常是 SDP。

请求：

```text
DESCRIBE rtsp://127.0.0.1/live/stream RTSP/1.0
CSeq: 2
Accept: application/sdp
```

响应：

```text
RTSP/1.0 200 OK
CSeq: 2
Content-Type: application/sdp
Content-Length: 180

v=0
o=- 0 0 IN IP4 127.0.0.1
s=No Name
c=IN IP4 127.0.0.1
t=0 0
m=video 0 RTP/AVP 96
a=rtpmap:96 H264/90000
a=control:trackID=0
```

SDP 关键字段：

| 字段 | 作用 |
|---|---|
| `v=` | SDP 版本 |
| `o=` | 会话 origin |
| `s=` | session name |
| `c=` | 连接地址 |
| `t=` | 时间范围，实时流常见 `0 0` |
| `m=video 0 RTP/AVP 96` | media 类型、端口、传输协议、payload type |
| `a=rtpmap:96 H264/90000` | PT 96 对应 H.264，clock rate 90000 |
| `a=control:trackID=0` | 后续 SETUP 这路 track 的控制地址 |
| `a=fmtp` | 编码附加参数，例如 H.264 的 sprop-parameter-sets |

注意：RTSP 的 SDP 里 `m=video 0` 很常见。这里端口是 `0` 不代表没有端口，而是实际 RTP/RTCP 端口会在后续 `SETUP` 的 `Transport` 头里协商。

### SDP 和包头是否重叠

SDP 和 RTP 包头、容器包头会出现一些相似信息，例如编码类型、时间基、流标识，但它们的层级不同，不是简单重复。

可以这样理解：

```text
SDP
  -> 会话开始前的媒体说明书 / 协商结果

RTP 头
  -> 传输过程中每个 RTP 包的实时标签

容器包头
  -> 封装结构内部每段数据的组织标签

编解码头
  -> 码流本身如何解码的参数和帧类型
```

例如 SDP 中：

```text
m=video 0 RTP/AVP 96
a=rtpmap:96 H264/90000
a=control:trackID=0
```

它告诉客户端：

```text
这路 media 是 video
传输协议是 RTP/AVP
payload type 96 表示 H.264
RTP clock rate 是 90000
后续 SETUP 的控制地址是 trackID=0
```

但 SDP 不会告诉你：

```text
这一包 RTP 的 sequence number 是多少
这一包 RTP timestamp 是多少
这一包 marker 是不是 1
这一包 payload 具体有多少字节
这一帧是否真的收齐
```

这些属于 RTP 包头和接收端重组逻辑。

如果 RTP payload 里承载的是 TS/PS/FLV 这类容器，那么容器内部还会有自己的包头或结构字段。此时可以这样看：

```text
SDP：告诉你“预期这路流是什么，应该按什么规则接收”
RTP 头：告诉你“当前网络包属于哪一路、序号多少、媒体时间是多少”
容器包头：告诉你“当前封装片段属于哪个流、边界在哪里、PTS/DTS 是多少”
编解码头：告诉你“payload 内部的码流参数、NALU 类型、帧类型是什么”
```

对比表：

| 信息 | SDP | RTP 头 | 容器包头 |
|---|---|---|---|
| 编码类型 | 有，`rtpmap/fmtp` | 只有 PT，需要 SDP 解释 | 有，例如 TS PMT、FLV codec id、MP4 sample description |
| Payload Type | 有，`m=` / `rtpmap` | 有，`PT` 字段 | 一般没有 RTP PT |
| 时间基 | 有，例如 `H264/90000` | timestamp 使用这个时间基 | 有自己的 time scale 或 PTS/DTS 单位 |
| 每包序号 | 没有 | 有 sequence number | TS 有 continuity counter，但语义不同 |
| 每包时间戳 | 没有具体值 | 有 RTP timestamp | PES/FLV/MP4 有 PTS/DTS 或 sample time |
| 流标识 | 有 track/control | 有 SSRC | 有 PID、stream_id、track id 等 |
| 参数集/元数据 | 可有 `fmtp` | 通常不放描述信息 | 容器可能有 codec extradata 或 metadata |

所以更准确的边界是：

```text
SDP 是会话级描述。
RTP 头是传输包级描述。
容器包头是封装结构级描述。
编解码头是码流解码级描述。
```

它们可能都出现“编码类型、时间、流标识”这些概念，但回答的问题不同。SDP 是开始传输前告诉你应该怎么解释后面的流；包头是在真正传输时逐包、逐段提供实际状态。

## 8. SETUP 和 Transport

`SETUP` 是 RTSP/RTP 链路最关键的一步。它告诉服务端这一路媒体怎么传。

### RTP over UDP

客户端请求：

```text
SETUP rtsp://127.0.0.1/live/stream/trackID=0 RTSP/1.0
CSeq: 3
Transport: RTP/AVP;unicast;client_port=10000-10001
```

服务端响应：

```text
RTSP/1.0 200 OK
CSeq: 3
Session: 12345678
Transport: RTP/AVP;unicast;client_port=10000-10001;server_port=50000-50001;ssrc=12345678
```

字段解释：

| 字段 | 含义 |
|---|---|
| `RTP/AVP` | RTP Audio/Video Profile，通常表示 RTP over UDP |
| `unicast` | 单播 |
| `client_port=10000-10001` | 客户端接收 RTP/RTCP 的端口，偶数 RTP，奇数 RTCP |
| `server_port=50000-50001` | 服务端发送 RTP/RTCP 的端口 |
| `ssrc=12345678` | 服务端声明 RTP SSRC，设备不一定都带 |

典型 UDP 端口关系：

```text
RTP  -> 偶数端口，例如 10000
RTCP -> RTP + 1，例如 10001
```

### RTP over TCP interleaved

有些网络环境 UDP 不通，会用 RTSP TCP 连接承载 RTP/RTCP，称为 interleaved。

请求：

```text
SETUP rtsp://127.0.0.1/live/stream/trackID=0 RTSP/1.0
CSeq: 3
Transport: RTP/AVP/TCP;unicast;interleaved=0-1
```

响应：

```text
RTSP/1.0 200 OK
CSeq: 3
Session: 12345678
Transport: RTP/AVP/TCP;unicast;interleaved=0-1
```

TCP interleaved 数据帧格式：

```text
$  channel  length_hi  length_lo  RTP/RTCP payload
```

常见约定：

```text
interleaved=0-1
  channel 0 -> RTP
  channel 1 -> RTCP
```

## 9. PLAY

`PLAY` 告诉服务端开始发送媒体。

请求：

```text
PLAY rtsp://127.0.0.1/live/stream RTSP/1.0
CSeq: 4
Session: 12345678
Range: npt=0.000-
```

响应：

```text
RTSP/1.0 200 OK
CSeq: 4
Session: 12345678
RTP-Info: url=rtsp://127.0.0.1/live/stream/trackID=0;seq=21601;rtptime=2663919073
```

`RTP-Info` 的作用是告诉客户端播放开始时某个 track 的初始 RTP 序号和时间戳。不是所有服务端都返回完整字段。

`PLAY` 之后才真正开始有 RTP 包过来。

## 10. PAUSE 和 TEARDOWN

`PAUSE` 暂停发送媒体：

```text
PAUSE rtsp://127.0.0.1/live/stream RTSP/1.0
CSeq: 5
Session: 12345678
```

`TEARDOWN` 结束会话：

```text
TEARDOWN rtsp://127.0.0.1/live/stream RTSP/1.0
CSeq: 6
Session: 12345678
```

学习重点：`PAUSE` 不一定释放会话资源，`TEARDOWN` 应该释放服务端会话和媒体通道，后续请求必须带正确 `Session`。

## 11. RTCP 怎么接入 RTSP 链路

RTCP 不通过 RTSP 文本传输，它和 RTP 一样属于媒体通道。

UDP 模式下常见：

```text
client_port=10000-10001
  10000 -> RTP
  10001 -> RTCP
```

TCP interleaved 模式下常见：

```text
interleaved=0-1
  channel 0 -> RTP
  channel 1 -> RTCP
```

RTCP 重点字段：

| 字段 | 说明 |
|---|---|
| `SSRC` | 对应 RTP 流 |
| `fraction lost` | 丢包比例 |
| `cumulative packets lost` | 累计丢包数 |
| `extended highest sequence number received` | 接收端看到的最高 RTP 扩展序号 |
| `interarrival jitter` | 到达抖动 |
| `LSR / DLSR` | 用于 RTT 估算 |
| `NTP timestamp` | SR 中用于把 RTP timestamp 对齐到真实时间 |
| `RTP timestamp` | SR 中和 NTP 时间对应的 RTP 时间戳 |

音视频同步时，RTCP SR 很关键：

```text
video RTP timestamp -> video NTP time
audio RTP timestamp -> audio NTP time
播放器再按 NTP 对齐音视频
```

## 12. RTP over UDP 和 RTP over TCP interleaved

常见说法容易混：

```text
RTP over UDP
  -> RTP 包通过独立 UDP 端口传输

RTP over RTSP / RTP over TCP interleaved
  -> RTP 包塞进 RTSP 的 TCP 连接里，用 `$` 帧区分 channel
```

对比：

| 维度 | RTP over UDP | RTP over TCP interleaved |
|---|---|---|
| 传输连接 | RTSP 控制 TCP + RTP/RTCP UDP 端口 | RTSP 控制和 RTP/RTCP 都走同一条 TCP |
| 丢包 | 可能丢包，需要 RTP/RTCP 处理 | TCP 不丢但可能队头阻塞 |
| 防火墙/NAT | 可能需要开放 UDP 端口 | 更容易穿过只允许 TCP 的网络 |
| 实时性 | 通常更好 | 网络差时延迟可能堆积 |
| 抓包识别 | UDP 包 Decode As RTP | TCP 流中 `$` interleaved frame 后面才是 RTP/RTCP |

## 13. Wireshark 学习重点

抓 RTSP 时，先看控制面：

```text
rtsp
```

看完整方法链：

```text
rtsp.request.method == "OPTIONS"
rtsp.request.method == "DESCRIBE"
rtsp.request.method == "SETUP"
rtsp.request.method == "PLAY"
rtsp.request.method == "TEARDOWN"
```

看 SDP：

```text
sdp
```

看 RTP：

```text
rtp
rtp.p_type == 96
rtp.ssrc == 0x12345678
rtp.marker == 1
```

看 RTCP：

```text
rtcp
rtcp.pt == 200   # Sender Report
rtcp.pt == 201   # Receiver Report
```

如果 RTP 没自动识别，按端口 Decode As RTP。UDP 模式下通常从 `SETUP` 的 `Transport` 头里找 `client_port` 或 `server_port`。

## 14. 和 GB28181 的对照

RTSP 和 GB28181 都经常配合 RTP/RTCP，但控制面不同：

| 维度 | RTSP | GB28181 |
|---|---|---|
| 控制协议 | RTSP 文本方法 | SIP / SDP / XML |
| 建会话 | DESCRIBE/SETUP/PLAY | REGISTER/INVITE/ACK/BYE 等 |
| 媒体描述 | SDP | SDP |
| 媒体承载 | RTP/RTCP | RTP/RTCP，常见 PS over RTP 或 H.264/H.265 over RTP |
| 设备管理 | 不负责设备目录、心跳、云台等 | 负责 Catalog、Keepalive、PTZ 等国标能力 |

可以这样记：

```text
RTSP 是播放控制协议。
GB28181 是监控联网协议体系。
二者都可以把媒体交给 RTP/RTCP 承载。
```

## 15. 学习顺序建议

1. 先读 `MediaProtrocl/rtp_study.md`，理解 RTP 包头、PT、Seq、Timestamp、Marker、SSRC。
2. 再读本文的 `OPTIONS -> DESCRIBE -> SETUP -> PLAY` 链路。
3. 抓一次 RTSP 拉流包，先看 RTSP，再看 SDP，再看 RTP/RTCP。
4. 对照 `Transport` 头，确认 RTP 是 UDP 端口还是 TCP interleaved。
5. 最后再写最小 RTSP client，先支持 DESCRIBE/SETUP/PLAY，再补 RTCP 和 TCP interleaved。
