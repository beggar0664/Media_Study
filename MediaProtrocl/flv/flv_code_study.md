# FLV 代码学习摘录

这份文档只放和 FLV 学习最相关的代码片段，不放整份源码。

当前有两条参考线：

- `ps2flv.py`：PS 转 FLV，适合学容器封装流程。
- `joa_rtmp.c`：RTMP/FLV 推流，适合学标准 H.264/AAC 写法。

## 1. 先看结论

标准可播放线：

```text
FLV + H.264/AVC (CodecID=7) + AAC (SoundFormat=10)
```

扩展实践线：

```text
FLV + HEVC/H.265 (CodecID=12 等扩展写法)
```

后者是否能播，取决于播放器实现。

## 2. `ps2flv.py` 里的基础字节构造

### 2.1 生成 U24 / U32 / Timestamp / Header

```python
def make_u24(val: int) -> bytes:
    return bytes([(val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF])

def make_s24(val: int) -> bytes:
    val = val & 0xFFFFFF
    return bytes([(val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF])

def make_u32(val: int) -> bytes:
    return struct.pack(">I", val)

def make_timestamp(ms: int) -> bytes:
    return make_u24(ms & 0xFFFFFF) + bytes([(ms >> 24) & 0xFF])

def make_flv_header(has_audio: bool, has_video: bool) -> bytes:
    flags = 0
    if has_audio:
        flags |= 0x04
    if has_video:
        flags |= 0x01
    return b"FLV" + bytes([0x01, flags]) + make_u32(9)

def make_prev_tag_size(size: int) -> bytes:
    return make_u32(size)

def make_tag_header(tag_type: int, data_size: int, timestamp_ms: int) -> bytes:
    return bytes([tag_type]) + make_u24(data_size) + make_timestamp(timestamp_ms) + make_u24(0)

def make_tag(tag_type: int, data: bytes, timestamp_ms: int) -> bytes:
    hdr = make_tag_header(tag_type, len(data), timestamp_ms)
    tag = hdr + data
    prev = make_prev_tag_size(len(tag))
    return tag + prev
```

这组函数对应的是 FLV 最核心的文件布局：

```text
FLV Header + PreviousTagSize0 + Tag + PreviousTagSize + Tag + ...
```

### 2.2 AMF0 的 `onMetaData`

```python
def amf0_string(s: str) -> bytes:
    b = s.encode("utf-8")
    return bytes([0x02]) + struct.pack(">H", len(b)) + b

def amf0_double(v: float) -> bytes:
    return bytes([0x00]) + struct.pack(">d", v)

def amf0_bool(v: bool) -> bytes:
    return bytes([0x01, 0x01 if v else 0x00])

def amf0_eof() -> bytes:
    return bytes([0x00, 0x00, 0x09])

def amf0_key_value(key: str, value_bytes: bytes) -> bytes:
    kb = key.encode("utf-8")
    return struct.pack(">H", len(kb)) + kb + value_bytes
```

`onMetaData` 的重点不是“写了什么字符串”，而是它如何告诉播放器：

```text
duration / width / height / videocodecid / audiocodecid
```

### 2.3 Script Tag

```python
def build_script_tag(width: int, height: int, duration_s: float,
                     video_codec_id: int, audio_codec_id: int = 10,
                     has_audio: bool = True) -> bytes:
    data = amf0_string("onMetaData")

    items = []
    if duration_s >= 0:
        items.append(amf0_key_value("duration", amf0_double(duration_s)))
    items.append(amf0_key_value("width", amf0_double(width)))
    items.append(amf0_key_value("height", amf0_double(height)))
    items.append(amf0_key_value("videocodecid", amf0_double(video_codec_id)))
    items.append(amf0_key_value("framerate", amf0_double(10.0)))
    if has_audio:
        items.append(amf0_key_value("audiocodecid", amf0_double(audio_codec_id)))
        items.append(amf0_key_value("audiosamplerate", amf0_double(16000.0)))
        items.append(amf0_key_value("stereo", amf0_double(0.0)))

    data += bytes([0x08]) + struct.pack(">I", len(items))
    for it in items:
        data += it
    data += amf0_eof()
    return data
```

学习时要盯住两点：

```text
1. Script Tag 是 TagType=18。
2. `videocodecid` 和 `audiocodecid` 直接影响播放器对后续 Tag 的解码路径。
```

### 2.4 AAC 配置

```python
def build_aac_config(sample_rate_idx: int = 4, channels: int = 1) -> bytes:
    aot = 2
    return struct.pack(">H", (aot << 11) | (sample_rate_idx << 7) | (channels << 3))
```

这就是 AudioSpecificConfig。真正的 AAC Raw Tag 里一般只保留裸音频帧，不再带 ADTS 头。

### 2.5 H.264 / H.265 配置记录

