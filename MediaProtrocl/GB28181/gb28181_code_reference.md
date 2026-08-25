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

`gb28181_device_stateful.cpp` 是 `gb28181_sip_register_client.cpp` 的状态机升级版：直线 demo 走完就退出，状态机版常驻、有保活循环、BYE 后能回注册态继续 INVITE、掉线指数退避重连。两者复用同一套 `gb28181_module.h` 信令函数，对照阅读能看清"直线流程→状态机"的演进。

## 2. 文件分工

| 文件 | 角色 | 看它学什么 |
|---|---|---|
| `gb28181_module.h` | 公共 C 接口 | 模块暴露了哪些能力：配置、SIP 解析结果、生命周期、报文构造、RTP 发送 |
| `gb28181_module.cpp` | 模块实现（内部用 `jrtplib`） | SIP 文本拼接、SDP、Digest MD5、XML 提取、PS/PES/PTS 字节、RTP session、分片发送 |
| `gb28181_minimal_example.cpp` | 媒体发送演示 | 裸 H.264 over RTP、PS over RTP、强制小包分片、FU-A 分片 |
| `gb28181_sip_register_client.cpp` | SIP 客户端演示（直线） | REGISTER->401->Digest->MESSAGE->INVITE->ACK->BYE 全流程，ACK 后发一包 PS |
| `gb28181_sip_mock_server.cpp` | SIP 平台 + RTP 接收 mock | 平台侧最小响应，**同时**是接收端：拆 RTP 头、识别 payload、拆 PS、FU-A 重组、落盘 |
| `gb28181_device_stateful.cpp` | 设备状态机（常驻） | 八态迁移、select 事件循环、Keepalive 周期、指数退避重连、BYE 后回 REGISTERED 可循环 INVITE |
| `CMakeLists.txt` | 构建入口 | 目标定义、内置 jrtplib/jthread、输出到 `out/` |

构建产物全部落到 `out/buildBin/`（VS 多配置生成器下 Debug 配置在 `out/buildBin/Debug/`）：

- `gb28181_minimal_example.exe`
- `gb28181_sip_register_client.exe`
- `gb28181_sip_mock_server.exe`
- `gb28181_device_stateful.exe`
- `gb28181_module`（静态库，被上面四个目标链接）

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

### 3.5 跑状态机（常驻设备）

先起 mock server，再跑状态机（默认 2 次 INVITE/BYE 循环后注销退出，可选参数控制循环次数，0=常驻到 Ctrl+C）：

```powershell
# 窗口 1
E:\code\Media\MediaProtrocl\GB28181\out\buildBin\gb28181_sip_mock_server.exe
# 窗口 2（第二参数可选：.h264 文件路径，不填走内置合成流）
E:\code\Media\MediaProtrocl\GB28181\out\buildBin\Debug\gb28181_device_stateful.exe 2
# 有真实码流时：
E:\code\Media\MediaProtrocl\GB28181\out\buildBin\Debug\gb28181_device_stateful.exe 2 your.h264
```

预期状态迁移日志：

```text
[state] IDLE -> REGISTERING
[rx] 401 in REGISTERING
[state] REGISTERING -> AUTHENTICATING
[rx] 200 in AUTHENTICATING
[state] AUTHENTICATING -> REGISTERED
[timeout] REGISTERED auto-invite trigger
[state] REGISTERED -> INVITING
[rx] 200 in INVITING, send ACK + start media
[state] INVITING -> STREAMING
[timeout] STREAMING hold elapsed, send BYE
[state] STREAMING -> BYE_PENDING
[rx] 200 in BYE_PENDING, back to REGISTERED
[state] BYE_PENDING -> REGISTERED      # 第二次循环再 INVITE
...
===== TX REGISTER (deregister Expires:0)
[state] BYE_PENDING -> DEREGISTERING
[state] DEREGISTERING -> IDLE
===== device stopped
```

mock 端会收到对应次数的 RTP（每次 STREAMING 一包 PS），打印 `payload type guess: PS pack header` + `PS scan`。

