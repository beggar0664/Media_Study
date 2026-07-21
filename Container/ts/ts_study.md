# TS 封装学习笔记：joa00002_seg2_e2_private_audio.ts

**当前学习样本**: `joa00002_seg2_e2_private_audio.ts`  
**来源 PS**: `joa00002_seg2.ps`  
**生成策略**: `E2` 单路 HEVC 视频 + `C0` 私有音频数据保留  
**文件大小**: 6,936,260 bytes = 36,895 个 TS packet

这份笔记替代旧的 `ts-complete-reference.md` 作为当前学习入口。旧文档对应的 TS 文件已经不再作为基准；本笔记以当前验证可用的 `joa00002_seg2_e2_private_audio.ts` 为准。

## 1. 本样本最终结论

`joa00002_seg2.ps` 不是简单的一路视频文件。它内部交错了三路独立 HEVC 视频流：

| PS stream_id | FFmpeg stream id | 含义 | 单路规模 |
|---|---:|---|---:|
| `0xE1` | `0x1e1` | HEVC 视频子流 | 约 10 fps，3546 个 PES |
| `0xE2` | `0x1e2` | HEVC 视频子流 | 约 10 fps，3541 个 PES |
| `0xE3` | `0x1e3` | HEVC 视频子流 | 约 10 fps，3541 个 PES |

不能把 `E1/E2/E3` 合并到同一个 TS video PID。这样会把三条 HEVC 参考帧链混在一起，播放器通常只能解出 IDR，表现为按 GOP 跳帧。

当前推荐学习封装：

| 源 PS | TS PID | PMT stream_type | 说明 |
|---|---:|---:|---|
| `0xE2` | `0x0100` | `0x24` | 单路 HEVC 视频，PCR 放在此 PID |
| `0xC0` | `0x0101` | `0x06` | 私有音频/私有数据，保留原始 payload |

## 2. 为什么只取 E2

FFmpeg 验证命令：

```bash
ffmpeg -i joa00002_seg2.ps -map 0:0 -an -c copy -f mpegts e2.ts
```

FFmpeg 输出显示 `0x1e2` 是一条可独立播放的 HEVC 视频流：

```text
Stream #0:0[0x1e2]: Video: hevc, 512x288, 10 fps
frame=3541 time=00:05:54.00
```

`e2.ts` 能被 PotPlayer 正常播放。手写转换器只封装 `0xE2` 后，`joa00002_seg2_e2_fixed.ts` 也能正常播放。由此确认：跳 GOP 的根因不是 PCR、PTS、PAT、PMT 或 AUD，而是错误合并了三路视频。

错误映射：

```text
E1 + E2 + E3 -> PID 0x0100
```

正确映射：

```text
E2 -> PID 0x0100
```

如果以后要保留多路，应拆成不同 PID 或不同 program：

```text
E1 -> PID 0x0100
E2 -> PID 0x0101
E3 -> PID 0x0102
```

## 3. 为什么 C0 用 0x06 private data

`C0` 音频 ES 开头有 ADTS 同步字，例如：

```text
FF F1 6C 40 ...
```

但实验结果显示：

| 文件 | 音频 PMT 类型 | PotPlayer 行为 |
|---|---:|---|
| `joa00002_seg2_e2_av.ts` | `0x0F` AAC | 弹“开始播放时发生问题” |
| `joa00002_seg2_e2_av_audio_adts_pts.ts` | `0x0F` AAC | 仍弹框 |
| `joa00002_seg2_e2_av_aac_bd.ts` | `0x0F` AAC，PES stream_id 改 `0xBD` | 仍弹框 |
| `joa00002_seg2_e2_av_audio_private06.ts` | `0x06` private data | 不弹框 |
| `joa00002_seg2_e2_private_audio.ts` | `0x06` private data | 当前推荐学习样本 |

结论：这路 `C0` 暂时不要硬标为标准 AAC。用 `stream_type=0x06` 更适合学习封装：TS 保留数据，但不承诺通用播放器能解码。

`0x06 private data` 的含义：

- 该 PID 是 PES 私有数据。
- 通用播放器可以忽略它，或显示为私有流。
- 专用业务端仍可按厂商协议解析 payload。
- 这比误标成 `0x0F AAC` 更诚实，也更不容易触发播放器错误。

## 4. TS 文件全局结构

TS 文件是 188 字节 packet 的连续排列：

```text
TS file = packet[0] + packet[1] + ... + packet[N-1]
packet size = 188 bytes
sync byte   = 0x47
```

当前样本统计：

| 项目 | 数值 |
|---|---:|
| TS packets | 36,895 |
| bad sync | 0 |
| CC jumps | 0 |
| PAT packets | 1,771 |
| PMT packets | 1,771 |
| video PES starts | 3,541 |
| private audio PES starts | 2,771 |

四层结构：

```text
第1层 TS header       4 bytes，所有 TS packet 都有
第2层 adaptation field 可选，由 AFC 决定
第3层 payload         PAT/PMT section 或 PES 数据
第4层 ES data         HEVC NAL 或私有音频 payload
```

## 5. TS header 字段

TS header 固定 4 字节：

```text
byte0: sync_byte = 0x47
byte1: TEI | PUSI | priority | PID[12:8]
byte2: PID[7:0]
byte3: scrambling | adaptation_field_control | continuity_counter
```

字段含义：

| 字段 | 位数 | 作用 |
|---|---:|---|
| sync_byte | 8 | 固定 `0x47`，用于同步 packet 边界 |
| TEI | 1 | transport_error_indicator，传输错误标记 |
| PUSI | 1 | payload_unit_start_indicator，payload 中有 section/PES 起点 |
| PID | 13 | 标识 payload 属于哪一路数据 |
| TSC | 2 | scrambling control，当前样本为 0，不加扰 |
| AFC | 2 | adaptation field control |
| CC | 4 | continuity counter，同 PID 上递增检测丢包 |

AFC 速查：

| AFC | 含义 |
|---:|---|
| `01` | 只有 payload |
| `10` | 只有 adaptation field |
| `11` | adaptation field + payload |

当前 PID 分配：

