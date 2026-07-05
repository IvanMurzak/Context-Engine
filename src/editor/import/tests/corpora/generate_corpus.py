#!/usr/bin/env python3
"""Regenerate the importer determinism + fuzz seed corpus (R-QA-011 / R-SEC-006).

The corpus is a COMMITTED, versioned deliverable: minimized valid + malformed seeds per importer that
the module ctests replay per PR (`import-test_determinism`, `import-test_fuzz_corpus`) — never
open-ended fuzz time. This generator is deterministic (byte-identical output every run) so the
committed files are reproducible; regenerate with:  python generate_corpus.py

Layout: <this dir>/{png,wav,gltf}/<seed>.<ext>
"""
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def write(rel, data):
    path = os.path.join(HERE, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(data)


# --- PNG ---------------------------------------------------------------------------------------
PNG_SIG = bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])


def png_chunk(typ, data):
    body = typ + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def make_png(w, h, color=6, with_srgb=False):
    out = PNG_SIG + png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color, 0, 0, 0))
    if with_srgb:
        out += png_chunk(b"sRGB", b"\x00")
    out += png_chunk(b"IDAT", b"\x08\x1d\x63\x00")
    out += png_chunk(b"IEND", b"")
    return out


write("png/valid_rgba.png", make_png(4, 4, 6))
write("png/valid_gray.png", make_png(2, 2, 0))
write("png/valid_srgb.png", make_png(3, 3, 6, with_srgb=True))
write("png/not_png.png", b"this is definitely not a png file")
write("png/truncated.png", make_png(8, 8, 6)[:20])
_bad = bytearray(make_png(4, 4, 6))
_bad[20] ^= 0xFF  # flip an IHDR-data byte -> CRC mismatch
write("png/bad_crc.png", bytes(_bad))
write("png/zero_dim.png", make_png(0, 4, 6))


# --- WAV ---------------------------------------------------------------------------------------
def make_wav(channels, rate, bits, frames, fmt=1):
    block = channels * (bits // 8)
    data_size = frames * block
    byte_rate = rate * block
    fmt_chunk = struct.pack("<HHIIHH", fmt, channels, rate, byte_rate, block, bits)
    body = (b"WAVE" + b"fmt " + struct.pack("<I", len(fmt_chunk)) + fmt_chunk +
            b"data" + struct.pack("<I", data_size) + b"\x00" * data_size)
    return b"RIFF" + struct.pack("<I", len(body)) + body


write("wav/valid_mono.wav", make_wav(1, 44100, 16, 8))
write("wav/valid_stereo.wav", make_wav(2, 48000, 16, 4))
write("wav/non_pcm.wav", make_wav(1, 44100, 32, 4, fmt=3))  # IEEE float -> unsupported in v1
write("wav/truncated.wav", b"RIFF")
_fmt_only = b"WAVE" + b"fmt " + struct.pack("<I", 16) + struct.pack("<HHIIHH", 1, 1, 44100, 88200, 2, 16)
write("wav/no_data.wav", b"RIFF" + struct.pack("<I", len(_fmt_only)) + _fmt_only)


# --- glTF --------------------------------------------------------------------------------------
_TRI = ('{"asset":{"version":"2.0"},"materials":[{"name":"m0"}],'
        '"accessors":[{"count":3},{"count":3},{"count":3},{"count":3},{"count":3}],'
        '"meshes":[{"name":"Tri","primitives":[{"attributes":'
        '{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":4,"material":0}]}]}')
_UV2 = ('{"asset":{"version":"2.0"},"materials":[{"name":"m0"}],'
        '"accessors":[{"count":3},{"count":3},{"count":3},{"count":3},{"count":3}],'
        '"meshes":[{"name":"Tri","primitives":[{"attributes":'
        '{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2,"TEXCOORD_1":3},"indices":4,"material":0}]}]}')


def make_glb(json_str):
    padded = json_str.encode()
    while len(padded) % 4:
        padded += b" "
    header = b"glTF" + struct.pack("<II", 2, 12 + 8 + len(padded))
    return header + struct.pack("<I", len(padded)) + b"JSON" + padded


write("gltf/valid_tri.gltf", _TRI.encode())
write("gltf/valid_uv2.gltf", _UV2.encode())
write("gltf/unsupported_version.gltf", b'{"asset":{"version":"1.0"}}')
write("gltf/malformed.gltf", b"{ this is not valid json ]")
write("gltf/valid.glb", make_glb(_TRI))

if __name__ == "__main__":
    print("corpus regenerated under", HERE)
