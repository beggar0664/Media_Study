# ISO 13818-1 TS 完整结构参考 — joa00002_seg2.ts
**文件**: joa00002_seg2.ts
**大小**: 17,457,680 bytes = 92,860 packets x 188 bytes
**生成日期**: 2026-06-04

## 0.0 勘误：joa00002_seg2.ps 的真实流结构（2026-07-14）

这份文档早期把 `joa00002_seg2.ps` 理解成“一路 H.265 视频 + 一路音频”，并围绕 PCR、PTS/DTS、AUD、PES length 等方向排查 PotPlayer 跳帧。后续用 FFmpeg 和手写解析器交叉验证后，结论需要修正：

`joa00002_seg2.ps` 里实际交错了三路独立 HEVC elementary stream：

| PS stream_id | FFmpeg stream id | 含义 | 单路帧率/帧数 |
|---|---:|---|---:|
| `0xE1` | `0x1e1` | HEVC 视频子流 | 约 10 fps，3546 个 PES |
| `0xE2` | `0x1e2` | HEVC 视频子流 | 约 10 fps，3541 个 PES |
| `0xE3` | `0x1e3` | HEVC 视频子流 | 约 10 fps，3541 个 PES |

三路合起来看接近 30 fps，但它们不是同一条视频的连续帧。把 `E1/E2/E3` 全部封进同一个 TS video PID 会把三条 HEVC 参考帧链混在一起，解码器只能稳定解出 IDR，非 IDR 因参考链错乱被丢弃，于是 PotPlayer 表现为按约 6 秒 GOP 跳帧，并可能弹出“未知的文件格式/开始播放时发生问题”。

已经排除的方向：

- TS sync byte 和 continuity counter：无丢包跳变。
- PAT/PMT CRC：表级结构正确。
- PCR 间隔：可做到 100 ms 内。
- PTS/DTS：补 DTS、保留源 PTS、加重排序延迟后仍跳。
- PES `stream_id=E0`、`PES_packet_length=0`、`data_alignment_indicator=1`：仍跳。
- HEVC AUD：每个访问单元前补 AUD 后仍跳。

最终验证：

```bash
ffmpeg -i joa00002_seg2.ps -map 0:0 -an -c copy -f mpegts e2.ts
```

FFmpeg 只 remux `0x1e2` 单路视频后，`e2.ts` 能被 PotPlayer 正常播放。手写转换器只选择 `0xE2` 生成 `joa00002_seg2_e2_fixed.ts` 后也能正常播放。后续 AV 实验发现：把 `C0` 声明成标准 AAC `stream_type=0x0F` 会触发 PotPlayer “开始播放时发生问题”的弹框；改为 `stream_type=0x06` private data 后不再弹框。因此学习封装方案采用 `E2 -> PID 0x0100` 和 `C0 private -> PID 0x0101`。

因此本样本的正确 TS 封装策略是二选一：

1. 只选择一路视频，例如 `E2 -> PID 0x0100`；需要保留音频数据时，先把 `C0` 作为私有流 `stream_type=0x06` 映射到 `PID 0x0101`。
2. 保留多路视频时，必须把 `E1/E2/E3` 分别映射到不同 TS PID 或不同 program，不能合并到同一个 PID。

后文中仍有基于旧文件 `joa00002_seg2.ts` 的逐字节示例，可继续用于学习 TS 包头、AFC、PAT/PMT、PCR、PES 基础结构；但涉及“本文件只有一路视频”“视频约 30 fps”“E1/E2/E3 是同一路视频”的解释，应以上述勘误为准。
### 0.0.1 当前推荐学习封装：E2 视频 + C0 私有音频

当前用于学习 TS 封装的推荐输出结构如下：

| 源 PS | TS PID | PMT stream_type | 说明 |
|---|---:|---:|---|
| `0xE2` | `0x0100` | `0x24` | 单路 HEVC 视频，PCR 放在此 PID |
| `0xC0` | `0x0101` | `0x06` | 私有音频/私有数据，保留原始 PES payload，不承诺通用播放器可解码 |

为什么 `C0` 暂时用 `0x06 private data`，而不是 `0x0F AAC`：

- `C0` ES 开头有 ADTS 同步字 `FF F1`，但 PotPlayer 按 PMT `0x0F` 标准 AAC 路径处理时会弹出“在开始播放时发生了问题”。
- 将同一 PID 声明为 `0x06` 后，PotPlayer 不再弹框，说明问题集中在“标准 AAC 解码声明/兼容性”，不是 TS 包层、视频层或 PID 映射。
- 在未完全确认厂商音频格式、私有头、坏帧或播放器兼容性之前，用 `0x06` 保留数据比误标成 `0x0F` 更稳妥。

`0x06 private data` 的学习意义：

- TS 仍然完整保留这路数据，业务端可以按私有协议解析。
- 通用播放器可能忽略这路 PID，或者只把它显示为私有数据。
- PMT 的 `stream_type` 是播放器选择解码器的入口；类型填错会直接影响播放行为。

如果后续目标是“通用播放器有声播放”，需要另做音频 ES 分析：逐帧校验 ADTS frame length、profile、采样率索引、声道配置，并确认是否存在厂商私有头或坏帧。这个阶段不要把 `C0` 直接当成标准 AAC 下结论。

## 0. TS 包全景图

> 先看森林, 再找树木。这一章用你文件 `joa00002_seg2.ts` 里的真实数据包,
> 从外层到内层把 TS 包的结构一层一层拆开。

### 0.1 四层嵌套结构

TS 包固定 **188 字节**，从外到内四层。每一层由上一层的特定字段驱动：

```
  第1层     第2层        第3层             第4层
  TS头  →  适配域  →   净荷   →  PES包 / PSI表  →  ES数据
  (4B)     (可选)     (PID决定)   (完整单元)       (核)
             ↑                       ↑
        AFC 决定               PID 决定净荷里装什么
```

**AFC 和 PID 是拆解四层结构的两把钥匙**：
- AFC（Byte 3 bit5-4）→ 决定有没有第 2 层（适配域）、第 3 层从哪开始
- PID（Byte 1 bit4-0 + Byte 2）→ 决定第 3 层净荷里装的是 PSI 表还是 PES 包

---

#### 逐字节全景拆解（包 #2，0x00000178，最复杂情况）

这是文件里第一个视频 PES 包，AFC=11（适配域+净荷）、PUSI=1（新 PES 起始），
四层一个不少，是拆解的最佳样本。

```
  偏移        字节值                    该字节属于哪一层 / 是什么字段
  ------     --------                  ------------------------------
  00000178   47         <- sync_byte -+
  00000179   41         <- TEI+PUSI+  |
                         PRI+PIDhi   +- 第1层: TS 头 (4 字节)
  0000017A   00         <- PID_lo     |    详见第 1 章
  0000017B   30         <- TSC+AFC+CC-+
            - - - - - - - - - - - - -   -------------------------------
  0000017C   07         <- af_len=7  -+
  0000017D   10         <- flags      |  PCR_flag=1 (后面有 6 字节 PCR)
            - - PCR - -               +- 第2层: 适配域 (8 字节)
  0000017E   0D 0F 8B   |             |    详见第 2 章、8.2 节
  0000017F              |             |
  00000180   ---------- |             |  PCR_base=438,244,915
  00000181              |             |  -> 4869.388s @ 90kHz
  00000182              |             |  PCR_ext=60
  00000183   19 FE 3C   |            -+
            - - - - - - - - - - - - -   -------------------------------
                                        |  净荷从这里开始 (offset +12)
  00000184   00 00 01 E2              -+
  00000188   25 25                    |  PES_packet_length = 9509
  0000018A   88                       |  flags1
  0000018B   81                       |  flags2: PTS=1, extension=1
  0000018C   16                       |  PES_header_data_length = 22
  0000018D   21 3D 39 8A 25           |  PTS (5B): 793,658,642 -> 8818.429s
  00000192   8E                       |  PES_ext_flags
  00000193   00 00 01 9E 71 B1 B9 3D  |
             00 00 00 00 00 00 00 00  |  16 字节 PES_private_data
            - - - - - - - - - - - - -  +- 第3层: PES 包头 (31 字节)
                                        |    详见第 3 章、第 10 章
                                        |
                                        |  跳过 22 字节 optional fields 后
                                        |  (PES_header_data_length 指定跳多少)
  000001A3   00 00 01 40              |  <- NAL start code (VPS)  -+
  000001A7   01 0C 01 FF FF 01 40     |                            |
  000001AD   00 00 03 00 00 03 00     |                            +- 第4层: ES 数据
             00 03 00 00 03 00 5A     |  H.265 NAL units           |    详见第 9.3 节
             AC 09 00 ...             |  (VPS -> SPS -> PPS -> IDR)|
                                      -+                            |
                                                                    |
  [后面一直到 0x00000233 共 145 字节全是 H.265 NAL 裸流]           -+
```

**每层在哪里、占多少字节：**

```
  +----------+----------+---------------------------+
  | 内容     | 偏移范围  | 大小                      |
  +----------+----------+---------------------------+
  | TS 头    | +0 ~ +3  | 4 字节 (固定)             |
  | 适配域   | +4 ~ +11 | 8 字节 (1+1+6)            |
  | PES 包头 | +12 ~ +42| 31 字节 (9 固定+22 可选)   |
  | ES 数据  | +43 ~    | 145 字节 (188-4-8-31)      |
  +----------+----------+---------------------------+
```

**偏移量计算公式（解复用器的核心逻辑）：**

```
  净荷起始 = 4                         <- TS 头固定 4 字节
  if (AFC == 10 或 11):                <- 有适配域
      净荷起始 += 1 + pkt[4]           <- af_len + 1（长度自身占 1 字节）

  if (PID == 视频/音频 PID):            <- 净荷是 PES 包
      PES 包头长 = 9 + pkt[净荷起始+8]   <- PES_header_data_length
      ES 数据起始 = 净荷起始 + PES 包头长
  else if (PID == PAT/PMT PID):         <- 净荷是 PSI 表
      Section 起始 = 净荷起始 + pointer_field
```

**本包实例：**

```
  AFC=0x30 -> bit5-4=11 -> 有适配域
  pkt[4]=0x07 -> 适配域占 1+7=8 字节
  -> 净荷起始 = 4 + 8 = +12 (0x00000184)

  PID=0x0100 -> 视频 -> 净荷是 PES 包
  pkt[12+8]=pkt[20]=0x16=22 -> PES 包头占 9+22=31 字节
  -> ES 数据起始 = 12 + 31 = +43 (0x000001A3)

  验证: 188 - 4(TS头) - 8(适配域) - 31(PES头) = 145 字节 H.265 裸流
```


**0.2 PID — 净荷类型路由**

### 0.2 "净荷"(payload) — 去掉 TS 头后剩下的全部

TS 包固定 188 字节，去掉 4 字节包头和可选的适配域后，
剩下的数据就是**净荷**（payload）。

但 "剩下的数据" 本身没有意义——**净荷的内容完全由 PID 决定**，
同样一批字节: 是 PSI section、还是 PES 包头、还是压缩帧、还是音频采样，
只看字节值猜不出来，必须先看 PID。

以本例 joa00002_seg2.ts 的实际数据拆开看:

**① PAT 净荷 (PID=0x0000): pointer_field + PSI section**
```
  pkt[4]=00               <- pointer_field (1 byte), 0=section从pkt[5]开始
  pkt[5]=00               <- table_id = 0x00 (PAT)
  pkt[6-7]=B0 0D          <- section_length = 13
  pkt[8-9]=00 01          <- transport_stream_id = 1
  pkt[10]=C1              <- version=0, cur_next=1
  pkt[11]=00              <- section_number
  pkt[12]=00              <- last_section_number
  pkt[13-14]=00 01        <- program_number = 1
  pkt[15-16]=F0 00        <- PMT_PID = 0x1000
  pkt[17-20]=2A B1 04 B2  <- CRC_32
  pkt[21-187]=FF...FF     <- stuffing (167 bytes)

  一层结构: TS头 -> [pointer_field] -> [PSI section: 表头+循环+CRC] -> [FF 填充]
  PAT 净荷 = 一段 PSI section (确定 PMT 在哪) + FF 填满
```

**② PMT 净荷 (PID=0x1000): pointer_field + PSI section**
```
  pkt[4]=00               <- pointer_field
  pkt[5]=02               <- table_id = 0x02 (PMT)
  pkt[6-7]=B0 17          <- section_length = 23
  pkt[8-9]=00 01          <- program_number = 1
  pkt[10]=C1              <- version/cur_next
  pkt[11-12]=00 00        <- section/last = 0/0
  pkt[13-14]=E1 00        <- PCR_PID = 0x0100
  pkt[15-16]=F0 00        <- program_info_length = 0
  pkt[17]=24              <- stream_type = H.265
  pkt[18-19]=E1 00        <- elementary_PID = 0x0100 (视频)
  pkt[20-21]=F0 00        <- ES_info_length = 0
  pkt[22]=90              <- stream_type = G.711
  pkt[23-24]=E1 01        <- elementary_PID = 0x0101 (音频)
  pkt[25-26]=F0 00        <- ES_info_length = 0
  pkt[27-30]=F3 3E 93 69  <- CRC_32
  pkt[31-187]=FF...FF     <- stuffing

  PMT 净荷 = 一段 PSI section (列出节目有哪些流) + FF 填满
```

**③ 视频净荷 (PID=0x0100, PUSI=1): PES 包头 + H.265 NAL units**
```
  pkt[12-14]=00 00 01 E2   <- PES start code + stream_id
  pkt[15-16]=25 25         <- PES_packet_length = 9509
  pkt[17]=88               <- flags1: PTS_DTS=10
  pkt[18]=81               <- flags2: PTS=1, ext=1
  pkt[19]=16               <- PES_header_data_length = 22
  pkt[20-24]=21 3D 39 8A 25<- PTS (5 bytes)
  pkt[25]=8E               <- PES extension flags
  pkt[26-41]=...           <- 16 bytes private_data
  pkt[42]=00 00 01 ...     <- H.265 NAL start code -> 压缩帧数据开始

  视频净荷 = PES 包头 (时间戳+控制) + H.265 压缩视频帧
```

**④ 视频净荷 (PID=0x0100, PUSI=0): 纯 H.265 NAL 续传**
```
  pkt[4-187]=... NAL data ...  <- 全部 184 字节都是压缩视频数据
                                  没有 PES 包头，直接接上一包的 NAL 续上

  视频续传净荷 = H.265 压缩帧数据的第 N 段 (无包头、无时间戳)
```

**⑤ 音频净荷 (PID=0x0101, PUSI=1): PES 包头 + G.711 采样**
```
  pkt[4-6]=00 00 01 C0     <- PES start code + stream_id (audio)
  pkt[7-8]=...             <- PES_packet_length
  pkt[9+]=...              <- flags + PTS
  pkt[...]=G.711 data      <- mu-law 压缩音频采样

  音频净荷 = PES 包头 (PTS时间戳) + G.711 音频数据
  (G.711 是 8kHz/8-bit 的简单压缩，每采样 1 字节，
   比 H.265 简单得多，一帧音频通常一个 TS 包就能装下)
```

**⑥ 音频净荷 (PID=0x0101, PUSI=0): 纯 G.711 续传**
```
  pkt[4-187]=... G.711 samples ...  <- 全部 184 字节都是音频采样
                                       没有包头，直接续传

  音频续传净荷 = G.711 音频采样的第 N 段
```

**归纳**

```
  PID=0x0000 → 净荷 = PSI section (PAT) + FF stuffing
  PID=0x1000 → 净荷 = PSI section (PMT) + FF stuffing
  PID=0x0100 → 净荷 = PES header + H.265 NAL (if PUSI=1)
              净荷 = H.265 NAL continuation (if PUSI=0)
  PID=0x0101 → 净荷 = PES header + G.711 samples (if PUSI=1)
              净荷 = G.711 samples continuation (if PUSI=0)
```

一个解复用器的净荷处理伪代码:
```c
  uint8_t *pld = pkt + payload_offset;  // 定位净荷
  if (pid == 0x0000)      parse_pat(pld);       // PSI section parser
  else if (pid == 0x1000) parse_pmt(pld);       // PSI section parser  
  else if (pid == 0x0100) push_video(pld, pld_len, pusi); // H.265 decoder
  else if (pid == 0x0101) push_audio(pld, pld_len, pusi); // G.711 decoder
```

净荷不是一种统一格式——PID 决定了它是什么，
PUSI 再决定它有没有 PES/PSI 包头。
真正的"视频数据"要去掉 PES 包头才是 ES (Elementary Stream)，
真正的"音频数据"要去掉 PES 包头才是 G.711 采样。

**0.3 PUSI — 数据边界标记**

PID 告诉你包"属于谁"，PUSI 告诉你"是不是新一段的开始"：

```
  Video PID=0x0100:
    PUSI=1:  [PES header + first NAL chunk]   <- new PES starts, with PTS
                                   (PCR is in adaptation field, separate)
    PUSI=0:  [NAL continuation 184B]          <- raw stream continuation
    PUSI=0:  [NAL continuation 184B]          <- ...
    ...      (many continuation packets)       <- ...
    PUSI=1:  [PES header + next frame NAL]    <- next frame starts

  Audio PID=0x0101:
    PUSI=1:  [PES header + first G.711 chunk] <- new PES starts, has PTS
    PUSI=0:  [G.711 continuation 184B]        <- raw samples

  PAT PID=0x0000:
    PUSI=1:  [00][PSI section ... FF ...]     <- pointer_field + section
    PUSI=1:  [00][PSI section ... FF ...]     <- PAT repeats, ~54 pkts
                                              PAT: ALWAYS PUSI=1
                                              (each packet = one complete section)

  PMT PID=0x1000:
    PUSI=1:  [00][PSI section ... FF ...]     <- same pattern
```