| PID | 类型 |
|---:|---|
| `0x0000` | PAT |
| `0x1000` | PMT |
| `0x0100` | HEVC 视频 PES |
| `0x0101` | private audio/data PES |

## 6. 适配域详解

适配域由 TS header 第 4 字节里的 AFC 决定。

```text
byte3 = TSC(2 bits) | AFC(2 bits) | CC(4 bits)
AFC = (byte3 >> 4) & 0x03
```

AFC 的含义：

| AFC | 名称 | 适配域 | 载荷 | 常见用途 |
|---:|---|---|---|---|
| `00` | reserved | 无 | 无 | 不应出现 |
| `01` | payload only | 无 | 有 | PAT、PMT、普通 PES 数据 |
| `10` | adaptation only | 有 | 无 | 纯 PCR、填充 |
| `11` | adaptation + payload | 有 | 有 | 视频 PES 起始包携带 PCR |

解析 payload 起点的通用规则：

```c
payload_offset = 4;                    // 跳过 TS header
if (AFC == 2 || AFC == 3) {
    adaptation_field_length = pkt[4];
    payload_offset = 4 + 1 + adaptation_field_length;
}
if (AFC == 2) {
    payload 不存在;
}
if (payload_offset >= 188) {
    payload 不存在或长度为 0;
}
```

也就是说：

```text
AFC=01: payload 从 byte 4 开始
AFC=11: byte 4 是 adaptation_field_length，payload 从 4 + 1 + adaptation_field_length 开始
AFC=10: 只有 adaptation field，没有 payload
```

### 6.1 当前样本中的视频适配域

第 2 个 TS packet 是第一个视频 PES 起点：

```text
47 41 00 30 07 10 00 00 00 00 7E 00 00 00 01 E0 ...
```

逐字段拆：

```text
47             sync_byte
41 00          PUSI=1, PID=0x0100
30             AFC=11, CC=0
07             adaptation_field_length=7
10             adaptation flags: PCR_flag=1
00 00 00 00 7E 00  PCR 字段，6 bytes
00 00 01 E0    payload 起点，PES start code
```

计算 payload 起点：

```text
TS header = 4 bytes
adaptation_field_length 字节本身 = 1 byte
adaptation_field_length = 7 bytes
payload_offset = 4 + 1 + 7 = 12
```

所以第 12 字节开始才是 PES：

```text
pkt[12] = 00
pkt[13] = 00
pkt[14] = 01
pkt[15] = E0
```

适配域内部结构：

```text
07                         adaptation_field_length
10                         flags，bit4=1 表示 PCR_flag
00 00 00 00 7E 00          PCR 6 字节
```

PCR 字段编码：

```text
PCR_base: 33 bits
reserved: 6 bits，通常是 111111
PCR_ext:  9 bits
PCR_27MHz = PCR_base * 300 + PCR_ext
```

当前第一个 PCR 为 0：

```text
PCR_base = 0
PCR_ext  = 0
PCR time = 0s
```

### 6.2 当前样本中的 PAT/PMT 无适配域

第 0 个 PAT packet：

```text
47 40 00 10 00 00 B0 0D ...
```

第 4 字节 `0x10`：

```text
AFC = 01
CC  = 0
```

所以没有 adaptation field，payload 直接从 byte 4 开始：

```text
pkt[4] = 00  # pointer_field
pkt[5] = 00  # PAT table_id
```

第 1 个 PMT packet 同理：

```text
47 50 00 10 00 02 B0 73 ...
```

```text
AFC=01，payload_offset=4
pkt[4] = 00  # pointer_field
pkt[5] = 02  # PMT table_id
```

### 6.3 当前样本中的私有音频无适配域

第一个私有音频 packet：

```text
47 41 01 10 00 00 01 C0 01 34 ...
```

第 4 字节 `0x10`：

```text
AFC = 01
CC  = 0
```

所以 payload 从 byte 4 开始：

```text
pkt[4..] = 00 00 01 C0 ...  # PES start code
```

## 7. 载荷详解

TS header 和 adaptation field 只负责“怎么找到数据”。真正的数据在 payload 里。payload 的解释方式由 PID 决定。

当前样本的 payload 类型：

| PID | payload 类型 | PUSI=1 时 payload 起点 |
|---:|---|---|
| `0x0000` | PAT section | `pointer_field` 后是 PAT section |
| `0x1000` | PMT section | `pointer_field` 后是 PMT section |
| `0x0100` | HEVC video PES | PES start code `00 00 01 E0` |
| `0x0101` | private audio/data PES | PES start code `00 00 01 C0` |

### 7.1 净荷到底是什么

“净荷”就是 TS packet 去掉外层控制信息后，真正交给上层解析的数据区域。英文常叫 payload。

但学习时要区分三层“净荷”：

| 层级 | 去掉什么后得到 | 当前样本例子 |
|---|---|---|
| TS payload / TS 净荷 | 去掉 TS header 和 adaptation field | PAT section、PMT section、PES 包片段 |
| PES payload / PES 净荷 | 去掉 PES header | HEVC NAL 数据，或私有音频数据 |
| ES payload / ES 数据 | 编码器产生的原始 elementary stream | `00 00 00 01 40...`、`FF F1...` |

所以“净荷”不是固定等于视频帧或音频帧。它取决于你站在哪一层看：

```text
TS packet
  ├─ TS header
  ├─ adaptation field 可选
  └─ TS payload
       ├─ PSI section        # PAT/PMT PID
       └─ PES packet/片段    # 视频/音频/私有数据 PID
            ├─ PES header
            └─ PES payload / ES data
```

### 7.2 按 PID 看载荷数据

PID 决定 TS payload 该交给谁解析。当前样本只有四类有效 PID：

| PID | 名称 | payload 第一层结构 | 继续解析 |
|---:|---|---|---|
| `0x0000` | PAT | PSI section | 读 program -> PMT PID |
| `0x1000` | PMT | PSI section | 读 stream_type -> ES PID |
| `0x0100` | HEVC 视频 | PES packet/fragment | 去掉 PES header -> HEVC NAL |
| `0x0101` | 私有音频/数据 | PES packet/fragment | 去掉 PES header -> 私有 payload |

