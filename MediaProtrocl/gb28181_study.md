# GB28181 学习笔记

GB28181 不是容器格式，它是一套国标监控联网协议。它在媒体链路里的角色是：用 SIP 做信令和事务控制，用 SDP 描述媒体参数，用 RTP/RTCP 承载真正的音视频 payload。

```text
容器层：PS / TS / FLV / MP4
协议传输层：RTSP / RTP / RTCP / RTMP / GB28181
编解码层：H.264 / H.265 / AAC
```

GB28181 要和 RTP 一起看，但不要把它当成 RTP 本身。GB28181 决定“谁和谁建立会话、用什么端口、什么 SSRC、什么 payload type”；RTP 决定“媒体包怎么编号、怎么打时间戳、怎么发到对端”。

## 1. GB28181 管什么

GB28181 主要回答这些问题：

```text
1. 设备怎么向平台注册
2. 平台怎么查设备目录和状态
3. 平台怎么请求实时预览或录像回放
4. 媒体流的 IP、端口、编码类型、SSRC 怎么协商
5. RTP 媒体包怎么发给平台
6. 会话结束时怎么 BYE
```

最小预览链路可以先简化成：

```text
REGISTER
  -> 平台 200 OK 或 401 Unauthorized
  -> 如果 401，需要 Digest 鉴权后重新 REGISTER

INVITE + SDP
  -> 携带媒体地址、端口、payload type、SSRC
  -> 平台 200 OK

RTP
  -> 发送 H.264/H.265/AAC payload

BYE
  -> 结束会话
```

## 2. SIP 事务层

SIP 是 GB28181 的信令文本格式。它的重点不是音视频字节，而是事务身份和会话状态。

常见字段：

| 字段 | 作用 |
|---|---|
| `REGISTER` | 设备注册到平台 |
| `INVITE` | 发起媒体会话，通常带 SDP |
| `BYE` | 结束会话 |
| `MESSAGE` | Keepalive、Catalog、DeviceInfo 等 XML 消息常用方法 |
| `Via` | 请求经过路径，本端地址和 branch 参数在这里 |
| `From` / `To` | 会话两端 SIP URI 和 tag |
| `Call-ID` | 一次会话或事务的标识 |
| `CSeq` | 请求序号和方法名，例如 `2 INVITE` |
| `Contact` | 对端后续联系本端的地址 |
| `Content-Type` | body 类型，SDP 常见为 `application/sdp` |
| `Content-Length` | body 字节数 |

响应报文首行长这样：

```text
SIP/2.0 200 OK
SIP/2.0 401 Unauthorized
```

学习时优先解析这些字段：`status_code`、`Call-ID`、`CSeq`、`From`、`To`、`Contact`、`Content-Length`、`body`。这些字段决定这条响应属于哪个请求、是否要鉴权、后续媒体协商是否成立。

本仓库的最小模块已经加了 `gb28181_parse_sip_message()`，它不做完整 RFC 级 SIP 解析，只做学习时最常用的头字段抽取。

## 3. SDP 协商层

SDP 是 SIP body 中的媒体描述。它不传媒体数据，只告诉对端媒体应该怎么传。

一个最小 SDP 例子：

```text
v=0
o=34020000001320000001 0 0 IN IP4 192.168.1.10
s=Play
c=IN IP4 192.168.1.10
t=0 0
m=video 10000 RTP/AVP 96
a=sendonly
a=rtpmap:96 H264/90000
a=ssrc:0300000001
```

关键字段：

| 字段 | 含义 |
|---|---|
| `v=0` | SDP 版本 |
| `o=` | origin，会话发起者、会话版本、地址 |
| `s=` | session name |
| `c=IN IP4 ...` | 媒体连接地址 |
| `t=0 0` | 会话时间，实时预览常用 0 0 |
| `m=video 10000 RTP/AVP 96` | 媒体类型、端口、传输协议、payload type |
| `a=sendonly` | 本端只发送媒体。若从平台角度发 INVITE，常见也会看到 `recvonly` |
| `a=rtpmap:96 H264/90000` | payload type 96 对应 H.264，RTP clock rate 是 90000 |
| `a=ssrc:...` | RTP SSRC，标识这一路同步源 |

