# FLV 容器格式完整参考

> 面向嵌入式/安防行业，和 TS 文档一脉相承——逐字段拆解、位级标注、实战心法。
> 无样本文件（目前工作区没有 .flv），按 Adobe FLV v10.1 规范 + 公有知识编写。
> 将来拿到真实 FLV 文件可像 TS 文档一样逐字节走读。

---

## 0. 全景图：FLV 三层嵌套（比 TS 少一层）

```
  FLV 文件
  +-- FLV Header  (固定 9 bytes)
  |     Signature "FLV" + Version + Flags + HeaderSize
  |
  +-- FLV Body  (PreviousTagSize + Tag 交替)
        |
        +-- PreviousTagSize (4 bytes, big-endian)  ← 反向索引/seek 用
        |
        +-- Tag
              +-- Tag Header (固定 11 bytes)
              |     Type(1) + DataSize(3) + Timestamp(3+1) + StreamID(3)
              |
              +-- Tag Data  (变长, = DataSize bytes)
                    +-- Script Tag  → AMF0/AMF3 编码的 onMetaData
                    +-- Video Tag   → FrameType+CodecID + VideoPacket
                    +-- Audio Tag   → SoundFormat+Rate+Size+Type + AudioPacket
```

**层边界公式:**

| 层 | 起始偏移公式 | 结束偏移 |
|----|-------------|---------|
| FLV Header | `0` | `DataOffset - 1` (通常 8) |
| PreviousTagSize₀ | `DataOffset` | `DataOffset + 3` (值恒为 0) |
| Tag #1 | `DataOffset + 4` | `DataOffset + 4 + 11 + DataSize - 1` |
| PreviousTagSize₁ | Tag #1 结束后 4 字节 | 值为 `11 + Tag#1.DataSize` |
| Tag #2 | PreviousTagSize₁ 结束 | ... |
| ... | ... 循环 | ... |

**和 TS 的关键差异:**

| 维度 | FLV | TS |
|------|-----|----|
| 包头长度 | 11 bytes (Tag Header 固定) | 4 bytes (TS Header 基本) |
| 包大小 | **可变长** (0 ~ 16MB) | **固定 188 bytes** |
| 时间戳 | **相对时间戳** (ms, 距上帧) | **绝对时间戳** (27MHz PCR / 90kHz PTS) |
| 节目信息 | **Script Tag (onMetaData)** | **PAT/PMT (PSI Table)** |
| 时钟参考 | **无 PCR** (用 tag timestamp) | **有 PCR** (27MHz) |
| 适配域 | **无** | **有** (PCR / stuffing) |
| 同步字节 | **"FLV" 前缀** (仅文件头) | **0x47** (每个包) |
| 错误恢复 | **PreviousTagSize 反向扫描** | **0x47 逐包同步** |
| 典型场景 | 直播 (RTMP/HTTP-FLV) / 点播 (文件) | 广播 / 闭路监控 / DVB / ATSC |
| 标准文件 | Adobe Flash Video Spec v10.1 | ISO/IEC 13818-1 |

---

## 1. FLV Header（固定 9 字节）

```
  Offset  Size  Field                Hex        Meaning
  ------  ----  -----                ---        -------
  0       3     Signature            46 4C 56   "FLV"
  3       1     Version              01         FLV version 1
  4       1     TypeFlags                      bit7-5: reserved(0)
                                    bit4: audio present (1=yes)
                                    bit3-1: reserved(0)
                                    bit0: video present (1=yes)
  5       4     DataOffset           00 00 00 09  Header size (always 9 for version 1)
```

### 1.1 Signature (offset 0, 3 bytes)

```
  0x00: 46 = 'F'
  0x01: 4C = 'L'
  0x02: 56 = 'V'

  和 MPEG PS 的 00 00 01 BA 不同，FLV 用 ASCII "FLV" 做魔数。
  探测 FLV 文件 → 读前 3 字节，等于 "FLV" 就是 FLV。
```

### 1.2 Version (offset 3, 1 byte)

```
  值 0x01 = FLV 版本 1。几乎所有现存的 FLV 文件都是 version 1。
```