#### PID 0x0000：PAT 载荷

PID `0x0000` 的 payload 是 PAT，不是 PES。

当前第一个 PAT packet：

```text
47 40 00 10 00 00 B0 0D 00 01 C1 00 00 00 01 F0 00 2A B1 04 B2 ...
```

TS header 后的 payload：

```text
00                         pointer_field
00 B0 0D ... 2A B1 04 B2   PAT section
FF FF ...                  stuffing
```

PAT payload 解析步骤：

```text
1. PID=0x0000 -> 这是 PAT PID。
2. AFC=01 -> payload_offset=4。
3. PUSI=1 -> payload[0] 是 pointer_field。
4. pointer_field=0 -> section 从 payload[1] 开始。
5. section[0]=0x00 -> table_id=PAT。
6. 从 section 里读 program_number=1, PMT_PID=0x1000。
```

PAT 载荷核心信息：

```text
program 1 -> PMT PID 0x1000
```

#### PID 0x1000：PMT 载荷

PID `0x1000` 的 payload 是 PMT，也不是 PES。

当前第一个 PMT packet：

```text
47 50 00 10 00 02 B0 73 00 01 C1 00 00 E1 00 F0 00 24 E1 00 ... 06 E1 01 F0 00 ...
```

TS header 后的 payload：

```text
00                         pointer_field
02 B0 73 ... 89 13 68 39   PMT section
FF FF ...                  stuffing
```

PMT payload 解析步骤：

```text
1. PID=0x1000 -> 这是 PMT PID。
2. AFC=01 -> payload_offset=4。
3. PUSI=1 -> payload[0] 是 pointer_field。
4. pointer_field=0 -> section 从 payload[1] 开始。
5. section[0]=0x02 -> table_id=PMT。
6. 读 PCR_PID=0x0100。
7. 读 elementary stream loop。
```

PMT 载荷核心信息：

```text
PCR_PID = 0x0100
stream_type 0x24 -> PID 0x0100  # HEVC
stream_type 0x06 -> PID 0x0101  # private data
```

#### PID 0x0100：视频载荷

PID `0x0100` 的 payload 是视频 PES 数据。它有两种情况：

```text
PUSI=1: payload 从一个新 PES 开始，能看到 00 00 01 E0
PUSI=0: payload 是上一个 PES 的续传，通常直接是 HEVC 数据片段
```

当前第一个视频 packet：

```text
47 41 00 30 07 10 00 00 00 00 7E 00 00 00 01 E0 25 19 ...
```

解析步骤：

```text
1. PID=0x0100 -> 视频 PID。
2. AFC=11 -> 有 adaptation field，也有 payload。
3. adaptation_field_length=7 -> payload_offset=12。
4. PUSI=1 -> payload 从新 PES 开始。
5. payload[0..3] = 00 00 01 E0。
6. 跳过 PES header 后，得到 HEVC NAL 数据。
```

视频 PID 的 TS payload 是：

```text
00 00 01 E0 25 19 88 C0 0A 31 00 01 46 51 11 00 01 00 01 ...
```

视频 PID 的 PES payload / ES 数据是：

```text
00 00 00 01 40 ...  # VPS
00 00 00 01 42 ...  # SPS
00 00 00 01 44 ...  # PPS
...
```

如果后续某个 PID `0x0100` packet 的 PUSI=0，则它不会再从 `00 00 01 E0` 开始。它只是当前视频 PES 的后续 TS payload，需要拼接到同一个 PES buffer 里。

#### PID 0x0101：私有音频/数据载荷

PID `0x0101` 的 payload 是私有音频/数据 PES。

当前第一个私有音频 packet：

```text
47 41 01 10 00 00 01 C0 01 34 88 80 05 21 00 01 46 51 FF F1 6C 40 ...
```

解析步骤：

```text
1. PID=0x0101 -> 私有音频/数据 PID。
2. AFC=01 -> payload_offset=4。
3. PUSI=1 -> payload 从新 PES 开始。
4. payload[0..3] = 00 00 01 C0。
5. 跳过 PES header 后，得到私有 payload。
```

私有音频 PID 的 TS payload 是：

```text
00 00 01 C0 01 34 88 80 05 21 00 01 46 51 FF F1 6C 40 ...
```

私有音频 PID 的 PES payload 是：

```text
FF F1 6C 40 25 9F FC ...
```

注意：虽然 payload 看起来像 ADTS，但 PMT 明确把 PID `0x0101` 声明为 `stream_type=0x06 private data`。学习阶段按私有数据保留，不按标准 AAC 解码。

#### 按 PID 分发 payload 的伪代码

```c
switch (pid) {
case 0x0000:
    parse_pat(payload, payload_len, pusi);
    break;
case 0x1000:
    parse_pmt(payload, payload_len, pusi);
    break;
case 0x0100:
    push_pes(video_pes_buffer, payload, payload_len, pusi);
    break;
case 0x0101:
    push_pes(private_audio_pes_buffer, payload, payload_len, pusi);
    break;
default:
    // unknown PID, ignore or log
    break;
}
```

`push_pes()` 的核心规则：

```text
如果 PUSI=1:
    先结束上一 PES
    从当前 payload 开始新 PES
如果 PUSI=0:
    当前 payload 追加到上一 PES
```
### 7.3 TS 净荷长度怎么算

通用公式：

```text
如果 AFC=01:
    TS payload offset = 4
    TS payload length = 188 - 4 = 184

如果 AFC=11:
    TS payload offset = 4 + 1 + adaptation_field_length
    TS payload length = 188 - payload_offset

如果 AFC=10:
    没有 TS payload
```

当前样本几个真实例子：

| 包 | AFC | adaptation_field_length | payload_offset | TS payload length | 净荷内容 |
|---|---:|---:|---:|---:|---|
| packet 0 PAT | `01` | 无 | 4 | 184 | `pointer_field + PAT section + stuffing` |
| packet 1 PMT | `01` | 无 | 4 | 184 | `pointer_field + PMT section + stuffing` |
| packet 2 video | `11` | 7 | 12 | 176 | `video PES 起点` |
| packet 62 private audio | `01` | 无 | 4 | 184 | `private audio PES 起点` |

