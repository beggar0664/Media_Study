# 容器层与媒体传输层的共性和个性

这份文档用来横向比较两类东西：

- 容器层：PS、TS、FLV、MP4 等
- 媒体传输层：RTSP、RTMP、ONVIF、GB28181、WebRTC 等

它们都服务于音视频数据流动，但解决的问题不同。最重要的边界是：容器层解决“数据怎么装”，传输层解决“数据怎么到达对端”。

```text
编码层: H.264 / H.265 / AAC / Opus / VP8 / VP9 / AV1
容器层: PS / TS / FLV / MP4
媒体传输层: RTSP / RTMP / ONVIF / GB28181 / WebRTC
网络传输层: UDP / TCP / TLS / DTLS
```

## 1. 总体边界

### 1.1 容器层在做什么

容器层负责把编码后的音视频数据组织起来，变成一个文件或连续字节流。

它重点回答：

1. 这一路数据是什么编码。
2. 一帧或一段数据从哪里开始、到哪里结束。
3. 音频和视频如何交错保存。
4. 时间戳怎么记录。
5. 解码需要的额外信息放在哪里，例如 SPS/PPS/VPS、AudioSpecificConfig。
6. 文件或流是否支持随机访问、索引、seek。

### 1.2 媒体传输层在做什么

媒体传输层负责通过网络建立会话、协商参数、传输媒体、处理质量反馈。

它重点回答：

1. 谁连接谁。
2. 如何认证。
3. 双方支持什么编码和媒体参数。
4. 媒体走 UDP 还是 TCP。
5. 是否需要重传、反馈、带宽估计。
6. 会话如何开始、暂停、结束。

### 1.3 一句话区分

```text
容器层: 数据装箱规则
媒体传输层: 网络会话和送达规则
```

类比：

```text
H.264/H.265/AAC = 包裹内容
PS/TS/FLV/MP4 = 包装箱
RTSP/RTMP/GB28181/WebRTC = 下单、调度、运输协议
UDP/TCP/IP = 道路和车辆
```

## 2. 容器层的共性

PS、TS、FLV、MP4 虽然格式不同，但都有这些共同问题：

| 共性 | 说明 |
|---|---|
| 流标识 | 区分视频、音频、字幕或私有数据 |
| 编码标识 | 标明 H.264、H.265、AAC、MP3 等类型 |
| 时间戳 | 记录 PTS，有些还记录 DTS |
| 帧边界 | 标明一个 sample、tag、PES 或 access unit 的边界 |
| 载荷长度 | 说明后面 payload 有多长，或通过固定包长/分片规则推导 |
| 参数集 | 保存 SPS/PPS/VPS、AAC config 等解码初始化信息 |
| 多路复用 | 把音频、视频、私有数据放进同一文件或同一流 |
| 同步 | 让播放器知道音视频如何按时间对齐 |

容器层最需要关注的是：头字段、时间戳、载荷边界、编码参数、索引或同步点。

## 3. 容器层的个性

### 3.1 PS

PS 常见于 DVD、监控录像库、GB28181 的媒体封装场景。

特点：

- 面向相对可靠的存储或传输环境。
- 由 pack header、system header、PSM、PES 等结构组成。
- 音视频通常放在 PES 里。
- 常见 stream_id：`0xE0` 视频、`0xC0` 音频、`0xBD` private stream。
- GB28181 里常见 RTP payload 承载 PS 流，再由 PS 承载 H.264/H.265。

学习重点：

- pack start code：`00 00 01 BA`
- system header：`00 00 01 BB`
- PSM：`00 00 01 BC`
- PES：`00 00 01 E0`、`00 00 01 C0`、`00 00 01 BD`
- PTS/DTS 在 PES header 中如何编码

### 3.2 TS

TS 常见于广播、HLS、IPTV、监控平台转封装。

特点：

- 固定 188 字节 packet。
- 每个 packet 都以 sync byte `0x47` 开头。
- 用 PID 区分不同流。
- PAT/PMT 描述节目和流。
- PCR 用于系统时钟恢复。
- 抗丢包能力比 PS 更适合传输场景。

学习重点：