### 1.3 TypeFlags (offset 4, 1 byte)

```
  按 bit 拆:
    bit7-5: 保留，必须为 0
    bit4:   TypeFlagsAudio  —  1 = 此文件含有音频 tag
    bit3-1: 保留，必须为 0
    bit0:   TypeFlagsVideo  —  1 = 此文件含有视频 tag

  常见值:
    0x01 = 仅有视频  0x04 = 仅有音频  0x05 = 音视频都有
```

### 1.4 DataOffset (offset 5, 4 bytes)

```
  4 字节大端无符号整数 = 9。FLV body 从第 9 字节开始。
  这是为未来 header 扩展预留的字段——规格上说可以 >9，但实际从未扩展过。
```

---

## 2. FLV Body 结构

### 2.1 PreviousTagSize (4 bytes, big-endian)

```
  每个 Tag 前面都有一个 4 字节的 PreviousTagSize:
    PreviousTagSize₀ = 0x00000000  (第一个永远是 0)
    PreviousTagSize₁ = Tag #1 的总大小 = 11 + Tag#1.DataSize
    PreviousTagSize₂ = 11 + Tag#2.DataSize
    ...
```

**为什么需要 PreviousTagSize？**

```
  FLV 没有像 0x47 那样的逐包同步字节。
  如果想从文件末尾向前解析，PreviousTagSize 就是"反向链表"。
  读取最后一个 PreviousTagSize → 前一个 Tag 的大小 → 定位到前一个 Tag 的开头。
```

> 实战：FLV 文件损坏时，从文件末尾往前扫 PreviousTagSize，可以找到最后一个完好 Tag。

### 2.2 Tag Header（固定 11 字节）

每个 Tag 的 11 字节头结构完全相同，不管 tag 类型：

```
  Offset  Size  Field              Type    说明
  ------  ----  -----              ----    ----
  0       1     TagType            UI8     0x08=Audio, 0x09=Video, 0x12=Script
  1       3     DataSize           UI24    紧跟 tag header 后的 tag data 的字节数 (大端)
  4       3     Timestamp          UI24    相对时间戳 (毫秒, 大端)
  7       1     TimestampExtended  UI8     时间戳高 8 位 → 构成 32 位时间戳
  8       3     StreamID           UI24    总是 0 (FLV 不支持多流复用)
```

#### TagType (offset +0, 1 byte)

```
  0x08 = Audio Tag    (音频帧)
  0x09 = Video Tag    (视频帧)
  0x12 = Script Tag   (元数据 / onMetaData)
```

#### DataSize (offset +1, 3 bytes)

```
  3 字节大端无符号整数。Tag Data 的字节数，不含 Tag Header 自己的 11 字节。
  范围: 0 ~ 2^24 - 1 ≈ 16.7 MB
  
  解复用器读 Tag Header → 取出 DataSize → 跳过这么多字节 → 到达下一个 PreviousTagSize。
```

#### Timestamp (offset +4, 3 bytes) + TimestampExtended (offset +7, 1 byte)

```
  组合成 32 位时间戳: timestamp = (extended << 24) | (ts_low)
  单位: 毫秒 (ms)
  含义: 这个 tag 相对于 FLV 文件起始的展示时刻
  
  和 TS 的关键区别:
    TS:  绝对时间戳 (90kHz PTS, 27MHz PCR)，26.5 小时回绕
    FLV: 相对时间戳 (ms), 从 0 开始累加, 49 天才会溢出

  第一个音/视频 Tag 的 timestamp 通常是 0。
  后续 tag 的 timestamp = 前一个同流 tag 的 timestamp + 帧间隔。
  
  示例:
    Tag #1 (video keyframe): timestamp = 0    (0ms)
    Tag #2 (video p-frame):  timestamp = 33   (33ms, 30fps)
    Tag #3 (audio aac):      timestamp = 0    (0ms, 独立时间线)
    Tag #4 (video p-frame):  timestamp = 66   (66ms)
    Tag #5 (audio aac):      timestamp = 23   (23ms, ~23ms per AAC frame)
```