#### PAT 包的 TS 净荷

packet 0：

```text
47 40 00 10 00 00 B0 0D 00 01 C1 00 00 00 01 F0 00 2A B1 04 B2 FF FF ...
            ^^
            TS payload 从 byte 4 开始
```

TS payload 拆开：

```text
00                         pointer_field
00 B0 0D ... 2A B1 04 B2   PAT section
FF FF ...                  stuffing，填满 188 字节
```

这里 TS 净荷不是 PES，而是 PSI section。

#### PMT 包的 TS 净荷

packet 1：

```text
47 50 00 10 00 02 B0 73 ... 24 ... 06 ... CRC FF FF ...
            ^^
            TS payload 从 byte 4 开始
```

TS payload 拆开：

```text
00                         pointer_field
02 B0 73 ... 89 13 68 39   PMT section
FF FF ...                  stuffing
```

PMT section 里声明两条 ES：

```text
0x24 -> PID 0x0100 HEVC
0x06 -> PID 0x0101 private data
```

#### 视频包的 TS 净荷

packet 2：

```text
47 41 00 30 07 10 00 00 00 00 7E 00 00 00 01 E0 25 19 ...
                                    ^^
                                    TS payload 从 byte 12 开始
```

前 12 字节：

```text
47 41 00 30                  TS header
07 10 00 00 00 00 7E 00      adaptation field，包含 PCR
```

TS 净荷：

```text
00 00 01 E0 25 19 88 C0 0A ...  # PES packet 起点
```

这个 TS 净荷里仍然包含 PES header。继续去掉 PES header 后，才是视频 ES：

```text
00 00 00 01 40 ...  # HEVC VPS
00 00 00 01 42 ...  # HEVC SPS
00 00 00 01 44 ...  # HEVC PPS
...
```

所以视频包里有两次“剥壳”：

```text
TS packet -> TS payload -> PES packet -> PES payload(HEVC ES)
```

#### 私有音频包的 TS 净荷

packet 62：

```text
47 41 01 10 00 00 01 C0 01 34 88 80 05 21 00 01 46 51 FF F1 ...
            ^^
            TS payload 从 byte 4 开始
```

TS 净荷：

```text
00 00 01 C0 01 34 88 80 05 21 00 01 46 51 FF F1 6C 40 ...
```

PES header：

```text
00 00 01 C0 01 34 88 80 05 21 00 01 46 51
```

PES 净荷 / 私有音频 ES：

```text
FF F1 6C 40 25 9F FC ...
```

注意：PMT 把 PID `0x0101` 声明为 `0x06 private data`，所以播放器不应该强行按标准 AAC 解码这段净荷。

#### 净荷解析的最小伪代码

```c
uint8_t *payload = NULL;
int payload_len = 0;
int off = 4;

if (afc == 1) {
    payload = pkt + 4;
    payload_len = 184;
} else if (afc == 3) {
    int afl = pkt[4];
    off = 4 + 1 + afl;
    if (off < 188) {
        payload = pkt + off;
        payload_len = 188 - off;
    }
} else if (afc == 2) {
    payload = NULL;
    payload_len = 0;
}

if (payload && pid == 0x0000) parse_pat(payload, payload_len, pusi);
if (payload && pid == 0x1000) parse_pmt(payload, payload_len, pusi);
if (payload && pid == 0x0100) push_pes(video_buf, payload, payload_len, pusi);
if (payload && pid == 0x0101) push_pes(private_audio_buf, payload, payload_len, pusi);
```

关键原则：

- 先用 AFC 找 TS 净荷起点。
- 再用 PID 判断净荷类型。
- PAT/PMT 净荷要先处理 `pointer_field`。
- PES 净荷要按 PID 拼接，PUSI=1 才是新 PES 起点。
- PES payload 不是从 TS payload 第一个字节直接开始，中间还有 PES header。
### 7.4 PSI 载荷：PAT/PMT

PAT 和 PMT 属于 PSI section。PUSI=1 时，payload 第一个字节是 `pointer_field`。

PAT 示例：

```text
47 40 00 10 00 00 B0 0D ...
            ^^ pointer_field = 0
               ^^ table_id = 0x00 (PAT)
```

PMT 示例：

```text
47 50 00 10 00 02 B0 73 ...
            ^^ pointer_field = 0
               ^^ table_id = 0x02 (PMT)
```

`pointer_field=0` 表示 section 紧跟在 pointer_field 后面。如果 pointer_field 不是 0，则要先跳过指定字节数再读 section。

PSI payload 解析流程：

```c
payload = pkt + payload_offset;
pointer_field = payload[0];
section = payload + 1 + pointer_field;
table_id = section[0];
section_length = ((section[1] & 0x0F) << 8) | section[2];
section_total_size = 3 + section_length;
```

### 7.5 PES 载荷：视频和私有音频

视频和私有音频 PID 的 payload 是 PES 数据。PUSI=1 表示一个新的 PES 从当前 payload 开始。

视频 PES 示例：

```text
00 00 01 E0 25 19 88 C0 0A 31 00 01 46 51 11 00 01 00 01 ...
```

私有音频 PES 示例：

```text
00 00 01 C0 01 34 88 80 05 21 00 01 46 51 FF F1 6C 40 ...
```

PES start code：

```text
00 00 01       start_code_prefix
E0 / C0        stream_id
```

PES payload 分两层：

```text
PES header: start code、stream_id、length、flags、PTS/DTS 等
ES payload: HEVC NAL 或私有音频数据
```

### 7.6 PUSI=0 的载荷

PUSI=0 表示当前 payload 不是新 PES/section 的起点，而是上一段数据的延续。

视频大 PES 通常跨多个 TS packet：

```text
packet N:   PUSI=1, payload = PES header + HEVC ES 开头
packet N+1: PUSI=0, payload = HEVC ES continuation
packet N+2: PUSI=0, payload = HEVC ES continuation
...
```

解析器必须按 PID 把 payload 拼起来：