- sync byte：`0x47`
- PID
- continuity counter
- payload_unit_start_indicator
- adaptation field
- PAT / PMT / PCR
- PES 如何切进 TS payload

### 3.3 FLV

FLV 常见于 RTMP 直播链路和历史 Web 播放场景。

特点：

- 文件结构简单：FLV Header + Tag 序列。
- Tag 分为 audio、video、script。
- RTMP 上传输的媒体数据常常就是 FLV tag 语义。
- H.264/H.265 在 FLV 里通常使用长度前缀，不使用 Annex-B 起始码。
- metadata 常用 AMF 编码。

学习重点：

- FLV Header
- PreviousTagSize
- TagType：`8` 音频、`9` 视频、`18` script
- Timestamp / TimestampExtended
- AVCDecoderConfigurationRecord
- HEVC 扩展 FLV 的兼容性问题
- AMF metadata

### 3.4 MP4

MP4 常见于点播、录制文件、浏览器播放、移动端文件。

特点：

- box/atom 结构。
- 元信息丰富，适合存储和随机访问。
- `moov` 描述索引和轨道，`mdat` 存放媒体数据。
- 普通 MP4 更偏文件，fMP4 可以用于流式传输。
- H.264/H.265 通常使用长度前缀，参数集放在 `avcC` / `hvcC`。

学习重点：

- `ftyp`
- `moov`
- `trak`
- `mdia`
- `stbl`
- `mdat`
- `stts`、`ctts`、`stsc`、`stsz`、`stco/co64`
- `avcC` / `hvcC`

## 4. 媒体传输层的共性

RTSP、RTMP、ONVIF、GB28181、WebRTC 的共性不是包格式相同，而是它们都绕不开这几个阶段：

| 阶段 | 说明 |
|---|---|
| 建立连接 | TCP、UDP、HTTP、SIP、ICE 等方式建立通信路径 |
| 认证鉴权 | 用户名密码、Digest、Token、证书、DTLS 等 |
| 能力协商 | 编码、分辨率、payload type、端口、方向、加密参数 |
| 媒体传输 | RTP、SRTP、RTMP chunk、HTTP body 等承载媒体 |
| 质量控制 | RTCP、NACK、PLI、FIR、带宽估计、重传、缓冲 |
| 会话结束 | TEARDOWN、BYE、close、DTLS close、ICE 断开 |

传输层最需要关注的是：握手流程、信令格式、鉴权、媒体协商、数据承载方式、异常恢复。

### 4.1 认证授权的位置

不是所有媒体传输协议都把认证授权写死在协议里，但真实系统里几乎都会有某种鉴权控制。区别在于它放在什么位置。

| 协议 | 常见认证方式 | 鉴权位置 |
|---|---|---|
| RTSP | Basic / Digest / URL token | 协议内或服务端策略 |
| RTMP | 推流密钥 / Token / URL 参数 | 多在服务端和业务层 |
| ONVIF | HTTP Digest / WS-Security | 协议内 + 设备管理接口 |
| GB28181 | SIP Digest | 协议内，注册和呼叫阶段常见 |
| WebRTC | 信令鉴权 / ICE 凭证 / TURN 凭证 / DTLS-SRTP | 多在应用层和信令层 |

可以把鉴权分成三层看：

```text
接入认证: 你是谁，能不能连上来
会话认证: 你能不能发起播放、推流、回放
资源授权: 你能不能访问某一路通道、某个码流、某个设备
```

结论是：

- 协议层不一定统一规定“必须如何认证”
- 工程上通常都会做认证或授权
- 越是公网、平台化、安防场景，鉴权越是必需

### 4.2 媒体参数描述层

学习 RTSP、GB28181、ONVIF、RTMP 时，会反复遇到一个问题：对端怎么知道这一路流是什么编码、用什么端口、payload type 是多少、分辨率和采样率是多少、解码初始化参数在哪里。

这类信息可以统一理解成“媒体参数描述层”。它的职责不是传真实音视频帧，而是告诉接收端如何理解后续媒体数据。

不同协议使用的形式不同：

