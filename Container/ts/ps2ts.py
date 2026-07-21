#!/usr/bin/env python3
# -*- coding: gbk -*-
"""
PS to TS Converter (MPEG-2 Program Stream -> Transport Stream)
Pure Python, no external dependencies.
Usage: python3 ps2ts.py <input.ps> [output.ts]

Converts PS files to TS by repackaging PES data into 188-byte TS packets
with proper PAT/PMT tables and PCR timing.
"""

import struct
import sys
import os

# ============================================================
# Constants
# ============================================================
TS_SIZE = 188
TS_SYNC = 0x47

PID_PAT   = 0x0000
PID_PMT   = 0x1000
PID_VIDEO = 0x0100
PID_AUDIO = 0x0101
PID_NULL  = 0x1FFF

ST_H265  = 0x24
ST_AAC   = 0x0F  # AAC ADTS (ISO 13818-7)
ST_PRIVATE = 0x06  # PES private data
ST_G711  = 0x90
PROG_NUM = 1

TS_ID    = 0x0001

PES_VIDEO_SET  = {0xE0, 0xE1, 0xE2, 0xE3}
PES_AUDIO_SET  = {0xC0, 0xC1}

# Jooan PS samples carry three interleaved HEVC elementary streams (E1/E2/E3).
# Each is a complete independent stream; mixing them into one PID breaks ref chain.
# Take only one stream.
SELECT_VIDEO_STREAM_ID = 0xE2
INCLUDE_AUDIO = True
AUDIO_AS_PRIVATE = True

# ============================================================
# Audio codec auto-detection
# ============================================================
def detect_audio_codec(es_data: bytes) -> int:
    """Detect audio codec from ES payload."""
    if len(es_data) >= 2:
        if es_data[0] == 0xFF and (es_data[1] & 0xF0) == 0xF0:
            return ST_AAC
    return ST_G711

# ============================================================
# CRC32 (MPEG-2 standard, polynomial 0x04C11DB7)
# ============================================================
_CRC_TABLE = []
for _i in range(256):
    c = _i << 24
    for _j in range(8):
        c = (c << 1) ^ 0x04C11DB7 if c & 0x80000000 else c << 1
    _CRC_TABLE.append(c & 0xFFFFFFFF)

def crc32_mpeg(data: bytes) -> int:
    c = 0xFFFFFFFF
    for b in data:
        c = ((c << 8) ^ _CRC_TABLE[((c >> 24) ^ b) & 0xFF]) & 0xFFFFFFFF
    return c

# ============================================================
# SCR/PTS Decoding
# ============================================================
def decode_scr(pack_payload: bytes) -> int:
    """Decode SCR (42-bit, 27MHz clock) from PS Pack Header."""
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
    return scr_base * 300 + scr_ext

def decode_pts_dts(pes_data: bytes, pts_flag: int, dts_flag: int):
    """Decode PTS/DTS (33-bit, 90kHz) from PES header."""
    pts = None
    dts = None
    hdr_len = pes_data[8] if len(pes_data) > 8 else 0
    offset = 9
    if pts_flag and offset + 5 <= len(pes_data):
        b = pes_data[offset:offset+5]
        pts = ((b[0] & 0x0E) << 29) | (b[1] << 22) | ((b[2] & 0xFE) << 14) | \
              (b[3] << 7) | ((b[4] & 0xFE) >> 1)
        offset += 5
    if dts_flag and offset + 5 <= len(pes_data):
        b = pes_data[offset:offset+5]
        dts = ((b[0] & 0x0E) << 29) | (b[1] << 22) | ((b[2] & 0xFE) << 14) | \
              (b[3] << 7) | ((b[4] & 0xFE) >> 1)
    return pts, dts

def encode_timestamp(ts33: int, prefix: int) -> bytes:
    """Encode a 33-bit PTS/DTS timestamp per ISO 13818-1."""
    if ts33 < 0:
        ts33 = 0
    if ts33 > 0x1FFFFFFFF:
        ts33 &= 0x1FFFFFFFF
    return bytes([
        ((prefix & 0x0F) << 4) | (((ts33 >> 30) & 0x07) << 1) | 0x01,
        (ts33 >> 22) & 0xFF,
        (((ts33 >> 15) & 0x7F) << 1) | 0x01,
        (ts33 >> 7) & 0xFF,
        ((ts33 & 0x7F) << 1) | 0x01
    ])