```c
if (pid == video_pid) {
    if (pusi) start_new_pes();
    append_payload_to_current_pes(payload, payload_len);
}
```

不能看到 PUSI=0 就当成新帧；它只是当前 PES 的后续片段。

### 7.7 188 字节固定长度和 stuffing

TS packet 固定 188 字节，但 PES/PSI 数据长度不一定刚好填满。所以 TS 用 adaptation field 里的 stuffing 补齐。

常见情况：

```text
payload 数据正好填满: AFC=01，无 stuffing
payload 数据不够:     AFC=11，adaptation field 里放 stuffing
需要 PCR:             AFC=11，adaptation field 里放 PCR，必要时再 stuffing
```

stuffing 字节通常是 `0xFF`。解析 payload 时不要把 stuffing 当成 PES/ES 数据；只要按 adaptation_field_length 跳过，payload 起点就是干净的。
## 8. 用 WinHex 定位当前样本

当前 TS packet 固定 188 字节，所以第 `N` 个 packet 的文件偏移是：

```text
packet_offset = N * 188
             = N * 0xBC
```

在 WinHex 里可以用两种方式定位：

- `Ctrl+G`：跳转到指定十六进制偏移。
- `Ctrl+F`：搜索十六进制字节串，例如 `00 00 01 E0`。

建议 WinHex 里用十六进制偏移查看。下面偏移都按文件开头从 0 计算。

### 8.1 常用定位点

| 目标 | packet index | packet 偏移 | 目标字段偏移 | WinHex 操作 |
|---|---:|---:|---:|---|
| 第一个 PAT packet | 0 | `0x00000000` | PAT section: `0x00000005` | Ctrl+G `0` 或 `5` |
| 第一个 PMT packet | 1 | `0x000000BC` | PMT section: `0x000000C1` | Ctrl+G `BC` 或 `C1` |
| 第一个 video packet | 2 | `0x00000178` | video PES: `0x00000184` | Ctrl+G `178` 或 `184` |
| 第一个 private audio packet | 62 | `0x00002D88` | audio PES: `0x00002D8C` | Ctrl+G `2D88` 或 `2D8C` |

为什么这些偏移成立：

```text
packet 0  offset = 0  * 188 = 0x00000000
packet 1  offset = 1  * 188 = 0x000000BC
packet 2  offset = 2  * 188 = 0x00000178
packet 62 offset = 62 * 188 = 0x00002D88
```

### 8.2 WinHex 验证 PAT

跳到 `0x00000000`，应看到：

```text
47 40 00 10 00 00 B0 0D 00 01 C1 00 00 00 01 F0 00 2A B1 04 B2
```

逐步看：

```text
0x00000000: 47             TS sync byte
0x00000001: 40             PUSI=1, PID 高位
0x00000002: 00             PID 低位 -> PID=0x0000
0x00000003: 10             AFC=01, CC=0
0x00000004: 00             pointer_field=0
0x00000005: 00             PAT table_id=0x00
0x00000006: B0
0x00000007: 0D             section_length=13
0x00000011: F0
0x00000012: 00             PMT PID=0x1000
```

WinHex 搜索方式：

```text
Ctrl+F hex: 00 B0 0D 00 01 C1 00 00 00 01 F0 00
```

这个特征是 PAT section 的主体。

### 8.3 WinHex 验证 PMT

跳到 `0x000000BC`，应看到：

```text
47 50 00 10 00 02 B0 73 00 01 C1 00 00 E1 00 F0 00 24 E1 00 F0 5C ... 06 E1 01 F0 00
```

关键偏移：

```text
0x000000BC: 47             TS sync byte
0x000000BD: 50
0x000000BE: 00             PID=0x1000
0x000000BF: 10             AFC=01, CC=0
0x000000C0: 00             pointer_field=0
0x000000C1: 02             PMT table_id=0x02
0x000000C2: B0
0x000000C3: 73             section_length=115
0x000000C9: E1
0x000000CA: 00             PCR_PID=0x0100
0x000000CD: 24             stream_type=0x24 HEVC
0x000000CE: E1
0x000000CF: 00             video elementary_PID=0x0100
```

PMT 里音频 private stream 的位置可以用搜索找：

```text
Ctrl+F hex: 06 E1 01 F0 00
```

含义：

```text
06             stream_type=0x06 private data
E1 01          elementary_PID=0x0101
F0 00          ES_info_length=0
```

### 8.4 WinHex 验证第一个视频 PES

跳到 packet 2：

```text
Ctrl+G: 178
```

应看到：

```text
47 41 00 30 07 10 00 00 00 00 7E 00 00 00 01 E0 25 19 ...
```

先拆 TS header 和 adaptation field：

```text
0x00000178: 47             sync byte
0x00000179: 41
0x0000017A: 00             PID=0x0100
0x0000017B: 30             AFC=11, CC=0
0x0000017C: 07             adaptation_field_length=7
0x0000017D: 10             PCR_flag=1
0x0000017E: 00 00 00 00 7E 00  PCR 6 bytes
```

payload 起点计算：

```text
packet offset = 0x178
payload_offset_in_packet = 4 + 1 + 7 = 12 = 0x0C
file offset = 0x178 + 0x0C = 0x184
```

跳到 `0x00000184`，应看到视频 PES：

```text
00 00 01 E0 25 19 88 C0 0A 31 00 01 46 51 11 00 01 00 01 ...
```

继续往后可看到 HEVC 起始码：

```text
00 00 00 01 40 ...  # VPS
00 00 00 01 42 ...  # SPS
00 00 00 01 44 ...  # PPS
```

WinHex 搜索方式：

```text
Ctrl+F hex: 00 00 01 E0 25 19
Ctrl+F hex: 00 00 00 01 40
```

### 8.5 WinHex 验证第一个私有音频 PES

跳到 packet 62：

```text
Ctrl+G: 2D88
```

应看到：

```text
47 41 01 10 00 00 01 C0 01 34 88 80 05 21 00 01 46 51 FF F1 6C 40 ...
```

字段拆解：

