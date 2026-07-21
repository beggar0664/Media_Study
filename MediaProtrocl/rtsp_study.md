# RTSP 代码学习摘录

这份文档补 RTSP 这一层的边界说明。

RTSP 不是容器格式，它是**会话控制协议**。在媒体链路里，RTSP 负责控制，RTP/RTCP 负责承载，真正的音视频编码数据仍然是 H.264 / H.265 / AAC 这一层。

## 1. 这一层管什么

RTSP 主要回答这些问题：

```text
1. 这路媒体怎么建立会话
2. 走 TCP 还是 UDP
3. 播放、暂停、结束怎么控制
4. 媒体数据从哪个端口/哪个 SSRC 来
5. RTP 和 RTCP 怎么配合
```

它不关心文件内部怎么打包，也不负责把裸流变成容器格式。

## 2. 和 RTP 的关系

RTSP 的媒体承载层通常是 RTP / RTCP：

```text
RTSP
  -> 控制会话
  -> 协商 SDP
  -> 指定 RTP/RTCP 端口

RTP
  -> 承载音视频 payload
  -> 携带 sequence number / timestamp / marker

RTCP
  -> 做统计、同步、反馈
```

所以在学习上，RTP 不是容器层，而是 RTSP 链路里的承载层。

## 3. 学习时要盯住的字段

RTSP 层常见关注点：

- `DESCRIBE`
- `SETUP`
- `PLAY`
- `PAUSE`
- `TEARDOWN`
- `Transport`
- `Session`
- `Content-Base`
- `SDP`

RTP 层常见关注点：

- `sequence number`
- `timestamp`
- `SSRC`
- `marker bit`
- `payload type`

## 4. 和容器层的边界

RTSP 不是 FLV / TS / MP4 这种文件容器。它更像“实时播放会话的控制面”。

```text
容器层：PS / TS / FLV / MP4
协议传输层：RTSP / RTP / RTCP / RTMP / GB28181
编解码层：H.264 / H.265 / AAC
```

## 5. 建议阅读顺序

1. 先看 `Container/container_layer_study.md`
2. 再看 `MediaProtrocl/GB28181/gb28181_study.md`
3. 再看 RTSP 相关代码和工程
4. 最后再回到 RTP 包格式和负载分片

