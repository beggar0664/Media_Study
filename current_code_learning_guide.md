# 当前代码学习路线

这份文档用于指导如何学习当前仓库里的代码。建议顺序是：先跑通程序，再用 Wireshark/WinHex 观察字节和报文，最后回到代码里逐层阅读。

当前最适合从 GB28181 模块开始，因为它已经具备最小可运行闭环：SIP 注册鉴权、MESSAGE Keepalive/Catalog、INVITE/SDP、BYE，以及 RTP/PS over RTP 示例。

## 0. 当前代码入口图

```mermaid
flowchart TD
    A[current_code_learning_guide.md] --> B[media_layer_commonality.md]
    A --> C[MediaProtrocl/gb28181_study.md]
    C --> D[gb28181_module.h]
    D --> E[gb28181_sip_register_client.cpp]
    D --> F[gb28181_sip_mock_server.cpp]
    D --> G[gb28181_minimal_example.cpp]
    E --> H[Wireshark: SIP 5060/5062]
    F --> H
    G --> I[Wireshark: RTP 30000]
```

这张图的读法是：先读总纲，再看 GB28181 模块接口，随后分别跑 SIP client/server 和 RTP 示例，用抓包反向验证字段。

## 1. 先建立分层概念

先读：

- [media_layer_commonality.md](media_layer_commonality.md)
- [MediaProtrocl/gb28181_study.md](MediaProtrocl/gb28181_study.md)

先记住这条主线：

```text
GB28181 = SIP 信令 + SDP 协商 + RTP/RTCP 承载
常见媒体链路 = H.264 NALU -> PES -> PS -> RTP
```

每看到一个字段，先判断它属于哪一层：

```text
SIP Via / CSeq / Call-ID      -> 信令事务层
MESSAGE XML CmdType / DeviceID -> 业务控制消息
SDP m= / a=rtpmap / a=ssrc    -> 媒体参数描述层
RTP seq / timestamp / marker  -> 媒体传输层
PS 00 00 01 BA                -> 容器层
PES 00 00 01 E0 / PTS         -> 容器层里的 elementary stream 包装
H.264 67 / 68 / 65            -> 编解码层
```

## 2. 跑 SIP 闭环

打开两个 PowerShell 窗口。

窗口 1 运行 mock server：

```powershell
E:\code\Media\MediaProtrocl\GB28181\out\buildBin\gb28181_sip_mock_server.exe
```

窗口 2 运行 client：

```powershell
E:\code\Media\MediaProtrocl\GB28181\out\buildBin\gb28181_sip_register_client.exe
```

预期流程：

```text
REGISTER
401 Unauthorized
REGISTER + Authorization
200 OK
MESSAGE Keepalive
200 OK
MESSAGE Catalog
200 OK
INVITE + SDP
200 OK + SDP
ACK
BYE
200 OK
```

对应时序图：

```mermaid
sequenceDiagram
    participant Client as gb28181_sip_register_client
    participant Server as gb28181_sip_mock_server

    Client->>Server: REGISTER without Authorization
    Server-->>Client: 401 Unauthorized + WWW-Authenticate
    Client->>Client: Parse challenge and calculate Digest
    Client->>Server: REGISTER + Authorization
    Server-->>Client: 200 OK
    Client->>Server: MESSAGE Keepalive XML
    Server-->>Client: 200 OK
    Client->>Server: MESSAGE Catalog XML
    Server-->>Client: 200 OK
    Client->>Server: INVITE + SDP
    Server-->>Client: 200 OK + SDP
    Client->>Server: ACK
    Client->>Server: BYE
    Server-->>Client: 200 OK
```

这一阶段只看 SIP 信令，不看 RTP 媒体数据。

## 3. 用 Wireshark 看 SIP

抓回环网卡，过滤：

```text
sip || udp.port == 5060 || udp.port == 5062
```

重点看：

| 报文 | 观察点 |
|---|---|
| `REGISTER` | `Via`、`From`、`To`、`Call-ID`、`CSeq`、`Contact` |
| `401 Unauthorized` | `WWW-Authenticate` 里的 `realm`、`nonce`、`qop` |
| 第二次 `REGISTER` | `Authorization` 里的 Digest response |
| `MESSAGE Keepalive` | XML body 里的 `<CmdType>Keepalive</CmdType>` |
| `MESSAGE Catalog` | XML body 里的 `<CmdType>Catalog</CmdType>` |
| `INVITE + SDP` | `Content-Type: application/sdp` 和 SDP body |
| `BYE` | 会话结束事务 |

这里要区分两种 SIP body：

```text
MESSAGE XML = 业务控制命令，比如 Keepalive、Catalog
SDP         = 媒体参数说明，比如端口、payload type、编码、SSRC
```

## 4. 跑 RTP / PS over RTP 示例

运行：

```powershell
E:\code\Media\MediaProtrocl\GB28181\out\buildBin\gb28181_minimal_example.exe
```

这个程序会发送几类 RTP payload：

```text
裸 H.264 SPS/PPS/IDR over RTP
PS over RTP
强制小包分片 PS over RTP
正常 1200 字节左右分片 PS over RTP
```

Wireshark 抓回环网卡，过滤：

```text
udp.dstport == 30000
```

如果 Wireshark 没自动识别 RTP，可以用命令打开抓包：