```text
0x00002D88: 47             sync byte
0x00002D89: 41
0x00002D8A: 01             PID=0x0101
0x00002D8B: 10             AFC=01, CC=0
0x00002D8C: 00 00 01 C0    private audio PES start
0x00002D90: 01 34          PES_packet_length=308
0x00002D92: 88             flags1
0x00002D93: 80             flags2，只有 PTS
0x00002D94: 05             PES_header_data_length=5
0x00002D95: 21 00 01 46 51 PTS
0x00002D9A: FF F1 6C 40 ... 私有音频 payload 起点
```

注意：虽然这里能看到 `FF F1`，PMT 仍然把 PID `0x0101` 声明成 `0x06 private data`。这是当前学习样本的保守封装策略。

WinHex 搜索方式：

```text
Ctrl+F hex: 00 00 01 C0 01 34
Ctrl+F hex: FF F1 6C 40
```

### 8.6 WinHex 手工检查一个 packet 是否对齐

任意跳到一个你认为是 TS packet 起点的位置，检查：

```text
offset + 0x00 应为 47
offset + 0xBC 应为下一包的 47
offset + 0x178 应为下下包的 47
```

因为 `0xBC = 188`。

如果每隔 `0xBC` 都能看到 `47`，说明你站在 TS packet 边界上。如果不是，可能是跳到了 packet 内部。

### 8.7 常用搜索字节串

| 搜索目标 | WinHex hex 搜索串 |
|---|---|
| TS packet sync | `47`，但太常见，不建议单独搜 |
| PAT section | `00 B0 0D 00 01 C1 00 00` |
| PMT section | `02 B0 73 00 01 C1 00 00` |
| HEVC video PES | `00 00 01 E0` |
| private audio PES | `00 00 01 C0` |
| HEVC VPS | `00 00 00 01 40` |
| HEVC SPS | `00 00 00 01 42` |
| HEVC PPS | `00 00 00 01 44` |
| private audio payload | `FF F1 6C 40` |
| PMT private audio entry | `06 E1 01 F0 00` |
## 9. PAT 详解

PAT 在 PID `0x0000`。当前第 0 个 TS packet 就是 PAT：

```text
47 40 00 10 00 00 B0 0D 00 01 C1 00 00 00 01 F0 00 2A B1 04 B2 ...
```

拆开：

```text
47             sync_byte
40 00          PUSI=1, PID=0x0000
10             AFC=01(payload only), CC=0
00             pointer_field=0
00             table_id=0x00 (PAT)
B0 0D          section_length=13
00 01          transport_stream_id=1
C1             version=0, current_next=1
00             section_number=0
00             last_section_number=0
00 01          program_number=1
F0 00          PMT PID=0x1000
2A B1 04 B2    CRC32
```

PAT 的作用：告诉播放器 program 1 的 PMT 在 PID `0x1000`。

## 10. PMT 详解

PMT 在 PID `0x1000`。当前第 1 个 TS packet 是 PMT：

```text
47 50 00 10 00 02 B0 73 00 01 C1 00 00 E1 00 F0 00
24 E1 00 F0 5C ... 06 E1 01 F0 00 89 13 68 39
```

TS header：

```text
47             sync_byte
50 00          PUSI=1, PID=0x1000
10             AFC=01(payload only), CC=0
00             pointer_field=0
```

PMT section 关键字段：

```text
02             table_id=0x02 (PMT)
B0 73          section_length=115
00 01          program_number=1
C1             version=0, current_next=1
00             section_number=0
00             last_section_number=0
E1 00          PCR_PID=0x0100
F0 00          program_info_length=0
```

第一条 elementary stream：

```text
24             stream_type=0x24 (HEVC/H.265)
E1 00          elementary_PID=0x0100
F0 5C          ES_info_length=92
05 5A ...      HEVC descriptor / 配置描述信息
```

第二条 elementary stream：

```text
06             stream_type=0x06 (PES private data)
E1 01          elementary_PID=0x0101
F0 00          ES_info_length=0
```

PMT 的核心作用：告诉播放器 PID `0x0100` 是 HEVC，PID `0x0101` 是私有数据，PCR 在 PID `0x0100`。

## 11. 视频 TS packet 和 PCR

第 2 个 TS packet 是第一个视频 PES 起点：

```text
47 41 00 30 07 10 00 00 00 00 7E 00
00 00 01 E0 25 19 88 C0 0A 31 00 01 46 51 11 00 01 00 01 ...
```

TS header：

```text
47             sync_byte
41 00          PUSI=1, PID=0x0100
30             AFC=11(adaptation + payload), CC=0
```

adaptation field：

```text
07             adaptation_field_length=7
10             PCR_flag=1
00 00 00 00 7E 00  PCR=0
```

PCR 编码：

```text
PCR_base: 33 bits，90kHz 单位
PCR_ext:  9 bits，27MHz 余数
PCR_27MHz = PCR_base * 300 + PCR_ext
```

当前策略：

```text
video DTS step = 9000 ticks   # 10 fps，90kHz 时钟
PCR = DTS * 300               # 转成 27MHz
PCR max interval = 100ms
```

## 12. 视频 PES 详解

第一个视频 PES 头：

```text
00 00 01 E0 25 19 88 C0 0A 31 00 01 46 51 11 00 01 00 01
```

字段拆解：

```text
00 00 01       PES start code prefix
E0             stream_id=0xE0，视频流
25 19          PES_packet_length=9497
88             flags1，data_alignment_indicator=1
C0             flags2，PTS_DTS_flags=11，表示 PTS+DTS
0A             PES_header_data_length=10
31 00 01 46 51 PTS 字段
11 00 01 00 01 DTS 字段
```

PES header 后面就是 HEVC ES：

```text
00 00 00 01 40 ... VPS
00 00 00 01 42 ... SPS
00 00 00 01 44 ... PPS
00 00 00 01 26/28 ... IDR 或普通 slice
```

注意：源 PS 的视频 stream_id 是 `0xE2`，但进入 TS 后规范化成 `0xE0`。这是因为 TS 中当前只保留一路视频，不再需要用 `E1/E2/E3` 区分多路。

## 13. 私有音频 TS/PES 详解

第一个私有音频 PES 起点在 packet 62：