#### StreamID (offset +8, 3 bytes)

```
  永远是 0x000000。FLV 不支持 TS 那样的多节目/多流复用(PID)。
  音视频通过不同的 Tag Type 区分，不是 PID。
```

### 2.3 Tag 大小公式

```
  Tag 总大小 = 11 (TagHeader) + DataSize (TagData)
  PreviousTagSize = Tag 总大小
  
  下一个 Tag 的起始偏移 = 当前 Tag 的起始偏移 + 4 (PreviousTagSize) + 11 + DataSize
```

---

## 3. Script Tag — onMetaData

Script Tag 是 FLV 的"元数据标签"，通常出现在文件第一个 Tag（第一个 PreviousTagSize=0 之后），
包含时长、分辨率、码率等。用 AMF (Action Message Format) 编码。

### 3.1 Tag Header

```
  TagType:   0x12 (Script)
  DataSize:  变长 (几十到几百字节)
  Timestamp: 0 (出现在文件开头)
  StreamID:  0
```

### 3.2 Tag Data 结构

```
  Data 起始:
    第一字节: AMF 类型
      0x02 = AMF0 String (脚本对象名, 通常是 0x0A "onMetaData")
      0x08 = AMF0 ECMA Array (键值对集合)
  
  典型内容 (AMF0):
    +0: 02 00 0A 6F 6E 4D 65 74 61 44 61 74 61     <- 字符串 "onMetaData"
    +12: 08 00 00 00 08                               <- ECMA array, 8 个元素
    +17: 00 08 64 75 72 61 74 69 6F 6E               <- key "duration"
    +...                                               <- value (Number, 双精度浮点)
    ...更多字段...
    +最后: 00 00 09                                     <- 数组结束标记
```

### 3.3 常见 onMetaData 字段

| Key | 类型 | 说明 |
|-----|------|------|
| `duration` | Number | 总时长 (秒) |
| `width` | Number | 视频宽度 (像素) |
| `height` | Number | 视频高度 (像素) |
| `videodatarate` | Number | 视频码率 (bps) |
| `framerate` | Number | 帧率 (fps) |
| `videocodecid` | Number | 视频编码 (7=H.264, 12=H.265) |
| `audiodatarate` | Number | 音频码率 (bps) |
| `audiosamplerate` | Number | 音频采样率 |
| `audiocodecid` | Number | 音频编码 (10=AAC) |
| `stereo` | Boolean | 是否立体声 |
| `filesize` | Number | 文件大小 |

---

## 4. Video Tag — H.264 / H.265

### 4.1 Tag Header

```
  TagType:   0x09 (Video)
  DataSize:  变长 (一帧视频数据的大小)
  Timestamp: 毫秒, 相对时间
  StreamID:  0
```

### 4.2 Tag Data — 通用头 (1 字节)

```
  Byte 0: FrameType(4bit) + CodecID(4bit)
```

#### FrameType (高 4 位)

| 值 | 含义 |
|----|------|
| 1 | **Key Frame** (I 帧, IDR) — AVC 可解码的起始点 |
| 2 | **Inter Frame** (P 帧 / B 帧) — 需要参考帧 |
| 3 | Disposable Inter Frame (H.263 only) |
| 4 | Generated Key Frame (服务端生成) |
| 5 | Video info / command frame |

#### CodecID (低 4 位)

| 值 | 编码 | 备注 |
|----|------|------|
| 1 | JPEG (no longer supported) | |
| 2 | Sorenson H.263 | |
| 3 | Screen Video | |
| 4 | On2 VP6 | |
| 5 | On2 VP6 alpha | |
| 7 | **AVC (H.264)** | **安防/直播主流** |
| 12 | **HEVC (H.265)** | **4K/8K** |

### 4.3 Video Tag Data — AVC (H.264)

当 CodecID=7(H.264) 时，紧接着通用头的 4 字节是 AVC 扩展：

```
  Byte 0: FrameType(4bit) | CodecID(4bit)
  Byte 1: AVCPacketType (1 byte)
  Byte 2-4: CompositionTime (3 bytes, 有符号 24-bit 大端)
  Byte 5+: AVC 包数据
```

