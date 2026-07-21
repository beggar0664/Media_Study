# FLV 封装学习笔记：joa00002_seg2_e2.flv

**当前学习样本**: `joa00002_seg2_e2.flv`  
**来源 PS**: `joa00002_seg2.ps`  
**转换脚本**: `ps2flv.py`  
**文件大小**: 5,589,296 bytes  
**封装策略**: 只取 `E2` 单路 HEVC 视频，音频按 AAC Tag 写入

这份笔记用于学习 FLV 容器结构。和 TS 不同，FLV 不是固定 188 字节包，而是由变长 Tag 串起来。注意：当前样本用于结构学习，不作为 PotPlayer 播放兼容性基准。

## 0. 可播放性说明：这两个 FLV 不作为 PotPlayer 播放基准

`E:\code\Media\MediaProtrocl\flv` 目录下目前有两个 FLV 文件：

| 文件 | 状态 | 说明 |
|---|---|---|
| `joa00002_seg2.flv` | 旧实验样本 | 旧脚本生成，`PreviousTagSize` 与 Tag 顺序不符合标准 FLV 文件顺序，不建议作为学习基准 |
| `joa00002_seg2_e2.flv` | 当前结构学习样本 | FLV Tag 顺序已修正，只取 E2 单路，但视频是 HEVC，使用 `CodecID=12` 扩展写法 |

PotPlayer 不能播放这两个文件，不等于 FLV 结构学习完全无效。原因分别是：

1. `joa00002_seg2.flv` 本身是旧封装实验文件，结构顺序有问题。
2. `joa00002_seg2_e2.flv` 虽然 FLV 基本结构更标准，但传统 Adobe FLV 标准只正式支持 H.264/AVC `CodecID=7`，不原生支持 HEVC/H.265。`CodecID=12` 属于扩展/事实约定，播放器是否支持取决于实现。

因此本目录当前 FLV 文件的定位是：

```text
用于学习 FLV Header / PreviousTagSize / Tag Header / TagData / Timestamp / WinHex 定位。
不作为 PotPlayer 通用播放兼容性样本。
```

如果目标是做 PotPlayer 更可能直接播放的 FLV，建议使用：

```text
视频：H.264/AVC，FLV CodecID=7
音频：AAC，SoundFormat=10
```

也就是说，需要先把 HEVC 转码成 H.264，或者另找 H.264 源样本。单纯把 HEVC 塞进 FLV，即使 Tag 结构正确，也可能因为播放器不支持 HEVC-in-FLV 而无法播放。

### 0.1 当前 FLV 资料定位

当前目录的 FLV 学习资料可以分成三类：

| 资料 | 用途 | 结论 |
|---|---|---|
| `joa00002_seg2_e2.flv` | 看真实 FLV 文件字节结构 | HEVC-in-FLV 扩展样本，适合学 Header/Tag/Timestamp/PreviousTagSize，不适合作为 PotPlayer 兼容样本 |
| `ps2flv.py` | 学习 PS 转 FLV 的封装过程 | 演示把 PS PES 里的 Annex-B NALU 转成 FLV 的 length-prefixed NALU |
| `joa_rtmp.c/h` | 学习标准 RTMP/FLV 推流写法 | 当前这份代码实际是 H.264/AVC + AAC，不是 H.265；它适合当标准 FLV 参考 |

所以 FLV 学习时建议同时记住两条线：

```text
标准兼容线：H.264/AVC CodecID=7 + AAC SoundFormat=10
扩展实践线：H.265/HEVC CodecID=12 或 Enhanced FLV，播放器兼容性不保证
```

`joa00002_seg2_e2.flv` 属于第二条线；`joa_rtmp.c` 当前实现属于第一条线。

## 1. FLV 总体结构

FLV 文件结构：

```text
FLV Header                 9 bytes
PreviousTagSize0           4 bytes，通常为 0
Tag #0                     11B Tag Header + TagData
PreviousTagSize #0         4 bytes，值 = 11 + TagDataSize
Tag #1
PreviousTagSize #1
...
```

当前样本开头真实字节：

```text
46 4C 56 01 05 00 00 00 09 00 00 00 00 12 00 00 6E 00 00 00 ...
```

拆开：

```text
46 4C 56       Signature = "FLV"
01             Version = 1
05             Flags = audio + video
00 00 00 09    DataOffset = 9
00 00 00 00    PreviousTagSize0 = 0
12             TagType = 18，Script Tag
```

## 2. FLV Header

FLV Header 固定 9 字节：