```text
47 41 01 10
00 00 01 C0 01 34 88 80 05 21 00 01 46 51
FF F1 6C 40 25 9F FC ...
```

TS header：

```text
47             sync_byte
41 01          PUSI=1, PID=0x0101
10             AFC=01(payload only), CC=0
```

PES header：

```text
00 00 01       PES start code prefix
C0             stream_id=0xC0，源 PS 音频 stream_id 保留
01 34          PES_packet_length=308
88             flags1
80             flags2，PTS_DTS_flags=10，只有 PTS
05             PES_header_data_length=5
21 00 01 46 51 PTS 字段
```

PES header 后 payload：

```text
FF F1 6C 40 25 9F FC ...
```

虽然 payload 看起来像 ADTS AAC，但 PMT 中声明为 `0x06 private data`。这是学习封装阶段的保守策略：保留数据，不误导通用播放器按 AAC 解码。

## 14. PTS、DTS、PCR 的关系

TS 常见有三种时钟相关字段：

| 字段 | 所在位置 | 单位 | 作用 |
|---|---|---:|---|
| PCR | TS adaptation field | 27MHz | 建立解码器系统时钟 |
| PTS | PES header | 90kHz | 这一帧什么时候显示 |
| DTS | PES header | 90kHz | 这一帧什么时候解码 |

当前视频策略：

```text
DTS = 0, 9000, 18000, ...
PTS = 源 E2 PTS 相对化 + reorder delay
PCR = DTS * 300
```

为什么需要 DTS：HEVC 可能存在显示顺序和解码顺序不一致。DTS 给解码器稳定的输入节奏，PTS 给播放器显示时间。

当前私有音频策略：

```text
保留 C0 源 PTS 的相对时间轴
PMT 标成 private data
不承诺通用播放器解码
```

## 15. CC 连续计数器

CC 是 4 bit，按 PID 独立维护。

规则：

```text
同一 PID 且 packet 有 payload 时，CC = (上一包 CC + 1) & 0x0F
adaptation-only packet 可以不递增
```

当前样本验证：

```text
CC jumps = 0
```

这说明 TS 包层没有明显丢包或乱序。

## 16. 排查过程与排除依据

这一节记录“为什么能排除某个方向”。排查原则是：每次只改一个变量，生成一个对照 TS，用 PotPlayer 或脚本验证现象是否改变。

### 16.1 现象基线

最初错误现象：

```text
把 E1/E2/E3 全部封进同一个 TS video PID 后：
- PotPlayer 播放会按 GOP 跳帧，约 6 秒跳一次。
- 可能弹出“未知的文件格式 / 在开始播放时发生了问题”。
- 画面只稳定显示 IDR，非 IDR 基本失效。
```

后续用 FFmpeg 验证：

```bash
ffmpeg -i joa00002_seg2.ps -map 0:0 -an -c copy -f mpegts e2.ts
```

结果：

```text
Stream #0:0[0x1e2]: Video: hevc, 512x288, 10 fps
frame=3541 time=00:05:54.00
```

`e2.ts` 能被 PotPlayer 正常播放。这个结果说明：至少 `E2` 单路 HEVC 本身是可播放的，问题不在 HEVC 编码数据整体损坏，而在我们封装/映射方式上。

### 16.2 排除 TS sync byte 和 continuity counter

验证方法：脚本逐包检查每 188 字节的同步字节和同 PID 的 CC 连续性。

检查逻辑：

```text
1. 文件按 188 字节切包。
2. 每包 byte0 必须是 0x47。
3. 对每个 PID 单独维护 last_cc。
4. 只有 AFC=01 或 AFC=11，也就是有 payload 时，CC 才递增。
5. 如果当前 CC != (last_cc + 1) & 0x0F，则记录跳变。
```

伪代码：

```c
for each packet:
    assert pkt[0] == 0x47
    pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
    afc = (pkt[3] >> 4) & 0x03
    cc  = pkt[3] & 0x0F

    if (afc == 1 || afc == 3) {
        expected = (last_cc[pid] + 1) & 0x0F
        if (pid_seen && cc != expected) report_cc_jump()
        last_cc[pid] = cc
    }
```

验证结果：

```text
bad sync = 0
CC jumps = 0
```

排除依据：如果 TS 层有丢包、错位或 CC 跳变，非 IDR 帧可能因为数据缺失而解不出来。但这里 sync 和 CC 都正常，所以“TS 包层丢包/乱序”不是主因。

### 16.3 排除 PAT/PMT CRC 和表结构

验证方法：解析 PAT/PMT section，重新计算 MPEG-2 CRC32，与文件内 CRC 对比。

检查点：

```text
PAT:
- table_id = 0x00
- section_length = 13
- program_number = 1
- PMT PID = 0x1000
- CRC32 匹配

PMT:
- table_id = 0x02
- PCR_PID = 0x0100
- video stream_type = 0x24
- audio/private stream_type = 0x06 或实验版 0x0F
- elementary_PID 正确
- CRC32 匹配
```

真实 PAT 示例：

```text
00 B0 0D 00 01 C1 00 00 00 01 F0 00 2A B1 04 B2
```

真实 PMT 示例关键字段：

```text
02 B0 73 ... E1 00 F0 00 24 E1 00 ... 06 E1 01 F0 00 ... CRC
```

验证结果：

```text
PAT CRC = OK
PMT CRC = OK
PMT 能正确指向 video PID 和 audio/private PID
```

排除依据：如果 PAT/PMT 错，播放器可能找不到节目、找错 PID、或直接无法建流。但 PAT/PMT CRC 和字段都正确，且纯 E2 video-only TS 能正常播放，所以表结构不是跳 GOP 的主因。

### 16.4 排除 PCR 间隔

验证方法：解析 adaptation field 中的 PCR，统计相邻 PCR 的间隔。

PCR 只在视频 PID `0x0100` 上发送：

```text
PMT: PCR_PID = 0x0100
video TS packet: AFC=11, PCR_flag=1
```

PCR 解析：

```text
PCR_base: 33 bits，90kHz
PCR_ext:  9 bits，27MHz remainder
PCR_27MHz = PCR_base * 300 + PCR_ext
```

