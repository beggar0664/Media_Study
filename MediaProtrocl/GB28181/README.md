# GB28181 Minimal Module

这个目录放一个最小可用 GB28181 学习模块，目标不是完整商用国标平台，而是把三层关系跑通：

```text
GB28181 信令层：REGISTER / INVITE / BYE / SIP 响应解析
SDP 协商层：IP、端口、payload type、SSRC、编码名
RTP 承载层：通过 jrtplib 发送 H.264/H.265/AAC 等编码 payload
```

## 文件说明

- `gb28181_module.h`：对外 C 接口，包含配置、SIP 构造、SIP 解析、RTP 发送。
- `gb28181_module.cpp`：内部 C++ 实现，使用 jrtplib 的 `RTPSession` 发送 RTP。
- `gb28181_minimal_example.cpp`：最小调用示例，打印 REGISTER、INVITE+SDP、BYE，并演示发送一个 RTP payload。
- `CMakeLists.txt`：最小构建入口。

## 配置字段边界

| 字段 | 含义 |
|---|---|
| `local_id` | 本设备或本通道国标 ID |
| `domain` | SIP 域，一般来自平台国标域 |
| `username` | SIP 用户名，学习阶段可先等于 `local_id` |
| `sip_server_ip` / `sip_server_port` | SIP 平台地址 |
| `local_ip` / `local_sip_port` | 本地 SIP Contact / Via 使用的地址 |
| `local_rtp_port` | 本地 RTP socket 绑定端口，jrtplib 的 `portbase` |
| `remote_rtp_ip` / `remote_rtp_port` | RTP 发送目的地址，来自平台 SDP 或测试工具 |
| `payload_type` | RTP PT，动态类型常用 96、97、98 |
| `ssrc` | RTP 同步源标识，GB28181 会在 SDP 中带 `a=ssrc` |

旧字段 `media_ip` / `media_port` 暂时保留为兼容别名：没有设置 `local_ip/local_rtp_port` 时会回退到它们。

## 构建

```powershell
cd E:\code\Media\MediaProtrocl\GB28181
cmake -S . -B build
cmake --build build
```

本目录默认使用本地 `jrtplib-3.11.2` 源码构建，并关闭 jthread 线程支持，便于先把 RTP UDP 发送链路跑起来。

## 最小发送流程

```text
1. 填 gb28181_config_t
2. gb28181_build_register() 生成 REGISTER 文本
3. gb28181_parse_sip_message() 解析平台 200/401 等响应
4. gb28181_build_invite() 生成 INVITE，并携带 SDP body
5. gb28181_start() 创建 jrtplib RTPSession，绑定本地 RTP 端口，添加远端 RTP 地址
6. gb28181_send_rtp_packet() 发送编码 payload，第四个参数是 RTP timestamp increment。10 fps 视频常用 90000 / 10 = 9000
7. gb28181_build_bye() / gb28181_stop() 结束会话
```

当前模块没有实现 SIP UDP socket 收发、Digest 鉴权、Catalog、Keepalive、实时 H.264/H.265 RTP 分片器。它保留这些边界，是为了先把 GB28181 中最容易混淆的 `SIP/SDP/RTP` 三层关系看清楚。