| 偏移 | 长度 | 字段 | 当前值 | 含义 |
|---:|---:|---|---|---|
| `0x00` | 3 | Signature | `46 4C 56` | ASCII `FLV` |
| `0x03` | 1 | Version | `01` | FLV version 1 |
| `0x04` | 1 | TypeFlags | `05` | bit2=audio，bit0=video |
| `0x05` | 4 | DataOffset | `00 00 00 09` | header 长度 9 |

TypeFlags：

```text
0x05 = 0000 0101b
bit2 = 1 -> 有音频
bit0 = 1 -> 有视频
```

## 3. PreviousTagSize

FLV 每个 Tag 后面都有 4 字节 `PreviousTagSize`。

```text
PreviousTagSize = 11 + TagDataSize
```

注意：文件头后面还有一个 `PreviousTagSize0`，它没有前一个 Tag，所以通常为 0。

当前样本：

```text
offset 0x09: 00 00 00 00  # PreviousTagSize0 = 0
```

第一个 Script Tag：

```text
TagDataSize = 110
Tag 总大小 = 11 + 110 = 121 = 0x79
PreviousTagSize = 00 00 00 79
```

它位于：

```text
Tag #0 起点: 0x0D
TagData 起点: 0x18
TagData 结束: 0x85
PreviousTagSize: 0x86..0x89 = 00 00 00 79
下一个 Tag 起点: 0x8A
```

## 4. Tag Header

每个 Tag Header 固定 11 字节：

```text
TagType       1 byte
DataSize      3 bytes, big-endian
Timestamp     3 bytes, low 24 bits
TimestampExt  1 byte, high 8 bits
StreamID      3 bytes, always 0
```

字段计算：

```text
Timestamp = TimestampLow24 | (TimestampExt << 24)
TagData starts at tag_offset + 11
PreviousTagSize starts at tag_offset + 11 + DataSize
NextTag starts at tag_offset + 11 + DataSize + 4
```

TagType 常见值：

| TagType | 含义 |
|---:|---|
| `8` | Audio Tag |
| `9` | Video Tag |
| `18` | Script Tag / metadata |

## 5. 当前样本前 10 个 Tag

| Tag | 偏移 | Type | DataSize | Timestamp | TagData 偏移 | PreviousTagSize 偏移 | PreviousTagSize |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | `0x0000000D` | 18 Script | 110 | 0 | `0x00000018` | `0x00000086` | 121 |
| 1 | `0x0000008A` | 9 Video | 121 | 0 | `0x00000095` | `0x0000010E` | 132 |
| 2 | `0x00000112` | 8 Audio | 4 | 0 | `0x0000011D` | `0x00000121` | 15 |
| 3 | `0x00000125` | 9 Video | 9486 | 0 | `0x00000130` | `0x0000263E` | 9497 |
| 4 | `0x00002642` | 9 Video | 1356 | 100 | `0x0000264D` | `0x00002B99` | 1367 |
| 5 | `0x00002B9D` | 8 Audio | 295 | 240225 | `0x00002BA8` | `0x00002CCF` | 306 |
| 6 | `0x00002CD3` | 9 Video | 1575 | 200 | `0x00002CDE` | `0x00003305` | 1586 |
| 7 | `0x00003309` | 9 Video | 1768 | 300 | `0x00003314` | `0x000039FC` | 1779 |
| 8 | `0x00003A00` | 9 Video | 1437 | 400 | `0x00003A0B` | `0x00003FA8` | 1448 |
| 9 | `0x00003FAC` | 9 Video | 947 | 500 | `0x00003FB7` | `0x0000436A` | 958 |

## 6. Script Tag：onMetaData

第一个 Tag 是 Script Tag：

```text
offset 0x0D:
12 00 00 6E 00 00 00 00 00 00 00
```

Tag Header：

```text
12             TagType=18 Script
00 00 6E       DataSize=110
00 00 00       Timestamp low=0
00             TimestampExt=0
00 00 00       StreamID=0
```

TagData 起点 `0x18`：

```text
02 00 0A 6F 6E 4D 65 74 61 44 61 74 61
08 00 00 00 05 ...
```

含义：

```text
02 00 0A "onMetaData"   AMF0 string
08 00 00 00 05           ECMA array, 5 entries
```

Script Tag 通常携带 metadata，例如 duration、width、height、videocodecid、audiocodecid 等。

## 7. Video Sequence Header Tag

Tag #1 是 Video Sequence Header：

```text
offset 0x8A:
09 00 00 79 00 00 00 00 00 00 00
```

Tag Header：

```text
09             TagType=9 Video
00 00 79       DataSize=121
00 00 00 00    Timestamp=0
00 00 00       StreamID=0
```

TagData 起点 `0x95`：

```text
1C 00 00 00 00 01 01 00 00 00 00 00 00 00 00 00 ...
```

FLV Video TagData 第一字节：

