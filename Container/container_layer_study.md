# 容器封装分层学习总纲：PS / TS / FLV 与 H.264/H.265

这份文档总结容器封装学习中的核心分层：

```text
封装层：PS / TS / FLV
编解码层：H.264 / H.265 / AAC 等 elementary stream
```

学习容器时必须把这两层分开。封装层负责“怎么组织、标识、同步、查找、传输”；编解码层负责“压缩数据本身怎么解码成画面/声音”。

## 1. 为什么要分层

一个视频文件不是单纯的 H.265 或 AAC 数据，而是：

```text
容器/封装层
  └── 音视频 elementary stream
        └── 编码数据
```

不同容器里的同一段 H.265 数据，外层长得完全不同：

```text
PS
  └── stream_id 0xE2
        └── PES
              └── H.265 NALU

TS
  └── PID 0x0100
        └── PES
              └── H.265 NALU

FLV
  └── Video Tag
        └── HEVC/H.264 NALU
```

封装层不解码视频，它只回答这些问题：

```text
这里有几路流？
每路是什么类型？
每路数据在哪里？
每帧什么时候解码/显示？
哪些数据属于同一路？
如何从文件中定位下一包/下一帧？
```

编解码层才回答：

```text
这是不是 H.265？
VPS/SPS/PPS 参数是什么？
这是 IDR 还是普通帧？
这个 slice 参考哪些历史帧？
如何重建图像？
```

## 2. 封装层重点

封装层重点可以归纳成六件事：

| 目标 | 说明 | 常见字段 |
|---|---|---|
| 分流 | 这段数据属于哪一路 | PS `stream_id`、TS `PID`、FLV `TagType` |
| 类型 | 这一路是什么编码 | TS `stream_type`、FLV `CodecID/SoundFormat` |
| 分包 | 数据怎么切成容器单位 | TS packet、PES、FLV Tag、PreviousTagSize |
| 时间 | 什么时候解码/显示 | PTS、DTS、FLV Timestamp |
| 同步 | 播放器时钟怎么建立 | SCR、PCR |
| 定位 | 下一包/下一帧在哪里 | start code、sync byte、DataSize、section_length |

一句话：

```text
封装层决定“数据如何被找到，并交给哪个解码器”。
```

## 3. 三种封装对比

### 3.1 PS

PS 更像文件/录像内部封装：

```text
Pack Header
System Header
PES video/audio
PES video/audio
...
```

PS 封装层重点：

| 字段 | 作用 |
|---|---|
| Pack start code `00 00 01 BA` | pack 起点 |
| SCR | PS 系统时钟 |
| PES start code `00 00 01 xx` | PES 起点 |
| stream_id | 标识这一路 PES，例如 `E2` / `C0` |
| PTS/DTS | 显示/解码时间 |
| PES_packet_length | PES 长度 |

Jooan 样本里的关键事实：

```text
E1 / E2 / E3  是三路独立 HEVC 视频
C0            是音频/私有音频
```

这里 `E1/E2/E3` 是封装层 stream_id，不是 NALU 类型，也不是帧类型。

### 3.2 TS

TS 更适合传输/广播：

```text
188-byte TS packet
PID
PAT
PMT
PES
PCR
```

TS 封装层重点：

| 字段 | 作用 |
|---|---|
| sync byte `0x47` | 每个 TS packet 的同步字节 |
| PID | 区分 PAT/PMT/视频/音频/私有流 |
| PAT | program -> PMT PID |
| PMT | stream_type -> elementary PID |
| stream_type | 编解码类型声明，例如 HEVC/AAC/private |
| adaptation field | PCR、stuffing 等 |
| PCR | 节目时钟，27MHz |
| PTS/DTS | PES 时间戳，90kHz |
| continuity counter | 同 PID 丢包/乱序检测 |

Jooan 当前正确 TS 映射：

```text
E2 -> PID 0x0100 -> stream_type 0x24 HEVC
C0 -> PID 0x0101 -> stream_type 0x06 private data
```

错误映射：

