# GB28181 学习代码参考

这份文档只讲一件事：**当前 `E:\code\Media\MediaProtrocl\GB28181` 这套代码到底实现了什么、怎么读、怎么跑、怎么验**。

它和其它文档的分工：

- [`../gb28181_study.md`](../gb28181_study.md)：GB28181 协议总纲（信令、SDP、RTP、PS 的分层和时序）。
- [`../../current_code_learning_guide.md`](../../current_code_learning_guide.md)：跨媒体协议的运行和学习路线。
- 本文：这套**学习代码**本身的真实能力清单，逐文件、逐函数、逐机制。

读代码时如果发现某处行为和总纲的"标准做法"不一样，以代码为准，本文负责解释为什么这样写。

## 1. 这套代码的定位

这是一份**协议学习用最小实现**，不是完整 GB28181 SDK，也不是生产设备。

它有意保持小到可以逐字段检查，目标是把 GB28181 的核心链路从信令到媒体到接收重组**全部跑通且可验证**：

```text
信令面：REGISTER + 401 Digest -> MESSAGE(Keepalive/Catalog/DeviceInfo/DeviceStatus) -> INVITE+SDP -> ACK -> BYE
媒体面发送：H.264 Annex-B -> PES -> PS -> RTP（含 FU-A 语义分片、通用字节分片）
媒体面接收：UDP -> RTP 头解析 -> payload 识别(PS/裸H.264/FU-A) -> PS拆层 -> FU-A 重组 -> 落盘
```

关键是：发送端和接收端都用本仓库自己的代码，形成一个**不依赖任何外部平台**的回环闭环，可以用 Wireshark / WinHex / ffplay 反向验证每一步。

## 2. 文件分工

| 文件 | 角色 | 看它学什么 |
|---|---|---|
| `gb28181_module.h` | 公共 C 接口 | 模块暴露了哪些能力：配置、SIP 解析结果、生命周期、报文构造、RTP 发送 |
| `gb28181_module.cpp` | 模块实现（内部用 `jrtplib`） | SIP 文本拼接、SDP、Digest MD5、XML 提取、PS/PES/PTS 字节、RTP session、分片发送 |
| `gb28181_minimal_example.cpp` | 媒体发送演示 | 裸 H.264 over RTP、PS over RTP、强制小包分片、FU-A 分片 |
| `gb28181_sip_register_client.cpp` | SIP 客户端演示 | REGISTER->401->Digest->MESSAGE->INVITE->ACK->BYE 全流程，ACK 后发一包 PS |
| `gb28181_sip_mock_server.cpp` | SIP 平台 + RTP 接收 mock | 平台侧最小响应，**同时**是接收端：拆 RTP 头、识别 payload、拆 PS、FU-A 重组、落盘 |
| `CMakeLists.txt` | 构建入口 | 目标定义、内置 jrtplib/jthread、输出到 `out/` |

构建产物全部落到 `out/buildBin/`：

- `gb28181_minimal_example.exe`
- `gb28181_sip_register_client.exe`
- `gb28181_sip_mock_server.exe`
- `gb28181_module`（静态库，被上面三个目标链接）

## 3. 构建与运行

### 3.1 构建

```powershell
cd E:\code\Media\MediaProtrocl\GB28181
cmake -S . -B build
cmake --build build --config Debug
```

`CMakeLists.txt` 默认 `GB28181_BUILD_LOCAL_THIRD_PARTY=ON`，会从仓库内 `jrtplib-3.11.2` / `jthread-1.3.3` 源码编译依赖，**不需要预先安装**任何第三方库。MSVC 下统一开了 `/utf-8`，源文件里的中文注释不会被当作错误编码。

注意平台目标：`CMakeLists.txt` 在 Windows 下把平台定为 `Win32`（x86）并强制 `/MACHINE:X86`。用 Visual Studio 生成器或 Ninja 都行，但链接器平台要一致。

### 3.2 跑信令闭环

两个 PowerShell 窗口。

窗口 1（先起平台 + 接收端）：

```powershell
E:\code\Media\MediaProtrocl\GB28181\out\buildBin\gb28181_sip_mock_server.exe
```

窗口 2（再起客户端）：

```powershell
E:\code\Media\MediaProtrocl\GB28181\out\buildBin\gb28181_sip_register_client.exe
```

mock server 同时监听 `udp/5060`（SIP）和 `udp/30000`（RTP 接收），启动时打印：