> DEREGISTERING 在 mock 上会收到 501——mock 不识别 `Expires:0` 注销语义，这是 mock 的局限不是状态机 bug。状态机按"非 200 即超时 force idle"处理，路径正确，真实平台会回 200。退避重连、鉴权失败转 IDLE 等路径同理：代码已写对，需真实平台验证。

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
- **设备状态机骨架**（`gb28181_device_stateful.cpp`）：常驻进程、`IDLE/REGISTERING/AUTHENTICATING/REGISTERED/INVITING/STREAMING/BYE_PENDING/DEREGISTERING` 八态迁移、`select()` 事件循环、Keepalive 周期定时器、连续超时判掉线、指数退避重连、BYE 后回 REGISTERED 可循环 INVITE、`Expires:0` 注销。
- **真实媒体源接入**：STREAMING 不再发一包 demo PS，而是从本地 `.h264`(Annex-B) 文件逐帧读（无文件走内置合成流），按 25fps 周期发 PS over RTP，PTS 按 3600/帧累计，文件读完自动 BYE。命令行 `gb28181_device_stateful.exe [cycles] [media_file]`，第二参数可选。
- **RTCP 收发**：发送端 jrtplib 自动发 RTCP（`SetMinimumRTCPTransmissionInterval(1.0)` 学习用 1s），并可显式发 RTCP APP（`gb28181_send_rtcp_app`）。接收端 mock 监听 udp/30001（=RTP 端口+1），`print_rtcp_packet_summary` 解复合 RTCP，识别 SR(200)/RR(201)/SDES(202)/BYE(203)/APP(204)，提取 SR 的 NTP/RTP timestamp/packets/octet 和 RR 的 fraction_lost/lost/jitter。

### 7.2 待补（走向生产设备，见 `gb28181_study.md` 第 14 节）

- RTCP 统计上报：当前只打印 RTCP 字段，未做丢包率/抖动/RTT 的持续性统计上报
- H.265：FU 分片与重组（当前仅 H.264）
- RTP over TCP：国标主动拉流/被动收流（当前仅 UDP，`use_tcp` 返回 -2）
- 真实平台互操作：当前全部是 mock↔mock 自测（退避重连、鉴权失败转 IDLE 等路径代码已写对，需真实平台验证）

## 8. 阅读顺序建议

1. `gb28181_module.h` —— 先看模块能做什么，不进实现。
2. `gb28181_sip_register_client.cpp` 的 `main()` —— 看一条 SIP 事务链怎么串（直线版）。
3. `gb28181_device_stateful.cpp` 的 `main()` —— 看状态机版怎么用同一套信令函数做成常驻设备（对照 2，理解直线→状态机的演进）。
4. `gb28181_sip_mock_server.cpp` 的 `main()` —— 看平台侧最小响应 + RTP 接收主循环。
5. `print_rtp_packet_summary` -> `print_ps_payload_summary` -> `fu_a_reassembly_handle_packet` —— 看接收端怎么逐层拆。
6. `gb28181_minimal_example.cpp` —— 看发送端三类 payload 和分片。
7. `gb28181_module.cpp` —— 最后看字节级实现：SIP 文本、SDP、Digest MD5、PS/PES/PTS、jrtplib session。

配套抓包：先跑 mock server + client 看 SIP 闭环，再跑 stateful 看状态机常驻循环，再跑 minimal_example 看 RTP/FU-A，最后 `ffplay gb28181_rx.h264` 验证接收重组闭环。

## 9. 设备状态机代码导读

本章把 `gb28181_device_stateful.cpp` 按"功能块 → 函数 → 学习点"逐层拆开，配合源码阅读。运行方式见第 3.5 节，能力清单见第 7 节，本章只讲**代码内部怎么读**。

### 9.1 核心思想：从直线流程到状态机

`gb28181_device_stateful.cpp` 与 `gb28181_sip_register_client.cpp` 用的是**同一套信令函数**（都来自 `gb28181_module.h`），差别只在怎么组织执行流：

