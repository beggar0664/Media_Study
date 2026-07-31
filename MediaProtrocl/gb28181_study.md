# GB28181 学习总纲

GB28181 不是容器格式，也不是单纯的 RTP 协议实现。它是一套国标监控联网协议，核心任务是把“设备、平台、会话、媒体流”组织起来。

```text
GB28181 = SIP 信令 + SDP 协商 + RTP/RTCP 承载
```

和其他层的关系可以这样拆：

```text
编码层: H.264 / H.265 / AAC
容器层: PS / TS / FLV / MP4
协议层: SIP / SDP / RTP / RTCP / GB28181
传输层: UDP / TCP / TLS
```

## 1. 为什么要分层

分层的目的不是概念化，而是为了把职责拆开：

- 编解码层负责“内容是什么”，例如一个 H.264 NALU 是 SPS、PPS 还是 IDR。
- 容器层负责“文件里怎么装”，例如 PS、TS、FLV、MP4 如何组织音视频数据。
- 协议层负责“怎么协商、怎么传、怎么维护会话”，例如 SIP 建会话、SDP 描述媒体、RTP 传负载、RTCP 反馈统计。

GB28181 的重点不在“包里是什么编码帧”，而在“会话怎么建立、媒体参数怎么协商、媒体流怎么按约定送达”。

## 2. GB28181 管什么

GB28181 主要回答这些问题：

1. 设备怎么注册到平台。
2. 平台怎么查询设备目录和状态。
3. 平台怎么发起实时预览或回放。
4. 媒体流的 IP、端口、编码类型、payload type、SSRC 怎么协商。
5. RTP 媒体包怎么发给平台。
6. 会话如何结束、如何 BYE。

最小流程可以先理解成：

```text
REGISTER
  -> 平台 200 OK 或 401 Unauthorized
  -> 如果是 401，设备用 Digest 鉴权后重新 REGISTER

MESSAGE
  -> Keepalive / Catalog / DeviceInfo / DeviceStatus

INVITE + SDP
  -> 协商媒体地址、端口、payload type、SSRC、编码名
  -> 平台 200 OK

RTP
  -> 发送 H.264 / H.265 / AAC payload

BYE
  -> 结束会话
```

## 3. SIP 是什么

SIP 是 GB28181 的事务和会话控制语言。它负责“谁和谁说话、说什么、什么时候结束”，但不直接承载音视频字节。

常见 SIP 方法：

| 方法 | 作用 |
|---|---|
| `REGISTER` | 设备注册到平台 |
| `INVITE` | 发起媒体会话，通常携带 SDP |
| `ACK` | 确认 INVITE |
| `BYE` | 结束会话 |
| `MESSAGE` | Keepalive、Catalog、DeviceInfo 等 XML 消息常用 |

常见 SIP 头字段：

| 字段 | 作用 |
|---|---|
| `Via` | 请求经过的路径和本端地址 |
| `From` / `To` | 会话两端标识，通常携带 tag |
| `Call-ID` | 同一事务/对话的标识 |
| `CSeq` | 请求序号和方法名 |
| `Contact` | 后续联系本端的地址 |
| `Content-Type` | body 类型，SDP 常见为 `application/sdp` |
| `Content-Length` | body 长度 |

响应最常见的是：

```text
SIP/2.0 200 OK
SIP/2.0 401 Unauthorized
```

学习时先抓这几个字段：`status_code`、`Call-ID`、`CSeq`、`From`、`To`、`Contact`、`Content-Length`、`body`。它们决定一条 SIP 事务是不是成功、是否需要鉴权、后续媒体协商是否成立。

### 3.1 Digest 鉴权

`401 Unauthorized` 之后，平台通常会要求 Digest 鉴权。设备要用 `WWW-Authenticate` 里的 `realm`、`nonce` 等信息重新计算响应值，再发第二次 `REGISTER` 或 `INVITE`。

这个闭环可以理解成：

```text
第一次 REGISTER / INVITE
  -> 401 Unauthorized
  -> 响应头里带 WWW-Authenticate: Digest realm=..., nonce=...
  -> 设备计算 Authorization: Digest ... response=...
  -> 再次发送 REGISTER / INVITE
```

当前最小模块已经补了两块底座：

- `gb28181_parse_www_authenticate()`：解析 `realm` / `nonce` / `qop` / `opaque` / `algorithm`
- `gb28181_build_digest_authorization()`：根据用户名、密码、方法、URI 和 challenge 计算 `Authorization` 头