```text
E1 + E2 + E3 -> PID 0x0100
```

这个错误会把三条 HEVC 参考帧链混在一起，导致非 IDR 帧失效。

### 3.3 FLV

FLV 更像 Tag 流：

```text
FLV Header
PreviousTagSize0
Tag
PreviousTagSize
Tag
PreviousTagSize
...
```

FLV 封装层重点：

| 字段 | 作用 |
|---|---|
| Signature `FLV` | 文件识别 |
| TypeFlags | 是否有音频/视频 |
| TagType | audio/video/script |
| DataSize | TagData 长度 |
| Timestamp | 毫秒时间戳 |
| StreamID | 通常为 0 |
| CodecID | 视频编码类型，如 AVC/HEVC 扩展 |
| SoundFormat | 音频编码类型，如 AAC |
| PreviousTagSize | 反向校验/跳转 |

FLV 没有 PCR，也没有 TS 那种 PID。它靠 Tag 顺序和 Timestamp 播放。

## 4. PTS、DTS、SCR、PCR

这些都是封装层时间系统，不属于 SPS/PPS/NALU。

| 字段 | 所在封装 | 单位 | 作用 |
|---|---|---:|---|
| SCR | PS Pack Header | 27MHz | PS 系统时钟 |
| PCR | TS adaptation field | 27MHz | TS 节目时钟 |
| PTS | PES header | 90kHz | 显示时间 |
| DTS | PES header | 90kHz | 解码时间 |
| FLV Timestamp | FLV Tag Header | ms | Tag 播放时间 |

关系：

```text
SCR/PCR：建立或采样系统时钟
DTS：什么时候送进解码器
PTS：什么时候显示
FLV Timestamp：FLV Tag 的播放时间
```

视频有重排序时：

```text
DTS 顺序 = 解码顺序
PTS 顺序 = 显示顺序
```

## 5. 流 ID 与编解码类型

流 ID 和编解码类型不是一回事。

### 5.1 流 ID

流 ID 只告诉你“这是哪一路”。

```text
PS:
  E2 = 某一路视频 PES
  C0 = 某一路音频 PES

TS:
  PID 0x0100 = 某一路 ES
  PID 0x0101 = 另一条 ES

FLV:
  TagType 9 = 视频 Tag
  TagType 8 = 音频 Tag
```

`E2` 不等于 H.265，`PID 0x0100` 也不等于 H.265。它们只是编号。

### 5.2 编解码类型

编解码类型告诉播放器“这一路交给哪个解码器”。

```text
TS PMT:
  stream_type 0x24 = HEVC
  stream_type 0x0F = AAC
  stream_type 0x06 = private data

FLV:
  CodecID 7  = AVC/H.264
  CodecID 12 = HEVC 扩展/事实约定
  SoundFormat 10 = AAC
```

Jooan 音频案例：

```text
C0 数据看起来像 ADTS AAC
但 PMT 声明 0x0F AAC 会让 PotPlayer 弹框
声明 0x06 private data 不弹框
```

这说明编解码类型声明会直接影响播放器行为。

## 6. 编解码层重点

对 H.264/H.265 来说，编解码层主要看 NALU，但不止 SPS/PPS。

更完整地说：

```text
编解码层 = NALU + 参数集 + slice header + 参考帧关系 + 帧类型
```

编解码层关注：

| 内容 | 作用 |
|---|---|
| VPS/SPS/PPS | 解码参数 |
| SEI | 辅助信息 |
| AUD | 访问单元分隔 |
| IDR/CRA/BLA | 随机访问点 |
| I/P/B slice | 图像预测类型 |
| POC | Picture Order Count，显示顺序相关 |
| reference picture set | 参考帧关系 |

一句话：

```text
编解码层决定“拿到数据后如何解成画面/声音”。
```

## 7. H.264 NALU

H.264 NAL header 是 1 字节：

```text
+---------------+
|F|NRI| Type    |
|1| 2 | 5 bits  |
+---------------+
```

取类型：

```c
nal_type = nal[0] & 0x1F;
```