def encode_pts(pts33: int) -> bytes:
    """Encode a PTS-only timestamp into 5 bytes per ISO 13818-1."""
    return encode_timestamp(pts33, 0x2)

def encode_pts_dts_pts(pts33: int) -> bytes:
    """Encode the PTS field used when both PTS and DTS are present."""
    return encode_timestamp(pts33, 0x3)

def encode_dts(dts33: int) -> bytes:
    """Encode a DTS field used after a PTS field."""
    return encode_timestamp(dts33, 0x1)

def rebuild_pes_timestamps(pes_data: bytes, pts: int, dts: int = None) -> bytes:
    """Rebuild a PES header with clean TS-compatible PTS or PTS+DTS fields."""
    if len(pes_data) < 9:
        return pes_data

    stream_id = pes_data[3]
    old_hdr_len = pes_data[8]
    es_start = min(9 + old_hdr_len, len(pes_data))
    es_data = pes_data[es_start:]

    flags1 = pes_data[6]
    if dts is None:
        flags2 = 0x80
        ts_bytes = encode_pts(pts)
    else:
        flags2 = 0xC0
        ts_bytes = encode_pts_dts_pts(pts) + encode_dts(dts)

    new_hdr_len = len(ts_bytes)
    new_pes_len = 3 + new_hdr_len + len(es_data)
    if new_pes_len > 0xFFFF and stream_id in PES_VIDEO_SET:
        new_pes_len = 0

    out = bytearray()
    out += pes_data[:4]
    out += struct.pack(">H", new_pes_len)
    out.append(flags1)
    out.append(flags2)
    out.append(new_hdr_len)
    out += ts_bytes
    out += es_data
    return bytes(out)

def rewrite_pes_pts(pes_data: bytes, new_pts: int) -> bytes:
    """Rewrite a PES packet as PTS-only, dropping PS-only optional fields."""
    return rebuild_pes_timestamps(pes_data, new_pts, None)
def rebase_pes_bytes(pes_data: bytes, pts_offset: int) -> bytes:
    """Rewrite PTS/DTS bytes in PES header to subtract pts_offset.
    Returns (modified_pes_data, new_pts, new_dts). PTS/DTS `None` if absent."""
    if len(pes_data) < 9:
        return pes_data, None, None
    
    flags2 = pes_data[7]
    pts_flag = (flags2 >> 7) & 1
    dts_flag = (flags2 >> 6) & 1
    hdr_len = pes_data[8]
    
    new_pts = None
    new_dts = None
    off = 9
    
    pts_bytes = b""
    if pts_flag and off + 5 <= len(pes_data):
        b = pes_data[off:off+5]
        pts = ((b[0] & 0x0E) << 29) | (b[1] << 22) | ((b[2] & 0xFE) << 14) | \
              (b[3] << 7) | ((b[4] & 0xFE) >> 1)
        new_pts = max(0, pts - pts_offset)
        pts_bytes = encode_pts(new_pts)
        off += 5
    
    dts_bytes = b""
    if dts_flag and off + 5 <= len(pes_data):
        b = pes_data[off:off+5]
        dts = ((b[0] & 0x0E) << 29) | (b[1] << 22) | ((b[2] & 0xFE) << 14) | \
              (b[3] << 7) | ((b[4] & 0xFE) >> 1)
        new_dts = max(0, dts - pts_offset)
        dts_bytes = encode_pts(new_dts)
    
    if pts_bytes:
        # Reconstruct PES with new PTS/DTS
        before_pts = pes_data[:9]
        after_ts = pes_data[9 + len(pts_bytes) + len(dts_bytes):]
        return before_pts + pts_bytes + dts_bytes + after_ts, new_pts, new_dts
    return pes_data, None, None