#### AVCPacketType

| 值 | 含义 | 出现时机 |
|----|------|---------|
| 0 | **AVC Sequence Header** | 文件第一个视频 tag |
| 1 | **AVC NALU** | 每个视频帧 |
| 2 | **AVC End of Sequence** | 文件末尾 (可选) |

#### AVC Sequence Header (AVCPacketType=0)

```
  数据 = AVCDecoderConfigurationRecord (ISO 14496-15)
  内容: SPS + PPS (NAL 形式的视频参数)

  这个和 MP4 文件 'avcC' box 完全一样, 解码器初始化的必需数据。
  
  结构:
    +0:  configurationVersion = 1
    +1:  AVCProfileIndication   (SPS[1])
    +2:  profile_compatibility  (SPS[2])
    +3:  AVCLevelIndication     (SPS[3])
    +4:  0xFF | lengthSizeMinusOne (通常 0xFF, NALU 长度用 4 字节)
    +5:  0xE0 | numOfSequenceParameterSets (通常 1)
    +6-7: SPS 长度 (大端)
    +8:  SPS NAL 数据
    +9:  numOfPictureParameterSets (通常 1)
    +10-11: PPS 长度 (大端)
    +12: PPS NAL 数据
```

#### CompositionTime (3 bytes, signed 24-bit)

```
  3 字节有符号大端整数，单位 = 1/1000 秒 (ms)。
  公式: 展示时间 PTS = tag.timestamp + CompositionTime

  为什么需要?
    H.264 的 B 帧解码顺序 ≠ 显示顺序。
    解码器先解码 I→P→B (解码序), 但显示顺序是 I→B→P。
    CompositionTime 就是这个偏移量。
  
  典型值:
    I 帧: CompositionTime = 0  (PTS = DTS)
    P 帧: CompositionTime = 0  (PTS = DTS)
    B 帧: CompositionTime > 0  (PTS = DTS + offset)
    
  TS 里 B 帧用 PTS+DTS 两套时间戳处理同样的问题,
  FLV 用 Timestamp(=DTS) + CompositionTime(=delta) 一个加法就够。
```

#### AVC NALU (AVCPacketType=1)

```
  数据 = 一个或多个 NAL 单元的序列，每个 NALU 前面是 4 字节长度前缀:
  
  [4B NALU size] [NALU data] [4B NALU size] [NALU data] ...
  
  和 MP4 一样用长度前缀分隔 NALU，不是 TS 的 00 00 01 start code 分界。
  
  示例 (一个 IDR 帧):
    +0: 00 00 06 10  <- 第一个 NALU 大小 = 1552, NALU type=5 (IDR)
    +4: 00 00 00 01 65 88 84 ...  <- IDR slice data
    +...: 后续 NAL 单元
```

### 4.4 Video Tag Data — HEVC (H.265)

当 CodecID=12(H.265) 时：

```
  Byte 0: FrameType(4bit) | CodecID(4bit) = 1C
  Byte 1: HVCPacketType = 0 (Sequence Header) / 1 (NALU) / 2 (End)
  Byte 2-4: CompositionTime (3 bytes, signed 24-bit)
  Byte 5+: HEVC 数据
```

HVCPacketType 语义和 AVC 一致。

---

## 5. Audio Tag — AAC

### 5.1 Tag Header

```
  TagType:   0x08 (Audio)
  DataSize:  变长
  Timestamp: 毫秒
  StreamID:  0
```

### 5.2 Tag Data — 通用头 (1~2 字节)

```
  Byte 0: SoundFormat(4bit) | SoundRate(2bit) | SoundSize(1bit) | SoundType(1bit)
```

#### SoundFormat (高 4 位)

| 值 | 编码 | 说明 |
|----|------|------|
| 0 | Linear PCM, platform endian | |
| 1 | ADPCM | |
| 2 | **MP3** | |
| 3 | Linear PCM, little endian | |
| 4 | Nellymoser 16kHz mono | |
| 5 | Nellymoser 8kHz mono | |
| 6 | Nellymoser | |
| 7 | G.711 A-law | |
| 8 | G.711 mu-law | |
| 10 | **AAC** | **安防/直播主流** |
| 11 | Speex | |
| 14 | MP3 8kHz | |

