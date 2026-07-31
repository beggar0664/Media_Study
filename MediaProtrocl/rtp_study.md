# RTP 学习笔记

RTP 不是容器格式，它是实时媒体承载协议。它通常出现在 RTSP、GB28181 等协议的媒体链路里：

```text
RTSP / GB28181
  -> RTP / RTCP
  -> H.264 / H.265 / AAC payload
```

学习 RTP 时要先把层次分清楚：

```text
容器层：PS / TS / FLV / MP4
协议传输层：RTSP / RTP / RTCP / RTMP / GB28181
编解码层：H.264 / H.265 / AAC
```

RTP 不负责描述文件结构，也不负责解码画面。它只负责把编码后的媒体 payload 放到网络包里，并提供序号、时间戳、负载类型、同步源等信息。

## 1. 三层边界

学习媒体链路时，最容易混的是“封装”和“承载”。可以先用这三层来拆：

```text
编解码层
  -> 决定画面/声音如何压缩
  -> 输出 H.264 NALU、H.265 NALU、AAC frame 等编码数据

容器层
  -> 决定编码数据如何组织成文件或流式容器
  -> 负责轨道、包结构、索引、时间戳、节目表、元数据等

协议传输层
  -> 决定数据如何在网络中建立会话、协商、分包和传输
  -> 负责信令、承载、实时反馈、丢包/乱序处理依据等
```

可以用一个快递类比帮助建立直觉：

```text
H.264 / H.265 / AAC
  -> 包裹里的真正内容

PS / TS / FLV / MP4
  -> 包装盒 / 文件箱 / 集装箱

RTP
  -> 快递小包裹的运单 + 分拣编号 + 实时投递包

UDP / IP
  -> 运输车辆 / 道路

RTSP / GB28181
  -> 下单、协商地址、告诉双方怎么寄、什么时候开始/停止的调度系统
```

这个类比不能反过来过度简化成“RTP、RTSP、GB28181 都是快递员”。它们在协议传输层里的分工不同：RTP 负责真正搬媒体 payload，RTSP/GB28181 更偏会话控制和调度，UDP/IP 才是更底层的网络运输。

例如当前 GB28181 最小示例是：

```text
货物：H.264 NALU
运单和分拣信息：RTP Header，包括 PT、Seq、Timestamp、SSRC、Marker
运输：UDP/IP
调度系统：GB28181 的 SIP/SDP
```

如果是 `PS over RTP`，则多一层包装：

```text
货物：H.264 / H.265 / AAC
包装箱：PS
运单：RTP Header
运输：UDP/IP
调度系统：GB28181
```

### 编解码层

编解码层关心“压缩数据本身怎么解释”。以 H.264 为例，真正决定能不能解出画面的是：

```text
SPS / PPS
IDR / 非 IDR slice
参考帧关系
NALU 类型
profile / level
分辨率、帧率、码流约束
```

RTP、TS、FLV 都不会替代 H.264 解码规则。它们只是把 H.264 的编码数据装起来或送出去。

### 容器层

容器层关心“编码数据怎么被组织”。例如：

| 容器 | 典型职责 |
|---|---|
| PS | 按 MPEG-PS pack/PES 组织音视频，GB28181 历史上常见 PS over RTP |
| TS | 固定 188 字节 TS packet，PAT/PMT 描述节目和 PID，PES 承载音视频 |
| FLV | 用 Tag 组织音频、视频、metadata，常作为 RTMP message body 的内容 |
| MP4 | 用 box/atom 组织轨道、样本表、索引、时间尺度，适合文件存储和点播 |

容器层通常会回答：

```text
1. 这个文件/流里有哪些轨道
2. 每路轨道是什么编码格式
3. 每个样本或 PES 的边界在哪里
4. 时间戳怎么存，单位是什么
5. 有没有索引，如何 seek
6. 元数据、节目表、私有数据放在哪里
```

### 协议传输层

协议传输层关心“网络上怎么建立关系并传输”。例如：

| 协议 | 典型职责 |
|---|---|
| RTSP | 会话控制，DESCRIBE/SETUP/PLAY/TEARDOWN，通常配合 RTP/RTCP 承载媒体 |
| RTP | 实时媒体承载，给每个网络媒体包提供 PT、Seq、Timestamp、SSRC、Marker |
| RTCP | RTP 的控制反馈，统计丢包、抖动、同步 RTP timestamp 和 NTP 时间 |
| RTMP | 长连接推流协议，message/chunk 传输，常承载 FLV Tag 风格数据 |
| GB28181 | 国标监控信令体系，用 SIP/SDP 建会话，媒体常通过 RTP/RTCP 承载 |