# ============================================================
# TS Packet Construction
# ============================================================
def make_ts_header(pid: int, pusl: int, cc: int, has_adapt: int = 0,
                   has_payload: int = 1) -> bytes:
    return bytes([
        TS_SYNC,
        (pusl << 6) | ((pid >> 8) & 0x1F),
        pid & 0xFF,
        ((has_adapt << 5) | (has_payload << 4) | (cc & 0x0F))
    ])

def make_pcr_field(pcr_27mhz: int) -> bytes:
    pcr_base = pcr_27mhz // 300
    pcr_ext  = pcr_27mhz % 300
    return bytes([
        ((pcr_base >> 25) & 0xFF),
        ((pcr_base >> 17) & 0xFF),
        ((pcr_base >> 9)  & 0xFF),
        ((pcr_base >> 1)  & 0xFF),
        (((pcr_base & 1) << 7) | 0x7E | ((pcr_ext >> 8) & 1)),
        (pcr_ext & 0xFF)
    ])

# ============================================================
# PAT (Program Association Table) 锟?ISO/IEC 13818-1 section 2.4.4.3
# ============================================================
def build_pat(cc: int) -> bytes:
    """
    Build one PAT TS packet on PID 0x0000.

    PAT section structure:
      table_id                8 bits   = 0x00
      section_syntax_indicator 1 bit   = 1
      zero                    1 bit    = 0
      reserved                2 bits   = 11
      section_length         12 bits   (calculated)
      transport_stream_id    16 bits   = TS_ID
      reserved                2 bits   = 11
      version_number          5 bits   = 0
      current_next_indicator  1 bit    = 1
      section_number          8 bits   = 0
      last_section_number     8 bits   = 0
      program_number         16 bits   = PROG_NUM
      reserved                3 bits   = 111
      program_map_PID        13 bits   = PID_PMT
      CRC_32                 32 bits
    """
    # Section header: table_id(1B) + section_length(2B placeholder)
    section = bytearray(3)
    section[0] = 0x00  # table_id = PAT
    # section[1:3] will be overwritten with calculated section_length

    # transport_stream_id (2B)
    section += struct.pack(">H", TS_ID)

    # reserved(2)=11, version(5)=0, current_next(1)=1,
    # section_number(1B)=0, last_section_number(1B)=0
    section += bytes([0xC1, 0x00, 0x00])

    # Program loop: program_number(2B)=1, reserved(3)=111, PMT_PID(13)=PID_PMT
    section += struct.pack(">H", PROG_NUM)
    section += struct.pack(">H", 0xE000 | (PID_PMT & 0x1FFF))

    # Calculate section_length (from this point to end of section, including CRC)
    section_len = len(section) - 3 + 4
    section[1] = 0xB0 | ((section_len >> 8) & 0x0F)
    section[2] = section_len & 0xFF

    # CRC covers table_id through last byte before CRC
    crc = crc32_mpeg(bytes(section[:section_len - 4 + 3]))
    section += struct.pack(">I", crc)

    # Wrap in TS packet
    header = make_ts_header(PID_PAT, 1, cc, 0, 1)
    payload = b"\x00" + bytes(section)  # pointer_field=0 (PSI starts immediately)
    if len(header) + len(payload) < TS_SIZE:
        payload += b"\xFF" * (TS_SIZE - len(header) - len(payload))
    return header + payload