#### SoundRate (bit 3-2)

| 值 | 采样率 |
|----|--------|
| 0 | 5.5 kHz |
| 1 | 11 kHz |
| 2 | 22.05 kHz |
| 3 | **44.1 kHz** |

#### SoundSize (bit 1)

| 值 | 位深 |
|----|------|
| 0 | 8 bit |
| 1 | 16 bit |

#### SoundType (bit 0)

| 值 | 声道 |
|----|------|
| 0 | Mono |
| 1 | Stereo |

> 注意：SoundRate/SoundSize/SoundType 仅对恒定参数编码（MP3/PCM/ADPCM）有意义。
> 对于 AAC，实际参数在 AAC Sequence Header (AudioSpecificConfig) 里定义，
> 这里的值仅供参考。

### 5.3 Audio Tag Data — AAC (SoundFormat=10)

AAC 音频包的 Byte 1 是 AACPacketType：

```
  Byte 0: SoundFormat(10) | SoundRate | SoundSize | SoundType
  Byte 1: AACPacketType
    = 0: AAC Sequence Header (AudioSpecificConfig)
    = 1: AAC Raw  (实际音频帧数据)
  Byte 2+: AAC 数据
```

#### AAC Sequence Header (AACPacketType=0)

```
  数据 = AudioSpecificConfig (ISO 14496-3)
  通常 2 字节:
    bit 0-4:  AudioObjectType (2=AAC-LC, 5=SBR, 29=Parametric Stereo)
    bit 5-8:  Sampling Frequency Index (3=48000, 4=44100, 5=32000, etc.)
    bit 9-12: Channel Configuration (1=mono, 2=stereo)
  
  解码器必须收到这个才能初始化 AAC 解码器。
  所以 FLV 的第一个 Audio Tag 一定是 AAC Sequence Header (timestamp=0)。
```

#### AAC Raw (AACPacketType=1)

```
  数据 = 原始 AAC 帧 (ADTS 或 raw AAC data)
  每个 tag 放一个 AAC 帧，1024 个采样点/frame。
  
  AAC-LC @ 44100Hz: 1024/44100 ≈ 23.2ms per frame
  所以 Audio Tag 的 timestamp 间隔 ≈ 23ms。
```

---

## 6. 从头到尾走读一个 FLV 文件（示意）

目前没有真实样本文件，以下用规格参数模拟典型场景。
等拿到 .flv 文件后再更新为 WinHex 逐字节对照。