| 协议/体系 | 是否直接使用 SDP | 类似 SDP 的信息载体 | 主要作用 |
|---|---:|---|---|
| RTSP | 是 | `DESCRIBE` 返回的 SDP | 描述 RTP 媒体流、payload type、编码、控制 URL |
| GB28181 | 是 | `INVITE` / `200 OK` body 中的 SDP | 协商 RTP 地址、端口、payload type、SSRC、方向 |
| ONVIF | 本身不直接依赖 SDP | SOAP/XML 的 Profile、EncoderConfig、StreamUri | 查询设备能力、编码配置、通道，并获取 RTSP URL |
| RTMP | 否 | AMF `onMetaData` + codec sequence header | 描述宽高、帧率、音频参数，并传 SPS/PPS/VPS、AAC config |
| WebRTC | 是 | SDP Offer/Answer | 协商 codec、payload type、ICE、DTLS-SRTP、RTCP feedback |

可以把它抽象成：

```text
媒体参数描述层 = 告诉接收端“后面的媒体数据应该怎么解释”
```

#### RTSP / GB28181：SDP

RTSP 和 GB28181 都常用 SDP。区别是外层信令不同：RTSP 通过 `DESCRIBE` 获取 SDP，GB28181 通过 SIP `INVITE` 和 `200 OK` 携带 SDP。

典型 SDP 字段：

| 字段 | 作用 |
|---|---|
| `m=video 10000 RTP/AVP 96` | 媒体类型、端口、RTP profile、payload type |
| `a=rtpmap:96 H264/90000` | payload type 96 对应 H.264，90k 时钟 |
| `a=control:trackID=1` | RTSP 中常见，表示 SETUP 控制的 track |
| `a=sendonly` / `a=recvonly` | 发送/接收方向，GB28181 中常见 |
| `a=ssrc:0305419896` | RTP 同步源标识，GB28181 中常见 |

SDP 只描述媒体，不承载媒体。真正的数据仍然走 RTP/RTCP。

#### ONVIF：SOAP/XML + RTSP SDP

ONVIF 更像设备管理和能力查询体系。它本身通常不直接传媒体，也不靠 SDP 完成媒体协商，而是用 SOAP/XML 查询：

- `GetProfiles`：有哪些媒体 profile。
- `GetVideoEncoderConfiguration`：视频编码、分辨率、帧率、码率等配置。
- `GetAudioEncoderConfiguration`：音频编码、采样率、通道等配置。
- `GetStreamUri`：获取 RTSP URL。

拿到 RTSP URL 后，真正拉流通常还是进入 RTSP：

```text
ONVIF GetStreamUri
  -> rtsp://192.168.1.100:554/Streaming/Channels/101
  -> RTSP DESCRIBE
  -> SDP
  -> RTSP SETUP / PLAY
  -> RTP/RTCP
```

所以 ONVIF 的位置可以理解为：先发现和管理设备，再把媒体播放交给 RTSP/RTP。

#### RTMP：AMF metadata + sequence header

RTMP 不使用 SDP。它把媒体参数放在 RTMP message 里，常见两类：

第一类是 AMF `onMetaData`：

```text
width
height
framerate
videocodecid
audiocodecid
audiosamplerate
audiosamplesize
stereo
```

第二类是 codec sequence header：

```text
H.264 AVC sequence header -> AVCDecoderConfigurationRecord -> SPS/PPS
H.265 HEVC sequence header -> HEVCDecoderConfigurationRecord -> VPS/SPS/PPS
AAC sequence header       -> AudioSpecificConfig
```

因此 RTMP 的“媒体描述”不是一段 SDP 文本，而是分散在 metadata 和编码初始化包里。播放器或服务器需要先读到这些信息，才能正确解释后续 audio/video message。

#### 容器层也有自己的媒体描述

媒体参数描述不只存在于传输协议里，容器层也有自己的表达方式：

| 容器 | 媒体描述位置 |
|---|---|
| PS | PSM、PES stream_id、编码层 SPS/PPS/VPS |
| TS | PAT/PMT、stream_type、PES、编码层 SPS/PPS/VPS |
| FLV | metadata tag、AVC/AAC/HEVC sequence header |
| MP4 | `moov`、`trak`、`stsd`、`avcC`、`hvcC`、`esds` |