RTP 位于协议传输层里的“媒体承载”位置。它不负责完整会话控制，通常由 RTSP 或 GB28181 负责告诉双方：IP、端口、payload type、clock rate、SSRC、发送方向等。

## 2. RTP 和容器层的区别

RTP 和 TS/PS/FLV/MP4 的核心区别是：

```text
容器层解决“数据如何组织成一个可描述、可存储、可复用的媒体结构”
RTP 解决“实时网络中每个媒体包如何编号、定时、标识和承载”
```

具体对比：

| 维度 | RTP | 容器层，如 TS/PS/FLV/MP4 |
|---|---|---|
| 主要目标 | 实时网络传输 | 文件/流式封装组织 |
| 基本单位 | RTP packet | TS packet、PES、FLV Tag、MP4 sample/box 等 |
| 是否描述完整文件结构 | 否 | 是，尤其 MP4/FLV/TS 都有自己的结构语义 |
| 是否提供轨道/节目表 | RTP 本身不提供 | TS 有 PAT/PMT，MP4 有 track box，FLV 有 Tag 类型和 metadata |
| 时间信息 | RTP timestamp，服务于实时播放和同步 | PTS/DTS/time scale 等，服务于解复用、播放顺序、seek |
| 包序号 | 有 sequence number，用于丢包/乱序判断 | 容器通常不以网络丢包序号为核心；TS 有 continuity counter 但语义不同 |
| 同步源 | 有 SSRC | 容器通常用 track id、PID、stream_id 等标识轨道或流 |
| 负载类型 | PT，需要 SDP 或静态表解释 | 容器内部有 codec id、stream_type、sample description 等 |
| 可靠性 | 通常 UDP，不保证到达 | 文件容器一般假设数据可完整读取；传输型容器另看底层链路 |
| 是否适合 seek | RTP 本身不适合文件 seek | MP4/FLV/TS 等可以通过索引或时间戳辅助 seek |

几个容易混的点：

```text
RTP timestamp 不是 PTS/DTS，但都服务于播放时序。
RTP sequence number 不是帧号，也不是 TS continuity counter。
RTP payload type 不是 codec id 本身，动态 PT 需要 SDP 的 rtpmap 解释。
RTP SSRC 不是容器 track id，也不是 TS PID。
```

### RTP 可以承载容器，也可以直接承载编码数据

RTP 的 payload 不一定直接是 H.264 NALU。它也可以承载更上层的封装。

常见两种路线：

```text
路线 A：RTP 直接承载编码 payload
RTP payload -> H.264 NALU / H.265 NALU / AAC frame

路线 B：RTP 承载容器 payload
RTP payload -> PS 包 / TS 包 -> PES -> H.264/H.265/AAC
```

这就是为什么 GB28181 里经常会听到：

```text
H.264 over RTP
PS over RTP
```

二者不是同一层东西。`H.264 over RTP` 是 RTP 直接背编码数据；`PS over RTP` 是 RTP 背 PS 容器，再由 PS/PES 背编码数据。

## 3. RTP 管什么

RTP 主要回答这些问题：

```text
1. 这一包属于哪一路媒体流
2. 这一包是第几个包
3. 这一包对应哪个媒体时间
4. 这一包里的 payload 是什么类型
5. 这一包是否是一个访问单元或帧的结束点
6. 接收端如何发现丢包、乱序和重排
```

RTP 本身不保证可靠传输。它通常跑在 UDP 上，丢包、乱序、抖动需要接收端结合 sequence number、timestamp、jitter buffer 和 RTCP 处理。

## 4. RTP 固定头结构

没有扩展头、没有 CSRC 的标准 RTP 固定头是 12 字节：