关键点：PUSI=1 的包，payload 第一个字节就是 pointer_field（PSI）或 PES header（视/音频）。

**0.4 AFC — 净荷偏移量控制**

AFC（adaptation_field_control）决定 TS 头之后，净荷从第几个字节开始。
2 个 bit，4 种组合，是解复用器里最重要的偏移量计算依据：

```
  AFC=01 (payload only)
    TS header -> [payload starts here]
    e.g. pkt#3 @ 0x00000234
      47 01 00 11 -> 97 44 18 A2 F1 9F 42 51 ...
    offset = 4, 184 bytes all payload

  AFC=11 (adaptation + payload)
    TS header -> [af_len][flags][PCR/stuffing...] -> [payload]
    e.g. pkt#2 @ 0x00000178
      47 41 00 30 -> 07 10 0D 0F 8B 19 FE 3C -> 00 00 01 E2 ...
    pkt[4]=0x07 -> offset = 4 + 1 + 7 = 12, skipping 8-byte adaptation

  AFC=10 (adaptation only)
    TS header -> [af_len][flags][...]
    offset = N/A (no payload)
    NOT present in this file (joa00002_seg2.ts)

  C code:
    int off = 4;
    uint8_t afc = (pkt[3] >> 4) & 0x03;
    if (afc >= 2)
        off += 1 + pkt[4];   // +1 for af_len byte itself
    uint8_t *payload = pkt + off;
```

**0.5 CC — 丢包检测**

同一 PID 的 CC（4-bit）连续递增，跳变 = 丢包：

```
  Packet#    PID      CC    Status
  -------   ------    --    ------
  #2         0x0100    0    start
  #3         0x0100    1    increment
  #4         0x0100    2    increment
  #5         0x0100    3    increment
  #6         0x0100    4    increment
  #7         0x0100    5    increment
  #8         0x0100    6    increment
  #9         0x0100    7    increment
  #10        0x0100    8    increment
  #11        0x0100    9    increment

  Conclusion: 0 gaps = 0 lost packets
```

同一 PID 的 CC 规则：
- 每个有净荷的包 CC+1，15→0 循环
- 相同 PID + 相同 CC = 重传包，丢弃
- 跳变（非 +1 且非重传）= 丢包
- 纯适配域包（AFC=10）不推进 CC
- 空包（PID=0x1FFF）不推进 CC
- 16 包 × 188B = 3008 字节一次 CC 回绕

**0.6 解复用流程图**

完整的 TS 解复用器核心循环，每个 188 字节包都走一遍：

```
  [receive 188 bytes]
      |
      +-- 1. Verify sync_byte
      |      if pkt[0] != 0x47: discard, re-sync
      |
      +-- 2. Extract header fields
      |      pid  = ((pkt[1] & 0x1F) << 8) | pkt[2];  // routing key
      |      pusi = (pkt[1] >> 6) & 1;                 // boundary marker
      |      afc  = (pkt[3] >> 4) & 3;                 // offset calculator
      |      cc   = pkt[3] & 0x0F;                     // loss detector
      |
      +-- 3. CC check
      |      expected = (last_cc[pid] + 1) & 0x0F;
      |      if cc != expected: log_packet_loss(pid);
      |      last_cc[pid] = cc;
      |
      +-- 4. Locate payload
      |      off = 4;
      |      if afc >= 2: off += 1 + pkt[4];
      |      payload = pkt + off;
      |      payload_len = 188 - off;
      |
      +-- 5. Dispatch by PID
             switch(pid):
               case 0x0000: parse_pat(payload);        // find program
               case 0x1000: parse_pmt(payload);        // find streams
               case 0x0100: push_to_video_decoder(
                              pusi ? parse_pes(payload) : payload,
                              pusi);                   // H.265 NAL
               case 0x0101: push_to_audio_decoder(
                              pusi ? parse_pes(payload) : payload,
                              pusi);                   // G.711
```

**0.7 一句话心法**

六组字段，六个问题，按顺序回答：

```
  FIELD             QUESTION                   ACTION
  ---------------   ------------------------   ---------------------
  PID               这是谁的包？                路由到对应解码器
  AFC               数据从第几字节开始？         计算净荷起始偏移
  PUSI              是不是新一段的开头？         标记分段边界
  CC                丢包了吗？                  检测丢包
  adaptation_field  PCR 时钟是多少？            同步解码器时钟
  PES header        这一帧什么时候显示？         提取 PTS 时间戳

  188 字节 = 4B 路由头 + 可选时钟域 + 压缩数据
  解码器从成百上千个 TS 包里拼出视频和音频，按 PTS 对齐播出
```

---
*以下章节逐一深入每个组成部分，可对照本章全景图阅读。*



---

## 目录

