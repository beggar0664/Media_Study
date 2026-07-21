#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ps2flv.py 閳?PS to FLV Converter
鐏?MPEG-2 Program Stream (.ps) 鏉烆兛璐?Flash Video (.flv)閵?缁?Python, 閺冪姴顦婚柈銊ょ贩鐠ф牓鈧?Usage: python3 ps2flv.py <input.ps> [output.flv]

閺嶇绺惧顔肩磽 (PS 閳?FLV):
  PS:  Pack(SCR) + PES 閸欘垰褰夐梹鍨瘶
  FLV: 9B Header + PreviousTagSize(4B) + Tag(11B Header + Data)
  - Tag 閺冨爼妫块幋宕囨暏閻╃顕В顐ゎ潡 (闂?90kHz 缂佹繂顕?
  - H.264 NALU 閻劑鏆辨惔锕€澧犵紓鈧?(闂?00 00 01 start code)
  - SPS/PPS 閻欘剛鐝?Sequence Header Tag (闂堢偛绁?PES)
  - 閺?PCR/闁倿鍘ら崺?"""

import struct
import sys
import os

# ============================================================
# Constants
# ============================================================
FLV_HEADER_SIZE = 9

TAG_AUDIO  = 0x08
TAG_VIDEO  = 0x09
TAG_SCRIPT = 0x12

FRAME_KEY   = 1  # key frame / IDR
FRAME_INTER = 2  # inter frame

CODEC_AVC  = 7   # H.264
CODEC_HEVC = 12  # H.265
CODEC_AAC  = 10

AVC_SEQHDR  = 0
AVC_NALU    = 1
AVC_EOS     = 2

AAC_SEQHDR  = 0
AAC_RAW     = 1

PES_VIDEO_SET = {0xE0, 0xE1, 0xE2, 0xE3}
PES_AUDIO_SET = {0xC0, 0xC1}
SELECT_VIDEO_STREAM_ID = 0xE2


# ============================================================
# SCR/PTS 鐟欙絿鐖?(濞岃法鏁?ps2ts.py 闁槒绶?
# ============================================================
def decode_scr(pack_payload: bytes) -> int:
    """Decode SCR (42-bit, 27MHz) from PS Pack Header."""
    if len(pack_payload) < 8:
        return 0
    bits = ""
    for b in pack_payload[:8]:
        bits += f"{b:08b}"
    idx = 2
    scr32_30 = int(bits[idx:idx+3], 2); idx += 3
    idx += 1
    scr29_15 = int(bits[idx:idx+15], 2); idx += 15
    idx += 1
    scr14_0  = int(bits[idx:idx+15], 2); idx += 15
    idx += 1
    scr_ext  = int(bits[idx:idx+9], 2)
    scr_base = (scr32_30 << 30) | (scr29_15 << 15) | scr14_0
    return (scr_base << 9) | scr_ext


def decode_pts(pes_data: bytes) -> int:
    """Decode PTS (33-bit, 90kHz) from PES header. Returns 0 if not present."""
    flags2 = pes_data[7] if len(pes_data) > 7 else 0
    pts_dts = (flags2 >> 6) & 0x03
    if pts_dts < 2:
        return 0
    offset = 9
    b = pes_data[offset:offset+5]
    pts = ((b[0] & 0x0E) << 29) | (b[1] << 22) | ((b[2] & 0xFE) << 14) | \
          (b[3] << 7) | ((b[4] & 0xFE) >> 1)
    return pts


def pts_to_ms(pts_90khz: int) -> int:
    """Convert 90kHz PTS to milliseconds."""
    return int(round(pts_90khz / 90.0))


# ============================================================
# NALU 閹绘劕褰?(娴?PS PES 閸栧懍鑵戦幐?H.264/H.265 NAL 閸楁洖鍘?
# ============================================================
def pes_header_len(pes_data: bytes) -> int:
    """Return byte offset of ES data start within PES packet."""
    if len(pes_data) < 9:
        return len(pes_data)
    hdr_len = pes_data[8]
    return 9 + hdr_len


def extract_nal_units(es_data: bytes) -> list:
    """
    Split raw ES data into NAL units by 00 00 01 / 00 00 00 01 start codes.
    Returns list of (bytes), each is one complete NAL (without start code prefix).
    """
    nals = []
    pos = 0
    data_len = len(es_data)
    while pos < data_len - 3:
        # Find next start code
        if es_data[pos] == 0 and es_data[pos+1] == 0:
            if es_data[pos+2] == 1:
                prefix_len = 3
            elif es_data[pos+2] == 0 and pos+3 < data_len and es_data[pos+3] == 1:
                prefix_len = 4
            else:
                pos += 1
                continue

            # This is the start of a NAL. Find the next start code (end of this NAL)
            nal_start = pos + prefix_len
            next_pos = nal_start
            while next_pos < data_len - 3:
                if es_data[next_pos] == 0 and es_data[next_pos+1] == 0:
                    if es_data[next_pos+2] == 1:
                        break
                    elif es_data[next_pos+2] == 0 and next_pos+3 < data_len \
                            and es_data[next_pos+3] == 1:
                        break
                next_pos += 1
            nals.append(es_data[nal_start:next_pos])
            pos = next_pos
        else:
            pos += 1
    return nals


def nal_type(nalu: bytes, is_hevc: bool = False) -> int:
    """Get NAL unit type.
       H.264: byte0 & 0x1F (5-bit type)
       H.265: (byte0 >> 1) & 0x3F (6-bit type)
       Caller passes is_hevc flag from context."""
    if len(nalu) < 1:
        return 0
    if is_hevc:
        return (nalu[0] >> 1) & 0x3F
    return nalu[0] & 0x1F

def is_hevc_nal(nalu: bytes) -> bool:
    """Detect H.265 NALU header.
       H.265 2-byte header: fbit(1) | type(6) | layer_tid(9)
       H.264 1-byte header: fbit(1) | ref(2) | type(5)
       Simple rule: if nal_type >= 16 after HEVC interpretation AND fbit=0, it's HEVC.
       (H.264 NAL types are 0-31, but types 0-15 overlap. Types 16+ are HEVC-only.)"""
    if len(nalu) < 2:
        return False
    fbit = (nalu[0] >> 7) & 1
    hevc_type = (nalu[0] >> 1) & 0x3F
    return fbit == 0 and hevc_type >= 16

# H.264/AVC NAL types
NAL_H264_SLICE = 1
NAL_H264_IDR   = 5
NAL_H264_SPS   = 7
NAL_H264_PPS   = 8
# H.265/HEVC NAL types
NAL_HEVC_VPS  = 32
NAL_HEVC_SPS  = 33
NAL_HEVC_PPS  = 34
NAL_HEVC_IDR  = 19
NAL_HEVC_IDR_N_LP = 20


# ============================================================
# FLV 閸愭瑥鍙嗗銉ュ徔
# ============================================================
def make_u24(val: int) -> bytes:
    """3-byte big-endian unsigned int."""
    return bytes([(val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF])

def make_s24(val: int) -> bytes:
    """3-byte big-endian signed int (composition time)."""
    val = val & 0xFFFFFF
    return bytes([(val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF])

def make_u32(val: int) -> bytes:
    return struct.pack(">I", val)

def make_timestamp(ms: int) -> bytes:
    """Timestamp: 3-byte low + 1-byte extended."""
    return make_u24(ms & 0xFFFFFF) + bytes([(ms >> 24) & 0xFF])

def make_flv_header(has_audio: bool, has_video: bool) -> bytes:
    flags = 0
    if has_audio: flags |= 0x04
    if has_video: flags |= 0x01
    return b"FLV" + bytes([0x01, flags]) + make_u32(9)

def make_prev_tag_size(size: int) -> bytes:
    return make_u32(size)

def make_tag_header(tag_type: int, data_size: int, timestamp_ms: int) -> bytes:
    return bytes([tag_type]) + make_u24(data_size) + make_timestamp(timestamp_ms) + make_u24(0)

def make_tag(tag_type: int, data: bytes, timestamp_ms: int) -> bytes:
    """Return FLV Tag followed by PreviousTagSize, per FLV file order."""
    hdr = make_tag_header(tag_type, len(data), timestamp_ms)
    tag = hdr + data
    prev = make_prev_tag_size(len(tag))
    return tag + prev

# ============================================================
# AMF0 缂傛牜鐖?閳?onMetaData 閺嬪嫰鈧?# ============================================================
def amf0_string(s: str) -> bytes:
    b = s.encode("utf-8")
    return bytes([0x02]) + struct.pack(">H", len(b)) + b

def amf0_double(v: float) -> bytes:
    return bytes([0x00]) + struct.pack(">d", v)

def amf0_bool(v: bool) -> bytes:
    return bytes([0x01, 0x01 if v else 0x00])

def amf0_eof() -> bytes:
    """End-of-object marker for ECMA array."""
    return bytes([0x00, 0x00, 0x09])

def amf0_key_value(key: str, value_bytes: bytes) -> bytes:
    """Encode a key-value pair: string_length(2B) + key_str + value."""
    kb = key.encode("utf-8")
    return struct.pack(">H", len(kb)) + kb + value_bytes

def build_script_tag(width: int, height: int, duration_s: float,
                     video_codec_id: int, audio_codec_id: int = 10,
                     has_audio: bool = True) -> bytes:
    """
    Build Script Tag data (AMF0 onMetaData).
    娑撳秴瀵橀崥?tag header 閸?PreviousTagSize閵?    """
    data = amf0_string("onMetaData")

    # ECMA Array: 0x08 + u32(count) + key-values + end marker
    items = []
    if duration_s >= 0:
        items.append(amf0_key_value("duration", amf0_double(duration_s)))
    if width > 0:
        items.append(amf0_key_value("width", amf0_double(width)))
    if height > 0:
        items.append(amf0_key_value("height", amf0_double(height)))
    items.append(amf0_key_value("videocodecid", amf0_double(video_codec_id)))
    items.append(amf0_key_value("hasVideo", amf0_bool(True)))
    items.append(amf0_key_value("audiocodecid", amf0_double(audio_codec_id)))
    items.append(amf0_key_value("hasAudio", amf0_bool(has_audio)))

    ecma = bytes([0x08]) + make_u32(len(items)) + b"".join(items) + amf0_eof()
    return data + ecma


# ============================================================
# AAC AudioSpecificConfig 閺嬪嫰鈧?(AAC-LC, 闁插洦鐗遍悳? 婢逛即浜?
# ============================================================
# === AudioSpecificConfig from ADTS header ===
ADTS_SR_TABLE = {
    0: 96000, 1: 88200, 2: 64000, 3: 48000, 4: 44100,
    5: 32000, 6: 24000, 7: 22050, 8: 16000, 9: 12000,
    10: 11025, 11: 8000, 12: 7350, 13: None, 14: None, 15: None
}

def adts_header_len(data: bytes) -> int:
    """Get ADTS header length (7 or 9 bytes). Returns 0 if not ADTS."""
    if len(data) < 7:
        return 0
    if data[0] != 0xFF or (data[1] & 0xF0) != 0xF0:
        return 0
    prot = (data[1] >> 0) & 1  # protection_absent
    return 7 if prot else 9

def adts_to_asc(data: bytes) -> tuple:
    """Extract AudioSpecificConfig from ADTS header. Returns (asc_bytes, sample_rate_idx, channels)."""
    if len(data) < 7:
        return (b"", 4, 1)
    aot = ((data[2] >> 6) & 0x03) + 1  # 2=AAC-LC, 5=AAC+SBR(=HE-AAC)
    sr_idx = (data[2] >> 2) & 0x0F
    ch = ((data[2] << 2) & 0x04) | ((data[3] >> 6) & 0x03)
    asc = (aot << 11) | (sr_idx << 7) | (ch << 3)
    return (struct.pack(">H", asc), sr_idx, ch)

def build_aac_config(sample_rate_idx: int = 4, channels: int = 1) -> bytes:
    """Build 2-byte AudioSpecificConfig for AAC-LC."""
    aot = 2
    return struct.pack(">H", (aot << 11) | (sample_rate_idx << 7) | (channels << 3))


# ============================================================
# AVC Decoder Configuration Record (AVCDCR)
# ============================================================
def build_avc_dcr(sps_list: list, pps_list: list, vps_list: list = None) -> bytes:
    """
    Build AVCDecoderConfigurationRecord (H.264) or
    HEVCDecoderConfigurationRecord (H.265) from VPS/SPS/PPS NAL units.
    Compatible with MP4 'avcC' / 'hvcC' box format.
    """
    if not sps_list or not pps_list:
        return b""

    sps = sps_list[0]
    pps = pps_list[0]

    # Detect H.265 by checking NAL header pattern
    if vps_list and len(vps_list) > 0:
        # H.265: build HEVCDecoderConfigurationRecord
        vps = vps_list[0]
        data = bytearray()
        data.append(0x01)  # configurationVersion
        # general_profile_space(2) + tier_flag(1) + profile_idc(5) 閳?from SPS[0] & bit pos
        if len(sps) > 12:
            # Extract from VPS and SPS:
            # SPS byte 0-1: forbidden(1)+type(6)+layer(6)+temporal(3)+reserved(5)
            # SPS byte 1-3: profile_tier_level info
            data.append(sps[1])  # general_profile_idc (simplified)
            data.append(0x00)
            data.append(0x00)    # general_profile_compatibility_flags (4 bytes)
            data.append(0x00)
            data.append(0x00)
            data.append(0x00)
            # general_constraint_indicator_flags (6 bytes, simplified)
            data += bytes([0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
            data.append(sps[12]) # general_level_idc
        else:
            data += bytes([0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])  # fallback
            data += bytes([0x00, 0x00, 0x00, 0x00])  # constraint flags
            data.append(0x5A)  # Level 90 = 3.0 fallback
        data += bytes([0xF0, 0x00, 0xFC, 0xFD])  # min_spatial_seg + parallel + chroma
        data.append(0xF8)  # bit_depth(8) - 8 = 0
        data += bytes([0xF8, 0x00, 0x00, 0x02, 0x00])
        data.append(0x01)  # numOfArrays = 1 (VPS)
        data.append(0xA0)  # completeness|NAL type = VPS(32)
        data += struct.pack(">H", 1)  # numNalus = 1
        data += struct.pack(">H", len(vps))
        data += vps
        # SPS array
        data.append(0x01)  # numOfArrays increment
        data.append(0xA1)  # completeness|NAL type = SPS(33)
        data += struct.pack(">H", 1)
        data += struct.pack(">H", len(sps))
        data += sps
        # PPS array
        data.append(0x01)  # numOfArrays increment
        data.append(0xA2)  # completeness|NAL type = PPS(34)
        data += struct.pack(">H", 1)
        data += struct.pack(">H", len(pps))
        data += pps
        return bytes(data)

    # H.264: build AVCDecoderConfigurationRecord
    data = bytearray()
    data.append(0x01)  # configurationVersion
    if len(sps) > 2:
        data.append(sps[1])  # AVCProfileIndication
        data.append(sps[2])  # profile_compatibility
        data.append(sps[3])  # AVCLevelIndication
    else:
        data += bytes([0x64, 0x00, 0x1F])  # fallback: High Profile, Level 3.1
    data.append(0xFF)  # lengthSizeMinusOne = 3 閳?NALU length = 4 bytes
    data.append(0xE1)  # numOfSequenceParameterSets = 1 (top 3 reserved=111)
    data += struct.pack(">H", len(sps))
    data += sps
    data.append(0x01)  # numOfPictureParameterSets = 1
    data += struct.pack(">H", len(pps))
    data += pps
    return bytes(data)


# ============================================================
# 娑撴槒娴嗛幑?# ============================================================
def convert_ps_to_flv(ps_path: str, flv_path: str):
    print(f"Reading PS: {ps_path}")
    with open(ps_path, "rb") as f:
        ps_data = f.read()
    print(f"File size: {len(ps_data):,} bytes ({len(ps_data)/1024/1024:.2f} MB)")

    # --- 缁楊兛绔撮柆? 閹殿偅寮?PS ---
    events = []  # (pos, type_str, (pes_data, pts_90khz))
    pos = 0

    # 閸忓牊鏁归梿?VPS/SPS/PPS + 鐎硅棄瀹虫妯哄 (娴犲海顑囨稉鈧稉顏囶潒妫版垵鎶氶惃?PES 闁插本褰侀崣?
    vps_nal = b""
    sps_nal = b""
    pps_nal = b""
    width = 0
    height = 0
    is_hevc = False

    print("Scanning PS structure...")
    while pos < len(ps_data) - 4:
        if ps_data[pos:pos+3] != b"\x00\x00\x01":
            pos += 1
            continue
        code = ps_data[pos+3]

        if code == 0xBA:  # Pack Header
            if pos + 14 > len(ps_data): pos += 4; continue
            stuff_len = ps_data[pos + 13] & 0x07
            pack_hdr_len = 14 + stuff_len
            pack_hdr_len = min(pack_hdr_len, len(ps_data) - pos)
            pack_payload = ps_data[pos+4:pos+pack_hdr_len]
            scr = decode_scr(pack_payload)
            events.append((pos, "PACK", (scr,)))
            pos += pack_hdr_len

        elif code == 0xBB:  # System Header
            if pos + 6 > len(ps_data): pos += 4; continue
            sh_len = struct.unpack_from(">H", ps_data, pos+4)[0] + 6
            pos += sh_len

        elif code == 0xBC:  # PSM
            if pos + 6 > len(ps_data): pos += 4; continue
            psm_len = struct.unpack_from(">H", ps_data, pos+4)[0] + 6
            pos += psm_len

        elif code in PES_VIDEO_SET:
            if pos + 9 > len(ps_data): pos += 4; continue
            pes_len_field = struct.unpack_from(">H", ps_data, pos+4)[0]
            if pes_len_field == 0:
                next_pos = ps_data.find(b"\x00\x00\x01", pos + 6)
                if next_pos < 0: next_pos = len(ps_data)
                total_len = next_pos - pos
            else:
                total_len = pes_len_field + 6
            total_len = min(total_len, len(ps_data) - pos)

            pes_data = ps_data[pos:pos+total_len]
            pts_90k = decode_pts(pes_data)

            # 閹绘劕褰?NAL 閸楁洑缍?(婵″倹鐏夋潻妯荤梾閹峰灝鍩?SPS/PPS/VPS)
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

        elif code in PES_AUDIO_SET:
            if pos + 9 > len(ps_data): pos += 4; continue
            pes_len_field = struct.unpack_from(">H", ps_data, pos+4)[0]
            if pes_len_field == 0:
                next_pos = ps_data.find(b"\x00\x00\x01", pos + 6)
                if next_pos < 0: next_pos = len(ps_data)
                total_len = next_pos - pos
            else:
                total_len = pes_len_field + 6
            total_len = min(total_len, len(ps_data) - pos)

            pes_data = ps_data[pos:pos+total_len]
            pts_90k = decode_pts(pes_data)
            events.append((pos, "AUDIO", (pes_data, pts_90k)))
            pos += total_len

        else:
            pos += 4

    has_audio = any(e[1] == "AUDIO" for e in events)
    has_video = any(e[1] == "VIDEO" for e in events)

    print(f"Scan done: {len(events)} events")
    print(f"  Has Video: {has_video}, Has Audio: {has_audio}")
    print(f"  Codec: {'H.265/HEVC' if is_hevc else 'H.264/AVC'}")
    print(f"  VPS: {'found' if vps_nal else 'N/A (H.264)'}")
    print(f"  SPS: {'found' if sps_nal else 'NOT FOUND'}")
    print(f"  PPS: {'found' if pps_nal else 'NOT FOUND'}")

    # --- 閺嬪嫬缂?FLV ---
    print(f"\nGenerating FLV: {flv_path}")
    output = bytearray()
    output += make_flv_header(has_audio, has_video)
    output += make_prev_tag_size(0)

    # Build AVC/HEVC Decoder Configuration Record
    avc_dcr = build_avc_dcr([sps_nal], [pps_nal],
                            [vps_nal] if vps_nal else None)               if sps_nal and pps_nal else b""

    video_codec_id = CODEC_HEVC if is_hevc else CODEC_AVC

    # --- 閹垫儳鍩岀粭顑跨娑擃亣顫嬫０?PTS 娴ｆ粈璐熼弮鍫曟？閸╁搫鍣?---
    first_video_pts = None
    first_audio_pts = None
    for _, etype, einfo in events:
        if etype == "PACK":
            continue
        _, pts = einfo
        if etype == "VIDEO" and first_video_pts is None and pts > 0:
            first_video_pts = pts
        if etype == "AUDIO" and first_audio_pts is None and pts > 0:
            first_audio_pts = pts
        if first_video_pts and first_audio_pts:
            break
    if first_video_pts is None:
        first_video_pts = 0
    if first_audio_pts is None:
        first_audio_pts = 0

    base_pts_90k = min(first_video_pts, first_audio_pts)

    video_pts_min_90k = None
    video_pts_max_90k = None
    for _, etype, einfo in events:
        if etype == "VIDEO":
            _, pts = einfo
            if pts > 0:
                if video_pts_min_90k is None or pts < video_pts_min_90k:
                    video_pts_min_90k = pts
                if video_pts_max_90k is None or pts > video_pts_max_90k:
                    video_pts_max_90k = pts

    duration_s = -1.0
    if video_pts_min_90k is not None and video_pts_max_90k is not None:
        duration_s = (video_pts_max_90k - video_pts_min_90k) / 90000.0

    # --- 閸愭瑥鍙?Script Tag (onMetaData, timestamp=0) ---
    audio_codec_id = 10  # AAC default
    if has_audio:
        # detect audio codec type
        for _, etype, einfo in events:
            if etype == "AUDIO":
                pes_data, _ = einfo
                es_off = pes_header_len(pes_data)
                es_data = pes_data[es_off:]
                hdr_len = adts_header_len(es_data)
                if hdr_len > 0:
                    audio_codec_id = 10  # AAC
                else:
                    audio_codec_id = 2   # MP3 (fallback, actual detection would need deeper check)
                break

    script_data = build_script_tag(width, height, duration_s, video_codec_id, audio_codec_id, has_audio)
    output += make_tag(TAG_SCRIPT, script_data, 0)

    # --- 閸愭瑥鍙?Video AVC Sequence Header Tag (timestamp=0) ---
    if avc_dcr:
        vsh_data = bytes([(FRAME_KEY << 4) | video_codec_id, AVC_SEQHDR]) + \
                   make_s24(0) + avc_dcr
        output += make_tag(TAG_VIDEO, vsh_data, 0)

    # --- 閸愭瑥鍙?Audio AAC Sequence Header Tag (timestamp=0) ---
    # ASC will be populated from first ADTS frame; use fallback if no ADTS available
    # We need to scan ahead for first audio PES to get real ASC
    if has_audio:
        aac_asc_bytes = b""
        aac_sr_idx = 4
        aac_channels = 1
        # find first audio frame for ASC extraction
        for _, etype, einfo in events:
            if etype == "AUDIO":
                pes_data, _ = einfo
                es_off = pes_header_len(pes_data)
                es_data = pes_data[es_off:]
                a_hdr_len = adts_header_len(es_data)
                if a_hdr_len > 0:
                    aac_asc_bytes, aac_sr_idx, aac_channels = adts_to_asc(es_data)
                break
        if not aac_asc_bytes:
            aac_asc_bytes = build_aac_config()
        sf_byte = (CODEC_AAC << 4) | (3 << 2) | (1 << 1) | (aac_channels - 1)
        ash_data = bytes([sf_byte, AAC_SEQHDR]) + aac_asc_bytes
        output += make_tag(TAG_AUDIO, ash_data, 0)
        print(f"  Audio ASC: sr_idx={aac_sr_idx} channels={aac_channels} asc={aac_asc_bytes.hex()}")

    # --- 闁秴宸?events, 閸?Video/Audio Tag ---
    prev_video_pts_90k = None
    prev_audio_pts_90k = None
    seq_header_sent = True  # already sent above

    video_cnt = 0
    audio_cnt = 0
    scr_90k = 0

    for idx, (offset, etype, einfo) in enumerate(events):
        if etype == "PACK":
            scr_27m = einfo[0]
            scr_90k = scr_27m // 300  # convert to 90kHz base
            continue

        pes_data, pts_90k = einfo

        if pts_90k == 0:
            continue

        relative_ms = pts_to_ms(pts_90k - base_pts_90k)
        if relative_ms < 0:
            relative_ms = 0

        if etype == "VIDEO":
            es_off = pes_header_len(pes_data)
            es_data = pes_data[es_off:]
            nals = extract_nal_units(es_data)

            if not nals:
                continue

            # Determine frame type
            first_nal_type = nal_type(nals[0], is_hevc)
            if is_hevc:
                is_key = first_nal_type in (NAL_HEVC_IDR, NAL_HEVC_IDR_N_LP)
            else:
                is_key = first_nal_type == NAL_H264_IDR

            frame_type = FRAME_KEY if is_key else FRAME_INTER

            # CompositionTime (for B-frames / reordering)
            if is_key:
                comp_time = 0
            else:
                # 缁犫偓閸? 閸嬪洩顔曢弮?B 鐢? CompositionTime=0
                comp_time = 0

            # Build NALU payload with length prefix
            nalu_payload = bytearray()
            for nal in nals:
                nalu_payload += struct.pack(">I", len(nal))
                nalu_payload += nal

            vtag_data = bytes([(frame_type << 4) | video_codec_id, AVC_NALU]) + \
                        make_s24(comp_time) + bytes(nalu_payload)

            output += make_tag(TAG_VIDEO, vtag_data, relative_ms)
            video_cnt += 1
            prev_video_pts_90k = pts_90k

        elif etype == "AUDIO":
            es_off = pes_header_len(pes_data)
            es_data = pes_data[es_off:]

            # Check for ADTS AAC header
            a_hdr_len = adts_header_len(es_data)
            if a_hdr_len > 0:
                # AAC with ADTS 閳?strip header, build ASC from first frame
                if not "aac_asc" in dir():
                    asc_bytes, sr_idx, channels = adts_to_asc(es_data)
                raw_aac = es_data[a_hdr_len:]  # strip ADTS header

                # SoundFormat=10(AAC) | Rate(44.1k=3) | Size(16bit=1) | Type
                sf_byte = (CODEC_AAC << 4) | (3 << 2) | (1 << 1) | (channels - 1)
                atag_data = bytes([sf_byte, AAC_RAW]) + raw_aac
            else:
                # Fallback: raw audio (G.711 or unknown), tag as-is
                atag_data = bytes([0xAF, AAC_RAW]) + es_data

            output += make_tag(TAG_AUDIO, atag_data, relative_ms)
            audio_cnt += 1
            prev_audio_pts_90k = pts_90k

        if (idx + 1) % 5000 == 0:
            print(f"  Progress: {idx+1}/{len(events)} ({100*(idx+1)/len(events):.0f}%)")

    # --- 閸愭瑥鍙嗛弬鍥︽ ---
    print(f"\nWriting FLV file...")
    with open(flv_path, "wb") as f:
        f.write(output)

    ps_size = len(ps_data)
    flv_size = len(output)
    print(f"\n========== Conversion Complete ==========")
    print(f"PS input:   {ps_size:,} bytes ({ps_size/1024/1024:.2f} MB)")
    print(f"FLV output: {flv_size:,} bytes ({flv_size/1024/1024:.2f} MB)")
    print(f"  Video Tags: {video_cnt}")
    print(f"  Audio Tags: {audio_cnt}")
    print(f"  Script Tags: 1")
    print(f"Flv structure:")
    print(f"  [0..8]: FLV Header (9B)")
    print(f"  [9..12]: PreviousTagSize0 = 0")
    print(f"  [13..]: Tag#1 = Script (onMetaData)")
    print(f"  ...: Video/Audio tags interleaved")
    print(f"Overhead: {flv_size-ps_size:,} bytes ({(flv_size/ps_size-1)*100:.1f}%)")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    ps_path = sys.argv[1]
    flv_path = sys.argv[2] if len(sys.argv) > 2 else ps_path.replace(".ps", ".flv")
    if not os.path.exists(ps_path):
        print(f"Error: file not found - {ps_path}")
        sys.exit(1)
    convert_ps_to_flv(ps_path, flv_path)