```python
def build_avc_dcr(sps_list: list, pps_list: list, vps_list: list = None) -> bytes:
    if not sps_list or not pps_list:
        return b""

    sps = sps_list[0]
    pps = pps_list[0]

    if vps_list and len(vps_list) > 0:
        vps = vps_list[0]
        data = bytearray()
        data.append(0x01)
        ...
        data.append(0x01)  # numOfArrays = 1 (VPS)
        data.append(0xA0)  # VPS
        ...
        data.append(0xA1)  # SPS
        ...
        data.append(0xA2)  # PPS
        return bytes(data)

    data = bytearray()
    data.append(0x01)
    data.append(sps[1])
    data.append(sps[2])
    data.append(sps[3])
    data.append(0xFF)
    data.append(0xE1)
    data += struct.pack(">H", len(sps))
    data += sps
    data.append(0x01)
    data += struct.pack(">H", len(pps))
    data += pps
    return bytes(data)
```

这里是一个重要分界：

```text
H.264: SPS / PPS
H.265: VPS / SPS / PPS
```

## 3. `ps2flv.py` 的封装主流程

```python
def convert_ps_to_flv(ps_path: str, flv_path: str):
    with open(ps_path, "rb") as f:
        ps_data = f.read()

    events = []
    pos = 0
    vps_nal = b""
    sps_nal = b""
    pps_nal = b""
    is_hevc = False

    while pos < len(ps_data) - 4:
        if ps_data[pos:pos+3] != b"\x00\x00\x01":
            pos += 1
            continue
        code = ps_data[pos+3]

        if code in PES_VIDEO_SET:
            pes_len_field = struct.unpack_from(">H", ps_data, pos+4)[0]
            if pes_len_field == 0:
                next_pos = ps_data.find(b"\x00\x00\x01", pos + 6)
                if next_pos < 0:
                    next_pos = len(ps_data)
                total_len = next_pos - pos
            else:
                total_len = pes_len_field + 6

            pes_data = ps_data[pos:pos+total_len]
            pts_90k = decode_pts(pes_data)

            if not sps_nal or not pps_nal:
                es_off = pes_header_len(pes_data)
                es_data = pes_data[es_off:]
                nals = extract_nal_units(es_data)
                _hevc = any(is_hevc_nal(n) for n in nals if n)
                if _hevc and not is_hevc:
                    is_hevc = _hevc
                for nal in nals:
                    nt = nal_type(nal, is_hevc)
                    if is_hevc:
                        if nt == NAL_HEVC_VPS and not vps_nal:
                            vps_nal = bytes(nal)
                        elif nt == NAL_HEVC_SPS and not sps_nal:
                            sps_nal = bytes(nal)
                        elif nt == NAL_HEVC_PPS and not pps_nal:
                            pps_nal = bytes(nal)
                    else:
                        if nt == NAL_H264_SPS and not sps_nal:
                            sps_nal = bytes(nal)
                        elif nt == NAL_H264_PPS and not pps_nal:
                            pps_nal = bytes(nal)

            if code == SELECT_VIDEO_STREAM_ID:
                events.append((pos, "VIDEO", (pes_data, pts_90k)))
            pos += total_len
            continue

        if code in PES_AUDIO_SET:
            ...
```

看这段代码时，重点是理解它做了三件事：

```text
1. 扫 PS，拆出视频/音频 PES。
2. 找出 SPS/PPS/VPS，先发 sequence header。
3. 把 PES 里的 NALU 改成 FLV 需要的 length-prefixed 格式。
```

### 3.1 写 FLV 头和序列头

```python
output = bytearray()
output += make_flv_header(has_audio, has_video)
output += make_prev_tag_size(0)

avc_dcr = build_avc_dcr([sps_nal], [pps_nal], [vps_nal] if vps_nal else None)
video_codec_id = CODEC_HEVC if is_hevc else CODEC_AVC

script_data = build_script_tag(width, height, duration_s, video_codec_id, audio_codec_id, has_audio)
output += make_tag(TAG_SCRIPT, script_data, 0)

if avc_dcr:
    vsh_data = bytes([(FRAME_KEY << 4) | video_codec_id, AVC_SEQHDR]) + make_s24(0) + avc_dcr
    output += make_tag(TAG_VIDEO, vsh_data, 0)
```

### 3.2 写视频 NALU Tag

```python
for nal in nals:
    nalu_payload += struct.pack(">I", len(nal))
    nalu_payload += nal

vtag_data = bytes([(frame_type << 4) | video_codec_id, AVC_NALU]) + \
            make_s24(comp_time) + bytes(nalu_payload)

output += make_tag(TAG_VIDEO, vtag_data, relative_ms)
```

### 3.3 写 AAC Tag

```python
sf_byte = (CODEC_AAC << 4) | (3 << 2) | (1 << 1) | (channels - 1)
atag_data = bytes([sf_byte, AAC_RAW]) + raw_aac
output += make_tag(TAG_AUDIO, atag_data, relative_ms)
```

## 4. `joa_rtmp.c` 里的标准 RTMP / FLV 写法

### 4.1 写 dump 文件的 FLV 头和 Tag