0. [0. TS 包全景图](#ch0)
    - [0.1 四层嵌套结构](#ch0_1)
    - [0.2 PID 决定净荷类型](#ch0_2)
    - [0.3 PUSI — 数据边界标记](#ch0_3)
    - [0.4 AFC — 偏移量控制](#ch0_4)
    - [0.5 CC — 丢包检测](#ch0_5)
    - [0.6 解析流程图](#ch0_6)
    - [0.7 一句话心法](#ch0_7)
1. [1. TS 包头 (4 bytes)](#ch1)
2. [2. 适配域 (Adaptation Field)](#ch2)
3. [3. PES 包头与负载](#ch3)
4. [4. PAT — 节目关联表](#ch4)
    - [PSI 的 PID 驱动模型（先看这里）](#psi_model)
5. [5. PMT — 节目映射表](#ch5)
6. [6. 文件全景与 PID 分布](#ch6)
7. [7. WinHex 速查卡](#ch7)
8. [8. 字段值含义速查 (ISO 13818-1)](#ch8)
    - [8.1 TS 包头 (4 bytes)](#ch8_1)
    - [8.2 适配域 (Adaptation Field)](#ch8_2)
    - [8.2.0 来自 joa00002_seg2.ts 的真实 AFC 统计](#ch8_2_0)
    - [8.3 PES 包](#ch8_3)
    - [8.4 PAT — Program Association Table](#ch8_4)
    - [8.5 PMT — Program Map Table](#ch8_5)
    - [8.6 TS 关键常量](#ch8_6)
    - [8.7 PS vs TS 关键差异](#ch8_7)
9. [9. PID 载荷数据详解](#ch9)
    - [9.1 PID=0x0000 (PAT — 节目关联表)](#ch9_1)
    - [9.2 PID=0x1000 (PMT — 节目映射表)](#ch9_2)
    - [9.3 PID=0x0100 (视频 — H.265/HEVC)](#ch9_3)
    - [9.4 PID=0x0101 (音频 — G.711 mu-law)](#ch9_4)
    - [9.5 各 PID 载荷对比](#ch9_5)
10. [10. PES Header Byte-by-Byte Parse](#ch10)
11. [11. PSI Section Byte-by-Byte Parse](#ch11)
12. [12. Cross-PID Cooperation: PAT -> PMT -> PES -> ES](#ch12)


## 1. TS 包头 (4 bytes)

**ISO 13818-1, Table 2-3 — Transport Packet Header**

### 1.0 Header Field Table

Same packet #2 header as 1.3 Example C: 47 41 00 30.
All 7 fields with byte/bit positions and extraction code.

**Header Layout (Packet #2)**
```
  raw: 47 41 00 30

  Byte 0:  47    [-------- sync_byte -------]   always 0x47
  Byte 1:  41    TEI=0  PUSI=1  PRI=0  PID[12:8]=00001
  Byte 2:  00    [---------- PID[7:0] ----------]  =0x00
  Byte 3:  30    TSC=00  AFC=11  CC=0000

    sync_byte      Byte 0   8 bit   always 0x47, packet boundary marker
    TEI            Byte1.b7 1 bit   0=no error, 1=uncorrectable (set by demod)
    PUSI           Byte1.b6 1 bit   1=payload starts new PES or PSI section
    PRI            Byte1.b5 1 bit   transport priority (rarely used)
    PID            Byte1.b4-0  + Byte2   13 bit (0x0000-0x1FFF)
    TSC            Byte3.b7-6 2 bit  00=clear, 01=resvd, 10/11=scrambled
    AFC            Byte3.b5-4 2 bit  00=forbidden, 01=payload, 10=adapt, 11=both
    CC             Byte3.b3-0 4 bit  +1 each payload pkt on same PID, 15->0

  C extract (one shot):
    sync  = pkt[0];                              // always 0x47
    tei   = (pkt[1] >> 7) & 0x01;
    pusi  = (pkt[1] >> 6) & 0x01;
    pri   = (pkt[1] >> 5) & 0x01;
    pid   = ((pkt[1] & 0x1F) << 8) | pkt[2];     // 13-bit
    tsc   = (pkt[3] >> 6) & 0x03;
    afc   = (pkt[3] >> 4) & 0x03;
    cc    = pkt[3] & 0x0F;
    // payload offset:
    int off = 4;
    if (afc >= 2) off += 1 + pkt[4];
```

### 1.1 Field Details (all from 47 41 00 30)

Every bit of all 4 header bytes, one field at a time. Same format as 1.3.

**sync_byte**
```
  Byte 0: 0x47

  bit#    7  6  5  4  3  2  1  0
  bin     0  1  0  0  0  1  1  1
  field  [-------- sync_byte -------]
  value  ALWAYS 0x47

  ISO 13818-1 2.4.3.2. Decoder scans bytes for 0x47,
  collects 188 bytes, verifies byte at +188 is also 0x47.
  If not -> false match, keep scanning.
  Extract: sync_byte = pkt[0]
```

**transport_error_indicator (TEI)**
```
  Byte 1 bit7

  bit#    7
  bin     0
  name   TEI
  value  0

  Set by physical layer / demodulator when uncorrectable
  bit errors detected. Demux only reports, never modifies.
  Some decoders drop entire TS packet when TEI=1.
  Extract: tei = (pkt[1] >> 7) & 1
```

**payload_unit_start_indicator (PUSI)**
```
  Byte 1 bit6

  bit#    6
  bin     1
  name   PUSI
  value  1  (new PES / PSI section starts at payload[0])

  PUSI=1 meanings by PID type:
    Video/audio PID -> payload starts new PES header.
    PSI PID (PAT/PMT) -> payload[0] = pointer_field,
      payload[1+pointer] = first byte of PSI section.
    Null PID (0x1FFF) -> PUSI has no meaning.
  Extract: pusi = (pkt[1] >> 6) & 1
```

**transport_priority (PRI)**
```
  Byte 1 bit5

  bit#    5
  bin     0
  name   PRI
  value  0  (normal priority)

  Same PID: PRI=1 packets may bypass output buffer when
  congested. Rarely used in real streams.
  Extract: pri = (pkt[1] >> 5) & 1
```

**packet_identifier (PID) — 13 bits across Byte1+Byte2**
```
  Byte 1: 0x41      Byte 2: 0x00

  bit#    Byte1 4..0      Byte2 7..0
  bin     00001           00000000
  field   PID[12:8]       PID[7:0]
  value   0x01            0x00

  Full PID = (0x01 << 8) | 0x00 = 0x0100 = 256

  PID determines what the payload is:
    0x0000       = PAT (fixed, see Ch4)
    0x0001       = CAT
    0x0002-0x000F = reserved
    0x0010-0x1FFE = audio/video/PMT/data  (assigned by PAT)
    0x1FFF       = null packet (bandwidth filler)
  This PID=0x0100 is H.265 video. See Ch4 and Ch5.
  Extract: pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
```

**transport_scrambling_control (TSC)**
```
  Byte 3 bits 7-6

  bit#    7  6
  bin     0  0
  field  [--- TSC ---]
  value  00  (clear / not scrambled)

  00=clear payload, 01=reserved, 10=even-key, 11=odd-key.
  TS header + adaptation field are NEVER scrambled.
  In embedded surveillance: rarely used.
  Extract: tsc = (pkt[3] >> 6) & 0x03
```

**adaptation_field_control (AFC)**
```
  Byte 3 bits 5-4

  bit#    5  4
  bin     1  1
  field  [--- AFC ---]
  value  11  (adaptation field + payload both present)

  The 2 most important bits for any TS demuxer.
  AFC = 00: forbidden (ISO 13818-1)
       01: payload only, no adaptation field
       10: adaptation field only, no payload (PCR-only)
       11: adaptation field + payload both present

  Payload start = 4 + (AFC>=2 ? 1+pkt[4] : 0)
  This packet:  pkt[4]=af_len=7 -> payload at 4+1+7 = 12
  Extract: afc = (pkt[3] >> 4) & 0x03
```

**continuity_counter (CC)**
```
  Byte 3 bits 3-0

  bit#    3  2  1  0
  bin     0  0  0  0
  field  [-------- CC --------]
  value  0000

  Same PID: CC+1 per payload-bearing packet.
  0->1->2->...->15->0->1...
  Duplicate (same PID + same CC) = retransmission -> discard.
  Loss detection: new_CC != (old_CC+1)%16 and not duplicate.
  Null packets (AFC=10) and pure-adapt packets do NOT advance CC.

  In this file, video PID (0x0100) has CC wrapping every 16
  packets (188*16 = 3008 bytes, ~3KB between wraps).
  Extract: cc = pkt[3] & 0x0F
```


### 1.2 C Code: Parse TS Header

```c
// TS header is exactly 4 bytes. Parse all fields in one shot.
uint8_t *pkt;  // points to start of TS packet

uint8_t  sync_byte  = pkt[0];                           // always 0x47
uint8_t  tei        = (pkt[1] >> 7) & 0x01;
uint8_t  pusi       = (pkt[1] >> 6) & 0x01;
uint8_t  priority   = (pkt[1] >> 5) & 0x01;
uint16_t pid        = ((pkt[1] & 0x1F) << 8) | pkt[2];  // 13-bit PID
uint8_t  tsc        = (pkt[3] >> 6) & 0x03;
uint8_t  afc        = (pkt[3] >> 4) & 0x03;
uint8_t  cc         = pkt[3] & 0x0F;

// Calculate payload offset
int payload_offset = 4;  // after 4-byte header
if (afc == 0x02 || afc == 0x03) {
    payload_offset += 1 + pkt[4];  // adaptation field length + 1
}
```

### 1.3 Live Examples (from joa00002_seg2.ts)

**Example A: PAT packet (PID=0x0000, PUSI=1)**
```
  raw bytes: 47 40 00 10

  BYTE 0: 0x47 = sync_byte (always 0x47)

  BYTE 1: 0x40  -- bit-by-bit --
   bit#    7  6  5  4  3  2  1  0
   bin     0  1  0  0  0  0  0  0
   field  TEI PUS PRI [--PID[12:8]--]
   value   0   1   0     00000 = 0

    TEI=0: no transport error
    PUSI=1: new PSI section starts at payload[0]
    PRI=0: normal priority
    PID[12:8] = 00000

  BYTE 2: 0x00
   bit#    7  6  5  4  3  2  1  0
   bin     0  0  0  0  0  0  0  0
   field  [-----------PID[7:0]----------]
   value   00000000 = 0x00

   PID = PID[12:8] | PID[7:0] = 00000 00000000 = 0x0000 = 0

  BYTE 3: 0x10  -- bit-by-bit --
   bit#    7  6  5  4  3  2  1  0
   bin     0  0  0  1  0  0  0  0
   field  [TSC] [AFC] [---CC---]
   value   00    01    0000

    TSC=00: not scrambled
    AFC=01: payload only, no adaptation field
    CC=0000: continuity counter = 0
```

**Example B: PMT packet (PID=0x1000, PUSI=1)**
```
  raw bytes: 47 50 00 10

  BYTE 0: 0x47 = sync_byte

  BYTE 1: 0x50  -- bit-by-bit --
   bit#    7  6  5  4  3  2  1  0
   bin     0  1  0  1  0  0  0  0
   field  TEI PUS PRI [--PID[12:8]--]
   value   0   1   0     10000 = 16

   PUSI=1: new PSI section (same as PAT)
   PID[12:8] = 10000 = 16

  BYTE 2: 0x00
   PID[7:0] = 00000000 = 0x00
   PID = 10000 00000000 = 0x1000 = 4096

  BYTE 3: 0x10  (same structure as PAT — AFC=01, CC=0)
```

**Example C: Video PES start (PID=0x0100, PUSI=1, AFC=11)**
```
  raw bytes: 47 41 00 30

  BYTE 0: 0x47 = sync_byte

  BYTE 1: 0x41  -- bit-by-bit --
   bit#    7  6  5  4  3  2  1  0
   bin     0  1  0  0  0  0  0  1
   field  TEI PUS PRI [--PID[12:8]--]
   value   0   1   0     00001 = 1

   PUSI=1: new PES packet starts here
   PID[12:8] = 00001 = 1

  BYTE 2: 0x00
   PID[7:0] = 00000000
   PID = 00001 00000000 = 0x0100 = 256

  BYTE 3: 0x30  -- bit-by-bit --
   bit#    7  6  5  4  3  2  1  0
   bin     0  0  1  1  0  0  0  0
   field  [TSC] [AFC] [---CC---]
   value   00    11    0000

    AFC=11: adaptation field + payload
    byte 4 = af_len, then adaptation field (with PCR), then PES data
```

**Example D: Video continuation (PID=0x0100, PUSI=0, AFC=01)**
```
  raw bytes: 47 01 00 11

  BYTE 0: 0x47 = sync_byte

  BYTE 1: 0x01  -- bit-by-bit --
   bit#    7  6  5  4  3  2  1  0
   bin     0  0  0  0  0  0  0  1
   field  TEI PUS PRI [--PID[12:8]--]
   value   0   0   0     00001 = 1

   PUSI=0: continuation, not a new frame/PES start
   PID[12:8] = 00001 = 1  (same video PID)

  BYTE 2: 0x00
   PID = 00001 00000000 = 0x0100 = 256  (same PID)

  BYTE 3: 0x11  -- bit-by-bit --
   bit#    7  6  5  4  3  2  1  0
   bin     0  0  0  1  0  0  0  1
   field  [TSC] [AFC] [---CC---]
   value   00    01    0001

    AFC=01: payload only
    CC=0001: previous was 0, now 1 — no packet loss
```

### 1.4 AFC (adaptation_field_control) Decision Table

The 2-bit AFC controls everything that follows the TS header:

```
  AFC | Meaning                     | Bytes after header      | Used by
  ----+-----------------------------+-------------------------+--------
   00 | RESERVED                    | (forbidden, never use)  |
   01 | Payload only                | 4B header -> payload     | PAT, PMT, most audio,
      |                             |                          | video continuation
   10 | Adaptation field only       | 4B header -> af -> pad   | null packets (PID=0x1FFF)
   11 | Adaptation + payload        | 4B header -> af -> pay   | video PES start with PCR
```

### 1.5 PID — 13 Bits, Not 12!

```
  13 bits = 5 bits (byte1[4:0]) + 8 bits (byte2[7:0])
          = 0x0000 ~ 0x1FFF (0 ~ 8191)

  Reserved PIDs:
    0x0000  PAT (Program Association Table)
    0x0001  CAT (Conditional Access Table)
    0x0002  TSDT (Transport Stream Description Table)
    0x0003-0x000F  reserved
    0x0010  NIT (Network Information Table) — optional
    0x0011  SDT/BAT (Service Description / Bouquet Association)
    0x0012  EIT (Event Information Table)
    0x0013  RST (Running Status Table)
    0x0014  TDT/TOT (Time & Date / Time Offset Table)
    0x0015-0x1FFE  user-defined (video, audio, subtitles, PMT...)
    0x1FFF  null packets (padding, ignore these)
```

### 1.6 CC (continuity_counter) — Your Loss Detector

```
  CC is 4 bits (0-15), wraps to 0 after 15.
  It increments ONLY on packets that carry payload (AFC=01 or 11).
  If AFC=10 (adaptation only), CC does NOT increment.

  Each PID has its OWN independent CC cycle:
    PID=0x0000 (PAT):   CC goes 0,1,2,3...
    PID=0x0100 (video): CC goes 0,1,2,3...  (separate counter)
    PID=0x0101 (audio): CC goes 0,1,2,3...  (separate counter)

  Loss detection:
    Expected: 0 -> 1 -> 2 -> 3 -> 4 -> 5
    Actual:   0 -> 1 -> 2 -> 5  <- gap! packets 3 and 4 lost!
    Actual:   0 -> 1 -> 1 -> 2  <- duplicate! retransmit or muxer bug
```

#### 1.6.1 PUSI vs CC — Two Different Things

A common confusion: when CC wraps from 15 back to 0, does it
mean a new packet/unit starts?

**No.**

PUSI and CC serve completely different purposes. Confusing them is
like confusing a chapter heading with a page number.

```
  PUSI (1 bit, b6 of Byte 1)
  ============================
  Marks the start of a new "payload unit":
    - For PSI (PAT/PMT): payload starts with pointer_field
                         then the beginning of a new section
    - For PES (video/audio): payload starts with PES header
                             (new frame / access unit)

  CC (4 bits, b3-b0 of Byte 3)
  ============================
  Sequential packet counter for loss detection ONLY.
  Does NOT carry any semantic meaning about what is inside.

  Real example — one video frame spanning many TS packets:

    Packet   CC    PUSI    What's happening
    ------   ---   ----    ----------------------------
    #1        0      1     New PES starts here (frame begin)
    #2        1      0     Continuation of same frame
    #3        2      0     Continuation
    ...      ...    ...    ...
    #16      15      0     Still same frame, CC just overflowed
    #17       0      0     CC wrapped 15->0, NOT a new frame!
    #18       1      0     Continuation
    ...      ...    ...    ...
    #193      0      1     NEW frame starts! PUSI=1, CC=0 (coincidence)

  Key takeaways:
    1. CC 15 -> 0 is just counter overflow, like your odometer
       rolling from 99999 to 00000. Same road, same car.
    2. PUSI=1 is the ONLY flag that declares "new unit starts here."
    3. CC and PUSI are decoupled: PUSI can be 1 at any CC value,
       and CC wraps to 0 at any PUSI value.
    4. Packet #193 above shows PUSI=1 AND CC=0 — but that is
       coincidence, NOT causation. PUSI=1 could equally fall on
       CC=7, CC=13, or any other value.

  The decoder's logic:
    if (PUSI == 1) {
        // Start a new PES packet or PSI section
        // CC may be any value 0-15, ignore it for this purpose
    }
    // Track CC per PID for loss detection only:
    if (cc_current != ((cc_prev + 1) & 0x0F)) {
        // Packet loss detected!
    }
```



---


## 2. 适配域 (Adaptation Field)

**仅当 AFC=10 或 AFC=11 时存在。位于 TS 包头之后。**

### 实例: Video+PCR 包 @ 0x00000178

TS 头: `47 41 00 30` (PUSI=1, PID=0x0100, AFC=11)

| 偏移 | 字节 | 字段 | 含义 |
|------|------|------|------|
| 0x0000017C | `07` | adaptation_field_length | 7 bytes |
| 0x0000017D | `10` | flags byte | 详见下面分解 |

**Flags 逐位分解** (`0x10` = `00010000`):

| 位 | 值 | 字段 | 含义 |
|----|----|------|------|
| b7 | 0 | DI | discontinuity_indicator |
| b6 | 0 | RAI | random_access_indicator |
| b5 | 0 | ESPI | elementary_stream_priority_indicator |
| b4 | 1 | **PCR_flag** | **=1 → 6-byte PCR follows!** |
| b3 | 0 | OPCR_flag | original PCR |
| b2 | 0 | SP_flag | splicing_point_flag |
| b1 | 0 | TPD_flag | transport_private_data_flag |
| b0 | 0 | AE_flag | adaptation_field_extension_flag |

### PCR (6 bytes) @ 0x0000017E

Raw: `0D 0F 8B 19 FE 3C`

```
Bit layout: [PCR_base: 33 bits] [reserved: 6 bits = 011111] [PCR_ext: 9 bits]

PCR_base = 438,244,915 (33-bit, 90 kHz counter)
PCR_ext  = 60 (9-bit, 27 MHz modulo: 0-299)
PCR full = 131,473,474,560 = 4869.387947 s @ 27 MHz
         = 4869.387944 s @ 90 kHz
```

适配域结束于 0x00000184, PES 包头从下一字节开始。

## 3. PES 包头与负载

**ISO 13818-1, Table 2-17 — PES Packet**

> TS 负载 = 完整的 PES 包 = PES 包头 + PES 负载 (ES 原始数据)。
> PS 和 TS 共用完全相同的 PES 格式, 区别只在外层容器。

### 实例: 第一个视频 PES 包 @ 0x00000184

| 偏移 | 字节 | 字段 | 值 |
|------|------|------|----|
| 0x00000184 | `00 00 01` | packet_start_code_prefix | 00 00 01 |
| 0x00000187 | `E2` | stream_id | 0xE2 (video stream 2) |
| 0x00000188 | `25 25` | PES_packet_length | 9509 (+6 = 9515 bytes total) |
| 0x0000018A | `88` | flags byte | PTS_DTS=10, DSM=1 |
| 0x0000018B | `81` | DSM trick mode | — |
| 0x0000018C | `16` | PES_header_data_length | 22 |
| 0x0000018D | `21 3D 39 8A 25` | PTS (33-bit, 90kHz) | 256787730 (2853.197s) |

**PES 包头总长**: 31 bytes
**PES 负载起始**: 0x000001A3
**PES 负载大小**: 9484 bytes (H.265 NAL units)

### TS / PS 中 PES 对比

| 特性 | PS 封装 | TS 封装 |
|------|---------|--------|
| 容器 | PS Pack (可变长, ≤65535B) | TS Packet (固定 188B) |
| 时钟 | SCR (System Clock Reference) | PCR (Program Clock Reference) |
| PES 包头 | 每个 PS Pack 内部 | TS 包 PUSI=1 时出现 |
| PES 跨包 | PS Pack 边界自动切 | PUSI 标记边界, 续包无头 |
| PES 格式本身 | **完全相同** | **完全相同** |

## 4. PAT — 节目关联表

### PSI 的 PID 驱动模型（先看这里）

**PSI (Program Specific Information) 是 TS 的"目录系统"。**

TS 流里几百个 PID、上万个包——播放器怎么知道哪个 PID
是视频、哪个是音频？答案：**通过 PSI 查 PID。**

PSI 不是某种特殊的数据格式，而是一组**靠 PID 来定位的表**：

```
  PSI 表           →  固定 PID        →   表的内容
  ─────           ────────────      ────────────
  PAT              PID=0x0000       告诉你有几个节目，每个节目的 PMT
  (Program Association Table)      在哪个 PID 上。标准写死 0x0000。

  PMT              PID=0x1000       告诉你这个节目的视频/音频/字幕
  (Program Map Table)  (本例)       分别在哪个 PID 上。PID 不是固定
                                   的，要通过 PAT 查出来。

  CAT              PID=0x0001       条件接收表（加密节目用）
  TSDT             PID=0x0002       传输流描述表（很少用）
  NIT              PID=从 PAT 查     网络信息表（频率/频道信息）
```

**核心逻辑：PID 是钥匙，PSI 表是锁。**

```
  播放器启动
    │
    ├── 1. 直接读 PID=0x0000 → PAT
    │       找到: program_number=1 → PMT 在 PID=0x1000
    │
    ├── 2. 读 PID=0x1000 → PMT
    │       找到: 视频在 PID=0x0100 (H.265)
    │            音频在 PID=0x0101 (G.711)
    │
    ├── 3. 设置 PID 过滤器: 只收集 0x0100 + 0x0101
    │
    └── 4. 开始解码播放
```

**PSI 包 vs 普通包的 TS 头区别：**

| 特征 | PSI 包 (PAT/PMT) | 普通包 (视频/音频) |
|------|-----------------|-------------------|
| AFC | 01 (纯净荷，无适配域) | 01(纯净荷) 或 11(净荷+适配域) |
| PUSI | 1 (包内含 PSI section 起始) | 大多数为 0 |
| CC | 独立递增 | 独立递增（每个 PID 各自计数） |
| payload | pointer_field + section | PES 头 + ES 数据 |

> **一句话：PSI = 靠 PID 来查的表。PAT 写死在 0x0000，PMT 的 PID 从 PAT 里查。**
> 解复用器的第一件事就是读 PID=0x0000，拿到 PMT 的 PID，再去读 PMT。

---

**PID = 0x0000, table_id = 0x00, ISO 13818-1, Section 2.4.4**

### 完整 188 字节 (0x000000 – 0x000000BB)

```
  00000000: 47 40 00 10 00 00 B0 0D 00 01 C1 00 00 00 01 F0
  00000010: 00 2A B1 04 B2 FF FF FF FF FF FF FF FF FF FF FF
  00000020: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00000030: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00000040: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00000050: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00000060: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00000070: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00000080: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00000090: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  000000A0: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  000000B0: FF FF FF FF FF FF FF FF FF FF FF FF  ← FF 填充
```

### 逐字段分解

| 偏移 | 字节 | 字段 | 值 |
|------|------|------|----|
| 0x00000000 | `47` | sync_byte | 0x47 |
| 0x00000001 | `40` | PUSI=1, PID[12:8]=0 | — |
| 0x00000002 | `00` | PID[7:0] → **PID=0x0000** | PAT |
| 0x00000003 | `10` | AFC=01 (净荷), CC=0 | — |
| 0x00000004 | `00` | pointer_field | 0 |
| 0x00000005 | `00` | table_id | 0x00 (PAT) |
| 0x00000006 | `B0` | section_syntax(1)+0+reserved(11)+sec_len[11:8] | — |
| 0x00000007 | `0D` | sec_len[7:0] → **section_length=13** | 13 bytes |
| 0x00000008 | `00 01` | transport_stream_id | 1 |
| 0x0000000A | `C1` | ver=0, cur_next=1 | 有效 |
| 0x0000000B | `00` | section_number | 0 |
| 0x0000000C | `00` | last_section_number | 0 |
| 0x0000000D | `00 01` | program_number | 1 |
| 0x0000000F | `F0 00` | reserved(111)+program_map_PID | **PMT_PID=0x1000** |
| 0x00000011 | `2A B1 04 B2` | CRC_32 | 4 bytes |
| 0x00000015–0x000000BB | `FF`×167 | stuffing | 167 bytes |

> 有用数据: 21 bytes, FF 填充: 167 bytes

## 5. PMT — 节目映射表

> PMT 的 PID 不固定。本例 PMT 在 PID=0x1000，但这是 PAT (PID=0x0000)
> 的 program_map_PID 字段告诉我们的——解复用器必须从 PAT 动态查，不能写死。

### 5.0 在 WinHex 里定位 PMT

分两步:

1. Ctrl+G → 0x000000BC（PMT 包 #1，文件第 2 个 TS 包）
2. 188 字节每行看过去。WinHex 一行 16 字节，所以 PMT 数据在:

```
  WinHex row BC-DB: PMT 有用数据 (31 bytes)
  WinHex row DC-177: FF 填充 (157 bytes)
```

整个 PMT section 的原始字节（跳过 TS 头 4 字节 + pointer_field 1 字节）:

```
  C1:  02           <- table_id = 0x02 (PMT)
  C2:  B0 17        <- section_length = 23
  C4:  00 01        <- program_number = 1
  C6:  C1           <- version_number=0, current_next_indicator=1
  C7:  00           <- section_number = 0
  C8:  00           <- last_section_number = 0
  C9:  E1 00        <- PCR_PID = 0x0100
  CB:  F0 00        <- program_info_length = 0
               --- video stream ---
  CD:  24           <- stream_type = 0x24 (H.265/HEVC)
  CE:  E1 00        <- elementary_PID = 0x0100
  D0:  F0 00        <- ES_info_length = 0
               --- audio stream ---
  D2:  90           <- stream_type = 0x90 (G.711 mu-law)
  D3:  E1 01        <- elementary_PID = 0x0101
  D5:  F0 00        <- ES_info_length = 0
  D7:  F3 3E 93 69  <- CRC_32
  DB:  FF FF FF ... <- stuffing to fill 188 bytes
```

接下来按字段顺序逐个拆解。打开 WinHex，对照偏移看。

### 5.1 TS 头（0x000000BC ~ 0x000000BF，4 字节）

PMT 的 TS 头和 PAT 结构完全一样，仅 PID 不同。

WinHex @ BC: 47 50 00 10

```
  Byte 0: 47     sync_byte (always 0x47)

  Byte 1: 50 = 01010000
      bit7=0: TEI=0, no error
      bit6=1: PUSI=1, PSI section starts here
      bit5=0: PRI=0, normal priority
      bit4-0=10000=16: PID[12:8]=0x10

  Byte 2: 00     PID[7:0]=0x00
      完整 PID = 0x1000 = 4096

  Byte 3: 10 = 00010000
      bit7-6=00: TSC=00, not scrambled
      bit5-4=01: AFC=01, payload only (no adaptation field)
      bit3-0=0000: CC=0

  payload 起始 = 4 (跳过 TS 头)
```

PUSI=1 → payload[0] 就是 pointer_field，payload[1] 开始就是 PSI section。

### 5.2 pointer_field（偏移 0x000000C0，1 字节）

WinHex @ C0: 00

```
  pointer_field = 0x00

  含义: section 从 payload[pointer_field] = payload[1] 开始。
  pointer_field=0 → section 紧跟在 pointer_field 后面，
  没有额外偏移。pointer_field 是 PSI 包特有的机制——
  当 section 在 payload 中间开始而非最开头时，用这个值跳过去。
  PAT 和 PMT 里 pointer_field 通常都是 0。
```

### 5.3 table_id（偏移 0x000000C1，1 字节）

WinHex @ C1: 02

```
  table_id = 0x02

  ISO 13818-1 定义的 table_id:
    0x00 = PAT (program_association_section)
    0x01 = CAT (conditional_access_section)
    0x02 = PMT (TS_program_map_section)  ← 这里

  通过 table_id 区分 PSI 表类型。PAT 是 0x00，PMT 是 0x02。
  解码器先看 table_id，再按对应语法解析后面的字段。
```

### 5.4 section_length（偏移 0x000000C2 ~ 0x000000C3，2 字节）

WinHex @ C2: B0 17

```
  B0 17 按 bit 拆:

  BYTE C2: B0 = 10110000
      bit7=1:     section_syntax_indicator (PMT 固定为 1)
      bit6=0:     reserved ('0')
      bit5-4=00:  reserved ('00')
      bit3-0=0000 + BYTE C3 0x17 = 0x017 = 23

  即 section_length = 23 字节。

  section_length 是从这个字段后面开始算的字节数:
    从 0xC4 到 0xDA 结束，共 23 字节。
    不包括:
      - table_id (1 byte)
      - section_length 自身 (2 bytes)
      - CRC_32 (4 bytes)

  验证: 有用的 PSI 数据 = 3 + 23 = 26 字节
    table_id(1) + sec_len(2) + 23 + CRC(4) = 30 bytes
    从 C1 到 DA 恰好 30 bytes ← 对上了。
```

### 5.5 program_number（偏移 0x000000C4 ~ 0x000000C5，2 字节）

WinHex @ C4: 00 01

```
  program_number = 0x0001 = 1

  PMT 里 program_number 标识这个 PMT 属于哪个节目。
  program_number=1 → 对应 PAT 里 program_number=1 的那条。
  本例只有一个节目。

**多节目实例: 4 路 IPC 录进同一个 TS 流**

安防 NVR 同时录 4 路摄像头，复用进一个 TS 流。
假设每个摄像头一路视频(G.711) + 一路音频(H.265)，
编码后 TS 复合输出:

  PAT (PID=0x0000):
    00 00 B0 1D 00 01 C1 00 00
      00 01 F0 10     <- 节目1, PMT 在 PID=0x1000
      00 02 F0 20     <- 节目2, PMT 在 PID=0x2000
      00 03 F0 30     <- 节目3, PMT 在 PID=0x3000
      00 04 F0 40     <- 节目4, PMT 在 PID=0x4000
    CRC

  解读:
    - program_number=0x0001: PMT_PID=0x1000 (摄像头1)
    - program_number=0x0002: PMT_PID=0x2000 (摄像头2)
    - program_number=0x0003: PMT_PID=0x3000 (摄像头3)
    - program_number=0x0004: PMT_PID=0x4000 (摄像头4)
  
  section_length = 0x1D = 29 (比单节目的 13 多了 16 bytes)
  多一个节目 = 多 4 bytes (program_number 2B + PMT_PID 2B)

  每个 PMT 各自独立:

  PMT1 (PID=0x1000):
    02 B0 17 00 01 C1 00 00 E1 00 F0 00
      24 E1 00 F0 00    <- program 1: 视频 0x0100
      90 E1 01 F0 00    <- program 1: 音频 0x0101
    CRC

  PMT2 (PID=0x2000):
    02 B0 17 00 02 C1 00 00 E2 00 F0 00
      24 E2 00 F0 00    <- program 2: 视频 0x0200
      90 E2 01 F0 00    <- program 2: 音频 0x0201
    CRC

  PMT3 (PID=0x3000):
    02 B0 17 00 03 C1 00 00 E3 00 F0 00
      24 E3 00 F0 00    <- program 3: 视频 0x0300
      90 E3 01 F0 00    <- program 3: 音频 0x0301
    CRC

  PMT4 (PID=0x4000):
    02 B0 17 00 04 C1 00 00 E4 00 F0 00
      24 E4 00 F0 00    <- program 4: 视频 0x0400
      90 E4 01 F0 00    <- program 4: 音频 0x0401
    CRC

  最终的 PID 路由表:

    PID        内容
    ----       ----
    0x0000     PAT (列出 4 个节目的 PMT 位置)
    0x1000     PMT1 (摄像头1: 视频0x0100, 音频0x0101)
    0x2000     PMT2 (摄像头2: 视频0x0200, 音频0x0201)
    0x3000     PMT3 (摄像头3: 视频0x0300, 音频0x0301)
    0x4000     PMT4 (摄像头4: 视频0x0400, 音频0x0401)
    0x0100     摄像头1 视频 (H.265)
    0x0101     摄像头1 音频 (G.711)
    0x0200     摄像头2 视频 (H.265)
    0x0201     摄像头2 音频 (G.711)
    0x0300     摄像头3 视频 (H.265)
    0x0301     摄像头3 音频 (G.711)
    0x0400     摄像头4 视频 (H.265)
    0x0401     摄像头4 音频 (G.711)

  每个节目有自己独立的 PCR_PID、video PID、audio PID。
  解码器按 program_number 选择要看哪个摄像头，
  只收对应 PMT 列出的 PID，其他 PID 全部丢弃。
  PAT 里 program_number=0x0000 保留给 NIT (网络信息表)。
```

### 5.6 version_number, current_next_indicator（偏移 0x000000C6，1 字节）

WinHex @ C6: C1

```
  C1 = 11000001

      bit7-6=11: reserved ('11')
      bit5-1=00000: version_number = 0
      bit0=1: current_next_indicator = 1

  version_number: 范围 0-31，PMT 内容变化时 +1。
      接收端检测到版本号变化 → 重新解析 stream type 和 PID。
      本例 version=0，说明是第一次发送的 PMT。

  current_next_indicator=1: 这个 PMT 当前有效。
      =0 时表示 PMT 尚未生效，解码器应等待下一份。
      在 PMT 更新过渡期，允许先发未来版本（cur_next=0），
      再发当前版本（cur_next=1），实现无缝切换。
```

### 5.7 section_number, last_section_number（偏移 0x000000C7 ~ 0x000000C8，2 字节）

WinHex @ C7: 00, @ C8: 00

```
  section_number = 00: 当前是第 0 个 section
  last_section_number = 00: 总共只有 1 个 section

  PMT 数据量很小时（本例 23 字节），一个 section 就装下了。
  如果描述符很多（比如多个语言的音轨、字幕），section_length
  可能超出 TS 包净荷容量（184B），这时会拆成多个 section，
  section_number 递增表示分段序号。
```

### 5.8 PCR_PID（偏移 0x000000C9 ~ 0x000000CA，2 字节）

WinHex @ C9: E1 00

```
  E1 00 按 bit 拆:

  BYTE C9: E1 = 11100001
      bit7-5=111: reserved
      bit4-0=00001 + BYTE CA 0x00 = 0x0100 = 256

  即 PCR_PID = 0x0100

  PCR_PID 告诉解码器: "PCR 时钟在 PID=0x0100 的包里"。
  本例 PCR_PID=0x0100，恰好等于视频 PID——
  PCR 随视频包一起发了，适配域里找 6 字节 PCR（见第 2 章）。

  如果 PCR_PID 指向不同的 PID（比如专门的 PCR 包），
  解码器就得同时收这个 PID，而不仅仅是视频+音频。
```

### 5.9 program_info_length（偏移 0x000000CB ~ 0x000000CC，2 字节）

WinHex @ CB: F0 00

```
  F0 00 按 bit 拆:

  BYTE CB: F0 = 11110000
      bit7-4=1111: reserved
      bit3-0=0000 + BYTE CC 0x00 = 0x000 = 0

  即 program_info_length = 0

  这个字段后面可以跟程序级描述符（如频道名、条件接收信息）。
  本例 program_info_length=0 → 没有程序级描述符，
  直接跳到 stream_type 开始。
```

### 5.10 第一条流: 视频（H.265）

WinHex @ CD: 24 E1 00 F0 00

这部分在 WinHex 里只有 5 个字节，分三个字段:

**stream_type = 0x24**

```
  BYTE CD: 24 = H.265/HEVC video

  常见 stream_type (ISO 13818-1 + amendments):
    0x01 = MPEG-1 Video
    0x02 = MPEG-2 Video (H.262)
    0x1B = H.264/AVC
    0x24 = H.265/HEVC       ← 这里
    0x03 = MPEG-1 Audio
    0x04 = MPEG-2 Audio
    0x0F = AAC
    0x90 = G.711 mu-law
    0x91 = G.711 A-law

  stream_type 决定了 ES 数据要交给哪个解码器。
  本例 0x24 → 送 H.265 解码器。
```

**elementary_PID = 0x0100**

```
  BYTE CE-CF: E1 00 = elementary_PID = 0x0100

  E1 = 11100001, 去掉 bit7-5 reserved = 00001
  combined with 0x00 = 0x0100 = 256

  "视频 ES 数据在 PID=0x0100 的包里"。
  解码器拿到 PMT 后，设置过滤器收 PID=0x0100，
  后面的 TS 包凡是 PID=0x0100 的就往视频解码器送。
```

**ES_info_length = 0**

```
  BYTE D0-D1: F0 00 = ES_info_length = 0

  类似 program_info_length，这条视频流没有额外的 ES 描述符。
  如果有的话会有 N 字节描述符数据跟在 ES_info_length 后面，
  比如额外的编解码参数。
```

### 5.11 第二条流: 音频（G.711 mu-law）

WinHex @ D2: 90 E1 01 F0 00

结构完全一样，三个字段:

**stream_type = 0x90**

```
  BYTE D2: 90 = G.711 mu-law audio

  0x90 = 电话质量的 8kHz PCM 压缩，8 位采样，
  常用于 IPC 监控的音频通道。
```

**elementary_PID = 0x0101**

```
  BYTE D3-D4: E1 01 = elementary_PID = 0x0101

  E1 = 11100001 → 00001, 01 = 0x0101 = 257

  "音频 ES 数据在 PID=0x0101 的包里"。
  解码器设置第二个过滤器: PID=0x0101 → 音频解码器。
```

**ES_info_length = 0**

```
  BYTE D5-D6: F0 00 = ES_info_length = 0
  无额外描述符。
```

### 5.12 CRC_32（偏移 0x000000D7 ~ 0x000000DA，4 字节）

WinHex @ D7: F3 3E 93 69

```
  CRC_32 = 0xF33E9369

  校验范围: 从 table_id(0xC1) 到 CRC 字段之前(0xD6)，
  共 22 字节。

  CRC 多项式: ISO 13818-1 附录 B 标准 CRC-32。
  接收端重新计算 CRC，与这个值比对——
  不一致 → section 有误码，丢弃整段 PMT。
  不丢 PMT 是解复用的底线: PMT 错了，后面所有 PID 路由全错。

  验证方法: 用 Python struct + binascii.crc32 算一遍:
    data = bytes.fromhex('02B0170001C10000E100F00024E100F00090E101F000')
    crc = binascii.crc32(data) & 0xFFFFFFFF
    # crc 应等于 0xF33E9369
```

### 5.13 Stuffing（偏移 0x000000DB ~ 0x00000177，157 字节）

WinHex @ DB-177: 全部 FF

```
  157 字节全是 0xFF，填充到 188 字节。

  为什么有这么多 FF?
  PMT section 只有 30 字节（table_id 到 CRC），
  加上 TS 头(4B) + pointer_field(1B) = 35 字节。
  但 TS 包固定 188 字节 → 需要填 153 字节。
  标准规定用 0xFF 填充（ISO 13818-1 stuffing_byte = 0xFF）。

  实际填充 = 188 - 4(TS头) - 1(pointer) - 30(section) = 153 bytes
```

### 5.14 PMT 全字段速查表

打开 WinHex，Ctrl+G 到 0xBC，对照看:

```
  WinHex 偏移  字节值       字段名                      值/含义
  -----------  -----------  -------------------------  ------------------
  0x000000BC   47           sync_byte                   0x47
  0x000000BD   50           TEI/PUSI/PRI/PIDhi          01010000
  0x000000BE   00           PIDlo                       0x00 -> PID=0x1000
  0x000000BF   10           TSC/AFC/CC                   00/01/0000
  0x000000C0   00           pointer_field                0 (section starts next)
  0x000000C1   02           table_id                     0x02 = PMT
  0x000000C2   B0 17        section_length               23 bytes
  0x000000C4   00 01        program_number               1
  0x000000C6   C1           version(0) + cur_next(1)     当前有效
  0x000000C7   00           section_number               0
  0x000000C8   00           last_section_number          0 (total 1 section)
  0x000000C9   E1 00        PCR_PID                      0x0100 (vid PID)
  0x000000CB   F0 00        program_info_length          0 (no descriptors)
  0x000000CD   24           stream_type (video)          0x24 = H.265/HEVC
  0x000000CE   E1 00        elementary_PID (video)       0x0100
  0x000000D0   F0 00        ES_info_length (video)       0
  0x000000D2   90           stream_type (audio)          0x90 = G.711 mu-law
  0x000000D3   E1 01        elementary_PID (audio)       0x0101
  0x000000D5   F0 00        ES_info_length (audio)       0
  0x000000D7   F3 3E 93 69  CRC_32                      整个section校验
  0x000000DB   FF FF ...    stuffing                    153B fill to 188
```

### 5.15 PMT 给了解码器什么?

PMT 是解码器的"节目说明书"。解析完 PMT 后解码器知道了三件事:

```
  PMT says:
    |
    +-- PCR clock is on PID=0x0100   (同视频PID)
    |
    +-- Video: PID=0x0100, stream_type=0x24 (H.265)
    |   -> 设过滤器: PID=0x0100 -> H.265 解码器
    |
    +-- Audio: PID=0x0101, stream_type=0x90 (G.711)
        -> 设过滤器: PID=0x0101 -> G.711 解码器

  After PMT:
    PID=0x0100 + PUSI=1 -> PES header -> H.265 decoder
    PID=0x0100 + PUSI=0 -> raw NAL -> H.265 decoder (continuation)
    PID=0x0101 + PUSI=1 -> PES header -> G.711 decoder
    PID=0x0101 + PUSI=0 -> raw samples -> G.711 decoder

  That's it. PMT itself never needs to be read again
  during playback — it repeats every ~54 packets
  just in case a late-joining decoder needs the info.
```


## 6. 文件全景与 PID 分布

**总览**: 92,860 packets × 188 bytes = 17,457,680 bytes

| 包号 | 偏移范围 | 内容 |
|------|----------|------|
| #0 | 0x000000–0x0000BB | PAT |
| #1 | 0x0000BC–0x000177 | PMT |
| #2 | 0x000178–0x000233 | Video PES start (PUSI=1, PCR) |
| #3–#55 | 0x000234–... | Video continuations |
| #54 | 0x000027A8–... | PAT repeat |
| #55 | 0x00002864–... | PMT repeat |
| #107 | 0x00004E94–... | First Audio PES (PID=0x101) |
| #92859 | 0x010A6154–0x010A620F | Last TS packet |

### PID 分布

| PID | 名称 | 包数 | 占比 |
|------|------|------|------|
| 0x0000 | PAT | 1,776 | 1.9% |
| 0x0100 | VIDEO (H.265) | 83,475 | 89.9% |
| 0x0101 | AUDIO (G.711) | 5,833 | 6.3% |
| 0x1000 | PMT | 1,776 | 1.9% |

### 重复间隔

- **PAT**: 1,776 total, interval = 54 packets (10,152 bytes)
- **PMT**: 1,776 total, interval = 54 packets (10,152 bytes)

**为什么 PAT 和 PMT 都是 54 包一次?**

不是巧合——编码器把 PAT 和 PMT **成对绑定发送**:

```
  每 54 个包一个循环:
    pkt#0    PAT  (PID=0x0000)
    pkt#1    PMT  (PID=0x1000)
    pkt#2-53 视频+音频 (PID=0x0100/0x0101)
    pkt#54   PAT  (再发一次)
    pkt#55   PMT  (再发一次)
    pkt#56-107 ...
    ...

  54 × 188 = 10,152 bytes = 一个 PAT/PMT 循环周期
  92,860 ÷ 54 = 1719.63... → 文件有 1719+ 个完整循环
  每个循环里有 1 个 PAT + 1 个 PMT → 各 ~1776 次
```

编码器为什么这么做? 两个原因:
1. **快进/跳播需要 PSI**: 用户拖进度条到文件任意位置,
   解码器必须在 ~50ms 内拿到 PAT+PMT 才能开始播放。
   54 包 × 188B ≈ 10KB ~= 在 4Mbps 码率下约 20ms 的缓冲,
   加上 PCR 锁相时间，总启动延迟 < 100ms。
2. **管道复用天然成对**: PAT 告诉 PMT 在哪, PMT 告诉流在哪,
   两者缺一个就播不了。绑一起发保证解码器永远不会
   拿 PAT 找不到 PMT 的状况。

**54 不是标准规定**——ISO 13818-1 只要求 PAT 至少每 100ms 发一次,
PMT 至少每 100ms~400ms。54 是本编码器的选择,
换一个编码器可能是 40、80 甚至 200。
- **NULL packets**: 0 (无空包, 文件复用 100%)

## 7. WinHex 速查卡

### TS 头 4 字节快速判读

| 偏移 | 看什么 | 含义 |
|------|--------|------|
| +0 | `47` | sync_byte (永远 0x47) |
| +1 | bit6=1? | PUSI → 新 PES/PSI 开始 |
| +1 bit4-0 → +2 | 13 bits | **PID** (0x0000–0x1FFF) |
| +3 | bit5-4 | **AFC**: 01=净荷, 10=适配, 11=适配+净荷 |
| +3 | bit3-0 | **CC**: 0→15→0 循环, 同 PID 递增 |
| +4 | (AFC=10/11) | adaptation_field_length |
| +5 | bit4 | PCR_flag: 1=6-byte PCR 在后面 |

### 关键偏移一键定位

| Ctrl+G 到 | 看 | 含义 |
|-----------|-----|------|
| 0x000000 | `47 40 00 10` | PAT 头, PID=0x0000 |
| 0x000005 | `00` | PAT table_id=0x00 |
| 0x00000D | `00 01` | program_number=1 |
| 0x00000F | `F0 00` | PMT_PID=0x1000 |
| 0x0000C1 | `02` | PMT table_id=0x02 |
| 0x0000C9 | `E1 00` | PCR_PID=0x0100 |
| 0x0000CD | `24` | H.265 video stream_type |
| 0x0000D2 | `90` | G.711 audio stream_type |
| 0x00017D | `10` | PCR_flag=1 |
| 0x00017E | `0D 0F ...` | PCR value (6B) |
| 0x000184 | `00 00 01` | PES start_code |
| 0x00018C | `16` | PES_header_data_length=22 |
| 0x00018D | `...` | PTS (5B, 33-bit) |
| 0x0001A3 | `00 00 00 01 40` | VPS NAL (H.265) |
| 0x00004E94 | — | First Audio PES |
| 0x000027A8 | — | 2nd PAT repeat |

### 解复用三步法

```
1. PID=0x0000 → PAT → "PMT 在 PID=0x1000"
2. PID=0x1000 → PMT → "Video=0x0100(H.265), Audio=0x0101(G.711), PCR=0x0100"
3. 收 0x0100 + 0x0101 → PCR 建时钟 → PTS 同步 → 播放
```

---
*Generated from joa00002_seg2.ts, 92,860 packets, no NULLs.*
## 8. 字段值含义速查 (ISO 13818-1)

### 8.1 TS 包头 (4 bytes)

#### 8.1.1 全量 TS 包头详细解析 (92,860 packets)

> 完整逐包解析已拆分为独立文件: **`ts-headers-detailed.csv`**
>
> | 列 | 内容 |
> |----|------|
> | `packet_index` | 包序号 0~92859 |
> | `offset_hex` / `offset_dec` | 文件偏移 |
> | `tei` / `pusi` / `transport_priority` | TS 头 bit 级字段 |
> | `pid` / `pid_hex` / `pid_type` | PID + 类型名 |
> | `tsc` / `afc` / `cc` + 名称 | 加扰/适配/连续计数器 |
> | `cc_expected` / `cc_status` | CC 预期值 + 状态 (OK/JUMP/DUPLICATE/FIRST) |
> | `adaptation_field_length` ~ `af_flag_EXT` | 适配域长度 + 8 个 flag |
> | `pcr_present` / `pcr_base` / `pcr_ext` / `pcr_seconds` | PCR 时钟解析 |
> | `packet_type` | PAT_START / VIDEO_PES_START / AUDIO_CONT 等 |
> | `raw_4bytes` | 原始 4 字节 hex |
>
> 共 92,860 行, 31 列, CC 跳变 0 处, PCR 出现 3550 次。
> Excel 打开可直接筛选/排序/透视。

> **CC 连续性**: 全文件 92,860 包中, 同 PID 的 CC 全部连续递增, 无跳变, 无丢包。

#### sync_byte (Byte 0, 8 bits)

| 值 | 含义 |
|----|------|
| `0x47` | 正常 TS 同步字节 (固定值) |
| 其他 | 非 TS 包 / 数据损坏 |

#### TEI — transport_error_indicator (Byte 1, bit 7)

| 值 | 含义 |
|----|------|
| `0` | 当前包无传输错误 |
| `1` | 当前包至少有一个不可纠正的比特错误 (解复用层应丢弃) |

#### PUSI — payload_unit_start_indicator (Byte 1, bit 6)

| 值 | 含义 |
|----|------|
| `0` | 净荷中无 PSI/PES 段起始 |
| `1` | PES 包或 PSI 段从本包净荷的第一个字节开始 (pointer_field 后) |

> PUSI=1 是解复用器识别 PES 包边界的关键信号。每个 PES 包的第一个 TS 包 PUSI=1, 后续续传包 PUSI=0。

#### transport_priority (Byte 1, bit 5)

| 值 | 含义 |
|----|------|
| `0` | 正常优先级 |
| `1` | 高优先级 (同 PID 中优先处理) |

#### PID — packet identifier (Byte 1 bits 4-0 + Byte 2, 13 bits)

| PID 值 | 含义 |
|--------|------|
| `0x0000` | **PAT** — Program Association Table (节目关联表) |
| `0x0001` | **CAT** — Conditional Access Table (条件接收表) |
| `0x0002` | **TSDT** — Transport Stream Description Table |
| `0x0003–0x000F` | **Reserved** (保留) |
| `0x0010–0x1FFE` | **用户分配** — 可分配给 PMT / 视频 / 音频 / 私有数据等 |
| `0x1FFF` | **NULL Packet** — 空包 (不携带任何数据, 仅用于填充码率) |

#### TSC — transport_scrambling_control (Byte 3, bits 7-6)

| 值 | 含义 |
|----|------|
| `00` | **未加密** (明文传输) |
| `01` | **Reserved** (保留, 用户自定义) |
| `10` | **偶密钥加密** (Even key scrambled, 条件接收) |
| `11` | **奇密钥加密** (Odd key scrambled, 条件接收) |

#### AFC — adaptation_field_control (Byte 3, bits 5-4)

| 值 | 含义 | 适配域 | 净荷 |
|----|------|--------|------|
| `00` | **ISO/IEC Reserved** (未来使用, 不应出现在正常流中) | ✗ | ✗ |
| `01` | **Payload only** — 只有净荷, 无适配域 | ✗ | ✓ |
| `10` | **Adaptation only** — 只有适配域, 无净荷 (如时钟/填充) | ✓ | ✗ |
| `11` | **Adaptation + Payload** — 适配域后跟净荷 (如 PCR + PES) | ✓ | ✓ |

> **"净荷"(payload) 是什么？**
>
> TS 包净荷 = TS 包 188 字节中, 去掉 4 字节包头和适配域(若有)后, 剩下的数据。
> 不同 PID 的净荷内容不同:
>
> | PID | 净荷内容 | 实例 |
> |-----|----------|------|
> | `0x0000` (PAT) | PSI Section = table_id + PAT 数据 + CRC32 | joa00002: 0x000005-0x000014, 后面 FF 填充 |
> | `0x1000` (PMT) | PSI Section = table_id + PMT 数据 + CRC32 | joa00002: 0x0000C1-0x0000DA, 后面 FF 填充 |
> | `0x0100` (VIDEO) | **PES 包** = PES 包头 + H.265 NAL units | joa00002: 0x000184 起, PES头31B + H.265裸流 |
> | `0x0101` (AUDIO) | **PES 包** = PES 包头 + G.711 samples | joa00002: 0x00004E94 起 |
> | `0x1FFF` (NULL) | 0xFF 填充 (无意义, 仅占位) | 你文件里没有 NULL 包 |
>
> 一句话: TS 包去掉头和适配域, 剩下的就是"货"——对 PAT/PMT 是 PSI 表, 对 VIDEO/AUDIO 是 PES 包。

#### CC — continuity_counter (Byte 3, bits 3-0)

| 规则 | 说明 |
|------|------|
| **范围** | 0–15, 循环递增 |
| **同 PID 连续** | 同一 PID 的相邻 TS 包 (含净荷), CC 应 +1 |
| **无净荷不递增** | AFC=10 (adaptation only) 的包, CC 不递增 |
| **重复包** | CC 与前一个相同 → 同一包重复发送 (冗余) |
| **跳变** | CC 不连续 → **丢包** |

### 8.2 适配域 (Adaptation Field)

适配域是 TS 包中夹在 4 字节包头和净荷之间的一段可选数据。它不是在每个包里都出现——是否出现由 TS 包头第 4 字节的 AFC 决定:

| AFC | 适配域 | 典型用途 |
|-----|--------|----------|
| `01` | 无 | 纯数据包 (大部分 VIDEO 续传包) |
| `10` | 有, 无净荷 | 仅发送时钟或填充 (NULL 包、纯 PCR 包) |
| `11` | 有, 有净荷 | PCR + PES 数据 (每个 Video PES 的第一个 TS 包) |

**适配域内部结构** (按字节顺序):

```
adaptation_field_length (1 byte)    ← 自身之后还有多少字节
flags (1 byte)                      ← 8 个 flag 位, 决定后续哪些字段存在
├── PCR (6 bytes)                   ← flags.b4=1 时
├── OPCR (6 bytes)                  ← flags.b3=1 时
├── splice_countdown (1 byte)       ← flags.b2=1 时
├── transport_private_data (变长)   ← flags.b1=1 时
└── adaptation_field_extension (变长) ← flags.b0=1 时
stuffing_bytes (变长)               ← 剩余空间填 0xFF
```

**适配域存在的根本原因**: TS 包固定 188 字节, 但 PES 包长度不固定, 且 PCR 时钟需要定期发送。PES 数据不一定刚好填满, 适配域就是用来:
1. **塞 PCR** — 在视频 PES 的第一个 TS 包里插入 6 字节时钟, 解码器据此同步
2. **凑 188 字节** — 数据不够填满时, 用 0xFF 填充 (stuffing)
3. **标记事件** — 随机接入点 (RAI)、拼接点、不连续指示等

**你的文件实例**: 第一个 Video+PES 包 (0x00000178, AFC=11), 适配域 8 字节 = 1B length + 1B flags(仅 PCR_flag=1) + 6B PCR, 之后紧跟 PES 包。其余视频续传包 AFC=01, 无适配域, 全是净荷。

---

### 8.2.0 来自 joa00002_seg2.ts 的真实 AFC 统计

> 以下数据从 92,860 个 TS 包的真实 AFC 字段统计得出, 不是理论推演。

#### 全文件 AFC 分布

| AFC | 含义 | 包数 | 占比 | 在此文件中出现场景 |
|-----|------|------|------|--------------------|
| `01` | PAYLOAD_ONLY | 76,092 | 81.9% | 大多数数据包 — 纯净荷, 无适配域 |
| `11` | ADAPT_PLUS_PAYLOAD | 16,768 | 18.1% | 视频 PES 起始包 — 同时携带 PCR + PES 数据 |

**核心结论**:

- AFC=00 从未出现 — 合法 TS 流不会用这个值
- AFC=01 (纯净荷) 占绝大部分 — 大部分 TS 包不携带适配域
- AFC=10 (纯适配) 未出现 — 此文件没有发送纯时钟/填充的独立包
- AFC=11 (适配+净荷) 出现在 PES 起始包中 — 本例 PCR_PID=0x0100(视频), 所以 AFC=11 只在视频上; 如果 PCR_PID 指向音频, 音频 PES 起始包也会有 AFC=11

#### 按 PID 拆解 AFC

| PID | 类型 | AFC=01<br>(净荷) | AFC=11<br>(适配+净荷) | AFC=10<br>(纯适配) | 解读 |
|-----|------|------:|------:|------:|------|
| `0x0100` | H.265_VIDEO | 69,470 | 14,005 | 0 | PES起始包有适配域(PCR_PID在此); 续传包无适配域 |
| `0x0101` | G.711_AUDIO | 3,070 | 2,763 | 0 | PCR_PID在视频上, 音频适配域不含PCR; AFC=11可能带其他适配域标志 |
| `0x0000` | PAT | 1,776 | 0 | 0 | PSI表不需适配域 |
| `0x1000` | PMT | 1,776 | 0 | 0 | PSI表不需适配域 |

#### PUSI 与 AFC 的联动关系

| PID | PUSI | AFC 分布 | 含义 |
|-----|------|----------|------|
| `0x0000` (PAT) | 1 | AFC=01(PAYLOAD_ONLY): 1776 | 每个 PAT section 一个包 |
| `0x0100` (H.265_VIDEO) | 0 | AFC=01(PAYLOAD_ONLY): 62725, AFC=11(ADAPT_PLUS_PAYLOAD): 10122 | PES 续传 — 纯裸流, 不需适配域 |
| `0x0100` (H.265_VIDEO) | 1 | AFC=01(PAYLOAD_ONLY): 6745, AFC=11(ADAPT_PLUS_PAYLOAD): 3883 | 新 PES 起始 — 需要 PES 包头 + PCR |
| `0x0101` (G.711_AUDIO) | 0 | AFC=01(PAYLOAD_ONLY): 299, AFC=11(ADAPT_PLUS_PAYLOAD): 2763 | 音频续传 — 纯采样数据 |
| `0x0101` (G.711_AUDIO) | 1 | AFC=01(PAYLOAD_ONLY): 2771 | 新音频 PES 起始 — 需要 PES 包头 |
| `0x1000` (PMT) | 1 | AFC=01(PAYLOAD_ONLY): 1776 | 每个 PMT section 一个包 |

#### PCR 与 AFC 的关系

- 全文件 PCR 出现: **3,550** 次
- 其中 AFC=11 (适配域+净荷): **3,550** 次 → PCR 总是和 PES 数据打包发送
- 其中 AFC=10 (纯适配域): **0** 次 → 没有纯 PCR 包

前 5 次 PCR 出现位置:

| 包号 | 偏移 | PID | AFC | PUSI | CC | PCR_base | 时间 |
|------|------|-----|-----|------|----|----------|------|
| #2 | 0x00000178 | 0x0100 | 11 | 1 | 0 | 438,244,915 | 4869.388s |
| #91 | 0x000042D4 | 0x0100 | 11 | 1 | 7 | 438,260,275 | 4869.559s |
| #109 | 0x0000500C | 0x0100 | 11 | 1 | 5 | 438,275,635 | 4869.729s |
| #118 | 0x000056A8 | 0x0100 | 11 | 1 | 14 | 438,279,782 | 4869.775s |
| #124 | 0x00005B10 | 0x0100 | 11 | 1 | 4 | 438,383,769 | 4870.931s |

**规律**: 每次 PCR 都出现在 PUSI=1 的视频包里 — PCR 和 PES 包头同时出现, 共用同一个 TS 包, 适配域塞 PCR, 净荷塞 PES。

#### 典型视频包的适配域节奏

```
  PUSI=1 (新PES)            PUSI=0 (续传)            PUSI=0 (续传)  ...
  ┌────────────────┐        ┌────────────────┐        ┌────────────────┐
  │ TS头 │适配域│PES头│→       │ TS头 │  NAL data ...│        │ TS头 │  NAL data ...│
  │      │ PCR  │+NAL│        │      │  (184 bytes) │        │      │  (184 bytes) │
  │ AFC=11        │           │ AFC=01           │       │ AFC=01           │
  └────────────────┘        └────────────────┘        └────────────────┘
   = 适配域 + 净荷            = 只有净荷                = 只有净荷
```

**对应字节布局**:

```
PUSI=1, AFC=11:  47 xx xx 3x  [长度][flags][PCR_6B]  [00 00 01 ... PES data]
                  └─TS头─┘     └─── 适配域 ────┘     └────── 净荷 ──────┘

PUSI=0, AFC=01:  47 xx xx 1x  [H.265 NAL data ... 184 bytes ...]
                  └─TS头─┘     └────────── 净荷 ────────────────┘
```

#### AFC 实战：解析器视角

理论看懂了, 但写代码解析 TS 的时候, AFC 到底怎么用？下面用文件里的真实数据包, 一步步演示。

**一句话总结 AFC 的作用**: AFC 告诉你 "跳过 4 字节 TS 头之后, 还要跳过多少字节才到净荷"。

##### 通用解析公式

```python
afc = (ts_packet[3] >> 4) & 0x03

if afc == 1:  # 仅净荷
    payload_offset = 4           # 紧接 TS 头
elif afc == 2:  # 仅适配域
    af_len = ts_packet[4]
    payload_offset = None        # 没有净荷！
elif afc == 3:  # 适配域 + 净荷
    af_len = ts_packet[4]
    payload_offset = 4 + 1 + af_len  # 4B头 + 1B长度 + 适配域

payload = ts_packet[payload_offset:]  # 从这里开始才是数据
```

##### 情况一：AFC=01 (净荷) + PUSI=0 — 最常见, 视频续传包

**实例**: 包 #3, 偏移 `0x00000234`, PID=0x0100

TS 包头 4 字节: `47 01 00 11`

Byte 4: `11` = `00010001` → bit5-4 = `01` = AFC=01

```
  0x47  01  00  11  97  44  18  A2  F1  9F  42  51  ...
   │    │        │    │
   │    │        │    └── byte 4 = 00010001
   │    │        │         bit5-4 = 01 → AFC=01 → 只有净荷
   │    │        │
   │    │        └── byte 3 = 0x00, byte 2 = 0x01 → PID = 0x0100
   │    │
   │    └── byte 1 = 0x01 → PUSI=0(续传), TEI=0
   │
   └── byte 0 = 0x47 (同步字节)
```

**解析动作**: AFC=01 → 净荷从 offset `+4` (0x00000238) 开始, 共 `188−4=184` 字节

前 32 字节净荷:
```
  97 44 18 A2 F1 9F 42 51 4E B4 C6 FF BC AD 58 6B 90 0D B6 73 C0 BF 96 59 EE 5C B3 3E B7 FB 95 0D
```

> 这 184 字节是上一个视频 PES 包的延续, 全是 H.265 NAL 裸流。解析器拿到之后直接交给解码器。

##### 情况二：AFC=01 (净荷) + PUSI=1 — PAT 表

**实例**: 包 #0, 偏移 `0x00000000`, PID=0x0000

TS 包头 4 字节: `47 40 00 10`

PUSI=1 表示这是一个新的 PSI section 开始, 但 AFC 仍然是 01——PAT 不需要适配域。

PUSI=1 时, net payload 前多一个 `pointer_field` 字节 (pkt[4]=`00`, 指向 payload 里 section 的起始位置)。

```
  00000000: 47 40 00 10  [00]  00 B0 0D 00 01  ...
                     ↑    ↑    └── PSI section 从这里开始
                AFC=01    └── pointer_field = 0 (section 紧接其后)
```

> AFC=01 但 PUSI=1 的差别: net payload 的偏移不是简单的 `+4`, 还要 `+1`(pointer_field)。
> 同样 AFC=01, PUSI=0 和 PUSI=1 的解析路径不同——AFC 管"有没有适配域", PUSI 管"有没有 pointer_field"。

##### 情况三：AFC=11 (适配+净荷) + PUSI=1 — 视频 PES 起始, 最复杂

**实例**: 包 #2, 偏移 `0x00000178`, PID=0x0100

TS 包头 4 字节: `47 41 00 30`

Byte 4: `30` = `00110000` → bit5-4 = `11` = AFC=11

**逐字节解析**:

```
  00000178: 47 41 00 30  [07]  [10]  0D 0F 8B 19 FE 3C  [00 00 01 E2 ...]
              └TS头─┘     │     │    └── 6 字节 PCR ──────┘  └── PES 包头 ──┘
                          │     │
                          │     └── flags = 0x10 → PCR_flag=1 (后面有 PCR)
                          │
                          └── adaptation_field_length = 7
                              → 适配域占 1(长度自身) + 7 = 8 字节
```

**解析动作**:

1. AFC=11 → 有适配域
2. pkt[4] = `7` (adaptation_field_length)
3. 适配域占用 = 1(length自己) + 7(内容) = **8 字节**
4. 净荷从 offset `4 + 1 + 7` = **`+12`** (0x00000184) 开始
5. PUSI=1, 所以净荷第一个字节是 PES start_code (00 00 01)

净荷(前 32 字节):
```
  00 00 01 E2 25 25 88 81 16 21 3D 39 8A 25 8E 00 00 01 9E 71 B1 B9 3D 00 00 00 00 00 00 00 00 00
```

- PES 包头占 31 字节
- 实际 ES 数据从 0x000001A3 开始, 仅 **145 字节**
- 188 − 4(TS头) − 8(适配域) − 31(PES头) = **145 字节 H.265 裸流**

##### 情况四：AFC=10 (仅适配) — 此文件未出现, 但理论上存在

AFC=10 表示只有适配域, 没有净荷。典型用途: 纯 PCR 时钟包、NULL 填充包。

```
  TS头(4B)  [af_len=183]  [flags]  [PCR?]  [stuffing 0xFF...]
  └────────┘ └────── 适配域 184 字节 ───────────────────────────┘
  解析: payload_offset = None → 这个包没有数据, 跳过
```

> 你的文件没有 AFC=10 的包, 说明编码器不做纯时钟投递——PCR 总是搭在视频 PES 起始包里发送, 不单发。

##### 四种 AFC 解析路径总结

| AFC | 名称 | TS头后是什么 | payload_offset | 真实数据量 | 你文件中有吗 |
|-----|------|-------------|----------------|-----------|------------|
| `00` | 保留 | 非法 | N/A | 0 | ❌ |
| `01` | 仅净荷 | payload | `4` (如有 pointer_field 则 +1) | 184 或 183 | ✅ 76,092 包 |
| `10` | 仅适配 | adaptation_field + stuffing | `None` | 0 | ❌ |
| `11` | 适配+净荷 | af_len → adaptation_field → payload | `4 + 1 + af_len` | `188 − 4 − 1 − af_len` | ✅ 16,768 包 |

**核心心法**: AFC 就是一个**偏移量指示器**。拿到一个 TS 包, 看 byte 4 的 bit5-4:

- `01` → 净荷从偏移 4 开始
- `10` → 没有净荷
- `11` → 读偏移 4 的 `af_len`, 净荷从 `4 + 1 + af_len` 开始

三句话讲清了。

#### adaptation_field_length

| 值 | 含义 |
|----|------|
| `0` | 仅后跟一个字节 (flags 字节), 无 PCR/OPCR 等 |
| `1–182` | 适配域长度 (不含自身和 TS 头) |
| `183` | 最大值 (188 − 4 − 1 = 183 + flags byte) |

#### Flags 字节各 bit 含义

| 位 | 名称 | `0` | `1` |
|----|------|-----|-----|
| b7 | **DI** (discontinuity_indicator) | 连续 | **不连续**— CC 可跳变, PCR 不连续, 解码器需重置 |
| b6 | **RAI** (random_access_indicator) | 非随机接入点 | **随机接入点**—解码器可选此为起始帧 |
| b5 | **ESPI** (elementary_stream_priority) | 该流段正常优先级 | 该流段高优先级 |
| b4 | **PCR_flag** | 无 PCR | **有 PCR** (后跟 6 bytes 时钟) |
| b3 | **OPCR_flag** | 无原 PCR | 有原 PCR (用于编辑/转码) |
| b2 | **splicing_point_flag** | 非拼接点 | 拼接点 (splice_countdown 字段存在) |
| b1 | **TPD_flag** (transport_private_data) | 无私有数据 | 有私有数据 |
| b0 | **AE_flag** (adaptation_field_extension) | 无扩展 | 有扩展适配域字段 |

#### PCR (Program Clock Reference, 42 bits in 6 bytes)

| 组件 | 位宽 | 时钟 | 范围 |
|------|------|------|------|
| **PCR_base** | 33 bits | 90 kHz | 0 ~ 26.5 小时回绕 |
| **reserved** | 6 bits | — | 固定 `011111` |
| **PCR_ext** | 9 bits | 27 MHz | 0 ~ 299 (模 300) |

> PCR 和 SCR 本质相同, 都是 27 MHz 时钟采样。编码器每 ≤100ms 必须发送一次 PCR。
> PCR 频率 ≤ 40ms 可满足消费电子级同步需求。

### 8.3 PES 包

#### stream_id

| 值 | 含义 |
|----|------|
| `0xBC` | program_stream_map |
| `0xBD` | private_stream_1 |
| `0xBE` | padding_stream (填充) |
| `0xBF` | private_stream_2 |
| `0xC0–0xDF` | **MPEG Audio** (stream_id & 0xE0 == 0xC0) |
| `0xE0–0xEF` | **MPEG Video** (stream_id & 0xF0 == 0xE0) |
| `0xF0` | ECM_stream (授权控制) |
| `0xF1` | EMM_stream (授权管理) |
| `0xF2` | DSM_CC_stream (数据/控制) |
| `0xF3–0xF7` | ISO/IEC 13522 流 |
| `0xF8` | H.222.1 type A |
| `0xF9` | H.222.1 type B |
| `0xFA` | H.222.1 type C |
| `0xFB` | H.222.1 type D |
| `0xFC` | H.222.1 type E |
| `0xFD` | ancillary_stream (辅助数据) |
| `0xFE` | **H.264/H.265 SVC/MVC 子流** |
| `0xFF` | program_stream_directory |

> 你的文件: `0xE2` = MPEG Video stream #2 (带帧序列号)

#### PES_packet_length

| 值 | 含义 |
|----|------|
| `0` | 不限长 — 仅用于视频 ES, PES 长度由 ES 数据决定 |
| `>0` | PES 包中后续字节数 (不含前 6 字节) |

#### PTS_DTS_flags (b7-6 of flags byte)

| 值 | 含义 |
|----|------|
| `00` | **无 PTS/DTS** — 不携带时间戳 |
| `01` | **禁止** — ISO 13818-1 不允许此组合 |
| `10` | **只有 PTS** — 仅携带显示时间戳 (B 帧视频 / 音频常用) |
| `11` | **PTS + DTS** — 同时携带显示时间戳和解码时间戳 (I/P 帧重排序) |

> PTS = DTS + 帧重排序延迟 (仅含 B 帧的 GOP 时 PTS > DTS)

#### PES_scrambling_control (b5-4)

| 值 | 含义 |
|----|------|
| `00` | PES 净荷未加密 |
| `01` | PES 净荷已加密 (算法由 CA 系统定义) |
| `10` | PES 净荷已加密 |
| `11` | PES 净荷已加密 |

#### PES_priority (b3)

| 值 | 含义 |
|----|------|
| `0` | 正常优先级 |
| `1` | 高优先级 — 解码器可优先处理该 PES 包 |

#### data_alignment_indicator (b2)

| 值 | 含义 |
|----|------|
| `0` | 无对齐保证 |
| `1` | PES 净荷起始于访问单元或 GOP 边界 (`00 00 01` 后) |

#### copyright (b1)

| 值 | 含义 |
|----|------|
| `0` | 不受版权保护 |
| `1` | 受版权保护 |

#### original_or_copy (b0)

| 值 | 含义 |
|----|------|
| `0` | 复制件 |
| `1` | 原始件 |

#### ESCR_flag / ES_rate_flag / DSM_trick_mode / additional_copy_info / PES_CRC / PES_extension

| 值 | 含义 |
|----|------|
| `0` | 对应字段不存在 |
| `1` | 对应字段存在, 后跟具体数据 |

#### PTS / DTS 编码格式 (5 bytes, 33-bit value)

```
PTS: 0010b | PTS[32:30] | 1 | PTS[29:15] | 1 | PTS[14:0] | 1
DTS: 0001b | DTS[32:30] | 1 | DTS[29:15] | 1 | DTS[14:0] | 1
```

| 参数 | 说明 |
|------|------|
| **时钟** | 90 kHz (继承自系统时钟频率 / 300) |
| **范围** | 0 ~ 2^33 − 1 ≈ 26.5 小时 (回绕) |
| **间隔** | ≤ 0.7s 必须出现一次 PTS (ISO 13818-1) |
| **PCR→PTS 关系** | PCR 是编码器时钟, PTS 是该时钟下的展示时刻 |

### 8.4 PAT — Program Association Table

#### table_id

每个 PSI section 的第一个字节就是 table_id，决定 section 按哪种语法解析。

| 值 | 含义 |
|----|------|
| `0x00` | PAT (当前流的节目关联表) |
| `0x01` | CAT (条件接收表) |
| `0x02` | PMT (节目映射表) |
| `0x03–0x3F` | ISO/IEC 13818-1 保留 |
| `0x40–0xFE` | 用户自定义 |
| `0xFF` | 禁止 |

**本文件中的 table_id 实例 (打开 WinHex 对照看):**

PAT 包的 table_id (PID=0x0000):
```
  Ctrl+G → 0x00000005 (跳过 TS 头4B + pointer_field 1B)

  0x00000005: 00  <- table_id = 0x00 = PAT

  完整的 PAT section 从 0x00000005 开始:
    00           <- table_id (PSI 类型)
    B0 0D        <- section_length
    00 01        <- transport_stream_id
    C1           <- version/cur_next
    00           <- section_number
    00           <- last_section_number
    00 01        <- program_number = 1
    F0 00        <- PMT_PID = 0x1000
    2A B1 04 B2  <- CRC

  后面每一次 PAT 重复 (≈54 包一次) 的 table_id 也是 0x00。
```

PMT 包的 table_id (PID=0x1000):
```
  Ctrl+G → 0x000000C1 (跳过 TS 头4B + pointer_field 1B)

  0x000000C1: 02  <- table_id = 0x02 = PMT

  完整的 PMT section 从 0x000000C1 开始:
    02           <- table_id (区别于 PAT/CAT)
    B0 17        <- section_length
    ...

  table_id=0x02 每次 PMT 重复都一样。
```

**判断逻辑:** 解复用器收到 PID=0x0000 (PAT) 或动态获取的 PMT_PID 后,
先读 pointer_field 跳到 section, 再读第一个字节(`table_id`):
- `0x00` → 按 PAT 语法解析 → 提取节目列表 + PMT_PID
- `0x02` → 按 PMT 语法解析 → 提取流列表 + elementary_PID
- `0x01` → CAT (加密场景, 本例未出现)
- 其他 → 按对应语法或丢弃

#### section_syntax_indicator

| 值 | 含义 |
|----|------|
| `1` | PAT/PMT/NIT/CAT 等标准表 (含 CRC32) |
| `0` | 私有 section (无 CRC32) |

#### version_number

| 值 | 含义 |
|----|------|
| `0–31` | 版本号, 内容变化时 +1, 接收端据此判断是否需要更新 PMT |

#### current_next_indicator

| 值 | 含义 |
|----|------|
| `0` | 此表尚未生效 (下一版本才有效) |
| `1` | 此表当前有效 — 立即使用 |

#### program_number

| 值 | 含义 |
|----|------|
| `0x0000` | **NIT 入口** — PID 指向 Network Information Table |
| `0x0001–0xFFFF` | **用户节目** — PID 指向该节目的 PMT |

#### PMT_PID / network_PID (13 bits in 2 bytes)

将 16-bit 值与 `0x1FFF` 做 AND 得到 13-bit PID。

### 8.5 PMT — Program Map Table

#### stream_type — 编码格式标识

| 值 | 编码类型 | 常见应用 |
|----|----------|----------|
| `0x00` | ITU-T / ISO Reserved | — |
| `0x01` | MPEG-1 Video | VCD 视频 |
| `0x02` | MPEG-2 Video (H.262) | DVD / DVB / ATSC 视频 |
| `0x03` | MPEG-1 Audio | MP3 / Layer II |
| `0x04` | MPEG-2 Audio | BC Layer I/II/III |
| `0x06` | PES private data | DSM-CC / MHEG / 字幕 |
| `0x0F` | ADTS AAC Audio | MPEG-2/4 AAC (传送流格式) |
| `0x10` | MPEG-4 Part 2 Video | MPEG-4 visual |
| `0x11` | LOAS AAC Audio | MPEG-4 AAC (低开销格式) |
| `0x15` | Metadata (MPEG-7) | 元数据流 |
| `0x1B` | **H.264 (AVC)** | 最主流 — 闭路监控/蓝光/流媒体 |
| `0x24` | **H.265 (HEVC)** | **你的视频流** — 安防监控/4K/8K |
| `0x42` | AVS Video (中国) | CCTV 使用 |
| `0x80` | DigiCipher II Video | 私有加密系统 |
| `0x81` | AC-3 / E-AC-3 Audio | Dolby Digital / Dolby Digital Plus |
| `0x90` | **G.711 mu-law** | **你的音频流** — 电话级语音, VoIP/对讲 |
| `0x0D` | JPEG 2000 Video | 电影/归档 |
| `0x06` | PES 私有数据 | 私有字幕 / 私有数据 |

#### PCR_PID

| 值 | 含义 |
|----|------|
| `0x1FFF` | **该节目无 PCR** (不用于同步播放) |
| 其他有效 PID | PCR 携带在该 PID 的适配域中 (通常与视频 PID 相同) |

#### ES_info_length

| 值 | 含义 |
|----|------|
| `0` | **无描述符** (常见, 简单流) |
| `>0` | 后跟 N 字节描述符 (如视频分辨率/音频采样率等附加信息) |

### 8.6 TS 关键常量

| 常量 | 值 | 说明 |
|------|----|------|
| TS 包大小 | **188 bytes** | 固定 (4B 头 + 0~184B 净荷) |
| 同步字节 | `0x47` | 永远在 Byte 0 |
| PID 最大值 | `0x1FFF` = 8191 | 13 bits |
| PCR 精度 | **27 MHz** (37 ns) | 系统时钟频率 |
| PCR 最小间隔 | **100 ms** | ISO 13818-1 强制 |
| PCR 推荐间隔 | **≤ 40 ms** | 消费级同步 |
| PTS 精度 | **90 kHz** (11.1 μs) | 系统时钟频率 / 300 |
| PTS 最大间隔 | **700 ms** | ISO 13818-1 强制 |
| Section 最大长度 | **1024 bytes** (PSI) / **4096 bytes** (private) | |
| CRC32 多项式 | `0x04C11DB7` | 同 MPEG-2 PSI |

### 8.7 PS vs TS 关键差异

| 维度 | PS (Program Stream) | TS (Transport Stream) |
|------|---------------------|-----------------------|
| **设计目标** | 无错环境 (光盘/文件) | 有错环境 (广播/网络/天线) |
| **包长** | 可变 (≤ 2048 bytes) | 固定 188 bytes |
| **时钟字段** | SCR — 每个 Pack 头 (≥ 700 ms) | PCR — 适配域可选 (≤ 100 ms) |
| **时钟精度** | 27 MHz (同) | 27 MHz (同) |
| **复用上限** | 单节目 | 多节目 (不同 PID) |
| **纠错能力** | 无 | Reed-Solomon / 交织 / 卷积码 (物理层) |
| **同步字节** | Pack Start Code (00 00 01 BA) | 0x47 |
| **PES 格式** | 完全相同 | 完全相同 |
| **核心标准** | ISO 13818-1 §2.5 | ISO 13818-1 §2.4 |
| **应用** | DVD / Blu-ray / 文件存储 | 闭路监控 / DVB / ATSC / IPTV / 流媒体 |


---
*Generated from joa00002_seg2.ts, 92,860 packets, no NULLs.*

---

## 9. PID 载荷数据详解

本章按 PID 分类, 逐个拆解每种 PID 的载荷数据——不只是包头, 而是 TS 包里面装的东西。

### 9.1 PID=0x0000 (PAT — 节目关联表)

总包数: 1,776, 间隔 54 包

#### 第一条 PAT @ 0x00000000 (包 #0)

**TS 头**: `47 40 00 10` — sync=0x47, PUSI=1, PID=0x0000, AFC=1(净荷), CC=0

**载荷起始**: 0x00000005, pointer_field=0

**PSI Section**: table_id=0x00(PAT), section_length=13

```
  00000000: 47 40 00 10 00 00 B0 0D 00 01 C1 00 00 00 01 F0
  00000010: 00 2A B1 04 B2 FF FF FF FF FF FF FF FF FF FF FF
  00000020: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00000030: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00000040: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00000050: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00000060: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00000070: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00000080: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00000090: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  000000A0: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  000000B0: FF FF FF FF FF FF FF FF FF FF FF FF
```

#### 第二条 PAT (重复) @ 0x000027A8 (包 #54)

**TS 头**: `47 40 00 11` — sync=0x47, PUSI=1, PID=0x0000, AFC=1(净荷), CC=1

**载荷起始**: 0x000027AD, pointer_field=0

**PSI Section**: table_id=0x00(PAT), section_length=13

```
  000027A8: 47 40 00 11 00 00 B0 0D 00 01 C1 00 00 00 01 F0
  000027B8: 00 2A B1 04 B2 FF FF FF FF FF FF FF FF FF FF FF
  000027C8: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  000027D8: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  000027E8: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  000027F8: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00002808: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00002818: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00002828: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00002838: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00002848: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00002858: FF FF FF FF FF FF FF FF FF FF FF FF
```

> 与第一条 PAT 对比: **不相同** (仅包头 CC 从 0→1, PSI 载荷数据完全一致)

#### PAT 重复规律

- 首次出现: 包 #0, 偏移 0x00000000
- 首次重复: 包 #54, 偏移 0x000027A8
- 重复间隔: **54 包** = 10,152 字节
- 总重复次数: **1,776** 次
- 内容变化: **无变化** — 所有 PAT 内容完全一致 (同一个 TS 流节目不变)

### 9.2 PID=0x1000 (PMT — 节目映射表)

总包数: 1,776, 间隔 54 包

#### 第一条 PMT @ 0x000000BC (包 #1)

**TS 头**: `47 50 00 10` — sync=0x47, PUSI=1, PID=0x1000, AFC=1(净荷), CC=0

**载荷起始**: 0x000000C1, pointer_field=0

**PSI Section**: table_id=0x02(PMT), section_length=23

```
  000000BC: 47 50 00 10 00 02 B0 17 00 01 C1 00 00 E1 00 F0
  000000CC: 00 24 E1 00 F0 00 90 E1 01 F0 00 F3 3E 93 69 FF
  000000DC: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  000000EC: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  000000FC: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  0000010C: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  0000011C: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  0000012C: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  0000013C: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  0000014C: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  0000015C: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  0000016C: FF FF FF FF FF FF FF FF FF FF FF FF
```

#### 第二条 PMT (重复) @ 0x00002864 (包 #55)

**TS 头**: `47 50 00 11` — sync=0x47, PUSI=1, PID=0x1000, AFC=1(净荷), CC=1

**载荷起始**: 0x00002869, pointer_field=0

**PSI Section**: table_id=0x02(PMT), section_length=23

```
  00002864: 47 50 00 11 00 02 B0 17 00 01 C1 00 00 E1 00 F0
  00002874: 00 24 E1 00 F0 00 90 E1 01 F0 00 F3 3E 93 69 FF
  00002884: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00002894: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  000028A4: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  000028B4: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  000028C4: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  000028D4: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  000028E4: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  000028F4: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00002904: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00002914: FF FF FF FF FF FF FF FF FF FF FF FF
```

> 与第一条 PMT 对比: **不相同** (仅包头 CC 从 0→1, PSI 载荷数据完全一致)

#### PMT 重复规律

- 首次出现: 包 #1, 偏移 0x000000BC
- 首次重复: 包 #55, 偏移 0x00002864
- 重复间隔: **54 包** = 10,152 字节
- 总重复次数: **1,776** 次
- 内容变化: **无变化** — 所有 PMT 内容完全一致

### 9.3 PID=0x0100 (视频 — H.265/HEVC)

总包数: 83,475

#### 第一个视频 PES 包起始 @ 0x00000178 (包 #2)

**TS 头**: `47 41 00 30` — sync=0x47, PUSI=1(**PES起始**), PID=0x0100, AFC=11(**适配域+净荷**), CC=0

**适配域**: length=7, flags=0x10 (PCR_flag=1)
**PCR**: `0D 0F 8B 19 FE 3C` → PCR_base=438,244,915 = 4869.388s

**PES 包头** @ 0x00000184:
- start_code: 00 00 01
- stream_id: 0xE2 (Video stream #2)
- PES_packet_length: 9509 (+6 = 9515 bytes total PES)
- PTS: 256787730 (2853.197s @90kHz)
- PES 包头总长: 31 bytes
- **ES 数据起始**: 0x000001A3

```
  00000178: 47 41 00 30 07 10 0D 0F 8B 19 FE 3C 00 00 01 E2
  00000188: 25 25 88 81 16 21 3D 39 8A 25 8E 00 00 01 9E 71
  00000198: B1 B9 3D 00 00 00 00 00 00 00 00 00 00 00 01 40
  000001A8: 01 0C 01 FF FF 01 40 00 00 03 00 00 03 00 00 03
  000001B8: 00 00 03 00 5A AC 09 00 00 00 01 42 01 01 01 40
  000001C8: 00 00 03 00 00 03 00 00 03 00 00 03 00 5A A0 04
  000001D8: 02 01 21 63 6B 92 4C 92 EE 01 00 00 03 00 00 03
  000001E8: 00 00 03 00 00 08 00 00 00 01 44 01 C0 76 B0 33
  000001F8: 24 00 00 00 01 26 01 AF 19 80 31 3F 84 EC 17 D4
  00000208: 7D D3 1C DF E6 35 0F 07 2B 74 A6 EE 89 37 93 CC
  00000218: 99 36 1C EF 0B 90 20 D3 94 14 CB E6 88 AF F4 C2
  00000228: 78 D9 C3 B8 6F 63 21 17 8E 16 C2 13
```

#### 第二个视频 PES 包起始 @ 0x00002920 (包 #56)

**TS 头**: `47 41 00 14` — sync=0x47, PUSI=1(**PES起始**), PID=0x0100, AFC=1(净荷), CC=4

```
  00002920: 47 41 00 14 00 00 01 E3 19 00 88 81 16 21 3D 39
  00002930: 9A 51 8E 00 00 01 9E 71 B1 B9 5E 00 00 00 00 00
  00002940: 00 00 00 00 00 00 01 40 01 0C 01 FF FF 01 40 00
  00002950: 00 03 00 00 03 00 00 03 00 00 03 00 5A AC 09 00
  ...
```

#### 视频续传包 @ 0x00000234 (包 #3)

**TS 头**: `47 01 00 11` — sync=0x47, PUSI=0(**续传**), PID=0x0100, AFC=01(**仅有净荷**), CC=1
**载荷**: 从 0x00000238 开始的 184 字节纯 H.265 NAL 数据 (上一个 PES 的延续)

```
  00000234: 47 01 00 11 97 44 18 A2 F1 9F 42 51 4E B4 C6 FF
  00000244: BC AD 58 6B 90 0D B6 73 C0 BF 96 59 EE 5C B3 3E
  00000254: B7 FB 95 0D 97 6B B5 E5 15 9F 41 8E 47 F7 78 75
  00000264: C1 69 35 DB 03 F9 A4 8E F8 42 47 5C 93 F9 B7 0F
  ...
```

**视频流统计**:
- PES 包总数: **10,628**
- 含 PCR 的 TS 包: **3,550**
- 纯续传包: **72,847**
- 每个视频 PES 的第一个 TS 包 = PUSI=1 (新 PES 起始)
- 每个视频 PES 的后继 TS 包 = PUSI=0 + AFC=01 (纯净荷, 无适配域)
- 仅第一个 TS 包携带有 PCR/PTS, 其余都是裸 NAL 数据

> PUSI 是区分 PES 边界的唯一信号——没有长度字段告诉你 PES 在哪结束, 全靠下一个同 PID 的 PUSI=1 来标记新 PES 开始。

### 9.4 PID=0x0101 (音频 — G.711 mu-law)

总包数: 5,833

#### 第一个音频 PES 包起始 @ 0x00004E94 (包 #107)

**TS 头**: `47 41 01 10` — sync=0x47, PUSI=1(**PES起始**), PID=0x0101, AFC=1(净荷), CC=0

```
  00004E94: 47 41 01 10 00 00 01 C0 01 45 88 81 16 21 42 61
  00004EA4: 56 59 8E 00 00 01 9E 71 B1 B7 0C 00 00 00 00 00
  00004EB4: 00 00 00 FF F1 6C 40 25 9F FC 01 2E 34 14 54 48
  00004EC4: 5B 1D 06 62 22 A1 18 22 10 F4 94 4B 65 E3 1A A6
  00004ED4: D7 54 5E 10 45 0F DD 02 E7 2D D4 96 91 6A A7 11
  00004EE4: A9 74 2D 72 96 47 B1 3A BE 3A DF 07 00 30 4F 8E
  ...
```

#### 音频续传包 @ 0x00004F50 (包 #108)

**TS 头**: `47 01 01 31` — sync=0x47, PUSI=0(**续传**), PID=0x0101, AFC=01(**仅有净荷**), CC=1
**载荷**: 184 字节纯 G.711 mu-law 采样的延续

```
  00004F50: 47 01 01 31 24 00 FF FF FF FF FF FF FF FF FF FF
  00004F60: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
  00004F70: FF FF FF FF FF FF FF FF FF DB 30 18 CC 60 EB 0A
  00004F80: 6C C8 3A 40 13 E8 C1 11 16 1B A6 29 23 35 69 A2
  ...
```

**音频流统计**:
- PES 包总数: **2,771**
- 纯续传包: **3,062**
- 音频 TS 包无适配域 (AFC=01) — 不需要 PCR
- G.711 mu-law 编码: 8kHz 采样率, 8-bit, 64kbps 单声道 → 每秒 8000 字节

### 9.5 各 PID 载荷对比

| PID | 类型 | 包内结构 | PUSI=1 的含义 | AFC 特征 |
|-----|------|----------|--------------|----------|
| `0x0000` | PAT | TS头 → PSI Section → FF填充 | 新 PAT | 01 (纯净荷) |
| `0x1000` | PMT | TS头 → PSI Section → FF填充 | 新 PMT | 01 (纯净荷) |
| `0x0100` | H.265 | TS头 → [适配域+PCR] → PES包头 → NAL | 新 PES 起始 | 11 或 01 |
| `0x0101` | G.711 | TS头 → PES包头 → 音频采样 | 新 PES 起始 | 01 (纯净荷) |
| `0x1FFF` | NULL | TS头 → FF填充 | 无意义 | 10 或 01 |

**核心规律**:

1. **PSI (PAT/PMT)**: 占用完整 188 字节包, PUSI=1 的 pointer_field 指向 section 起始, 有用数据只有几十字节, 其余 FF 填充
2. **Video PES**: 跨越多个 TS 包, PUSI=1 的包有完整的 PES 包头+PCR, PUSI=0 的包全是纯裸流
3. **Audio PES**: 同样跨 TS 包, 但无 PCR, 包小, PES 头也短
4. **CC 跟踪**: 同一 PID 按 PUSI=1 重置? 不, CC 只管同 PID 连续递增, 与 PUSI 无关

---
*Generated from joa00002_seg2.ts*

## 10. PES Header Byte-by-Byte Parse

> PES (Packetized Elementary Stream, 包化基本流) 把原始压缩音视频包起来，
> 使其能在 TS 包里传输。每个 PES 包有自己的包头，携带时间戳和流标识。
> 一个 PES = 一个视频帧或一个音频帧。

#### 10.1 Overall Structure

PES 包头是**变长**的，分两部分:

```
  PES header
  +-------- 9 bytes --------+---------- hdr_len bytes ----------+--- ...
  | 固定包头                | 可选字段                          | ES 数据
  | start_code stream_id    | PTS DTS ESCR ...                 | NAL 单元
  | pkt_len flags1 flags2   | stuff bytes                      | 或 音频
  +-------------------------+----------------------------------+--- ...
  长度: 固定 9 字节         长度: PES_header_data_length        PES 剩余数据
```

`PES_header_data_length` 告诉你: "跳过 N 字节就到 ES 数据"。
解析器必须先读 flags 知道有哪些可选字段，然后无条件跳过 hdr_len 字节。

#### 10.2 Fixed Header (First 9 Bytes)

以样本文件的包#2为例 (PID=0x0100, H.265 视频):

  Raw: `00 00 01 E2 25 25 88 81 16`

##### Byte 0-2: start_code_prefix (24 bits)

```
  Bytes:  00 00 01
  Value:  0x000001
  固定: 0x000001 (和 PS start code 一样)
  含义: "这是一个 PES 包的起始标记"
```

##### Byte 3: stream_id (8 bits)

```
  Raw byte: E2 = 226
```

标识这个 PES 包里装的是哪种基本流:

| stream_id | Meaning | This file |
|-----------|--------|-----------|
| E0 | Video stream (MPEG-1/2) | |
| E1 | Video stream (MPEG-1/2) | |
| E2 | H.265/HEVC video | 有 |
| E3 | AAC audio (ADTS) | |
| C0 | Audio stream (MPEG-1/2) | |
| C1 | Audio stream (MPEG-1/2) | |
| BD | Private stream 1 (often AC-3) | |
| FD | PES_extension (JPEG 2000, etc.) | |
| F0-FE | reserved | |

> 看见 E2 → PES 里是 H.265 视频。BD → 大概率 AC-3 音频。
> 解码器靠 stream_id 把数据送到正确的解码器。

##### Byte 4-5: PES_packet_length (16 bits)

```
  Bytes:  25 25
  值: 9509 (0x2525)
  含义: 从这 2 字节之后开始算的 PES 包总字节数。
```

Two special cases:

- **视频流**: 通常是 0。意思是"长度未知"——PES 一直延续到
  下一个 PES 起始（下个同 PID 的 PUSI=1 包）。大部分视频 PES 都是这样。
- **音频流**: 通常有实际值(如 200-2000)。音频帧小且固定，长度可预知。

##### Byte 6: Flags Byte #1

```
  Raw byte: 88 = 10001000
    bit 7-6: 10 = 固定标记 (总是 10)
    bit 5-4: 00 = PES 加扰控制
           00 = 未加扰  01/10/11 = 已加扰
    bit 3:   1   = PES 优先级
           0 = 普通  1 = 高优先级
    bit 2:   0   = 数据对齐指示
           0 = 无对齐  1 = ES 数据从访问单元边界开始
    bit 1:   0   = 版权
           0 = 无版权  1 = 有版权
    bit 0:   0   = 原件/复制件
           0 = 复制件  1 = 原件
```

> 大部分仅用于信息标注。只有 `data_alignment_indicator` 对解析有实际影响：
> 置位时，ES 数据从一个访问单元边界（新帧开头）开始。

##### Byte 7: 标志字节 #2 (核心——控制后面跟什么)

```
  Raw byte: 81 = 10000001

    bit 7-6: 10 = PTS_DTS_flags  ←  MOST IMPORTANT FIELD
           ---------------
           All 4 possible values:
           ---------------
           00 = No PTS, no DTS. No timestamp at all.
                Used for: PAT/PMT packets (they are not timed),
                or when the stream doesn't need timestamps.
                Parser: skip 0 bytes for timestamp.

           01 = FORBIDDEN by ISO/IEC 13818-1.
                If you see this in real data, it's a muxer bug.
                Note: some early draft implementations used this
                to mean "DTS only, no PTS", but the final standard
                made it illegal.

           10 = PTS only (5 bytes). ← THIS FILE uses this.
                Meaning: display timestamp present, no decode timestamp.
                Used for: I-frames and P-frames. These are decoded
                and displayed in the same order — no reordering needed.
                Parser: read 5-byte PTS, skip DTS.

           11 = PTS + DTS (10 bytes).
                Meaning: both display AND decode timestamps present.
                Used for: B-frames. B-frames reference future frames,
                so decoding order != display order.
                Example: display order I0 B1 B2 P3, decode order I0 P3 B1 B2.
                  Frame B1: PTS=1 (display at t=1), DTS=3 (decode after P3)
                  Frame B2: PTS=2 (display at t=2), DTS=4 (decode after B1)
                Parser: read 5-byte PTS, then 5-byte DTS.
           ---------------

           In this file (flags2=0x81): bits 7-6 = 10 -> PTS only.
           This is an I-frame or P-frame with no B-frame reordering.

    ---
    Bits 5-0: 六个标志位——每个控制一种可选字段。
              按它们在码流里出现的顺序排列。
              逐个检查，累加偏移。
    ---

    bit 5:   0   = ESCR_flag — Elementary Stream Clock Reference
           ----------------------------------------
           A 42-bit timestamp in 6 bytes, encoded exactly like PTS (33-bit
           value + 9 mark bits), but runs at the ES rate instead of 27MHz.

           Encoding (6 bytes):
             byte0: 0.......  mark          byte3: 0.......  mark
                     .000....  ESCR[32:30]           .1111111  ESCR[22:16]
                     ......0.  mark
                     .......0  mark
             byte1: 11111111  ESCR[29:22]    byte4: 11111111  ESCR[15:8]
             byte2: 0.......  mark           byte5: 0.......  mark
                     .1111111  ESCR[21:16]           .1111111  ESCR[7:0]

           When used: tying ES-level clock to PCR. Almost never seen in
           real-world streams — PCR already provides timing.
           Cost if set: 6 bytes.

    bit 4:   0   = ES_rate_flag — Elementary Stream Rate
           ----------------------------------------
           22-bit unsigned integer in 3 bytes, specifying the ES bit rate
           in units of 50 bytes/second (= 400 bits/second).

           Encoding (3 bytes):
             byte0: 0.......  mark bit (always 0)
                     .1111111  ES_rate[21:15] (top 7 bits)
             byte1: 0.......  mark bit
                     .1111111  ES_rate[14:8]
             byte2: 0.......  mark bit
                     .1111111  ES_rate[7:0]

           Example: ES_rate = 0x0C350 ≈ 50,000 units
                    → 50,000 × 400 = 20,000,000 bps = 20 Mbps

           When used: decoder can pre-allocate buffer based on rate.
           Optional — most decoders ignore it.
           Cost if set: 3 bytes.

    bit 3:   0   = DSM_trick_mode_flag — Digital Storage Media Trick Mode
           ----------------------------------------
           Controls trick-play: fast forward, slow motion, freeze, reverse.

           First byte = trick_mode_control (3 bits):
             000 = fast forward       100 = slow motion
             001 = slow motion        101 = freeze
             010 = freeze             110 = fast reverse
             011 = fast reverse       111 = slow reverse

           Remainder depends on mode:
             fast_forward/fast_reverse:
               field_id (2bits) + intra_slice_refresh (1bit) + frequency_truncation (2bits)
               Total: 1 byte
             slow_motion / slow_reverse:
               rep_cntrl (5 bits): 1=no repeat, N=repeat N times
               Total: 1 byte
             freeze:
               field_id (2bits) + reserved (3bits)
               Total: 1 byte

           When used: DVR playback (fast-forward, rewind). Almost never in
           live streaming or file-based TS samples.
           Cost if set: 1 byte (trick_mode_control + mode-specific data).
           WARNING: ISO 13818-1 says DSM_trick_mode = 1 to 3 bytes depending
           on mode. In practice it's almost always 1 byte. A robust parser
           should consume exactly 1 byte after seeing flag=1, because the
           mode byte already encodes the mode-specific bits inside it.

    bit 2:   0   = additional_copy_info_flag
           ----------------------------------------
           7 bits of copy-related metadata, packed in 1 byte:

             bit 7: 0.......  mark bit
                     .1111111  additional_copy_info (7 bits)

           The 7 bits encode copy control:
             00-7F = copy freely
             80-9F = no more copies (copy once allowed)
             A0-BF = copy prohibited
             C0-FF = reserved

           When used: content protection (Macrovision, CGMS-A). Rare outside
           consumer recording devices.
           Cost if set: 1 byte.

    bit 1:   0   = PES_CRC_flag — PES CRC-16 Check
           ----------------------------------------
           16-bit CRC of the entire PES packet (from byte 0 through the byte
           immediately BEFORE the CRC field itself). Uses CRC-16/CCITT
           polynomial: x^16 + x^12 + x^5 + 1 (0x1021), initial value 0x0000.

           Encoding (2 bytes):
             byte0: previous_PES_packet_CRC[15:8]
             byte1: previous_PES_packet_CRC[7:0]

           IMPORTANT: This field comes LAST among the standard optional
           fields, immediately BEFORE the PES extension (if present).
           If both PES_CRC_flag AND PES_extension_flag are 1, CRC checks
           the PES packet up to (but not including) the extension.

           When used: error detection in storage/retransmission. Rare in
           broadcast TS because TS-level error detection is sufficient.
           Cost if set: 2 bytes.

    bit 0:   1   = PES_extension_flag ← THIS FILE: bit 0 = 1
           ----------------------------------------
           Indicates additional extension data follows after all previous
           optional fields. See Section 10.3.3 for full breakdown.

           Extension flags byte (1 byte) controls:
             bit 7: PES_private_data_flag → 16 bytes fixed
             bit 6: pack_header_field_flag → variable (read 1-byte len first)
             bit 5: sequence_counter_flag → 2 bytes fixed
             bit 4: P-STD_buffer_flag → 2 bytes fixed
             bit 0: extension_flag_2 → recursive (extremely rare)

           Cost if set: 1 byte (flags) + sum of sub-fields.

    ---
    FIXED ORDER RULE: These 6 flags are checked in bit order (5→0).
    Each flag=1 adds bytes to the optional field region in FIXED positions.
    You cannot reorder them — this is the MPEG-TS bitstream syntax order.

    Cumulative offset calculation:
      offset = 9;  // start after fixed 9-byte header
      if (PTS_DTS >= 2)    offset += 5;   // PTS
      if (PTS_DTS == 3)    offset += 5;   // DTS
      if (ESCR)            offset += 6;
      if (ES_rate)         offset += 3;
      if (DSM_trick_mode)  offset += 1;   // see warning above
      if (copy_info)       offset += 1;
      if (PES_CRC)         offset += 2;
      if (PES_extension)   offset += 1 + parse_extension_bytes();
      // Now at offset = 9 + PES_header_data_length → ES data starts
    ---
```

**Summary: what follows based on byte 7 flags:**

```
In this file (flags2=0x81):
  -> PTS (5 bytes)             ← from bits 7-6 = 10
  -> PES_extension (variable)  ← from bit 0 = 1
  Total after flags: 5 + extension bytes
```

**PTS_DTS_flags 速查表:**

| 值 | 含义 | 时间戳字节 | 典型场景 |
|-------|---------|----------------|--------------|
| 00 | 无时间戳 | 0 | PAT, PMT, SI 表 |
| 01 | 禁止 | -- | 永远不合法 |
| 10 | 仅有 PTS | 5 | I/P 帧, 音频 |
| 11 | PTS + DTS | 10 | B 帧 (解码顺序≠显示顺序) |

##### Byte 8: PES_header_data_length (8 bits)

```
  原始字节: 16 = 22 (十进制)
  含义: 后面 22 字节是可选字段。
        全部跳过就到 ES 数据了。
```

这是解析的关键: 读这个值，跳过这么多字节，然后就到 NAL 单元(或音频帧)了。
如果你只要载荷，不需要逐个解析可选字段——全跳过就行。
但如果需要时间戳，就必须解析 flags，知道有哪些字段。

#### 10.3 Optional Fields — Byte-Level Breakdown

所有可选字段从 PES 包头的第 9 字节开始。flags(Byte 7) 决定有哪些、按什么顺序。
PES_header_data_length 给出总大小。

##### 10.3.1 PTS — 显示时间戳 (5 字节)

每个 PTS 都是同样的 5 字节编码格式:

```
  Raw 5 bytes: 21 3D 39 8A 25

  Byte 0: 00100001
          ...1....  标记位 1
          ....000...  PTS[32:30] = 高 3 位
          .......1  标记位 0

  Byte 1: 00111101  = PTS[29:22]
  Byte 2: 00111001
          0.......  标记位
          .0111001  PTS[21:15] (中间 15 位的高 7)

  Byte 3: 10001010  = PTS[14:7]
  Byte 4: 00100101
          0.......  标记位
          .0100101  PTS[6:0] (低 15 的低 7)

  解码公式:
    top_3   = (byte0 >> 1) & 0x07  = 0
    mid_15  = ((byte0 & 1) << 14) | (byte1 << 7) | ((byte2 >> 1) & 0x7F)
            = (1 << 14) | (61 << 7) | (28 & 127)
            = 24220
    low_15  = ((byte2 & 1) << 14) | (byte3 << 7) | ((byte4 >> 1) & 0x7F)
            = (1 << 14) | (138 << 7) | (18 & 127)
            = 17682
    PTS = (0 << 30) | (24220 << 15) | 17682
    PTS = 793658642
    Time = 793658642 / 90000 = 8818.429 sec
```

**33 位为什么要 5 字节?** 33 位值被分隔/标记位切开(每字节的 MSB 都是标记)。
这样解码器可以检测时间戳里的位错误。

**为什么是 90kHz?** MPEG 选 90kHz 作为折中: 27MHz / 300 = 90,000 Hz。
对帧定时够精细(视频帧通常 15-60 fps)，又小到能放进 33 位(~26.5 小时才会回绕)。

##### 10.3.3 PES 扩展 (flags 在偏移 +14)

```
  Raw byte: 8E = 10001110

    bit 7: 1 = PES 私有数据标志
           若 1 → 后面有 16 字节私有数据。
           本例: bit 7 = 1 → 16 字节私有数据存在
           PES_private_data: `00 00 01 9E 71 B1 B9 3D 00 00 00 00 00 00 00 00`
           这些是编码器自定义字节。前 4 个常形成自定义标记: `00 00 01 9E`
           解码器忽略这些——靠 PES_header_data_length 直接跳过。

    bit 6: 0 = pack_header 标志
           若 1 → 后面有 pack header (8 字节)。

    bit 5: 0 = 节目包序号标志
           若 1 → 后面有 2 字节序号 + MPEG1/MPEG2 鉴别符。

    bit 4: 0 = P-STD_buffer 标志
           若 1 → 后面有 2 字节 STD 缓冲区信息。

    bit 3-1: 111 = 保留 (固定 '111')

    bit 0: 0 = PES 扩展标志 2
           若 1 → 还有一层扩展 (极少见)。

    扩展总字节数: 1 (flags) + 16 = 17
```

**PES_extension 为什么存在?** 有些编码器在 PES 包里塞附加元数据。
对于 H.265，编码器可能在私有数据中嵌入解码器配置或厂商自定义信息。
标准解码器忽略这一切，直接跳到 ES 数据。

**字段大小校验**: flags 说有 22 字节可选字段。
PES_header_data_length 也是 22。吻合！全部对上，无填充。

#### 10.4 Full Walkthrough — Packet #2 PES Header

以下是样本文件里第一个视频 PES 包头的完整字节标注:

```
  +  0: 00 00 01 E2 25 25 88 81 16 21 3D 39 8A 25 8E 00
  + 16: 00 01 9E 71 B1 B9 3D 00 00 00 00 00 00 00 00 00
  + 32: 00 00 01 40 01 0C 01 FF FF 01 40 00 00 03 00 00
```

| 偏移 | 字节 | 字段 | 值 | 含义 |
|--------|-------|-------|-------|--------|
| +0 | 00 00 01 | start_code_prefix | 0x000001 | PES 起始标记 |
| +3 | E2 | stream_id | 0xE2 | H.265/HEVC 视频 |
| +4 | 25 25 | PES_packet_length | 9509 | 视频 → 无界(0) |
| +6 | 88 | flags1 | -- | 未加扰, 无对齐 |
| +7 | 81 | flags2 | 10000001 | PTS 存在, PES扩展存在 |
| +8 | 16 | header_data_length | 22 | 22 字节可选字段 |
| +9 | 21..25 | PTS (5B) | 793658642 | = 8818.429s 显示时间 |
| +14 | 8E | PES_ext_flags | 10001110 | private_data=1 |
| +15 | 00..00 | PES_private_data (16B) | -- | 编码器自定义 |
| +31 | 00 00 00 01 | NAL起始 + VPS | -- | 第4层: ES 数据开始 |

**核心认知**: 解析器读 Byte 8(=0x16)，跳过 22 字节(从 +9 到 +30)，直接落到 +31，NAL start code 开始。中间的全是可选的元数据。

#### 10.5 PES 解析决策树

```
  读 Byte 7 (flags2)
  |
  +-- PTS_DTS = 00? → 无时间戳, 跳到下一个 flag
  +-- PTS_DTS = 10? → 读 5 字节 PTS, 前进 5
  +-- PTS_DTS = 11? → 读 5 字节 PTS + 5 字节 DTS, 前进 10
  |
  +-- ESCR_flag = 1? → 跳过 6 字节
  +-- ES_rate_flag = 1? → 跳过 3 字节
  +-- DSM_trick_mode = 1? → 跳过 1-3 字节
  +-- copy_info = 1? → 跳过 1 字节
  +-- PES_CRC = 1? → 跳过 2 字节, 可选校验 CRC
  |
  +-- PES_extension = 1? →
      |
      +-- 读扩展 flags (1 字节)
      +-- private_data = 1? → 跳过 16 字节
      +-- pack_header = 1? → 跳过 8 字节
      +-- seq_counter = 1? → 跳过 2 字节
      +-- std_buffer = 1? → 跳过 2 字节
      +-- ext2 = 1? → 递归解析扩展
  |
  比较: 已消费字节 vs PES_header_data_length
  |
  +-- consumed < hdr_len? → 跳过剩余字节 (填充)
  +-- consumed == hdr_len? → 完美, 到达 ES 数据
  +-- consumed > hdr_len? → 错误: 解析不匹配
```

#### 10.6 Quick Reference Table

| 字段 | 偏移 | 大小 | 怎么读 |
|-------|--------|------|-----------|
| start_code_prefix | +0 | 3 | 永远 00 00 01 |
| stream_id | +3 | 1 | E2=H.265 E1/E0=MPEG C0=MPEG 音频 BD=私有 |
| PES_packet_length | +4 | 2 | 大端 uint16。0 = 无界 (视频) |
| flags1 | +6 | 1 | 主要为信息标注用 |
| flags2 | +7 | 1 | bit 7-6 = PTS/DTS, bit 0 = 扩展。核心 |
| hdr_data_len | +8 | 1 | 跳过这么多字节就到 ES 数据 |
| PTS | +9 | 5 | 33 位, 90kHz 单位, 跨 5 字节分散编码 |
| DTS | +14 或 +20 | 5 | 和 PTS 一样编码, 仅 flags2=11 时存在 |
| ESCR | 可变 | 6 | 极少使用 |
| ES_rate | 可变 | 3 | 极少使用 |
| PES_extension | 可变 | 1+N | flags2 bit0=1 时存在 |
| ES 数据 | +9+hdr_len | 剩余 | NAL 单元(视频) 或 音频帧 |

> **一句话总结**: 读 Byte 7 (flags2) → 知道后面有哪些可选字段。
> 读 Byte 8 → 知道总长度。跳过那么多字节。到了，这就是真正的数据。


## 11. PSI Section 逐字节解析

#### 11.1 PAT Section (PID=0x0000)

Source: Packet #0, pointer_field=00, section at offset 0x00000005

```
  table_id:                00  (0x00 = PAT)
  section_syntax_indicator: 1
  section_length:          13  (includes CRC, counted from next field)
  transport_stream_id:     1 (0x0001)
  version_number:          0
  current_next_indicator:  1
  section_number:          0
  last_section_number:     0
```

**Program loop** (0 programs):

| # | program_number | program_map_PID |
|---|---------------|----------------|

  CRC32: 0x0001F000  (computed from table_id through last byte before CRC)

> PAT 是节目→PID 索引。找到 program_number，拿到 PMT 的 PID，然后去读那个 PID。

#### 11.2 PMT Section (PID=0x1000)

```
  table_id:               02  (0x02 = PMT)
  section_length:         23
  program_number:         1
  PCR_PID:                0x0100  (PCR for this program comes in packets with this PID)
  program_info_length:    0  (program-level descriptors)
```

**Stream descriptors**:

| # | stream_type | elementary_PID | ES_info_len |
|---|------------|---------------|-------------|
| 1 | 0x24 (H.265/HEVC) | 0x0100 | 0 |
| 2 | 0x90 (G.711 mu-law) | 0x0101 | 0 |

  CRC32: 0xF33E9369

> PMT 是流索引。列出所有基本流及其 PID 和编码类型。
> 解复用器据此构建路由: 视频→0x0100, 音频→0x0101。

## 12. 跨 PID 协作: PAT → PMT → PES → ES

#### 12.1 端到端数据流

```
  包 #0: PAT → "节目1 的 PMT 在 PID=0x1000"
      |
  包 #1: PMT → "视频 PID=0x0100 (H.265), 音频 PID=0x0101 (G.711)"
      |
  包 #2: 视频 PES 起始: PCR=4869.388s  PTS=2853.197s
      |                        (第一帧)
  包 #3+: 视频续传 ×N (纯 NAL)
  ...
  后面: 音频 PES 起始: PTS ≈ 第一个视频 PTS (同步!)
```

#### 12.2 前 21 帧 PTS 时间线

| 帧 # | 视频包号 | 视频 PTS | 音频包号 | 音频 PTS | 音视频差 |
|---------|----------|-----------|----------|-----------|---------|
| 1 | 2 | 8818.429s | 107 | 9058.836s | -240407ms |
| 2 | 56 | 8818.452s | 182 | 9058.789s | -240337ms |
| 3 | 91 | 8818.529s | 233 | 9059.099s | -240570ms |
| 4 | 99 | 8818.552s | -- | -- | -- |
| 5 | 109 | 8818.811s | -- | -- | -- |
| 6 | 118 | 8818.838s | -- | -- | -- |
| 7 | 124 | 8819.515s | -- | -- | -- |
| 8 | 135 | 8818.911s | -- | -- | -- |
| 9 | 145 | 8818.935s | -- | -- | -- |
| 10 | 150 | 8819.616s | -- | -- | -- |
| 11 | 159 | 8818.829s | -- | -- | -- |
| 12 | 167 | 8818.853s | -- | -- | -- |
| 13 | 172 | 8818.929s | -- | -- | -- |
| 14 | 178 | 8818.952s | -- | -- | -- |
| 15 | 184 | 8819.211s | -- | -- | -- |
| 16 | 190 | 8819.234s | -- | -- | -- |
| 17 | 195 | 8819.535s | -- | -- | -- |
| 18 | 205 | 8819.311s | -- | -- | -- |
| 19 | 215 | 8819.152s | -- | -- | -- |
| 20 | 220 | 8819.229s | -- | -- | -- |
| 21 | 227 | 8819.252s | -- | -- | -- |

#### 12.3 四层完整走读 (包 #2)

```
  00000178: 47 41 00 30  07 10 0D 0F 8B 19 FE 3C  00 00 01 E2 25 25 88...
            `-Layer 1:TS hdr-` `---Layer 2:Adapt Field---` `------Layer 3:PES hdr-------`

  第1层 - TS 头:   PID=0x0100(视频) PUSI=1(新PES) AFC=11(适配+净荷) CC=0
  第2层 - 适配域:  af_len=7, flags=0x10(PCR_flag=1), PCR_base=438244915, PCR_ext=60
                    27MHz 时间 = 4869.388s
  第3层 - PES:     start_code=00 00 01, stream_id=E2(H.265)
                    PTS_DTS=10(仅PTS) → PTS=815318946 = 9059.099s
  第4层 - ES:      H.265 NAL 单元 (VPS/SPS/PPS/IDR...)
                    以 VPS NAL (00 00 01 40) 开始
```

**四个核心结论**:

1. PCR ≠ PTS: PCR=4869.388s (编码器时钟), PTS=9059.099s (显示时间)
   PCR 是时钟源, PTS 是动作指令。两者本就不相等。
2. PTS 单位 90kHz: 1 个 PTS 单位 = 1/90000 秒 ≈ 11.1μs
3. 33 位回绕: PCR/PTS 在 90kHz 下约 26.5 小时回绕一次
4. 音视频同步靠 PTS: 同一帧的音视频 PTS 近似相等
   本例: 第一帧视频 PTS=8818.429s, 音频 PTS=9058.836s
   差值 = 240407ms → 同步

---
*第 10-12 章覆盖三个实战缺口: PES/PTS/DTS 解析、PSI section 解码、跨 PID 协作。*