常见 H.264 NAL 类型：

| nal_unit_type | 名称 | 含义 |
|---:|---|---|
| 1 | non-IDR slice | 普通 slice，可能是 I/P/B |
| 5 | IDR slice | IDR，强随机访问点 |
| 6 | SEI | 补充增强信息 |
| 7 | SPS | Sequence Parameter Set |
| 8 | PPS | Picture Parameter Set |
| 9 | AUD | Access Unit Delimiter |
| 10 | End of Sequence | 序列结束 |
| 11 | End of Stream | 流结束 |
| 12 | Filler Data | 填充数据 |

H.264 参数集：

```text
SPS(type 7): 序列级参数，profile/level/宽高/bit depth/参考帧等
PPS(type 8): 图像级参数，熵编码、QP、deblocking、slice 配置等
```

H.264 图像：

```text
type 5 = IDR slice，一定是 IDR/I
type 1 = non-IDR slice，可能是 I/P/B
```

要判断 type 1 里到底是 I/P/B，需要继续解析 slice header 的 `slice_type`。

## 8. H.265 / HEVC NALU

H.265 NAL header 是 2 字节：

```text
+------------------------------------------------+
| F | Type(6) | nuh_layer_id(6) | temporal_id+1 |
+------------------------------------------------+
 1     6             6                 3
```

取类型：

```c
nal_type = (nal[0] >> 1) & 0x3F;
```

常见 H.265 NAL 类型：

| nal_unit_type | 名称 | 含义 |
|---:|---|---|
| 0 | TRAIL_N | trailing picture, non-reference |
| 1 | TRAIL_R | trailing picture, reference |
| 16 | BLA_W_LP | Broken Link Access |
| 17 | BLA_W_RADL | Broken Link Access |
| 18 | BLA_N_LP | Broken Link Access |
| 19 | IDR_W_RADL | IDR picture |
| 20 | IDR_N_LP | IDR picture |
| 21 | CRA_NUT | Clean Random Access |
| 32 | VPS | Video Parameter Set |
| 33 | SPS | Sequence Parameter Set |
| 34 | PPS | Picture Parameter Set |
| 35 | AUD | Access Unit Delimiter |
| 39 | Prefix SEI | SEI before picture data |
| 40 | Suffix SEI | SEI after picture data |

H.265 参数集：

```text
VPS(type 32): 视频参数集，多层/时域/profile/level 等
SPS(type 33): 序列参数，宽高、bit depth、POC 长度等
PPS(type 34): 图像参数，tile/slice/QP/deblocking/SAO 等
```

Jooan 样本中主要看到：

```text
32 VPS
33 SPS
34 PPS
19 IDR
1  TRAIL_R 普通参考帧
35 AUD  # 实验中补过
```

## 9. I 帧、IDR、Keyframe

核心关系：

```text
IDR 是 I 帧
I 帧不一定是 IDR
Keyframe 是容器/播放器层标记，不一定严格等于 IDR
```

### 9.1 I 帧

I 帧表示帧内编码：

```text
这一帧主要自己解，不依赖其他帧预测。
```

但普通 I 帧不一定刷新参考帧缓冲。

### 9.2 IDR

IDR 是更强的 I 帧：

```text
解码器看到 IDR 后，会刷新参考帧缓冲。
IDR 后面的帧不能引用 IDR 前面的帧。
```

所以：

```text
I 帧 = 这帧自己能解
IDR = 这帧自己能解，而且从这里重新开始最干净
```

### 9.3 Keyframe

Keyframe 通常是容器层或播放器层概念：

```text
FLV: FrameType=1 表示 keyframe
MP4: sample sync 表标记 key sample
```

它通常对应 IDR/CRA 这类随机访问点，但严格判断还要看编码层 NAL type。

## 10. IDR / CRA / BLA

H.265 随机访问点比 H.264 更细：