```text
GB28181 SIP mock server listening on udp/5060
GB28181 RTP mock receiver listening on udp/30000
```

预期信令流程：

```text
REGISTER          -> 401 Unauthorized
REGISTER + Auth   -> 200 OK
MESSAGE Keepalive -> 200 OK
MESSAGE Catalog   -> 200 OK + Catalog Response MESSAGE -> 200 OK
MESSAGE DeviceInfo-> 200 OK + DeviceInfo Response MESSAGE -> 200 OK
MESSAGE DeviceStatus -> 200 OK + DeviceStatus Response MESSAGE -> 200 OK
INVITE + SDP      -> 200 OK + SDP
ACK               -> (媒体会话建立)
RTP/PS 一包        -> 接收端打印 RTP 头 + PS 拆层
BYE               -> 200 OK
```

注意平台收到客户端对"响应 MESSAGE"回的 `200 OK` 时，会识别 `msg.is_response` 并**忽略**它，不会误当成新请求；否则 DeviceInfo/DeviceStatus/INVITE 的接收队列会被旧 MESSAGE 污染。

### 3.3 跑纯媒体示例

```powershell
E:\code\Media\MediaProtrocl\GB28181\out\buildBin\gb28181_minimal_example.exe
```

它会依次发送几类 RTP payload 到 `127.0.0.1:30000`（需先起 mock server 才能看到接收端解析）：

```text
裸 H.264 访问单元：SPS -> PPS -> IDR（单包）
PS over RTP 单包
重复 PS 包（观察 seq/timestamp/marker）
强制小包分片 PS（max_payload=24）
FU-A 分片 IDR（max_payload=24）
正常 1200 字节分片 PS
重复 IDR（抓包对比）
```

### 3.4 验证接收闭环

mock server 把 FU-A 重组后的完整 NALU 写入**运行目录**下的 `gb28181_rx.h264`（Annex-B 裸流）。验证：

```powershell
ffplay gb28181_rx.h264
# 或只验证能否被正确解封装：
ffmpeg -i gb28181_rx.h264 -f null -
```

注意：demo 媒体是固定的 SPS/PPS/IDR 测试字节，不是真实采集码流，所以画面只有一帧/几帧，不会连续播放。验证目标是"分片发送 -> 接收重组 -> NALU 还原"这条链路正确，不是"能播长视频"。

## 4. 模块能力清单（逐函数）

`gb28181_module.h` 暴露的能力，按职责分组。每组先讲"做什么"，再讲"边界在哪"。

### 4.1 生命周期

| 函数 | 做什么 | 边界 |
|---|---|---|
| `gb28181_create(config)` | 分配上下文，拷贝配置，准备 SSRC/seq/timestamp 初值 | 不启动网络 |
| `gb28181_start(handle)` | 初始化 Winsock、建 `RTPSession`、绑本地 RTP 端口、`AddDestination` 远端 | 当前**只支持 UDP**；`config.use_tcp != 0` 时直接 `return -2`（未实现） |
| `gb28181_stop(handle)` | `Destroy` RTP session、`WSACleanup` | 可重复调用 |
| `gb28181_destroy(handle)` | stop + free | — |

`gb28181_start()` 内部关键调用链（见 `gb28181_study.md` 8.2）：

```text
SetOwnTimestampUnit(1.0/90000.0)   // 90kHz 时钟
SetUsePredefinedSSRC(true) + SetPredefinedSSRC(ssrc)
SetMaximumPacketSize(1400)
RTPUDPv4TransmissionParams::SetPortbase(local_rtp_port)
RTPSession::Create(...)            // 失败返回负的 jrtplib 错误码
AddDestination(remote_ip, remote_rtp_port)
SetDefaultPayloadType(payload_type)
```

### 4.2 SIP 报文构造

| 函数 | 产出 | 备注 |
|---|---|---|
| `gb28181_build_register` | 第一次 REGISTER（无 Authorization） | branch/Call-ID/CSeq 固定，便于学习对齐 |
| `gb28181_build_invite` | INVITE + SDP | 内部调 `gb28181_build_sdp` |
| `gb28181_build_bye` | BYE | — |
| `gb28181_build_sdp` | 最小 SDP：`m=video` / `a=sendonly` / `a=rtpmap PT H264/90000` / `a=ssrc` | ssrc 用 `%010u` 补 10 位 |