现在又补了一个最小 UDP 收发示例：

- `gb28181_sip_register_client.cpp`：发第一次 `REGISTER`，接 401，再发带 `Authorization` 的第二次 `REGISTER`

这还不是完整 SIP 状态机，但已经足够支撑你下一步接真实 401 响应做学习验证。

## 4. SDP 是什么

SDP 是 SIP body 里的媒体描述文本。它不传媒体数据，只告诉对端“媒体要怎么传”。

一个最小例子：

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

字段含义：

| 字段 | 含义 |
|---|---|
| `v=0` | SDP 版本 |
| `o=` | origin，会话发起者和版本信息 |
| `s=` | session name |
| `c=IN IP4 ...` | 媒体连接地址 |
| `t=0 0` | 会话时间，实时预览常用 `0 0` |
| `m=video 10000 RTP/AVP 96` | 媒体类型、端口、传输协议、payload type |
| `a=sendonly` | 本端只发送媒体 |
| `a=recvonly` | 本端只接收媒体 |
| `a=rtpmap:96 H264/90000` | payload type 96 对应 H.264，clock rate 90000 |
| `a=ssrc:...` | RTP 同步源标识 |

要特别注意：`m=` 行里的端口和 IP 是 RTP 传输端口，不是 SIP 5060 端口。SIP 负责谈事，RTP 负责传媒体。

### 4.1 SDP 和容器头是不是重叠

不重叠，但信息有交集。

- 容器头描述的是“文件内部怎么组织数据”。
- SDP 描述的是“网络上怎么实时传输这些数据”。

例如编码名、采样率、通道数、分辨率这些信息可能在容器和 SDP 里都能看到，但作用不同：一个面向文件，一个面向实时会话。

## 5. RTP 是什么

RTP 是真正承载媒体负载的传输格式。GB28181 常用 RTP/RTCP 承载音视频。

RTP 头部里最重要的字段：

| 字段 | 作用 |
|---|---|
| Payload Type | 对应 SDP 中的 `m=` 和 `a=rtpmap` |
| Sequence Number | 包序号，用于检测丢包和乱序 |
| Timestamp | 媒体时间戳，视频常用 90000 Hz 时钟 |
| SSRC | 同步源标识 |
| Marker | 常用于标记一个访问单元结束或关键边界 |

RTP payload 里放的是编码层数据，不是容器文件。它可以是：

- H.264 NALU 的 RTP 负载格式
- H.265 NALU 的 RTP 负载格式
- AAC 的 RTP 负载格式

RTP 不负责理解 SPS、PPS、VPS、IDR 的语义，它只负责分包、编号、时间戳和发送。

### 5.1 负载层和编码层的关系

可以把它理解为：

```text
H.264/H.265 帧 = 包裹内容
RTP = 快递单 + 快递袋
SIP/SDP = 下单和收件地址信息
UDP/IP = 运输车辆和道路
```

这也是为什么 `RTP` 和 `GB28181` 不等价。GB28181 负责把路和地址谈好，RTP 负责把包裹送过去。

## 6. RTCP 是什么

RTCP 是 RTP 的配套控制协议，负责统计和同步。

常见作用：

- 汇报收包质量、丢包、抖动
- 同步音视频流
- 发送 BYE，结束同步源

在 GB28181 的学习阶段，RTCP 常常被低估，但它不是可有可无。它至少帮助你理解：RTP 不是单向裸发包，协议本身还有反馈和统计。

## 7. GB28181 与 RTSP / RTMP 的关系

可以把它们类比成：

```text
SIP / RTSP / GB28181 = 接单和调度系统
RTP = 负责送货的快递车
H.264 / H.265 / AAC = 包裹内容
PS / TS / FLV / MP4 = 包裹箱子
```

其中：

- RTSP 更偏控制实时拉流会话。
- GB28181 更偏国标监控体系里的设备接入和平台联动。
- RTMP 更偏推流链路里的经典直播协议。

## 8. 当前仓库里的最小 GB28181 模块

位置：

```text
E:\code\Media\MediaProtrocl\GB28181
```

核心文件：

- `gb28181_module.h`
- `gb28181_module.cpp`
- `gb28181_minimal_example.cpp`

当前模块已经覆盖：