# ============================================================
# PMT (Program Map Table) 锟?ISO/IEC 13818-1 section 2.4.4.9
# ============================================================
def build_pmt(cc: int, audio_st: int = None, hevc_desc: bytes = b"") -> bytes:
    """
    Build one PMT TS packet on PID 0x1000.

    PMT section structure:
      table_id                8 bits   = 0x02
      section_syntax_indicator 1 bit   = 1
      zero                    1 bit    = 0
      reserved                2 bits   = 11
      section_length         12 bits   (calculated)
      program_number         16 bits   = PROG_NUM
      reserved                2 bits   = 11
      version_number          5 bits   = 0
      current_next_indicator  1 bit    = 1
      section_number          8 bits   = 0
      last_section_number     8 bits   = 0
      reserved                3 bits   = 111
      PCR_PID                13 bits   = PID_VIDEO
      reserved                4 bits   = 1111
      program_info_length    12 bits   = 0
      stream entries...
      CRC_32                 32 bits
    """
    # Section header: table_id(1B) + section_length(2B placeholder)
    section = bytearray(3)
    section[0] = 0x02  # table_id = PMT
    # section[1:3] will be overwritten with calculated section_length

    # program_number (2B)
    section += struct.pack(">H", PROG_NUM)

    # reserved(2)=11, version(5)=0, current_next(1)=1,
    # section_number(1B)=0, last_section_number(1B)=0
    section += bytes([0xC1, 0x00, 0x00])

    # reserved(3)=111, PCR_PID(13)=PID_VIDEO
    section += struct.pack(">H", 0xE000 | (PID_VIDEO & 0x1FFF))

    # reserved(4)=1111, program_info_length(12)=0
    section += struct.pack(">H", 0xF000)

    # Video stream entry (with optional HEVC descriptor)
    section += bytes([ST_H265])                                # stream_type
    section += struct.pack(">H", 0xE000 | (PID_VIDEO & 0x1FFF)) # elementary_PID
    section += struct.pack(">H", 0xF000 | len(hevc_desc))      # ES_info_length + descriptor
    section += hevc_desc

    # Audio stream entry (optional). Some Jooan PS files expose broken audio
    # parameters to generic demuxers, so video-only is the default learning path.
    if audio_st is not None:
        section += bytes([audio_st])                                # stream_type
        section += struct.pack(">H", 0xE000 | (PID_AUDIO & 0x1FFF)) # elementary_PID
        section += struct.pack(">H", 0xF000)                        # ES_info_length=0

    # Calculate section_length
    section_len = len(section) - 3 + 4
    section[1] = 0xB0 | ((section_len >> 8) & 0x0F)
    section[2] = section_len & 0xFF

    # CRC
    crc = crc32_mpeg(bytes(section[:section_len - 4 + 3]))
    section += struct.pack(">I", crc)

    # Wrap in TS packet
    header = make_ts_header(PID_PMT, 1, cc, 0, 1)
    payload = b"\x00" + bytes(section)
    if len(header) + len(payload) < TS_SIZE:
        payload += b"\xFF" * (TS_SIZE - len(header) - len(payload))
    return header + payload

# ============================================================
# PES -> TS Packetization
# ============================================================
def extract_vps_sps_pps_from_events(data: bytes, events) -> tuple:
    """Extract VPS/SPS/PPS NALUs from first video IDR frame."""
    vps = sps = pps = None
    for _, etype, einfo in events:
        if etype != "VIDEO":
            continue
        pes_data, _, _ = einfo
        hdr_len = pes_data[8]
        es_data = pes_data[9+hdr_len:]
        j = 0
        while j < len(es_data) - 4:
            if es_data[j:j+4] == b'\x00\x00\x00\x01':
                sc = 4
            elif es_data[j:j+3] == b'\x00\x00\x01':
                sc = 3
            else:
                j += 1; continue
            if j+sc >= len(es_data):
                j += sc; continue
            nt = (es_data[j+sc] >> 1) & 0x3F
            # Find next start code
            nal_end = len(es_data)
            for k in range(j+sc+1, len(es_data)-3):
                if es_data[k:k+4] == b'\x00\x00\x00\x01' or es_data[k:k+3] == b'\x00\x00\x01':
                    nal_end = k
                    break
            nal = es_data[j+sc:nal_end]
            if nt == 32 and vps is None:
                vps = nal
            elif nt == 33 and sps is None:
                sps = nal
            elif nt == 34 and pps is None:
                pps = nal
            j = nal_end
        if vps and sps and pps:
            break
    return vps, sps, pps