```c
static int rtmp_dump_write_header(rtmp_context_t *ctx)
{
    if (!ctx->dump_fp || ctx->dump_header_written) {
        return 0;
    }
    unsigned char flags = 0x01;
    if (ctx->config.audio_profile_no >= 0) {
        flags = 0x05;
    }
    unsigned char header[13] = {
        0x46, 0x4C, 0x56, 0x01, flags,
        0x00, 0x00, 0x00, 0x09,
        0x00, 0x00, 0x00, 0x00
    };
    ...
}

static int rtmp_dump_write_tag(rtmp_context_t *ctx, uint8_t tag_type,
                               const unsigned char *data, int size, uint32_t timestamp)
{
    unsigned char tag_header[11];
    tag_header[0] = tag_type;
    tag_header[1] = (size >> 16) & 0xFF;
    tag_header[2] = (size >> 8) & 0xFF;
    tag_header[3] = size & 0xFF;
    tag_header[4] = (timestamp >> 16) & 0xFF;
    tag_header[5] = (timestamp >> 8) & 0xFF;
    tag_header[6] = timestamp & 0xFF;
    tag_header[7] = (timestamp >> 24) & 0xFF;
    tag_header[8] = tag_header[9] = tag_header[10] = 0x00;
    ...
}
```

这段最值得记的是：`PreviousTagSize = 11 + data_size`。

### 4.2 metadata

```c
static int rtmp_build_metadata_body(unsigned char *buf, int buf_size)
{
    p = amf_write_string(p, end, "onMetaData");
    *p++ = AMF_ECMA_ARRAY;
    p = amf_write_u32(p, end, 5);

    p = amf_write_string_raw(p, end, "duration");
    p = amf_write_number(p, end, 0.0);
    p = amf_write_string_raw(p, end, "width");
    p = amf_write_number(p, end, 640.0);
    p = amf_write_string_raw(p, end, "height");
    p = amf_write_number(p, end, 360.0);
    p = amf_write_string_raw(p, end, "videocodecid");
    p = amf_write_number(p, end, 7.0);
    p = amf_write_string_raw(p, end, "framerate");
    p = amf_write_number(p, end, 15.0);
    p = amf_write_string_raw(p, end, "audiocodecid");
    p = amf_write_number(p, end, 10.0);
    ...
}
```

这里把标准 FLV 的路子写得很清楚：

```text
videocodecid = 7
audiocodecid = 10
```

### 4.3 AVC 和 AAC 的 Tag

```c
static int rtmp_send_avc_config(rtmp_context_t *ctx, uint32_t timestamp)
{
    body[pos++] = 0x17; // KeyFrame + AVC
    body[pos++] = 0x00; // AVC sequence header
    body[pos++] = 0x00;
    body[pos++] = 0x00;
    body[pos++] = 0x00;
    ...
}

static int rtmp_send_h264_frame(rtmp_context_t *ctx, const unsigned char *data,
                                int len, int keyframe, uint32_t timestamp)
{
    ...
    int nal_type = nal[0] & 0x1f;
    if (nal_type == 7 || nal_type == 8) {
        p = next ? next : end;
        continue;
    }
    ...
    body[0] = (keyframe ? 0x17 : 0x27);
    body[1] = 0x01;
    body[2] = 0x00;
    body[3] = 0x00;
    body[4] = 0x00;
    ...
}

static int rtmp_send_aac_config(rtmp_context_t *ctx, uint32_t timestamp)
{
    unsigned char soundFormat = 10;  // AAC
    ...
    body[pos++] = (soundFormat << 4) | (soundRate << 2) | (soundSize << 1) | soundType;
    body[pos++] = 0x00;  // AAC sequence header
    body[pos++] = aac_config[0];
    body[pos++] = aac_config[1];
    ...
}

static int rtmp_send_aac_frame(rtmp_context_t *ctx, const unsigned char *data,
                               int len, uint32_t timestamp)
{
    body[pos++] = (soundFormat << 4) | (soundRate << 2) | (soundSize << 1) | soundType;
    body[pos++] = 0x01;  // AAC raw data
    memcpy(body + pos, data, len);
    ...
}
```

### 4.4 启动时为什么它是 H.264，不是 H.265

```c
ctx->video_codec = header.ucCodec;
if (ctx->video_codec != SHM_ENUM_VIDEO_CODEC_TYPE_H264) {
    UTIL_ERR("RTMP only supports H264, current codec=%d\n", ctx->video_codec);
    return -1;
}
```

这句已经把路径定死了。它适合当标准 FLV 参考，不适合拿来学 HEVC-in-FLV。

## 5. 学习顺序建议

1. 先看 `make_flv_header` / `make_tag_header` / `make_tag`。
2. 再看 `build_script_tag`，理解 metadata。
3. 再看 `build_avc_dcr`，区分 H.264 和 H.265 的配置记录。
4. 然后对照 `joa_rtmp.c` 的 `rtmp_send_avc_config` / `rtmp_send_h264_frame`。
5. 最后回到 `flv_study.md`，用 WinHex 看真实样本的字节。