1. `REGISTER` 文本生成。
2. `INVITE + SDP` 文本生成。
3. `BYE` 文本生成。
4. SIP 响应的基础头字段解析。
5. 使用 `jrtplib` 建立 RTP 会话并发送 payload。

### 8.1 `gb28181_module.h` 的边界

这个头文件只保留最小学习接口：

- `gb28181_config_t`：配置
- `gb28181_sip_message_t`：SIP 解析结果
- `gb28181_create / start / stop / destroy`：生命周期
- `gb28181_build_register / invite / bye / sdp`：报文构造
- `gb28181_parse_sip_message`：基础响应解析
- `gb28181_send_rtp_packet`：RTP 发送

### 8.2 `gb28181_module.cpp` 实际做了什么

内部用的是 `jrtplib`，不是自己手写 RTP socket。

关键流程是：

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
```

这里的 `timestamp_inc` 是“发完当前包后，时间戳增加多少”，不是绝对 PTS。

## 9. 当前最小模块还没做完什么

现在的模块适合学习，不是完整国标设备端。还缺这些能力：

- SIP UDP/TCP 收发循环
- 完整 SIP 客户端状态机
- `MESSAGE` Keepalive
- Catalog / DeviceInfo / DeviceStatus XML
- INVITE / ACK / BYE 的完整 dialog 状态管理
- H.264 / H.265 RTP 分片器，例如 H.264 FU-A、H.265 FU
- RTP over TCP 或国标主动/被动模式

其中 Digest 的“头字段解析 + Authorization 生成”已经有最小实现，缺的是把它接到真实 socket 收发循环里。

## 10. 如何验证

最小验证思路是：

1. 先构造 `REGISTER` / `INVITE + SDP` 文本，确认字段正确。
2. 再用 `jrtplib` 发一个简单 RTP payload。
3. 用 Wireshark 抓回环网卡，看 RTP 头部字段是否和预期一致。
4. 再补真实的分片和真实的 SIP 会话控制。

Wireshark 里重点看：

- `udp.port == 10000`
- `rtp`
- `ssrc`
- `sequence number`
- `timestamp`
- `payload type`

如果你在做 RTP 的学习，建议把 `gb28181_minimal_example.cpp` 和 Wireshark 抓包结果一起看，这样最容易把“信令、协商、承载”三层分开。

### 10.1 当前可跑的 SIP 学习闭环

现在还可以先只看 SIP / Digest，不接真实平台：

```text
gb28181_sip_mock_server.exe
  -> 回 401，再回 200

gb28181_sip_register_client.exe
  -> 第一次 REGISTER
  -> 401 Unauthorized
  -> 第二次 REGISTER + Authorization
  -> 200 OK
  -> INVITE + SDP
  -> 200 OK + SDP
  -> ACK
  -> BYE
  -> 200 OK
```

这个 mock 闭环的作用是确认：

- SIP 报文头是否能被正确解析
- Digest 的 `realm / nonce / qop` 是否能被正确提取
- `Authorization` 是否能被正确生成并回送
- `INVITE / ACK / BYE` 的最小会话状态是否能跑通

## 11. 典型误区

### 11.1 把 GB28181 当成 RTP

这是最常见的误区。GB28181 不是 RTP，RTP 只是它后半段承载媒体的那部分。

### 11.2 把 SDP 当成媒体数据

SDP 只描述媒体，不传媒体。

### 11.3 把容器头和协议头混为一谈

容器层解决文件组织，协议层解决网络协商。它们可以重复表达编码信息，但职责不同。

### 11.4 把 I 帧和 IDR 帧完全等同

IDR 是一种 I 帧，但 I 帧不一定是 IDR。IDR 的特点是不会引用更早的参考帧，适合作为随机访问点。

## 12. 学习顺序建议

建议顺序是：

1. 先把 `REGISTER -> INVITE + SDP -> RTP -> BYE` 跑通。
2. 再补 `401 Digest`。
3. 再补 `MESSAGE` / `Catalog` / `DeviceInfo`。
4. 再补真正的 H.264/H.265 RTP 分片。
5. 最后再看真实设备联调和异常恢复。

## 13. 现有文件入口

- [GB28181 README](GB28181/README.md)
- [GB28181 minimal example](GB28181/gb28181_minimal_example.cpp)
- [GB28181 module header](GB28181/gb28181_module.h)
- [GB28181 module implementation](GB28181/gb28181_module.cpp)