```text
0                   1                   2                   3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|X|  CC   |M|     PT      |       sequence number         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           timestamp                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           synchronization source (SSRC) identifier             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

字段说明：

| 字段 | 位数 | 作用 |
|---|---:|---|
| `V` | 2 | RTP 版本，当前固定为 `2` |
| `P` | 1 | Padding 标记，表示包尾是否有填充字节 |
| `X` | 1 | Extension 标记，表示固定头后是否有 RTP 扩展头 |
| `CC` | 4 | CSRC 个数，普通点对点媒体包一般为 `0` |
| `M` | 1 | Marker，语义由 payload 格式决定。视频里常用于标记一个访问单元/帧结束 |
| `PT` | 7 | Payload Type，指示 RTP payload 的编码类型或动态类型编号 |
| `sequence number` | 16 | RTP 包序号，每发一个 RTP 包递增 1，用于发现丢包和乱序 |
| `timestamp` | 32 | 媒体时间戳，不是系统时间。视频常用 90000 Hz 时钟 |
| `SSRC` | 32 | Synchronization Source，一路 RTP 流的同步源标识 |

如果 `CC > 0`，固定头后还会跟 `CC * 4` 字节的 CSRC 列表。如果 `X = 1`，CSRC 后面还会跟 RTP header extension。

## 5. 字段重点

### Payload Type

RTP 的 `PT` 只有 7 bit，取值范围 `0..127`。

常见关系：

```text
静态 PT：老标准里定义了一些固定映射，例如 PCMU=0
动态 PT：96..127 常用于 SDP 协商，例如 H264/90000
```

这次 GB28181 示例 SDP 里写的是：

```text
a=rtpmap:96 H264/90000
```

所以 RTP 头里的 payload type 应该是 `96`。

### Sequence Number

`sequence number` 是 RTP 包序号，不是帧号。一个视频帧如果被拆成多个 RTP 包，每个 RTP 包都会消耗一个 sequence number。

接收端可以用它判断：

```text
连续：21601, 21602, 21603
丢包：21601, 21603，中间少了 21602
乱序：21601, 21603, 21602
```

### Timestamp

`timestamp` 是媒体时间，不是抓包时间，也不是 wall clock。

对 H.264/H.265 视频，常见 clock rate 是 `90000`。如果是 10 fps：

```text
90000 / 10 = 9000
```

所以相邻两帧的 RTP timestamp 通常相差 `9000`。

同一个访问单元内的多个 RTP 包，通常使用同一个 RTP timestamp。例如 SPS、PPS、IDR 被作为同一帧前的参数集和 IDR 访问单元一起发送时，可以看到它们 timestamp 相同，最后一个包 marker 为 1。

### Marker

Marker 的含义不是 RTP 固定定义死的，它由 payload 格式决定。

视频场景里常见用法：

```text
一帧或一个 access unit 的最后一个 RTP 包：Marker = 1
其他包：Marker = 0
```

这对接收端很重要，因为它可以用 marker 判断一个访问单元是否收完。

### SSRC

SSRC 是一路 RTP 流的身份。它不是 SIP ID，也不是 GB28181 设备 ID，但 GB28181/SDP 里常会带 `a=ssrc` 来约定这一路 RTP 流的 SSRC。

这次示例中：

```text
SSRC hex: 0x12345678
SSRC dec: 305419896
SDP a=ssrc:0305419896
```

`0305419896` 是十进制补齐到 10 位的写法。

## 6. Wireshark Decode As RTP

本仓库 GB28181 示例使用本机回环发送 RTP：

```text
127.0.0.1:10000 -> 127.0.0.1:30000
```

抓包时选回环网卡：

```text
Npcap Loopback Adapter
```

显示过滤条件：

```text
udp.dstport == 30000
```

如果 Wireshark 只显示 `UDP payload`，说明它还没有自动识别 RTP。可以用：

```text
Analyze -> Decode As...
Field: UDP port
Value: 30000
Current: RTP
```

或者命令行强制按 RTP 打开：

```powershell
& 'E:\tool\Wireshark\Wireshark.exe' -r 'C:\Users\admin\Documents\GB28181_TEST2.pcapng' -d udp.port==30000,rtp
```

用 `tshark` 验证：

```powershell
& 'E:\tool\Wireshark\tshark.exe' -r 'C:\Users\admin\Documents\GB28181_TEST2.pcapng' -d udp.port==30000,rtp -Y "udp.dstport == 30000" -T fields -e frame.number -e udp.srcport -e udp.dstport -e rtp.p_type -e rtp.seq -e rtp.timestamp -e rtp.marker -e rtp.ssrc
```

## 7. 真实抓包字段对照

抓包文件：

```text
C:\Users\admin\Documents\GB28181_TEST2.pcapng
```

程序发包配置：

```text
local RTP port: 10000
remote RTP port: 30000
payload type: 96
SSRC: 0x12345678
clock rate: 90000
fps: 10
timestamp increment: 9000
```

`tshark` 解析结果中的关键包：

| Frame | 方向 | PT | Seq | Timestamp | Marker | SSRC | Payload |
|---:|---|---:|---:|---:|---|---|---|
| 18 | `127.0.0.1:10000 -> 127.0.0.1:30000` | 96 | 21601 | 2663919073 | False | `0x12345678` | SPS |
| 20 | 同上 | 96 | 21602 | 2663919073 | False | `0x12345678` | PPS |
| 22 | 同上 | 96 | 21603 | 2663919073 | True | `0x12345678` | IDR |
| 32 | 同上 | 96 | 21604 | 2663928073 | True | `0x12345678` | IDR |
| 34 | 同上 | 96 | 21605 | 2663937073 | True | `0x12345678` | IDR |
| 36 | 同上 | 96 | 21606 | 2663946073 | True | `0x12345678` | IDR |
| 38 | 同上 | 96 | 21607 | 2663955073 | True | `0x12345678` | IDR |

可以看到：

```text
21601 -> 21602 -> 21603 -> 21604 ... sequence number 连续递增
2663919073 是 SPS/PPS/IDR 首组访问单元的相同 timestamp
后续 IDR 每次 timestamp + 9000
IDR 包 marker = True
SSRC 始终是 0x12345678
```

## 8. Frame 38 字节级拆解

Frame 38 的 UDP payload 是 26 字节：

```text
80 e0 54 67 9e c8 ba 81 12 34 56 78 65 88 84 21 a0 10 11 12 13 14 15 16 17 18
```

前 12 字节是 RTP header：

```text
80 e0 54 67 9e c8 ba 81 12 34 56 78
```

后 14 字节是 H.264 payload：

```text
65 88 84 21 a0 10 11 12 13 14 15 16 17 18
```

逐字段拆 RTP header：

| 字节 | 值 | 字段 | 解释 |
|---|---|---|---|
| 0 | `80` | `V/P/X/CC` | `0x80 = 1000 0000`，Version=2，P=0，X=0，CC=0 |
| 1 | `e0` | `M/PT` | `0xe0 = 1110 0000`，Marker=1，Payload Type=96 |
| 2-3 | `54 67` | Sequence Number | `0x5467 = 21607` |
| 4-7 | `9e c8 ba 81` | Timestamp | `0x9EC8BA81 = 2663955073` |
| 8-11 | `12 34 56 78` | SSRC | `0x12345678 = 305419896` |

H.264 payload 第一个字节：

```text
65
```

H.264 NALU header 拆法：

```text
0x65 = 0110 0101
```

| 字段 | 位 | 值 | 含义 |
|---|---|---|---|
| `forbidden_zero_bit` | bit7 | 0 | 正常 NALU 必须为 0 |
| `nal_ref_idc` | bit6..5 | 3 | 参考级别高 |
| `nal_unit_type` | bit4..0 | 5 | IDR slice |

所以 Frame 38 可以完整理解为：

```text
一个 RTP Version 2 包
payload type = 96，也就是 SDP 协商的 H264/90000
sequence number = 21607
timestamp = 2663955073
marker = 1，表示这个 IDR 访问单元结束
SSRC = 0x12345678
RTP payload 是一个 H.264 IDR NALU
```

## 9. H.264 在 RTP 中怎么看

RTP payload 里通常不是 Annex-B 起始码形式。也就是说，RTP payload 一般不带：

```text
00 00 00 01
```

单 NAL 模式下，payload 第一个字节就是 H.264 NALU header：

```text
67 -> SPS
68 -> PPS
65 -> IDR slice
61 / 41 / 01 等 -> 非 IDR slice，具体看低 5 bit
```

常见 H.264 RTP payload 形态：

| 模式 | 说明 |
|---|---|
| Single NAL Unit | 一个 RTP 包里放一个完整 NALU，小包最容易学习 |
| STAP-A | 一个 RTP 包里聚合多个小 NALU，例如 SPS + PPS |
| FU-A | 一个大 NALU 拆成多个 RTP 包，用于超过 MTU 的帧 |

当前示例使用 Single NAL Unit，便于直接从 RTP payload 第一个字节判断 NALU 类型。后续学习真正工程发送时，需要补 STAP-A/FU-A，尤其是大 IDR 帧通常必须走 FU-A 分片。

## 10. 和 RTCP 的关系

RTP 负责发媒体数据，RTCP 负责控制和反馈。

```text
RTP
  -> 媒体 payload
  -> sequence number
  -> timestamp
  -> marker
  -> payload type
  -> SSRC

RTCP
  -> Sender Report / Receiver Report
  -> 丢包统计
  -> jitter
  -> RTP timestamp 与 NTP 时间的映射
```

学习 RTP 包头时可以先不展开 RTCP，但做音视频同步、丢包统计、码率反馈时必须回来看 RTCP。

## 11. 学习顺序建议

1. 先看 RTP 12 字节固定头。
2. 用 Wireshark Decode As RTP 看 PT、Seq、Timestamp、Marker、SSRC。
3. 再看 H.264 payload 第一个字节，判断 SPS/PPS/IDR。
4. 再学习 Single NAL、STAP-A、FU-A。
5. 最后结合 RTSP / GB28181 的 SDP，看 payload type、clock rate、SSRC 是怎么协商出来的。