```text
1C = 0001 1100b
FrameType = 1  -> keyframe
CodecID   = 12 -> HEVC/H.265 扩展写法
```

### 7.1 VideoTagHeader 对照：H.264 与 H.265

FLV Video TagData 的第 1 字节可以拆成：

```text
高 4 bit = FrameType
低 4 bit = CodecID
```

常见取值：

| 字节 | 拆解 | 含义 |
|---|---|---|
| `17` | FrameType=1, CodecID=7 | H.264 keyframe |
| `27` | FrameType=2, CodecID=7 | H.264 inter frame |
| `1C` | FrameType=1, CodecID=12 | H.265 keyframe，扩展写法 |
| `2C` | FrameType=2, CodecID=12 | H.265 inter frame，扩展写法 |

标准 FLV 的正式基线是 H.264/AVC `CodecID=7`。`CodecID=12` 属于 HEVC 扩展约定，是否能播取决于播放器实现。

后续字节：

```text
00             AVCPacketType / HEVC sequence header
00 00 00       CompositionTime = 0
...            HEVCDecoderConfigurationRecord
```

注意：传统 Adobe FLV 标准只正式定义 AVC/H.264 `CodecID=7`。这里 `CodecID=12` 是 HEVC 的扩展/事实约定，播放器兼容性取决于实现。

## 8. Audio Sequence Header Tag

Tag #2 是 Audio Sequence Header：

```text
offset 0x112:
08 00 00 04 00 00 00 00 00 00 00
```

TagData 起点 `0x11D`：

```text
AE 00 15 88
```

拆解：

```text
AE = 1010 1110b
SoundFormat = 10 -> AAC
SoundRate   = 3  -> 44kHz 标记位（AAC 实际采样率看 ASC）
SoundSize   = 1  -> 16-bit
SoundType   = 0  -> mono

00           AACPacketType=0，AAC sequence header
15 88        AudioSpecificConfig
```

ASC `15 88` 来自 ADTS：

```text
audioObjectType = 2  # AAC-LC
samplingFrequencyIndex = 11  # 8000 Hz
channelConfiguration = 1  # mono
```

## 9. Video NALU Tag

Tag #3 是第一个视频数据 Tag：

```text
offset 0x125:
09 00 25 0E 00 00 00 00 00 00 00
```

Tag Header：

```text
09             TagType=9 Video
00 25 0E       DataSize=9486
00 00 00 00    Timestamp=0
00 00 00       StreamID=0
```

TagData 起点 `0x130`：

```text
2C 01 00 00 00 00 00 00 18 40 01 0C 01 FF FF 01 ...
```

Video TagData：

```text
2C             FrameType=2 inter frame, CodecID=12 HEVC
01             AVCPacketType=1 NALU
00 00 00       CompositionTime=0
00 00 00 18    NALU length=24
40 01 ...      HEVC VPS NAL payload
```

FLV 中 H.264/H.265 NALU 通常不是 Annex-B `00 00 01` 起始码，而是长度前缀：

```text
[4-byte NAL length][NAL bytes]
[4-byte NAL length][NAL bytes]
...
```

这是 FLV 和 PS/TS 裸流最重要的差异之一。

## 10. Audio Raw Tag

Tag #5 是第一个音频数据 Tag：

```text
offset 0x2B9D:
08 00 01 27 03 AA 61 00 00 00 00
```

Tag Header：

```text
08             TagType=8 Audio
00 01 27       DataSize=295
03 AA 61 00    Timestamp=240225 ms
00 00 00       StreamID=0
```

TagData 起点 `0x2BA8`：

```text
AE 01 01 2E 34 14 54 48 5B 1D 06 ...
```

Audio TagData：

```text
AE             SoundFormat=10 AAC, mono
01             AACPacketType=1 raw AAC frame
01 2E 34 ...   AAC raw payload，已经去掉 ADTS header
```

## 11. FLV 时间戳

FLV Timestamp 单位是毫秒。

Tag Header 中时间戳分成 4 字节：

```text
Timestamp lower 24 bits: 3 bytes
TimestampExtended:       1 byte
Timestamp = lower24 | (extended << 24)
```

当前视频 Tag 时间戳示例：

```text
Tag #3 video ts = 0
Tag #4 video ts = 100
Tag #6 video ts = 200
Tag #7 video ts = 300
```

这对应 E2 单路约 10 fps。

当前音频 Tag #5 时间戳为 `240225 ms`，说明脚本当前音频时间轴仍按源音频 PTS 相对基准计算，后续如果要严格 AV 同步，还需要继续清理音频基准。当前 FLV 学习重点先放在容器结构。

## 12. WinHex 定位方法

常用跳转点：