```powershell
& 'E:\tool\Wireshark\Wireshark.exe' -r '你的抓包文件.pcapng' -d udp.port==30000,rtp
```

重点看 RTP 头：

| 字段 | 观察点 |
|---|---|
| Payload type | 示例里是动态类型 `96` |
| Sequence number | 是否连续递增 |
| Timestamp | 同一个 PS 分片组应保持同一时间戳语义 |
| Marker | 最后一包为 `1`，前面分片为 `0` |
| SSRC | 示例里是 `0x12345678` |

再看 RTP payload：

```text
裸 H.264:
67 -> SPS
68 -> PPS
65 -> IDR

PS over RTP:
00 00 01 BA -> PS pack
00 00 01 E0 -> video PES
00 00 00 01 67 -> SPS
00 00 00 01 68 -> PPS
00 00 00 01 65 -> IDR
```

PS over RTP 的载荷关系图：

```mermaid
flowchart TD
    A[H.264 Annex-B NALU] --> B[PES video stream_id 0xE0]
    B --> C[PS pack start code 00 00 01 BA]
    C --> D[RTP payload]
    D --> E[UDP 127.0.0.1:30000]
```

## 5. 按顺序读代码

### 5.1 先读头文件

入口：

- [MediaProtrocl/GB28181/gb28181_module.h](MediaProtrocl/GB28181/gb28181_module.h)

先看模块暴露了哪些能力：

```cpp
gb28181_build_register()
gb28181_build_message_keepalive()
gb28181_build_message_catalog()
gb28181_build_invite()
gb28181_build_bye()
gb28181_parse_sip_message()
gb28181_parse_www_authenticate()
gb28181_build_digest_authorization()
gb28181_build_ps_pack_h264()
gb28181_send_rtp_packet()
gb28181_send_rtp_payload_fragmented()
```

头文件解决的是“这个模块能做什么”，不要先陷入实现细节。

### 5.2 再读 SIP client

入口：

- [MediaProtrocl/GB28181/gb28181_sip_register_client.cpp](MediaProtrocl/GB28181/gb28181_sip_register_client.cpp)

看它如何串起 SIP 流程：

```text
发 REGISTER
收 401
解析 WWW-Authenticate
生成 Authorization
再发 REGISTER
发 MESSAGE Keepalive
发 MESSAGE Catalog
发 INVITE
发 ACK
发 BYE
```

这一份代码是学习 SIP 事务顺序的主入口。

### 5.3 再读 mock server

入口：

- [MediaProtrocl/GB28181/gb28181_sip_mock_server.cpp](MediaProtrocl/GB28181/gb28181_sip_mock_server.cpp)

看它如何模拟平台：

```text
收到无鉴权 REGISTER -> 回 401
收到带 Authorization REGISTER -> 回 200
收到 MESSAGE -> 解析 XML CmdType -> 回 200
收到 INVITE -> 回 200 + SDP
收到 ACK -> 标记媒体会话建立
收到 BYE -> 回 200
```

这份代码适合理解平台端最小响应逻辑。

### 5.4 再读 RTP 示例

入口：

- [MediaProtrocl/GB28181/gb28181_minimal_example.cpp](MediaProtrocl/GB28181/gb28181_minimal_example.cpp)

重点看：

```text
如何构造 SPS/PPS/IDR
如何生成 PS pack
如何发送裸 H.264 over RTP
如何发送 PS over RTP
如何按 max_payload 分片
```

这份代码要和 Wireshark 抓包一起看。

### 5.5 最后读模块实现

入口：

- [MediaProtrocl/GB28181/gb28181_module.cpp](MediaProtrocl/GB28181/gb28181_module.cpp)

最后再看实现细节：

```text
SIP 文本如何拼接
SDP 如何拼接
Digest MD5 如何计算
XML tag 如何提取
PS pack / PES / PTS 如何写字节
jrtplib 如何创建 RTP session
RTP 分片如何控制 marker/timestamp
```

## 6. 分阶段学习目标

建议按这个顺序推进：

| 阶段 | 学习目标 | 验证方式 |
|---|---|---|
| 1 | SIP 文本和事务 | 跑 mock server + client，看 REGISTER/401/200 |
| 2 | Digest 鉴权 | Wireshark 看 `WWW-Authenticate` 和 `Authorization` |
| 3 | SIP MESSAGE XML | 看 Keepalive/Catalog 的 `<CmdType>` |
| 4 | SDP 媒体协商 | 看 `m=`、`a=rtpmap`、`a=ssrc` |
| 5 | RTP 头 | 看 PT、Seq、Timestamp、Marker、SSRC |
| 6 | PS over RTP | 看 `00 00 01 BA`、`00 00 01 E0`、NALU 起始码 |
| 7 | 分片 | 看多包同一 timestamp、最后一包 marker=1 |

## 7. 后续可以扩展什么

当前代码是学习用最小模块，不是完整设备端。后续可以按优先级补：

1. Catalog 响应列表解析。
2. DeviceInfo / DeviceStatus XML。
3. 更完整的 SIP dialog 状态管理。
4. H.264 FU-A 和 H.265 FU RTP 分片。
5. RTP over TCP / 国标主动和被动模式。
6. 对接真实 GB28181 平台。

学习时不要追求一次写完整协议栈。先让每一层能跑、能抓包、能解释字段，再逐步补完整。