这也是为什么分层很重要：协议层的 SDP/metadata 解决“网络会话如何收这路流”，容器层的头和 box 解决“这段字节内部如何组织”，编码层的 SPS/PPS/VPS 解决“解码器如何初始化”。三者可能重复表达编码信息，但职责不同。

## 5. 媒体传输层的个性

### 5.1 RTSP

RTSP 是实时流控制协议，常见于 IPC 摄像头和播放器拉流。

特点：

- 信令像 HTTP 文本协议。
- 常见流程：`OPTIONS -> DESCRIBE -> SETUP -> PLAY -> TEARDOWN`。
- DESCRIBE 返回 SDP。
- 媒体通常走 RTP/RTCP。
- RTP 可走 UDP，也可走 RTSP TCP interleaved。

核心字段：

- CSeq
- Transport
- Session
- Range
- SDP 中的 `m=` / `a=rtpmap` / `a=control`

### 5.2 RTMP

RTMP 常见于直播推流。

特点：

- 基于 TCP。
- 有自己的握手和 chunk stream。
- 控制命令使用 AMF。
- 常承载 FLV tag 语义。
- 延迟通常高于 WebRTC，但实现和运维成熟。

核心字段：

- C0/C1/S0/S1/S2/C2 handshake
- chunk basic header
- message header
- message type id
- stream id
- AMF command：`connect`、`createStream`、`publish`、`play`

### 5.3 ONVIF

ONVIF 不是单纯的媒体传输协议，更像安防设备的 Web Service 标准集合。

特点：

- 用 SOAP/XML 描述设备发现、能力、配置、媒体 profile。
- 常用 WS-Discovery 发现设备。
- 真正视频流通常仍交给 RTSP/RTP。
- 重点在设备管理和能力查询，不在直接传媒体 payload。

核心接口：

- Probe / ProbeMatch
- GetCapabilities
- GetProfiles
- GetStreamUri
- PTZ 控制
- Event 订阅

### 5.4 GB28181

GB28181 是国标监控联网协议。

特点：

- 信令基于 SIP。
- 媒体协商使用 SDP。
- 媒体承载使用 RTP/RTCP。
- 监控场景常见 RTP payload 承载 PS，再由 PS 承载 H.264/H.265。
- 重点是设备注册、目录、心跳、点播、回放、平台级联。

核心流程：

- REGISTER
- MESSAGE Keepalive
- Catalog / DeviceInfo / DeviceStatus
- INVITE + SDP
- RTP/RTCP
- BYE

核心字段：

- SIP：Via、From、To、Call-ID、CSeq、Contact
- SDP：`m=`、`a=rtpmap`、`a=ssrc`
- RTP：PT、Sequence、Timestamp、SSRC

### 5.5 WebRTC

WebRTC 面向低延迟实时通信。

特点：

- 信令通道不固定，通常由业务用 WebSocket/HTTP 自己实现。
- 媒体协商使用 SDP Offer/Answer。
- 网络打洞使用 ICE/STUN/TURN。
- 媒体加密使用 DTLS-SRTP。
- 使用 SRTP 承载音视频，控制反馈依赖 RTCP/NACK/PLI/FIR/TWCC 等。
- 低延迟、强交互、复杂度高。

核心模块：

- SDP Offer/Answer
- ICE candidate
- STUN / TURN
- DTLS
- SRTP
- RTCP feedback
- jitter buffer
- bandwidth estimation

## 6. 横向对比表

### 6.1 容器格式对比

| 格式 | 核心定位 | 典型场景 | 时间戳 | 随机访问 | 网络友好性 | 重点字段 |
|---|---|---|---|---|---|---|
| PS | 节目流封装 | 监控录像、GB28181 payload | SCR、PTS/DTS | 一般 | 一般 | pack、system header、PSM、PES |
| TS | 传输流封装 | HLS、广播、IPTV | PCR、PTS/DTS | 一般 | 强 | 188 packet、PID、PAT、PMT、PCR |
| FLV | 简单流式封装 | RTMP、直播 | Tag timestamp | 弱 | 较强 | Tag、PreviousTagSize、metadata |
| MP4 | 文件封装 | 点播、录制、浏览器 | sample table | 强 | 普通 MP4 一般，fMP4 较强 | box、moov、mdat、stbl、avcC/hvcC |