| | 直线版 | 状态机版 |
|---|---|---|
| 执行流 | `main()` 一条直线：发→同步 recv→发→recv... | 事件循环：select 等事件→按当前状态解释→执行动作→切状态 |
| 状态 | 隐含在代码执行位置 | 显式枚举，有名字 |
| 超时 | 收不到就打印错误继续 | 有定时器，重试/退避/判掉线 |
| 生命周期 | 走完退出 | 常驻，BYE 后能回注册态再 INVITE |

理解这张表，就理解了"从学习 demo 到生产设备"的第一步本质：**把隐含的执行位置，改成显式状态 + 事件驱动**。

### 9.2 状态枚举（`gb_device_state_t`）

```
IDLE  REGISTERING  AUTHENTICATING  REGISTERED
INVITING  STREAMING  BYE_PENDING  DEREGISTERING
```

每个状态对应"我现在在等什么、下一步期望什么"。例如 `REGISTERING` = 已发无 auth REGISTER，正在等 401；`AUTHENTICATING` = 已发带 auth REGISTER，正在等 200。状态名本身就是文档。完整迁移图见 `gb28181_study.md` 第 14.2 节。

### 9.3 上下文结构（`gb_device_ctx_t`）

这是直线版没有的——把一条会话的全部运行时状态集中存起来。重点字段：

| 字段 | 作用 | 为什么需要 |
|---|---|---|
| `state` | 当前状态 | 事件循环靠它分发 |
| `cseq` | 单调递增 CSeq | 直线版 CSeq 写死，状态机每次发都要递增 |
| `call_id_register/invite` | 事务标识 | 每个事务独立 Call-ID，便于对端配对 |
| `to_tag` | 平台 200 OK 回的 tag | 后续 ACK/BYE 要带，直线版写死 `tag=mock` |
| `challenge` | 401 缓存的鉴权参数 | 跨"收到 401 → 发 auth REGISTER"两步保存 |
| `backoff_ms` / `state_deadline_ms` | 退避值 / 状态超时点 | 实现重连和超时判定的核心 |
| `next_keepalive_ms` | 下次保活时间 | Keepalive 周期的定时器 |
| `keepalive_misses` | 连续未收到 200 次数 | 到上限判掉线 |
| `media_handle` | RTP 会话句柄 | STREAMING 期间持有，BYE 时释放 |
| `invite_after_ms` | 注册后发起 INVITE 的时间点 | 让 REGISTERED 稳定一会再自动点播 |

### 9.4 时间工具

```
now_ms()    跨平台毫秒墙钟（Win32 GetTickCount / POSIX clock_gettime）
min_u64()   取两值较小者
```

状态机的所有定时都基于"墙钟毫秒 + deadline 点"，不依赖多线程。`now_ms()` 是整个事件循环的时间基准。

### 9.5 SIP 报文构造（带参版本）

这是和直线版最直接的对照学习点。直线版直接 `snprintf` 写死 CSeq/branch，状态机需要递增，所以自建带参版本：

| 函数 | 对应直线版的什么 | 关键差异 |
|---|---|---|
| `make_branch()` | 直线版写死 `z9hG4bK-gb28181-register` | 用 `tag + cseq + now_ms()` 拼唯一 branch |
| `build_register_no_auth()` | `gb28181_build_register` | CSeq/branch/Call-ID 由 ctx 控制 |
| `build_register_with_auth()` | client 里手拼的第二次 REGISTER | Authorization 仍复用 `gb28181_build_digest_authorization` |
| `build_invite_request()` | client 的 `build_invite_request` | 目标通道、CSeq 来自 ctx |
| `build_ack_request()` / `build_bye_request()` | client 的同名函数 | 带上 `to_tag`（ctx 里存的平台 tag） |

学习点：**信令文本构造完全复用，只是参数来源从"常量"换成"上下文字段"**。这是状态机不重写信令、只加状态结构的核心手法。

### 9.6 状态迁移与动作

每个 `enter_*` / `send_*` 函数做三件事：**执行动作 → 设定下一个 deadline → 切状态**。

