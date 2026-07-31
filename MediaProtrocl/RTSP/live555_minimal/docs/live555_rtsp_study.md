# live555 RTSP 最小模块学习

## 1. 当前模块边界

本目录只做 RTSP 最小可用链路，不混入 Qt UI：

- `rtsp_push_server`: RTSP 服务端，负责把 H.264 Annex-B 裸流发布成一个 RTSP URL。
- `rtsp_pull_client`: RTSP 客户端，负责拉取 URL，完成 RTSP 控制交互，并从 RTP 收包侧打印负载信息。
- Qt5 后续只作为界面层，调用核心模块，不直接承担协议逻辑。

第一阶段先做控制台程序，因为控制台更适合确认协议链路：能看到 RTSP 请求响应、SDP 内容、SETUP 后的 RTP 负载输出。等链路稳定后，再把核心类封到 Qt 界面里。

## 2. live555 在这里承担什么

live555 同时覆盖 RTSP、SDP、RTP、RTCP：

- RTSP: `RTSPServer`、`RTSPClient` 负责 OPTIONS、DESCRIBE、SETUP、PLAY、TEARDOWN。
- SDP: 服务端根据 `ServerMediaSession` 和 subsession 生成 SDP，客户端用 `MediaSession::createNew()` 解析 SDP。
- RTP: `H264VideoFileServerMediaSubsession` 把 H.264 NALU 按 RTP/H264 负载格式发送，客户端 `MediaSubsession::readSource()` 输出还原后的帧数据。
- RTCP: live555 在 `MediaSubsession` 中创建 `RTCPInstance`，用于发送/接收统计、同步和 BYE。

## 3. 控制台验证流程

构建命令：

```powershell
cmd /c '"E:\tool\Visual Studio\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 && cmake -S E:\code\Media\MediaProtrocl\RTSP\live555_minimal -B E:\code\Media\MediaProtrocl\RTSP\live555_minimal\build_nmake -G "NMake Makefiles" && cmake --build E:\code\Media\MediaProtrocl\RTSP\live555_minimal\build_nmake'
```

生成文件：

```text
E:\code\Media\MediaProtrocl\RTSP\live555_minimal\out\buildBin\rtsp_push_server.exe
E:\code\Media\MediaProtrocl\RTSP\live555_minimal\out\buildBin\rtsp_pull_client.exe
```

服务端：

```powershell
E:\code\Media\MediaProtrocl\RTSP\live555_minimal\out\buildBin\rtsp_push_server.exe E:\path\to\test.h264 test
```

输出重点看：

```text
RTSP server started
  input : E:\path\to\test.h264
  url   : rtsp://127.0.0.1:8554/test
```

客户端：

```powershell
E:\code\Media\MediaProtrocl\RTSP\live555_minimal\out\buildBin\rtsp_pull_client.exe rtsp://127.0.0.1:8554/test
```

客户端输出重点看：

- `OPTIONS ok`: 服务端支持哪些 RTSP 方法。
- `DESCRIBE ok, SDP`: 服务端声明媒体类型、payload type、clock rate、control URL。
- `SETUP ok`: 服务端确认 RTP/RTCP 传输端口或 TCP interleaved 通道。
- `PLAY ok`: 进入媒体发送阶段。
- `[RTP payload]`: 客户端已经收到并交给 live555 还原后的媒体帧。

## 4. 和前面 RTP 文档的对应关系

RTSP 不是媒体数据包本身，它是控制面；RTP 才是媒体传输面。

一次典型流程是：

```text
RTSP OPTIONS   -> 问服务端支持什么命令
RTSP DESCRIBE  -> 获取 SDP，知道有几路媒体、编码类型、payload type、clock rate
RTSP SETUP     -> 为每一路媒体协商 RTP/RTCP 传输参数
RTSP PLAY      -> 开始发送 RTP
RTP            -> 连续承载 H.264/H.265/AAC 等媒体负载
RTCP           -> 旁路反馈收包统计、同步信息、BYE
RTSP TEARDOWN  -> 结束会话
```

当前客户端打印的 `pt`、`clock` 来自 SDP 和 RTP 会话配置；`first_byte`、`h264_nal_type` 来自 RTP 负载还原后的编解码层数据。

## 5. 为什么服务端先要求 H.264 Annex-B 裸流

live555 的 `H264VideoFileServerMediaSubsession` 面向的是 H.264 elementary stream，不是 MP4、FLV、TS、PS 这类容器文件。

也就是说，输入文件应该长这样：

```text
00 00 00 01 67 ...  // SPS
00 00 00 01 68 ...  // PPS
00 00 00 01 65 ...  // IDR
00 00 00 01 41 ...  // non-IDR
```

如果拿 MP4/FLV/TS 直接传给这个 subsession，live555 不会先帮你解容器；正确做法是先从容器层拆出 H.264/H.265 裸流，再交给 RTP 负载模块。

这也对应前面的分层：

```text
编码层: H.264 NALU
容器层: MP4/FLV/TS/PS，负责文件内组织
协议层: RTSP/SDP/RTP/RTCP，负责实时会话与网络传输
```