def build_hevc_descriptor(vps: bytes, sps: bytes, pps: bytes) -> bytes:
    """Build HEVC descriptor tag for PMT stream entry.
    
    Required by ISO 13818-1 when stream_type=0x24 (HEVC).
    Encodes numOfArrays=3 with VPS/SPS/PPS NALUs.
    """
    if not vps or not sps or not pps:
        return b""
    desc = bytes([0x03])  # numOfArrays = 3
    for nal in [vps, sps, pps]:
        nt = (nal[0] >> 1) & 0x3F
        desc += bytes([0x80 | nt])  # array_completeness=1, NAL_unit_type
        desc += struct.pack('>H', 1)  # numNalus = 1
        desc += struct.pack('>H', len(nal))  # nalUnitLength
        desc += nal
    # Wrap in descriptor tag
    tag = 0x05  # HEVC_descriptor
    return bytes([tag, len(desc)]) + desc

# ============================================================
# PES -> TS Packetization
# ============================================================
def strip_pes_extension(pes: bytes, skip: bool = False) -> bytes:
    """Strip PES extension bytes for TS compatibility.
    
    ISO 13818-1: PES_extension_flag (bit 0 of flags2) = 0 in Transport Streams.
    PS sources may have extensions (P-STD buffer, private data, etc.) that must
    be removed before wrapping in TS packets.
    
    Set skip=True to bypass stripping (debugging compatibility issues).
    """
    if skip:
        return pes
    if len(pes) < 9:
        return pes
    flags2 = pes[7]
    if not (flags2 & 0x01):  # No extension
        return pes
    
    hdr_len = pes[8]
    pts_flag = (flags2 >> 7) & 1
    dts_flag = (flags2 >> 6) & 1
    pts_dts_bytes = 5 * (pts_flag + dts_flag)
    ext_bytes = hdr_len - pts_dts_bytes
    
    if ext_bytes <= 0:
        return pes
    
    # Build new PES: remove extension bytes, fix header + length
    new_flags2 = flags2 & 0xFE  # Clear extension flag
    new_hdr_len = hdr_len - ext_bytes
    
    # Calculate new PES_packet_length (bytes after the 6-byte prefix)
    old_len = struct.unpack(">H", pes[4:6])[0]
    if old_len > 0:  # Non-zero 锟?update
        new_pes_len = old_len - ext_bytes
        if new_pes_len < 0:
            new_pes_len = 0
    else:
        new_pes_len = 0  # Already unbounded, keep it
    
    # Update PES_packet_length: old_len - ext_bytes
    old_pes_len = struct.unpack(">H", pes[4:6])[0]
    if old_pes_len > 0:
        new_pes_len = old_pes_len - ext_bytes
    else:
        new_pes_len = 0
    
    new_pes = bytearray()
    new_pes += pes[:4]  # start_code(3) + stream_id(1)
    new_pes += struct.pack(">H", new_pes_len)  # Updated length
    new_pes.append(pes[6])  # flags1 (unchanged)
    new_pes.append(new_flags2)
    new_pes.append(new_hdr_len)
    new_pes += pes[9:9+pts_dts_bytes]  # PTS/DTS
    new_pes += pes[9+hdr_len:]  # ES data after header
    return bytes(new_pes)

def pes_to_ts_packets(pid: int, pes_packet: bytes, cc_start: int,
                       pcr_value: int = 0, insert_pcr: bool = False):
    """Split one PES packet into 188-byte TS packets."""
    packets = []
    cc = cc_start & 0x0F
    # Strip PES extension for TS compatibility
    payload = strip_pes_extension(pes_packet)
    offset = 0
    first = True

    while offset < len(payload):
        remaining = len(payload) - offset
        pusl = 1 if first else 0

        if first and insert_pcr:
            pcr_bytes = make_pcr_field(pcr_value)
            af_payload = bytes([0x10]) + pcr_bytes
            hdr_and_af = 4 + 1 + len(af_payload)
            payload_space = TS_SIZE - hdr_and_af
            chunk = min(payload_space, remaining)
            af_len = len(af_payload)
            if chunk < payload_space:
                stuff_len = payload_space - chunk
                af_len += stuff_len
                af_payload += b"\xFF" * stuff_len
            hdr = make_ts_header(pid, pusl, cc, 1, 1 if chunk > 0 else 0)
            pkt = hdr + bytes([af_len]) + af_payload + payload[offset:offset+chunk]
        else:
            max_payload = TS_SIZE - 4
            chunk = min(max_payload, remaining)
            stuff_len = TS_SIZE - 4 - chunk if chunk < max_payload else 0
            if stuff_len > 0:
                hdr = make_ts_header(pid, pusl, cc, 1, 1)
                af = (bytes([stuff_len - 1, 0x00]) + b"\xFF" * (stuff_len - 2)
                      if stuff_len >= 2 else bytes([0]))
                pkt = hdr + af + payload[offset:offset+chunk]
            else:
                hdr = make_ts_header(pid, pusl, cc, 0, 1)
                pkt = hdr + payload[offset:offset+chunk]

        packets.append(pkt)
        offset += chunk
        cc = (cc + 1) & 0x0F
        first = False

    return packets, cc