> 客户端里 `INVITE/ACK/BYE` 是本地 `build_invite_request/build_ack_request/build_bye_request`，复用同一 `stream_id/domain/local_ip`，让一条 dialog 沿同一上下文推进。

### 4.3 MESSAGE + XML 构造

所有 MESSAGE 外壳相同，差别只在 XML `<CmdType>` 和响应字段：

| 函数 | CmdType | 方向 |
|---|---|---|
| `gb28181_build_message_keepalive` | Keepalive | 设备->平台（`<Notify>`） |
| `gb28181_build_message_catalog` | Catalog | 设备->平台查询（`<Query>`） |
| `gb28181_build_message_catalog_response` | Catalog | 平台->设备响应（固定 2 条 Item，一 ON 一 OFF） |
| `gb28181_build_message_device_info_query` | DeviceInfo | 查询 |
| `gb28181_build_message_device_info` | DeviceInfo | 响应（DeviceName/Manufacturer/Model/Firmware） |
| `gb28181_build_message_device_status_query` | DeviceStatus | 查询 |
| `gb28181_build_message_device_status` | DeviceStatus | 响应（Online/Status/Encode/Record） |
| `gb28181_extract_xml_tag` | — | 最小标签提取，给 mock 解析 `<CmdType>/<SN>/<DeviceID>` 用 |

Catalog 响应是**学习用固定两条目录项**，不是动态设备树。客户端会选第一条 `Status=ON` 的 Item 作为后续 INVITE 目标通道。

### 4.4 Digest 鉴权

| 函数 | 做什么 |
|---|---|
| `gb28181_parse_www_authenticate` | 从 401 的 `WWW-Authenticate` 提取 `realm/nonce/qop/opaque/algorithm` |
| `gb28181_build_digest_authorization` | 按 Digest 计算 `response`，生成 `Authorization` 头 |

实现细节：

- HA1 = MD5(`username:realm:password`)；HA2 = MD5(`method:uri`)。
- 有 `qop` 时走 `auth` 模式：response = MD5(HA1:nonce:nc:cnonce:qop:HA2)，`cnonce` 固定为 `"gb28181"`，`nc=00000001`。
- 无 `qop` 时走老式：response = MD5(HA1:nonce:HA2)。
- Windows 下 MD5 用 CryptoAPI（`crypt32`），非 Windows 走桩返回全 0（当前未实现跨平台 MD5）。

### 4.5 媒体发送（RTP）

三层发送原语，职责严格分开（这是阅读重点）：

| 函数 | 层次 | 行为 |
|---|---|---|
| `gb28181_send_rtp_packet` | 单包原语 | 不理解 payload 内容；只调 `SendPacket(payload, len, pt, marker, timestamp_inc)`；marker/timestamp_inc 由上层决定 |
| `gb28181_send_rtp_payload_fragmented` | 通用字节切片 | 机械按 `max_payload_size` 切；前片 marker=0/不推进时间戳，末片 marker=1/推进；**不理解内容**，主要用于 PS over RTP |
| `gb28181_send_h264_fu_a` | H.264 语义分片 | 理解输入是裸 NALU；写 FU indicator(`NRI\|28`)+FU header(`S/E\|type`)；每片留 2 字节给 FU-A 头 |

三者关系：单包原语 < 通用字节切片 < H.264 语义分片。`timestamp_inc` 是"发完当前包后时间戳增量"，不是绝对 PTS。

### 4.6 PS/PES 打包

| 函数 | 做什么 |
|---|---|
| `gb28181_build_ps_pack_h264` | Annex-B H.264 -> 视频 PES -> PS pack，输出内存字节流 |

输出字节布局（WinHex 逐字节对照点）：

```text
00 00 01 BA          PS pack header
  44 00 04 00 04 01 89 C3   固定 pack header 形态
00 00 01 E0          视频 PES start code
  <u16 pes_len>      = 3+1+2+3+5+es_len - 6
  80 80 05           PES 标志(PTS only) + header_data_len=5
  <5B PTS>           90kHz，build_pts 写入
  00 00 00 01 ..     Annex-B NALU 原样跟在 PES payload 后
```

> 当前 PES 只写 PTS（`dts_90khz` 参数被忽略，见源码 `(void)dts_90khz`）。单 PES + 单 pack，不含 PSM/system header。这是学习最小形态，不是完整 PS 复用器。

## 5. 接收端：mock server 的媒体面