| 目标 | 偏移 |
|---|---:|
| FLV Header | `0x00000000` |
| PreviousTagSize0 | `0x00000009` |
| Script Tag | `0x0000000D` |
| Script TagData | `0x00000018` |
| Video Sequence Header Tag | `0x0000008A` |
| Video Sequence Header Data | `0x00000095` |
| Audio Sequence Header Tag | `0x00000112` |
| Audio Sequence Header Data | `0x0000011D` |
| First Video NALU Tag | `0x00000125` |
| First Video NALU Data | `0x00000130` |
| First Audio Raw Tag | `0x00002B9D` |
| First Audio Raw Data | `0x00002BA8` |

WinHex 搜索串：

| 搜索目标 | 十六进制搜索串 |
|---|---|
| FLV Header | `46 4C 56 01 05 00 00 00 09` |
| onMetaData | `02 00 0A 6F 6E 4D 65 74 61 44 61 74 61` |
| HEVC sequence header | `1C 00 00 00 00` |
| AAC sequence header | `AE 00 15 88` |
| HEVC NALU TagData | `2C 01 00 00 00` |
| AAC raw TagData | `AE 01` |

手动跳 Tag 的公式：

```text
next_tag_offset = tag_offset + 11 + DataSize + 4
PreviousTagSize = 11 + DataSize
```

例如 Script Tag：

```text
tag_offset = 0x0D
DataSize = 0x6E = 110
next = 0x0D + 11 + 110 + 4 = 0x8A
```

## 13. 和 TS 的关键差异

| 维度 | FLV | TS |
|---|---|---|
| 基本单位 | 变长 Tag | 固定 188 字节 packet |
| 同步方式 | 文件头 `FLV`，靠 TagSize 跳转 | 每包 `0x47` |
| 节目信息 | Script Tag / metadata | PAT/PMT |
| 时间戳 | Tag timestamp，毫秒 | PCR 27MHz，PTS/DTS 90kHz |
| 视频数据 | 长度前缀 NALU | PES 中常见 Annex-B start code |
| 音频 AAC | SequenceHeader + Raw AAC | ADTS AAC 或私有数据 |
| 适配域 | 无 | 有 adaptation field/PCR/stuffing |
| 多路节目 | 不适合复杂多节目 | 天然支持多 PID/多节目 |

## 14. 当前 ps2flv.py 注意点

当前脚本已做两点修正：

```text
1. FLV 文件顺序为 Header + PreviousTagSize0 + Tag + PreviousTagSize。
2. 默认只选择 PS 里的 E2 视频流，避免 E1/E2/E3 混流。
```

生成命令：

```bash
python ps2flv.py ..\ts\joa00002_seg2.ps joa00002_seg2_e2.flv
```

后续仍可继续完善：

- 音频时间轴基准。
- HEVC in FLV 的兼容性说明。
- Script Tag metadata 字段逐项解析。
- HEVCDecoderConfigurationRecord 字段逐项解析。

## 15. 标准 RTMP/FLV 代码对照：`joa_rtmp.c`

`D:\wechat_company\WXWork\1688857297759642\Cache\File\2025-12\joa_rtmp.c` 这份代码可以作为标准 RTMP/FLV 参考，但它当前实现的是 **H.264/AVC + AAC**，不是 H.265。

关键证据：

```c
if (ctx->video_codec != SHM_ENUM_VIDEO_CODEC_TYPE_H264) {
    UTIL_ERR("RTMP only supports H264, current codec=%d\n", ctx->video_codec);
    return -1;
}
```

视频 Tag 头写法：

```c
body[pos++] = 0x17; // KeyFrame + AVC
body[pos++] = 0x00; // AVC sequence header
...
body[0] = (keyframe ? 0x17 : 0x27);
body[1] = 0x01; // AVC NALU
```

SPS/PPS 解析也按 H.264 规则处理：

```c
int nal_type = nal[0] & 0x1f;
if (nal_type == 7) { /* SPS */ }
else if (nal_type == 8) { /* PPS */ }
```

metadata 里也明确写了：

```c
videocodecid = 7.0
audiocodecid = 10.0
```

这说明这份代码适合用来理解：

```text
标准 FLV = H.264/AVC CodecID=7 + AAC SoundFormat=10
```

它和当前 `joa00002_seg2_e2.flv` 的关系是：

```text
joa_rtmp.c  -> 标准兼容线
joa00002_seg2_e2.flv -> HEVC 扩展实践线
```

如果后续要做可播放性更好的 FLV 学习样本，优先补 H.264 + AAC 的资料；如果要继续研究 HEVC-in-FLV，则把它作为扩展写法单独记录，不要和标准 FLV 混在一起。