| 类型 | 含义 | 特点 |
|---|---|---|
| IDR | Instantaneous Decoder Refresh | 最强刷新点，清参考链 |
| CRA | Clean Random Access | 可作为随机访问点，常用于开放 GOP |
| BLA | Broken Link Access | 断链访问点，用于拼接/切流场景 |

学习阶段可以先这样记：

```text
IDR 最稳
CRA 也可随机访问，但语义比 IDR 弱
BLA 偏断链/拼接场景
```

## 11. NALU 在不同封装里的形态

### 11.1 PS / TS 常见 Annex-B

PS/TS 中 H.264/H.265 ES 常见是 Annex-B：

```text
00 00 01 [NAL]
00 00 00 01 [NAL]
```

H.265 示例：

```text
00 00 00 01 40 ...  # VPS, type 32
00 00 00 01 42 ...  # SPS, type 33
00 00 00 01 44 ...  # PPS, type 34
00 00 00 01 26 ...  # IDR, type 19
```

### 11.2 FLV / MP4 常见 length-prefixed

FLV 和 MP4 常用长度前缀：

```text
00 00 00 18 [NAL 24 bytes]
00 00 00 2B [NAL 43 bytes]
```

也就是：

```text
不是靠 00 00 01 找 NAL
而是先读 NAL length，再读指定长度的 NAL
```

PS/TS 转 FLV/MP4 时必须转换：

```text
Annex-B start code -> length-prefixed NALU
```

## 12. 两层如何协作

以 TS + HEVC 为例：

```text
TS PID 0x0100
  -> PMT 说 stream_type=0x24
  -> demuxer 知道这是 HEVC
  -> 按 PID 拼 PES
  -> 去掉 PES header
  -> 得到 HEVC NALU
  -> decoder 读 VPS/SPS/PPS
  -> decoder 解 IDR/P/B slice
```

如果封装层错了：

```text
E1/E2/E3 混成同一个 PID
```

编解码层收到的 NALU 顺序就是错的。每个 NALU 本身可能没坏，但参考关系会错。

如果编解码层错了：

```text
缺 SPS/PPS
PPS id 不存在
slice 数据坏
参考帧丢失
```

封装层再正确，解码器也解不出来。

## 13. Jooan 样本教训

错误理解：

```text
E1/E2/E3 是同一路视频的不同 PES
```

正确理解：

```text
E1/E2/E3 是三路独立 HEVC 视频流
```

错误封装：

```text
E1 + E2 + E3 -> 一个 TS PID
```

正确封装：

```text
E2 -> 一个 TS PID
```

错误后果：

```text
三路参考链混在一起
普通非 IDR 帧引用关系失效
只剩 IDR 能稳定显示
PotPlayer 表现为按 GOP 跳帧
```

这不是 SPS/PPS 本身坏，也不是 PTS/PCR 本身坏，而是封装层把不该合并的数据合并了。

## 14. 学习检查顺序

遇到一个容器文件，可以按这个顺序查：

1. 查封装层文件结构

```text
PS: pack / PES
TS: 188 packet / PAT / PMT / PID
FLV: Header / Tag / PreviousTagSize
```

2. 查有几路流

```text
PS stream_id
TS PID + PMT
FLV TagType + CodecID
```

3. 查每路类型

```text
HEVC / AVC / AAC / private
```

4. 查时间戳

```text
PTS / DTS / SCR / PCR / FLV Timestamp
```

5. 查 payload 是否能拼出 ES

```text
PES payload
FLV TagData
NALU length/start code
```

6. 最后进入编解码层

```text
VPS/SPS/PPS
IDR / CRA / BLA
普通 slice
POC
参考帧
```

## 15. 一句话总结

```text
封装层告诉你：这是什么流、什么时候播、数据在哪里。
编解码层告诉你：这帧怎么解、是不是关键刷新点、需要哪些参数集。
```

PTS、DTS、SCR、PCR、流 ID、编解码类型主要属于封装层。  
NALU、VPS/SPS/PPS、IDR/P/B、参考帧关系主要属于编解码层。

RTP 更适合作为 RTSP / GB28181 的媒体承载层单独学习，不放进这个容器总纲里。
