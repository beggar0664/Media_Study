# RTP 代码学习摘录

RTP 不是容器格式，它是**实时媒体承载协议**。在这套仓库里，它的位置是：

```text
RTSP / GB28181
  -> 通过 RTP / RTCP 承载音视频
  -> 再往下是 H.264 / H.265 / AAC payload
```

## 1. 这一层管什么

RTP 关心的是“媒体数据怎么在网络里分包和排序”，不是“文件怎么存”。

它主要回答这些问题：

```text
1. 这一包是第几个
2. 这一包对应哪个播放时刻
3. 这一包属于哪一路媒体
4. 这一包是不是一个访问单元的结束点
5. 这一包里装的是哪种编码的 payload
```

## 2. RTP 头部的核心字段

标准 RTP Header 的关键字段：

| 字段 | 作用 |
|---|---|
| Version | 协议版本，通常是 2 |
| Padding | 是否有填充字节 |
| Extension | 是否带扩展头 |
| CC | CSRC 数量 |
| Marker | 标记一个语义边界，常见于一帧结束 |
| Payload Type | 负载类型，指示承载的媒体格式 |
| Sequence Number | 包序号，检测丢包和乱序 |
| Timestamp | 媒体时间戳，和播放时序相关 |
| SSRC | 同步源标识，一路流的身份 |

这些字段是 RTP 的主体，不属于容器层。

## 3. RTP 和 RTCP 的关系

RTP 负责传媒体数据，RTCP 负责做控制和反馈。

```text
RTP
  -> 送媒体包
  -> 序号、时间戳、marker、payload

RTCP
  -> Sender Report / Receiver Report
  -> 同步、统计、丢包反馈
```

所以 RTP / RTCP 要一起看，但角色不同。

## 4. RTP 里的负载分片

RTP 不会像 FLV 那样把一整帧都放进一个固定 Tag，也不像 TS 那样固定 188 字节。它会把编码后的 NALU 切成多个 RTP 包。

常见情况：

- H.264 / H.265 的一个 NALU 太大，需要分片
- 一帧对应多个 RTP 包
- 最后一个分片常常带 `Marker=1`

RTP 关心的是分包，不关心编码算法内部怎么解码。

## 5. H.264 / H.265 在 RTP 中怎么看

RTP payload 里通常放的是编码数据的网络化表示，而不是容器格式。

### H.264

- 单 NAL 单包
- STAP-A 聚合包
- FU-A 分片包

### H.265

- 单 NAL 单包
- AP 聚合包
- FU 分片包

这里真正重要的是：

```text
RTP 只负责把编码后的帧切包送出去。
VPS/SPS/PPS、IDR、参考帧关系仍然是编解码层。
```

## 6. 和 RTSP / GB28181 的边界

RTSP / GB28181 负责控制，RTP 负责承载。

```text
容器层：PS / TS / FLV / MP4
协议传输层：RTSP / RTP / RTCP / RTMP / GB28181
编解码层：H.264 / H.265 / AAC
```

RTP 放在这里，是因为它不是容器层，也不是编解码层，而是实时传输承载层。

## 7. 学习顺序建议

1. 先看 `Container/container_layer_study.md`
2. 再看 `MediaProtrocl/rtsp_study.md`
3. 再看 `MediaProtrocl/gb28181_study.md`
4. 最后看 RTP 的包头和分片规则