```
  [FLV Header: 9 bytes]
  0x00000000: 46 4C 56 01 05 00 00 00 09
    46 4C 56 = "FLV"
    01       = version 1
    05       = audio(bit2)+video(bit0) present
    00 00 00 09 = header size 9

  [PreviousTagSize₀: 4 bytes]
  0x00000009: 00 00 00 00
    value=0, always 0

  [Tag #1: Script Data "onMetaData"]
  Tag Header (11 bytes):
    0x12          = TagType: Script
    00 01 2A      = DataSize: 298 bytes
    00 00 00      = Timestamp: 0ms
    00            = TimestampExtended: 0
    00 00 00      = StreamID: 0
  Tag Data (298 bytes):
    AMF0 "onMetaData" → duration/width/height/codec info...

  [PreviousTagSize₁: 4 bytes]
    00 00 01 35 = 11 + 298 = 309 = Tag#1 size

  [Tag #2: Video AVC Sequence Header]
  Tag Header:
    0x09          = Video
    00 00 2A      = VideoTag DataSize: 42 bytes
    00 00 00      = Timestamp: 0ms
    00
    00 00 00
  Tag Data (42 bytes):
    Byte 0: FrameType(1=keyframe) | CodecID(7=AVC) = 0x17
    Byte 1: AVCPacketType = 0 (Sequence Header)
    Byte 2-4: CompositionTime = 0x000000
    Byte 5+: AVCDecoderConfigurationRecord (SPS+PPS)

  [PreviousTagSize₂]
    00 00 00 35 = 11 + 42 = 53

  [Tag #3: Audio AAC Sequence Header]
  Tag Header:
    0x08          = Audio
    00 00 04      = AudioTag DataSize: 4 bytes
    00 00 00      = Timestamp: 0ms
    00
    00 00 00
  Tag Data (4 bytes):
    Byte 0: SoundFormat(10=AAC)|Rate(3)|Size(1)|Type(1) = 0xAF
    Byte 1: AACPacketType = 0 (Sequence Header)
    Byte 2-3: AudioSpecificConfig (2 bytes, e.g. AAC-LC 44100Hz stereo)

  [PreviousTagSize₃]

  [Tag #4: Video IDR Key Frame]
  Tag Header:
    0x09          = Video
    00 8C 00      = DataSize: 35840 bytes
    00 00 00      = Timestamp: 0ms
    00
    00 00 00
  Tag Data (35840 bytes):
    Byte 0: FrameType(1) | CodecID(7) = 0x17
    Byte 1: AVCPacketType = 1 (NALU)
    Byte 2-4: CompositionTime = 0
    Byte 5+: length-prefixed NAL units

  [PreviousTagSize₄]

  [Tag #5: Audio AAC Frame]
  Tag Header:
    0x08          = Audio
    00 01 1A      = DataSize: 282 bytes
    00 00 17      = Timestamp: 23ms
    00
    00 00 00
  Tag Data (282 bytes):
    Byte 0: SoundFormat(10) | ... = 0xAF
    Byte 1: AACPacketType = 1 (Raw AAC)
    Byte 2+: raw AAC frame data

  [Tag #6: Video P Frame]
  Tag Header:
    0x09          = Video
    ...           = DataSize
    00 00 21      = Timestamp: 33ms (33ms after keyframe, ~30fps)
    ...
  Tag Data:
    FrameType=2 (inter) | CodecID=7
    ...

  ...循环继续...
```

**和 TS 包 #2 的 PES 起始对比:**

| | FLV Video Tag | TS PES 起始包 |
|---|---|---|
| 帧类型标记 | FrameType(4bit) 在 Tag Data 第一字节 | PES start_code + stream_id |
| 关键帧识别 | FrameType=1 | 解码 NAL header 才知道 |
| 时间戳 | 32-bit ms, 相对时间 | 33-bit @90kHz, 绝对时间 |
| 时钟精度 | 1ms | 11.1μs |
| NALU 分隔 | 4 字节长度前缀 | 00 00 01 start code |
| SPS/PPS 位置 | 独立 Sequence Header Tag | 嵌入 PES 包 + sps/pps NALU |
| 包大小 | 可变, 可以一帧一包 | 固定 188B, 一帧拆分多包 |

---

## 7. FLV vs PS vs TS 三格式横评

```
  维度          | PS (Program Stream)        | TS (Transport Stream)      | FLV (Flash Video)
  --------------|---------------------------|----------------------------|--------------------------
  设计目标      | 无错环境 (光盘/文件)       | 有错环境 (广播/网络)       | 网络流媒体 (RTMP/HTTP-FLV)
  包长          | 可变 Pack (≤ 65535 bytes) | 固定 188 bytes             | 可变 Tag (0 ~ 16.7 MB)
  同步标记      | 00 00 01 BA (Pack Start)  | 0x47 (every packet)        | "FLV" (file header only)
  时间基        | SCR (27MHz)               | PCR (27MHz) + PTS (90kHz)  | Tag Timestamp (ms)
  时钟精度      | 37 ns                     | 37 ns / 11.1 μs            | 1 ms
  节目/流标识   | stream_id (PES)           | PID (13-bit)               | TagType (1 byte) + StreamID=0
  节目信息      | PSM 或隐含                | PAT/PMT → PID → stream_type| Script Tag (onMetaData)
  视频编码信息  | PES 包头 + NALU start code| PMT stream_type + PES      | AVC Sequence Header Tag
  音频编码信息  | PES stream_id             | PMT stream_type            | AudioSpecificConfig in Tag
  PTS/DTS        | PES 包头 (33-bit @90kHz) | PES 包头 (33-bit @90kHz)   | Tag Timestamp + CompositionTime
  B 帧处理      | PTS + DTS 双时间戳        | PTS + DTS 双时间戳         | CompositionTime offset
  封装冗余      | 低 (Pack 头 + PES 头)     | 高 (TS 头 + 适配域 + 填充) | 中 (Tag 头 + PreviousTagSize)
  seek 定位     | Pack Header + PTS 解析     | 靠 PAT/PMT + PCR/PTS       | PreviousTagSize 反向链 + KeyFrame
  典型应用      | DVD, Blu-ray, .mpg 文件   | DVB/ATSC/IP CCTV/安防DVR   | HTTP-FLV 直播, RTMP, 点播
  复杂度        | 高 (Pack/PSM/PES 三层)    | 最高 (TS/PES/PSI/Section)  | 低 (Header+Tag 两层)
```