`gb28181_sip_mock_server.cpp` 不只是平台，它**同时是 RTP 接收端**，这是最容易忽略的一块。它的价值是让你不依赖 Wireshark 也能看到"收到了什么、怎么拆的"。

### 5.1 主循环

```text
select() 同时监听 udp/5060 和 udp/30000
  -> RTP 就绪：print_rtp_packet_summary(buf, len)
  -> SIP 就绪：parse -> 按 method 分支响应
```

SIP 侧分支：无鉴权 REGISTER->401 / 带鉴权 REGISTER->200 / MESSAGE->200(按 CmdType 再回 Catalog/DeviceInfo/DeviceStatus Response) / INVITE->200+SDP / ACK / BYE->200 / 其余->501。

### 5.2 RTP 头解析与 payload 识别

`print_rtp_packet_summary()` 拆最小 12 字节固定头：`V/M/PT/seq/timestamp/ssrc`，打印 payload head 前 16 字节，再按 payload 开头分流：

| payload 开头 | 判定 | 处理 |
|---|---|---|
| `00 00 01 BA` | PS pack header | `print_ps_payload_summary()` 拆 PS->PES->PTS->NALU |
| 低 5 位 == 5 | 裸 H.264 IDR 单包 | 只标记，不拆 |
| 低 5 位 == 28 | H.264 FU-A | `fu_a_reassembly_handle_packet()` 重组 |
| 其它 | unknown/demo | 只打头部 |

> 当前固定 `payload_offset=12`，**不处理 CC>0 / X=1 / P=1** 的扩展情况——源码注释明确这是学习 demo 的已知简化。

### 5.3 PS 拆层

`print_ps_payload_summary()` 在整个 payload 上做关键字节定位（学习式，非完整解复用）：

```text
PS scan: pack_start=.. video_pes=.. video_stream_id=0x.. annexb_nalu=..
PES detail: stream_id / pes_len / flags / header_len
PTS detail: 5 字节 -> 90kHz 值
NALU detail: first_byte / h264_type (7=SPS 8=PPS 5=IDR)
```

注意注释里标注的工程化提醒：**不能在整个 payload 全局扫 `00 00 01`**，因为 PS pack / PES / Annex-B 都有这个前缀；正确做法是按 PS 结构先跳过 pack header/system header/PSM，再按 PES `stream_id` 分类（`0xE0-0xEF` 视频、`0xC0-0xDF` 音频、`0xBD` 私有、`0xBC` PSM），进 PES payload 后才按 Annex-B 找 NALU。当前学习版做了简化扫描，注释保留正确做法作为后续方向。

### 5.4 FU-A 重组状态机（重点）

`fu_a_reassembly_handle_packet()` 是这套代码里最接近"工程级"的一块。它不只是把分片拼回去，还处理丢包、乱序、超时、重复。

状态结构 `h264_fu_a_reassembly_t`（静态单例，挂在 `print_rtp_packet_summary` 里）：

| 字段 | 作用 |
|---|---|
| `active` | 当前是否在重组一个 NALU |
| `timestamp` / `ssrc` | 归组依据，变化则丢弃旧上下文 |
| `expected_seq` | 期望的下一片 seq |
| `pending_fragments` | 已收分片数，超 `MAX_FRAGMENTS=64` 判丢末片 |
| `nalu_header` | 首片重建的原始 NALU 头 |
| `buffer[4096]` / `length` | 重组缓冲 |
| `reorder[8]` | **8 槽乱序重排序窗口**，每个 slot 独立存 seq/used/data[1500]/len/fu_end |
| `start_tick` | 首片墙钟时间，超 `TIMEOUT=2000ms` 判丢末片 |
| `output_cb` | 重组完成回调 |

处理逻辑（按到达顺序）：

```text
首片(S=1):
  -> 若已 active，提示覆盖未完成 NALU，restart
  -> active=1，记 timestamp/ssrc/nalu_header，buffer[0]=nalu_header
  -> expected_seq = seq+1，start_tick=now
  -> 若同时是末片(单片 NALU)，直接输出+reset
非首片:
  -> 未 active：丢(缺首片)
  -> 墙钟超时：丢当前不完整 NALU
  -> pending>=64：丢(丢末片保护)
  -> timestamp/ssrc 变：丢旧上下文
  -> seq==上一已处理：重复，静默丢
  -> seq!=expected：
       落在 expected 之后 [1,8] 窗口内 -> reorder slot 暂存，等期望包补齐
       超出窗口 -> 丢当前 NALU
  -> seq==expected：追加，expected++，再 drain 窗口里连续跟随的暂存包
  -> 末片(E=1)：print + output_cb + reset
```