### 6.2 传输协议对比

| 协议 | 核心定位 | 信令 | 媒体承载 | 鉴权 | 典型延迟 | 典型场景 |
|---|---|---|---|---|---|---|
| RTSP | 实时拉流控制 | RTSP 文本 | RTP/RTCP | Basic/Digest | 中 | IPC、播放器 |
| RTMP | 直播推拉流 | RTMP + AMF | RTMP chunk / FLV tag | URL/Token/业务鉴权 | 中高 | 直播推流 |
| ONVIF | 安防设备管理 | SOAP/XML | 通常交给 RTSP/RTP | WS-Security | 取决于媒体协议 | 摄像头发现、配置、控制 |
| GB28181 | 国标监控联网 | SIP + SDP | RTP/RTCP，常见 PS over RTP | SIP Digest | 中 | 公安/安防平台 |
| WebRTC | 低延迟实时通信 | 业务自定义 + SDP | SRTP/SRTCP | DTLS/业务鉴权 | 低 | 视频通话、低延迟互动 |

## 7. 最容易混淆的点

### 7.1 FLV 和 RTMP

FLV 是容器格式，RTMP 是传输协议。RTMP 经常传 FLV tag 语义，所以二者容易混在一起，但它们不是同一层。

### 7.2 RTSP 和 RTP

RTSP 负责控制会话，RTP 负责传媒体。RTSP 本身不等于媒体 payload。

### 7.3 GB28181 和 RTP/PS

GB28181 是国标协议体系，RTP 是媒体承载方式，PS 是常见 payload 内部封装。常见链路是：

```text
GB28181 SIP/SDP 协商
  -> RTP packet
  -> PS stream
  -> PES
  -> H.264/H.265 NALU
```

### 7.4 ONVIF 和 RTSP

ONVIF 常用于发现设备和获取 RTSP URL。真正拉流时，很多情况下还是 RTSP/RTP。

### 7.5 MP4 和 RTP

MP4 是文件容器。RTP 是实时传输包格式。不能把 MP4 文件直接塞进 RTP 就认为是标准实时视频流，通常需要拆出编码帧后按对应 RTP payload 格式打包。

### 7.6 H.264 Annex-B 和 MP4/FLV 长度前缀

H.264 在 TS/PS/RTP 学习中常见 Annex-B 起始码：

```text
00 00 00 01 67 ...
```

但在 MP4/FLV 中常见长度前缀：

```text
00 00 00 0C 67 ...
```

这不是编码内容不同，而是外层封装记录 NALU 边界的方式不同。

## 8. 学习抓手

### 8.1 学容器层看什么

优先看：

- 文件起始标识或包同步字节
- 流类型字段
- 时间戳字段
- payload 长度和边界
- 编码初始化信息
- 音视频如何交错

推荐用 WinHex 对照：

- TS：搜索 `47`
- PS：搜索 `00 00 01 BA`、`00 00 01 E0`
- FLV：搜索 `46 4C 56`
- MP4：搜索 `66 74 79 70`、`6D 6F 6F 76`、`6D 64 61 74`

### 8.2 学传输层看什么

优先看：

- 第一个请求是什么
- 怎么认证
- 媒体参数在哪里协商
- 真实媒体 payload 走哪条通道
- 如何结束会话
- 抓包里信令和媒体是否分离

推荐用 Wireshark 对照：

- RTSP：`rtsp || rtp || rtcp`
- RTMP：`tcp.port == 1935`
- GB28181：`sip || rtp || udp.port == 5060`
- WebRTC：`stun || dtls || rtp || rtcp`

## 9. 总结

容器层和媒体传输层有共同目标：让编码后的音视频能被正确识别、同步、播放。

但它们的重点不同：

```text
容器层重点: 结构、时间戳、索引、载荷边界、编码参数
传输层重点: 会话、鉴权、协商、网络承载、反馈控制、异常恢复
```

学的时候不要从名字记起，而要从职责记起。看到一个字段，先问它解决的是“装箱问题”还是“送达问题”。这个判断清楚后，PS、TS、FLV、MP4、RTSP、RTMP、ONVIF、GB28181、WebRTC 的位置就不会混。
