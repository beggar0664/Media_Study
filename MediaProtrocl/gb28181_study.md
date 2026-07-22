# GB28181 代码学习摘录

这份文档补 GB28181 这一层的边界说明。

GB28181 不是容器格式，它是一套**国标监控联网协议**。在媒体链路里，GB28181 负责信令和会话，RTP 负责媒体承载。

## 1. 这一层管什么

GB28181 主要回答这些问题：

```text
1. 设备怎么注册
2. 平台怎么目录查询
3. 怎么发起播放/回放
4. 媒体流怎么建立
5. 媒体数据用什么端口和 SSRC 传输
```

它不负责把视频码流封成文件容器，也不负责解码画面。

## 2. 和 RTP 的关系

GB28181 的媒体承载一般是 RTP / RTCP：

```text
GB28181 信令
  -> 设备注册 / 目录 / 设备控制 / 播放控制

RTP
  -> 携带 H.264 / H.265 / AAC payload

RTCP
  -> 做同步、统计、反馈
```

所以 RTP 在 GB28181 中是承载层，不是容器层。

## 3. 学习时要盯住的字段

GB28181 / SIP 常见关注点：

- `REGISTER`
- `INVITE`
- `BYE`
- `MESSAGE`
- `SUBSCRIBE`
- `NOTIFY`
- `Call-ID`
- `From` / `To`
- `Via`
- `CSeq`
- `Contact`
- `SDP`

RTP 侧常见关注点：

- `SSRC`
- `sequence number`
- `timestamp`
- `payload type`
- `marker bit`

## 4. 和容器层的边界

GB28181 不等于 TS / FLV / MP4。它更像“国标实时会话 + 媒体承载规范”。

```text
容器层：PS / TS / FLV / MP4
协议传输层：RTSP / RTP / RTCP / RTMP / GB28181
编解码层：H.264 / H.265 / AAC
```

## 5. 建议阅读顺序

1. 先看 `Container/container_layer_study.md`
2. 再看 GB28181 的信令代码或协议资料
3. 再看 RTP 承载与打包
4. 最后回到编码层看 H.264 / H.265 payload

## 6. 这个目录里的源码怎么对照

`E:\code\Media\MediaProtrocl\GB28181` 目录里现在放的是 `jrtplib` 和 `jthread` 的源码包，这很适合拿来当 GB28181 的 RTP 实现参考。

它们的分工可以这样看：

```text
jrtplib
  -> 负责 RTP / RTCP 的收发、会话、序号、时间戳、SSRC

jthread
  -> 负责线程基础设施，给 jrtplib 的线程支持提供依赖
```

对 GB28181 来说，这两个库的关系是：

```text
GB28181 信令 -> SIP / SDP
媒体承载    -> RTP / RTCP
实现工具    -> jrtplib + jthread
```

建议优先看的源码位置：

- `jrtplib-3.11.2/src/rtpsession.*`：RTP 会话主入口
- `jrtplib-3.11.2/src/rtptransmitter.*`：传输器抽象
- `jrtplib-3.11.2/src/rtpudpv4transmitter.*`：UDP/IPv4 承载实现
- `jrtplib-3.11.2/src/rtpudpv6transmitter.*`：UDP/IPv6 承载实现
- `jrtplib-3.11.2/src/rtcpsrpacket.*`、`rtcprrpacket.*`：RTCP 报文
- `jthread-1.3.3/src/*`：线程相关基础实现

读这套源码时，不要把它当容器代码看。它更接近“实时传输引擎”。真正的媒体内容仍然是 H.264 / H.265 / AAC 的 payload，RTP 只负责把它们搬运和编号。