`fu_a_reorder_drain()` 会连续刷出窗口里 `seq==expected` 的暂存包，直到遇到空洞或刷出末片。`output_cb` 默认指向 `fu_a_default_output_cb`，把重组 NALU 加 `00 00 00 01` 写入 `gb28181_rx.h264`。

异常处理对照（与 `gb28181_study.md` 10.4 节一致）：

| 场景 | 处理 |
|---|---|
| 丢首片 | 丢后续中间/末片（没原始 NALU 头无法开始） |
| 丢中间片 | seq 不连续且超窗口 -> 丢当前 NALU |
| 丢末片 | 墙钟超时 / pending 超限 -> 丢不完整 NALU |
| 乱序 | 落窗口内暂存，期望包补齐后按序刷出；窗口满才丢 |
| 重复片 | seq==上一已处理，静默丢 |
| timestamp 变 | 丢旧上下文 |

> 这是学习版状态机，不是完整播放器级恢复。剩余工程化项：窗口大小自适应、更大 NALU 的 buffer 扩容、与真实解码器对接。

## 6. 输出与验证产物

| 产物 | 来源 | 用途 |
|---|---|---|
| `gb28181_rx.h264` | mock server 的 FU-A 重组回调，写运行目录 | Annex-B 裸流，`ffplay`/`ffmpeg` 直接打开 |
| `out/buildBin/*.log` | 历史 mock 运行日志 | 参考，非当前产物 |
| 控制台日志 | 三个 exe 各自打印 | TX/RTP RX/FU-A detail/PS scan 等，对照 Wireshark |

Wireshark 抓包对照（见 `current_code_learning_guide.md`）：

```text
SIP:   sip || udp.port==5060 || udp.port==5062
RTP:   udp.dstport==30000，未自动识别时 -d udp.port==30000,rtp
```

## 7. 已实现 vs 待补（对照代码事实）

### 7.1 已实现

- SIP 信令：REGISTER+401 Digest、MESSAGE(Keepalive/Catalog/DeviceInfo/DeviceStatus)、INVITE+SDP+ACK+BYE
- SDP 协商：m=/a=rtpmap/a=sendonly·recvonly/a=ssrc 构造
- 媒体发送：PS/PES 打包、RTP 单包、通用字节分片、H.264 FU-A 分片
- 媒体接收：RTP 头解析、payload 识别(PS/裸H.264/FU-A)、PS 拆层、**FU-A 重组状态机**（重排序窗+超时+丢包/乱序/重复处理）
- 验证闭环：重组 NALU 落盘 `gb28181_rx.h264`，可用 ffplay/ffmpeg 验证

### 7.2 待补（走向生产设备，见 `gb28181_study.md` 第 14 节）

- 设备状态机：常驻、重试、Keepalive 周期、断线重连（当前 client 是直线走完退出）
- 真实媒体源：替换固定 SPS/PPS/IDR 为真实编码器/IPC SDK 取流
- RTCP：SR/RR、丢包率/抖动、RTT、多流时钟同步（仓库尚无 RTCP 收发）
- H.265：FU 分片与重组（当前仅 H.264）
- RTP over TCP：国标主动拉流/被动收流（当前仅 UDP，`use_tcp` 返回 -2）
- 真实平台互操作：当前全部是 mock↔mock 自测

## 8. 阅读顺序建议

1. `gb28181_module.h` —— 先看模块能做什么，不进实现。
2. `gb28181_sip_register_client.cpp` 的 `main()` —— 看一条 SIP 事务链怎么串。
3. `gb28181_sip_mock_server.cpp` 的 `main()` —— 看平台侧最小响应 + RTP 接收主循环。
4. `print_rtp_packet_summary` -> `print_ps_payload_summary` -> `fu_a_reassembly_handle_packet` —— 看接收端怎么逐层拆。
5. `gb28181_minimal_example.cpp` —— 看发送端三类 payload 和分片。
6. `gb28181_module.cpp` —— 最后看字节级实现：SIP 文本、SDP、Digest MD5、PS/PES/PTS、jrtplib session。

配套抓包：先跑 mock server + client 看 SIP 闭环，再跑 minimal_example 看 RTP/FU-A，最后 `ffplay gb28181_rx.h264` 验证接收重组闭环。
