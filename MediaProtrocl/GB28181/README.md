# GB28181 学习模块

这个目录是 GB28181 协议的**学习用最小实现**，覆盖从信令到媒体到接收重组的可验证闭环。

相关文档：

- [gb28181_code_reference.md](gb28181_code_reference.md) —— 本套代码的能力清单（逐函数、接收端状态机、运行验证）
- [../gb28181_study.md](../gb28181_study.md) —— GB28181 协议总纲（信令、SDP、RTP、PS 分层和时序）
- [../../current_code_learning_guide.md](../../current_code_learning_guide.md) —— 跨媒体协议的运行和学习路线

## What it covers

- `REGISTER` 文本生成 + 401 Digest 鉴权（`WWW-Authenticate` 解析、`Authorization` 生成）
- `INVITE + SDP` / `ACK` / `BYE` 文本生成
- `MESSAGE` Keepalive / Catalog（查询+响应）/ DeviceInfo（查询+响应）/ DeviceStatus（查询+响应）XML 生成
- Catalog 多 `Item` 响应解析与在线通道选择
- SIP 报文头字段解析、Digest challenge 解析
- PS/PES 打包（Annex-B H.264 -> PES -> PS pack，写 PTS）
- RTP 发送：单包原语、通用字节分片、H.264 FU-A 语义分片（基于 `jrtplib`，UDP）
- RTP 接收：RTP 头解析、payload 识别（PS / 裸 H.264 / FU-A）、PS 拆层（pack->PES->PTS->NALU）
- FU-A 重组状态机：带丢包/乱序/超时/重复检测与 8 槽重排序窗口，重组 NALU 落盘 `gb28181_rx.h264`

代码能力边界与逐函数清单见 [gb28181_code_reference.md](gb28181_code_reference.md)。

## Files

- `gb28181_module.h`：公共 C 接口
- `gb28181_module.cpp`：内部 C++ 实现，使用 `jrtplib`
- `gb28181_minimal_example.cpp`：媒体发送演示（裸 H.264 / PS over RTP / FU-A 分片）
- `gb28181_sip_register_client.cpp`：SIP UDP/Digest 全流程客户端（直线版），ACK 后发一包 PS
- `gb28181_sip_mock_server.cpp`：SIP 平台 mock **兼** RTP 接收端，含 FU-A 重组状态机
- `gb28181_device_stateful.cpp`：设备状态机（常驻版），八态迁移 + Keepalive 周期 + 指数退避重连
- `gb28181_code_reference.md`：代码能力参考文档
- `CMakeLists.txt`：构建入口

## Build

```powershell
cd E:\code\Media\MediaProtrocl\GB28181
cmake -S . -B build
cmake --build build
```

默认从仓库内 `jrtplib-3.11.2` / `jthread-1.3.3` 源码编译依赖，无需预装。Windows 平台目标为 Win32 (x86)。

## Notes

这是协议学习用最小实现，目标是把 GB28181 核心链路从信令到媒体到接收重组跑通且可验证，**不是完整 GB28181 SDK**。当前未覆盖：

- 设备状态机（常驻、重试、Keepalive 周期、断线重连）
- 真实媒体源（当前用固定 SPS/PPS/IDR 测试数据）
- RTCP 收发
- H.265 FU 分片与重组（当前仅 H.264）
- RTP over TCP（当前仅 UDP）
- 真实国标平台互操作（当前全部 mock↔mock 自测）

走向生产设备的具体路线见 [../gb28181_study.md](../gb28181_study.md) 第 14 节。

## Mock server usage

运行 mock server（兼 SIP 平台 + RTP 接收端，监听 udp/5060 与 udp/30000）：

```powershell
E:\code\Media\MediaProtrocl\GB28181\out\buildBin\gb28181_sip_mock_server.exe
```

然后运行 client：

```powershell
E:\code\Media\MediaProtrocl\GB28181\out\buildBin\gb28181_sip_register_client.exe
```

信令流程：

```text
REGISTER
401 Unauthorized
REGISTER + Authorization
200 OK
MESSAGE Keepalive
200 OK
MESSAGE Catalog
200 OK
Catalog Response MESSAGE
200 OK
MESSAGE DeviceInfo
200 OK
DeviceInfo Response MESSAGE
200 OK
MESSAGE DeviceStatus
200 OK
DeviceStatus Response MESSAGE
200 OK
INVITE + SDP
200 OK + SDP
ACK
PS over RTP 一包（接收端打印 RTP 头 + PS 拆层）
BYE
200 OK
```

要观察 FU-A 重组与落盘闭环，运行 media 示例（需 mock server 已起）：

```powershell
E:\code\Media\MediaProtrocl\GB28181\out\buildBin\gb28181_minimal_example.exe
```

mock server 会把重组后的 NALU 写入运行目录 `gb28181_rx.h264`，用 `ffplay gb28181_rx.h264` 或 `ffmpeg -i gb28181_rx.h264 -f null -` 验证接收链路。