# ============================================================
# Main: PS File -> TS File
# ============================================================
def convert_ps_to_ts(ps_path: str, ts_path: str):
    print(f"Reading PS: {ps_path}")
    with open(ps_path, "rb") as f:
        data = f.read()
    print(f"File size: {len(data):,} bytes ({len(data)/1024/1024:.2f} MB)")

    # First pass: scan PS structure
    events = []
    pos = 0
    print("Scanning PS structure...")
    while pos < len(data) - 4:
        if data[pos:pos+3] != b"\x00\x00\x01":
            pos += 1
            continue
        code = data[pos+3]

        if code == 0xBA:  # Pack Header
            if pos + 14 > len(data):
                pos += 4; continue
            stuff_len = data[pos + 13] & 0x07
            pack_hdr_len = 14 + stuff_len
            pack_hdr_len = min(pack_hdr_len, len(data) - pos)
            pack_payload = data[pos+4:pos+pack_hdr_len]
            scr = decode_scr(pack_payload)
            events.append((pos, "PACK", scr))
            pos += pack_hdr_len
        elif code == 0xBB:  # System Header
            if pos + 6 > len(data):
                pos += 4; continue
            sh_len = struct.unpack_from(">H", data, pos+4)[0] + 6
            pos += sh_len
        elif code == 0xBC:  # PSM
            if pos + 6 > len(data):
                pos += 4; continue
            psm_len = struct.unpack_from(">H", data, pos+4)[0] + 6
            pos += psm_len
        elif code in PES_VIDEO_SET:
            if pos + 9 > len(data):
                pos += 4; continue
            pes_len_field = struct.unpack_from(">H", data, pos+4)[0]
            pts_flag = (data[pos+7] >> 7) & 1
            dts_flag = (data[pos+7] >> 6) & 1
            hdr_len = data[pos+8] if pos+8 < len(data) else 0

            if pes_len_field == 0:
                next_pos = data.find(b"\x00\x00\x01", pos + 6)
                if next_pos < 0:
                    next_pos = len(data)
                pes_total_len = next_pos - pos
            else:
                pes_total_len = pes_len_field + 6

            pes_total_len = min(pes_total_len, len(data) - pos)
            pes_data = data[pos:pos+pes_total_len]
            if code == SELECT_VIDEO_STREAM_ID:
                pts, dts = decode_pts_dts(pes_data, pts_flag, dts_flag)
                events.append((pos, "VIDEO", (pes_data, pts, dts)))
            pos += pes_total_len
        elif code in PES_AUDIO_SET:
            if pos + 9 > len(data):
                pos += 4; continue
            pes_len_field = struct.unpack_from(">H", data, pos+4)[0]
            pts_flag = (data[pos+7] >> 7) & 1
            if pes_len_field == 0:
                next_pos = data.find(b"\x00\x00\x01", pos + 6)
                if next_pos < 0:
                    next_pos = len(data)
                pes_total_len = next_pos - pos
            else:
                pes_total_len = pes_len_field + 6
            pes_total_len = min(pes_total_len, len(data) - pos)
            pes_data = data[pos:pos+pes_total_len]
            if INCLUDE_AUDIO:
                pts, _ = decode_pts_dts(pes_data, pts_flag, 0)
                events.append((pos, "AUDIO", (pes_data, pts)))
            pos += pes_total_len
        else:
            pos += 4

    print(f"Scan done: {len(events)} events")
    pack_count = sum(1 for e in events if e[1] == "PACK")
    video_count = sum(1 for e in events if e[1] == "VIDEO")
    audio_count = sum(1 for e in events if e[1] == "AUDIO")
    print(f"  Pack Headers: {pack_count}")
    print(f"  Video PES:    {video_count}")
    print(f"  Audio PES:    {audio_count}")

    # Use independent stream timestamp bases. The Jooan PS stores audio and
    # video in different absolute epochs, but both spans are aligned.
    video_pts_base = 0
    audio_pts_base = 0
    for _, etype, einfo in events:
        if etype == "VIDEO":
            _, pts, _ = einfo
            if pts is not None:
                video_pts_base = pts
                break
    for _, etype, einfo in events:
        if etype == "AUDIO":
            _, pts = einfo
            if pts is not None:
                audio_pts_base = pts
                break
    min_video_pts_minus_dts = 0
    video_index = 0
    for _, etype, einfo in events:
        if etype == "VIDEO":
            _, pts, _ = einfo
            if pts is not None:
                min_video_pts_minus_dts = min(
                    min_video_pts_minus_dts,
                    (pts - video_pts_base) - video_index * 9000
                )
            video_index += 1
    pts_delay = ((-min_video_pts_minus_dts + 8999) // 9000) * 9000

    print(f"  Video PTS base offset: {video_pts_base/90000:.3f}s")
    print(f"  Audio PTS base offset: {audio_pts_base/90000:.3f}s")
    print(f"  PTS reorder delay: {pts_delay/90000:.3f}s")

    rebased_events = []
    for offset, etype, einfo in events:
        if etype == "PACK":
            rebased_events.append((offset, "PACK", einfo))
        elif etype == "AUDIO":
            pes_data, pts = einfo
            new_pts = (max(0, pts - audio_pts_base) + pts_delay) if pts is not None else None
            rebased_events.append((offset, "AUDIO", (pes_data, new_pts)))
        elif etype == "VIDEO":
            pes_data, pts, dts = einfo
            new_pts = (max(0, pts - video_pts_base) + pts_delay) if pts is not None else None
            rebased_events.append((offset, "VIDEO", (pes_data, new_pts, dts)))
        else:
            rebased_events.append((offset, etype, einfo))
    events = rebased_events
    # For this Jooan sample, C0 looks ADTS-like but PotPlayer reports an error
    # when PMT declares it as standard AAC (0x0F). Keep it as private data for
    # TS structure learning and business-side parsing.
    audio_stream_type = ST_PRIVATE if AUDIO_AS_PRIVATE else ST_G711
    if INCLUDE_AUDIO and not AUDIO_AS_PRIVATE:
        for _, etype, einfo in events:
            if etype == "AUDIO":
                pes_data, _ = einfo
                hdr_len = pes_data[8] if len(pes_data) > 8 else 0
                es_data = pes_data[9+hdr_len:]
                if len(es_data) >= 2:
                    audio_stream_type = detect_audio_codec(es_data)
                break
    audio_name = {ST_AAC: "AAC-ADTS", ST_PRIVATE: "private-data", ST_G711: "G.711"}.get(audio_stream_type, f"0x{audio_stream_type:02X}")
    print(f"  Audio codec: {audio_name} (stream_type=0x{audio_stream_type:02X})")
    # Extract VPS/SPS/PPS from first video IDR for HEVC descriptor
    vps, sps, pps = extract_vps_sps_pps_from_events(data, events)
    hevc_descriptor = build_hevc_descriptor(vps, sps, pps)
    if hevc_descriptor:
        print(f"  HEVC descriptor: {len(hevc_descriptor)} bytes")
    else:
        print(f"  HEVC descriptor: NONE (may cause playback issues)")

    # Second pass: generate TS packets
    print(f"\nGenerating TS: {ts_path}")
    ts_packets = []

    pat_cc = 0; pmt_cc = 0; video_cc = 0; audio_cc = 0

    # PCR follows monotonically increasing decode time (DTS). Video PTS keeps
    # the source display timestamps so HEVC GOP reordering remains visible to
    # the decoder instead of being flattened away.
    pcr_clock = 0
    video_dts = 0
    video_dts_step = 9000  # Selected Jooan substream is 10 fps.
    last_pcr_scr = 0; last_pat_scr = 0
    pcr_interval = 60 * 27000    # 60ms @ 27MHz
    psi_interval = 200 * 27000   # 200ms @ 27MHz
    # Initial PAT/PMT
    ts_packets.append(build_pat(pat_cc)); pat_cc = (pat_cc + 1) & 0x0F
    ts_packets.append(build_pmt(pmt_cc, audio_stream_type if INCLUDE_AUDIO else None, hevc_descriptor)); pmt_cc = (pmt_cc + 1) & 0x0F

    pat_count = 1; pmt_count = 1; pcr_count = 0
    total_video_pkts = 0; total_audio_pkts = 0
    
    for idx, (offset, etype, einfo) in enumerate(events):
        if etype == "PACK":
            continue
        elif etype == "VIDEO":
            pes_data, pts, _ = einfo
            dts = video_dts
            if pts is None:
                pts = dts
            pes_data = rebuild_pes_timestamps(pes_data, pts, dts)
            pes_data = pes_data[:3] + b"\xE0" + pes_data[4:]
            pcr_clock = dts * 300
            video_dts += video_dts_step
            insert_pcr = False
            if (pcr_clock - last_pcr_scr) >= pcr_interval or pcr_count == 0:
                insert_pcr = True
                last_pcr_scr = pcr_clock
                pcr_count += 1
            pkts, video_cc = pes_to_ts_packets(
                PID_VIDEO, pes_data, video_cc, pcr_clock, insert_pcr
            )
            ts_packets.extend(pkts)
            total_video_pkts += len(pkts)
            if (pcr_clock - last_pat_scr) >= psi_interval:
                ts_packets.append(build_pat(pat_cc)); pat_cc = (pat_cc + 1) & 0x0F
                ts_packets.append(build_pmt(pmt_cc, audio_stream_type if INCLUDE_AUDIO else None, hevc_descriptor)); pmt_cc = (pmt_cc + 1) & 0x0F
                last_pat_scr = pcr_clock
                pat_count += 1; pmt_count += 1
        elif etype == "AUDIO":
            pes_data, pts = einfo
            if pts is None:
                pts = 0
            pes_data = rebuild_pes_timestamps(pes_data, pts, None)
            pkts, audio_cc = pes_to_ts_packets(PID_AUDIO, pes_data, audio_cc)
            ts_packets.extend(pkts)
            total_audio_pkts += len(pkts)
        if (idx + 1) % 5000 == 0:
            print(f"  Progress: {idx+1}/{len(events)} ({100*(idx+1)/len(events):.0f}%)")

    # Write output
    print(f"\nWriting TS file...")
    with open(ts_path, "wb") as f:
        for pkt in ts_packets:
            f.write(pkt)

    ts_size = len(ts_packets) * TS_SIZE
    ps_size = len(data)
    print(f"\n========== Conversion Complete ==========")
    print(f"PS input:  {ps_size:,} bytes ({ps_size/1024/1024:.2f} MB)")
    print(f"TS output: {ts_size:,} bytes ({ts_size/1024/1024:.2f} MB)")
    print(f"TS packets: {len(ts_packets)}")
    print(f"  Video packets: {total_video_pkts}")
    print(f"  Audio packets: {total_audio_pkts}")
    print(f"  PAT packets:   {pat_count}")
    print(f"  PMT packets:   {pmt_count}")
    print(f"  PCR insertions: {pcr_count}")
    print(f"Overhead: {ts_size-ps_size:,} bytes ({(ts_size/ps_size-1)*100:.1f}% increase)")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    ps_path = sys.argv[1]
    ts_path = sys.argv[2] if len(sys.argv) > 2 else ps_path.replace(".ps", ".ts")
    if not os.path.exists(ps_path):
        print(f"Error: file not found - {ps_path}")
        sys.exit(1)
    convert_ps_to_ts(ps_path, ts_path)