当前最终样本：

```text
PCR insertions = 3541
max PCR delta = 100ms
```

中间实验也做过更密集 PCR：约 66.7ms 间隔。

排除依据：TS 规范要求 PCR 间隔不能太大，常见建议在 100ms 内。实验中 PCR 已经满足 100ms 内，甚至做过 66.7ms 版本仍然跳 GOP。因此“PCR 太稀疏导致解码器时钟不稳”不是主因。

### 16.5 排除 PTS/DTS 时间戳策略

最初怀疑：源 PS 的视频 PTS 存在 GOP 内重排序或跳变，可能导致播放器丢帧。

观察到的源视频 PTS 特征：

```text
E1/E2/E3 全混时，视频 PES PTS 有 +60930 后 -54360 这类回退。
```

做过的实验：

1. 把视频 PTS 压成固定 33ms 递增。
2. 保留源 PTS，相对化到 0。
3. 增加 DTS，让 DTS 单调递增。
4. 增加 reorder delay，保证同一帧 `DTS <= PTS`。
5. PCR 跟随 DTS。

验证结果：

```text
视频 PES 有 PTS+DTS。
DTS 单调。
PTS-DTS 最小值为正。
PCR 跟随 DTS。
PotPlayer 仍然按 GOP 跳。
```

排除依据：如果问题是简单的 PTS/DTS 非单调或 `PTS < DTS`，补 DTS、修正排序延迟后应该改善。但所有时间戳修正后仍跳，说明主因不是时间戳，而是 ES 参考链被混流破坏。

### 16.6 排除 PES stream_id、PES_packet_length、data_alignment_indicator

做过的封装兼容性实验：

| 实验 | 改动 | 结果 |
|---|---|---|
| `sid_e0` | 视频 PES stream_id 统一改成 `0xE0` | 仍跳 |
| `len0_align` | 视频 PES length 改 0，设置 data_alignment_indicator | 仍跳 |
| `len0_align_pri` | 在上面基础上再设置 PES priority | 仍跳 |

这些实验针对的是播放器 demux 兼容性：

```text
stream_id=E0:     更符合单路 TS 视频常见写法
PES length=0:     视频 PES 在 TS 中常见可为 0
alignment=1:      告诉解复用器 payload 从访问单元边界开始
```

排除依据：这些 PES 头部兼容性改动都没有改变“按 GOP 跳”的现象，所以问题不在 PES 头字段的表层兼容性。

### 16.7 排除 HEVC AUD

怀疑点：有些 HEVC in TS 的 demux/decoder 依赖 AUD（Access Unit Delimiter，NAL type 35）来识别访问单元边界。

做过的实验：

```text
给每个视频 PES 前插入 HEVC AUD:
00 00 00 01 46 01 50
```

验证结果：

```text
每个视频访问单元前都有 NAL type 35。
NAL 分布中 AUD 数量 = video PES 数量。
PotPlayer 仍然按 GOP 跳。
```

排除依据：如果问题是访问单元边界缺失，补 AUD 后应改善。但补 AUD 后仍跳，说明访问单元边界不是主因。

### 16.8 关键证据：三路 HEVC 被混成一路

手写解析和 FFmpeg 都确认 PS 中有三路 HEVC：

```text
0xE1 -> HEVC 子流
0xE2 -> HEVC 子流
0xE3 -> HEVC 子流
```

每路都有自己的 VPS/SPS/PPS/IDR：

```text
E1: IDR 59 次，普通帧约 3487 个
E2: IDR 60 次，普通帧约 3481 个
E3: IDR 60 次，普通帧约 3481 个
```

错误封装把三路混成：

```text
E1 + E2 + E3 -> PID 0x0100
```

播放器看到的是一条“混合 HEVC 流”，其中参考帧来自不同子流，非 IDR 很难正确引用。于是只剩 IDR 能稳定显示。

最终验证：

```text
只取 E2 -> PID 0x0100 -> PotPlayer 正常播放
```

这就是根因闭环。
## 17. 常见错误和本次排查结论

### 13.1 错误：把 E1/E2/E3 合并成一路

错误：

```text
E1 + E2 + E3 -> PID 0x0100
```

现象：PotPlayer 按 GOP 跳帧。

原因：三条 HEVC 参考帧链混在一起，非 IDR 参考关系失效。

正确：

```text
E2 -> PID 0x0100
```

### 13.2 错误：把 C0 直接声明成标准 AAC

错误：

```text
C0 -> PID 0x0101 -> stream_type 0x0F AAC
```

现象：PotPlayer 弹“在开始播放时发生了问题”。

当前学习方案：

```text
C0 -> PID 0x0101 -> stream_type 0x06 private data
```

### 13.3 不是根因的方向

本次已经排除：

- TS sync byte
- CC 连续性
- PAT/PMT CRC
- PCR 间隔
- PTS/DTS 基本关系
- PES stream_id 改 `E0`
- PES length 改 0
- data_alignment_indicator
- HEVC AUD

真正根因是 ES/PID 映射错误。

## 18. 当前 ps2ts.py 默认配置

```python
SELECT_VIDEO_STREAM_ID = 0xE2
INCLUDE_AUDIO = True
AUDIO_AS_PRIVATE = True
```

含义：

- 只选择 `E2` 视频。
- 保留 `C0` 音频/私有数据。
- PMT 中把 `C0` 声明为 `0x06 private data`。

生成命令示例：

```bash
python ps2ts.py joa00002_seg2.ps joa00002_seg2_e2_private_audio.ts
```

## 19. 后续扩展方向

如果目标是学习 TS 结构，当前样本已经足够覆盖：

- PAT/PMT
- PID 映射
- PCR
- PES 分包
- HEVC stream_type
- private data stream_type
- 多路 ES 不能混 PID

如果目标是通用播放器有声播放，还需要单独研究 `C0` 音频：

- 逐帧解析 ADTS header。
- 校验 frame length 是否连续覆盖 payload。
- 检查 profile、sampling_frequency_index、channel_config 是否稳定。
- 排查是否有厂商私有头或坏帧。
- 再决定能否安全声明为 `0x0F AAC`。