**选择建议 (嵌入式安防视角):**

```
  本地存储 → PS   (文件级存储, 简单, PS 就是 PES 套了 Pack 壳)
  网络传输 → TS   (抗丢包, 固定包长, 行业标配)
  云端/直播 → FLV  (HTTP 友好, CDN 缓存, Web 播放器原生支持)
  
  PS→TS 差别最大:  固定化 + 加 PAT/PMT/PCR
  TS→FLV 再简化:   去固定包长 + 去适配域 + 去 PCR
```

---

## 8. FLV 实战要点

### 8.1 第一个 Tag

```
  顺序规律:
    Tag #1:  Script Tag (onMetaData)          ← 元数据
    Tag #2:  Video AVC Sequence Header        ← 解码器初始化
    Tag #3:  Audio AAC Sequence Header        ← 解码器初始化
    Tag #4:  Video IDR Key Frame              ← 第一个可解码帧
    Tag #5+: 交错排列音视频帧

  如果 FLV 文件开头缺少 AVC Sequence Header,
  播放器不知道 SPS/PPS，无法初始化 H.264 解码器 → 黑屏。
```

### 8.2 Seek 定位

```
  FLV 没有内置索引。要跳到第 N 秒:

  方法 1: 用 Script Tag 里的 keyframes 索引 (如果有)
  方法 2: 从头扫 Tag Header, 找 timestamp >= N*1000 的 Key Frame (FrameType=1)
  方法 3: 从 PreviousTagSize 反向链 + Video Tag Header 解析

  对比:
    TS seek:  二分查找 I 帧索引 (FileIframeIndex) → O(log N)
    FLV seek: 线性扫描 → O(N), 除非有 keyframes 元数据
```

### 8.3 HTTP-FLV 直播

```
  直播时 FLV 是"无限长文件"——没有 duration, 没有 PreviousTagSizeₙ。
  客户端 HTTP 连接拿到 Header + Script Tag → 持续读 Tag 直到连接断开。

  HTTP-FLV 延迟: 通常 1-3 秒 (GOP 缓存 + 网络)
  对比:
    RTMP:   1-2 秒
    HLS:    10-30 秒 (切片延迟)
    WebRTC: 0.5 秒以内
```

### 8.4 编码信息在哪取

```
  想知道视频分辨率/帧率:
    → Script Tag onMetaData["width"] / ["height"] / ["framerate"]
    → 或者: 解析 AVC Sequence Header 里的 SPS → 宽度/高度

  想知道编码类型:
    → Video Tag Data 第一字节低 4 位: CodecID
        7=AAC H.264, 12=HEVC H.265
    → Audio Tag Data 第一字节高 4 位: SoundFormat
        10=AAC

  对比 TS: PMT stream_type 一个字段就拿到了，更直接。
```

---

*暂时无样本文件，上述内容基于 Adobe FLV v10.1 规范 + 公有知识。
拿到真实 FLV 文件后可像 TS 文档 ch1~ch12 一样逐字节走读。*