要注意：`m=` 行里的端口和 IP 是 RTP 目的/接收端口，不是 SIP 5060 端口。SIP 负责谈事，RTP 负责传媒体。

## 4. RTP 承载层

GB28181 常用 RTP/RTCP 承载音视频。RTP 头部重点字段是：

| 字段 | 作用 |
|---|---|
| Payload Type | 对应 SDP 中的 `m=` 和 `a=rtpmap` |
| Sequence Number | RTP 包序号，用于检测丢包、乱序 |
| Timestamp | 媒体时间戳，视频一般 90000 Hz 时钟 |
| SSRC | 一路媒体流的同步源身份 |
| Marker | 常用于标记一个访问单元或帧的结束 |

RTP payload 里放的是编码层数据，例如 H.264 NALU 或 H.265 NALU 的 RTP payload 格式。RTP 不理解 SPS/PPS/VPS/IDR 的解码含义，它只负责分包、编号、打时间戳和发送。

## 5. jrtplib 和 jthread 的位置

`E:\code\Media\MediaProtrocl\GB28181` 目录里有：

```text
jrtplib-3.11.2
  -> RTP / RTCP 会话库

jthread-1.3.3
  -> jrtplib 可选线程支持库

gb28181_module.h / gb28181_module.cpp
  -> 最小 GB28181 学习模块
```

对 GB28181 来说，`jrtplib` 不是国标协议库，它只负责 RTP/RTCP 这部分。SIP 报文构造、响应解析、鉴权、SDP 字段解释仍然属于 GB28181 模块自己的职责。

最小模块里 jrtplib 的调用关系：

```text
gb28181_start()
  -> RTPSessionParams::SetOwnTimestampUnit(1.0 / 90000.0)
  -> RTPSessionParams::SetUsePredefinedSSRC(true)
  -> RTPSessionParams::SetPredefinedSSRC(ssrc)
  -> RTPUDPv4TransmissionParams::SetPortbase(local_rtp_port)
  -> RTPSession::Create(...)
  -> RTPSession::AddDestination(remote_rtp_ip, remote_rtp_port)

gb28181_send_rtp_packet()
  -> RTPSession::SendPacket(payload, len, payload_type, marker, timestamp_inc)
  -> timestamp_inc 是“发完当前包后递增多少”，不是绝对 PTS
```

## 6. 当前最小模块怎么读

代码位置：`MediaProtrocl/GB28181/gb28181_module.h` 和 `MediaProtrocl/GB28181/gb28181_module.cpp`。

已经覆盖三块：

```text
1. jrtplib RTP 发送
   - 创建 RTPSession
   - 绑定本地 RTP 端口
   - 添加远端 RTP 地址
   - 发送 payload

2. SIP 事务/响应解析
   - 解析 SIP/2.0 状态码
   - 解析 Call-ID、CSeq、From、To、Contact、Content-Length
   - body 指向 SDP 或 XML 内容

3. SDP + RTP 最小发送流
   - INVITE 中带 SDP body
   - SDP 描述 video、RTP/AVP、payload type、SSRC
   - RTP 发送接口接收编码 payload
```

最小示例：`MediaProtrocl/GB28181/gb28181_minimal_example.cpp`。

构建入口：

```powershell
cd E:\code\Media\MediaProtrocl\GB28181
cmake -S . -B build
cmake --build build
```

## 7. 还没做的完整国标能力

当前模块用于学习边界，不是完整生产 GB28181 设备端。还缺这些能力：

- SIP UDP/TCP socket 收发循环。
- `401 Unauthorized` 后的 Digest 鉴权计算。
- `MESSAGE` Keepalive。
- Catalog、DeviceInfo、DeviceStatus 等 XML 消息。
- INVITE/ACK/BYE 的完整 dialog 状态维护。
- H.264/H.265 RTP 分片器，例如 H.264 FU-A、H.265 FU。
- RTP over TCP 或国标 TCP 被动/主动模式。

学习顺序建议先把现有模块里的 `REGISTER -> INVITE+SDP -> RTP -> BYE` 跑通，再补 SIP socket 和 Digest，最后补真正的编码 payload 分片。