| 函数 | 做什么 | 学习点 |
|---|---|---|
| `enter_state()` | 切状态 + 打日志 | 所有状态切换都过这里，日志可追溯 |
| `enter_idle_with_backoff()` | 关 socket + 算退避 + 设 deadline | 掉线重连入口：退避 `base << 1` 翻倍，封顶 |
| `start_registering()` | （重）开 socket + 新 Call-ID + 发无 auth REGISTER | 退避到期或首次启动都走这里；socket 重建是关键 |
| `send_register_with_auth()` | 用缓存 challenge 发带 auth REGISTER | 跨"收 401→发 auth"两步 |
| `enter_registered()` | 重置退避 + 启动 Keepalive + 计划 INVITE | 注册成功后的稳态：保活 + 自动点播 |
| `send_keepalive()` | 发 Keepalive，misses++ | 到 `KEEPALIVE_MISS_MAX` 判掉线转退避 |
| `send_invite()` | 发 INVITE，进 INVITING | — |
| `send_ack_and_start_media()` | 发 ACK + 发一包 demo PS | SIP 三步握手完成，媒体通道打开 |
| `send_bye()` | 释放 media_handle + 发 BYE | 资源清理和信令绑定在一起 |
| `extract_to_tag()` | 从 200 OK 的 To 头取 tag | 后续 ACK/BYE 要回带，这是 dialog 标识 |

### 9.7 报文分发（`handle_incoming`）

这是状态机的"大脑"。收到一条 SIP 报文后，按 `switch (ctx->state)` 分发：**同一个 200 OK，在不同状态含义不同**：

- `AUTHENTICATING` 收到 200 → 注册成功 → `enter_registered`
- `REGISTERED` 收到 200 → Keepalive 回执 → 清零 misses
- `INVITING` 收到 200 → 会话接受 → 发 ACK + 开媒体
- `BYE_PENDING` 收到 200 → 会话结束 → 回 `REGISTERED`（可再 INVITE）

学习点：**报文的语义由"当前状态 + 报文内容"共同决定，不由收到顺序决定**。这正是状态机比直线流程强的本质——它能根据上下文解释同一条报文。

### 9.8 超时处理（`handle_state_timeout`）

每个状态有自己的超时动作，和 `handle_incoming` 对称：

| 状态超时 | 动作 |
|---|---|
| IDLE | 退避到期 → `start_registering` |
| REGISTERING | 无 401 → 重试，到上限转退避 |
| AUTHENTICATING | 无 200 → 退回重发无 auth |
| REGISTERED | 两类定时：keepalive 到点发保活 / invite_after 到点发 INVITE |
| INVITING | 无 200 → 回 REGISTERED |
| STREAMING | 停留时长到 → 发 BYE |
| BYE_PENDING | 无 200 → 回 REGISTERED |

REGISTERED 那段特别值得看：一个状态同时挂两个定时器（keepalive + invite_after），用 `min_u64` 取较早者作 deadline，到点再区分是哪个触发的。

### 9.9 主循环（`main`）

```
select(sip_sock, timeout=距最近 deadline 的剩余)
  可读 → recvfrom → handle_incoming
  超时 → handle_state_timeout
```

学习点：

- **单线程 select 复用 socket + 定时器**，不引入多线程就能"同时"收信令和守时
- timeout 用"距最近 deadline 的剩余时间"算，IDLE 退避中无 socket 就 Sleep
- DEREGISTERING 完成后 `invite_cycles_max = -1` 让循环退出——这是唯一的退出路径

### 9.10 阅读顺序建议

1. 先读状态枚举 + `gb_device_ctx_t`（理解"状态"和"上下文"是什么）
2. 再读 `main` 主循环（理解事件驱动骨架）
3. 再读 `handle_incoming`（理解"按状态解释报文"）
4. 再读 `handle_state_timeout`（理解"按状态处理超时"）
5. 最后逐个读 `enter_*` / `send_*` 动作函数（理解每个迁移怎么落地）
6. 对照直线版 `gb28181_sip_register_client.cpp` 的 `main()`，看同样的事务链从直线变成状态机后差在哪

