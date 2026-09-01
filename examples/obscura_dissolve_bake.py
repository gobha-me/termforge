#!/usr/bin/env python3
"""Bake and verify the committed OBSCURA M0 dissolve frames.

The final frame is a byte-for-byte snapshot of OBSCURA's hold-d0 plate at
commit aa99f30c35e34a3df06ed8a143677b2560557886.  Frames one through seven
lift the plate's transparent index with the same 4x4 Bayer ordering used by
the source baker, two ranks per frame.  The build consumes only the committed
PNGs; this stdlib-only tool is author-time provenance and drift checking.

Generation:
  obscura_dissolve_bake.py --source /path/to/hold-d0.png --out-dir DIR

Verification:
  obscura_dissolve_bake.py --check DIR
"""

import argparse
import hashlib
import os
import struct
import sys
import zlib

WIDTH = 240
HEIGHT = 160
SOURCE_SHA256 = "18c1671831682a5b104678fc8044e3fb39b14956d6033d2b190a3220b1d4dfa3"
SIGNATURE = b"\x89PNG\r\n\x1a\n"
BAYER = (
    (0, 8, 2, 10),
    (12, 4, 14, 6),
    (3, 11, 1, 9),
    (15, 7, 13, 5),
)


def chunks(blob):
    if not blob.startswith(SIGNATURE):
        raise ValueError("PNG signature mismatch")
    offset = len(SIGNATURE)
    while offset < len(blob):
        if offset + 12 > len(blob):
            raise ValueError("truncated PNG chunk")
        length = struct.unpack(">I", blob[offset : offset + 4])[0]
        end = offset + 12 + length
        if end > len(blob):
            raise ValueError("truncated PNG chunk payload")
        kind = blob[offset + 4 : offset + 8]
        data = blob[offset + 8 : offset + 8 + length]
        stored = struct.unpack(">I", blob[offset + 8 + length : end])[0]
        if stored != zlib.crc32(kind + data) & 0xFFFFFFFF:
            raise ValueError(f"bad CRC on {kind!r}")
        yield kind, data
        offset = end


def decode(blob):
    found = {}
    idat = bytearray()
    for kind, data in chunks(blob):
        if kind == b"IDAT":
            idat.extend(data)
        else:
            found[kind] = data

    want_ihdr = struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 3, 0, 0, 0)
    if found.get(b"IHDR") != want_ihdr:
        raise ValueError("plate must be 240x160, indexed, 8-bit, non-interlaced PNG")
    if found.get(b"tRNS") != b"\x00":
        raise ValueError("plate must use palette index 0 as its transparent hole")
    palette = found.get(b"PLTE")
    if palette is None or len(palette) != 15:
        raise ValueError("plate must carry exactly five palette entries")

    try:
        raw = zlib.decompress(bytes(idat))
    except zlib.error as error:
        raise ValueError(f"IDAT will not decompress: {error}") from error
    stride = WIDTH + 1
    if len(raw) != stride * HEIGHT:
        raise ValueError("decompressed scanline size does not match 240x160")

    rows = []
    for y in range(HEIGHT):
        row = raw[y * stride : (y + 1) * stride]
        if row[0] != 0:
            raise ValueError("source snapshot must use deterministic PNG filter 0")
        rows.append(bytes(row[1:]))
    return palette, rows


def png_chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def encode(palette, rows):
    ihdr = struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 3, 0, 0, 0)
    scanlines = b"".join(b"\x00" + row for row in rows)
    return (
        SIGNATURE
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"PLTE", palette)
        + png_chunk(b"tRNS", b"\x00")
        + png_chunk(b"IDAT", zlib.compress(scanlines, 9))
        + png_chunk(b"IEND", b"")
    )


def reveal_rows(source, frame):
    if frame == 8:
        return source
    threshold = frame * 2
    rows = []
    for y, source_row in enumerate(source):
        out = bytearray(WIDTH)
        for x, index in enumerate(source_row):
            if index != 0 and BAYER[y % 4][x % 4] < threshold:
                out[x] = index
        rows.append(bytes(out))
    return rows


def frame_path(directory, frame):
    return os.path.join(directory, f"reveal-{frame:02d}.png")


def generate(source_path, output_dir):
    with open(source_path, "rb") as handle:
        source_blob = handle.read()
    digest = hashlib.sha256(source_blob).hexdigest()
    if digest != SOURCE_SHA256:
        raise ValueError(f"source SHA-256 {digest} != pinned {SOURCE_SHA256}")
    palette, source_rows = decode(source_blob)
    os.makedirs(output_dir, exist_ok=True)
    for frame in range(1, 8):
        path = frame_path(output_dir, frame)
        blob = encode(palette, reveal_rows(source_rows, frame))
        with open(path, "wb") as handle:
            handle.write(blob)
        print(f"obscura-dissolve-bake: wrote {path} ({len(blob)} bytes)")
    final_path = frame_path(output_dir, 8)
    with open(final_path, "wb") as handle:
        handle.write(source_blob)
    print(f"obscura-dissolve-bake: wrote {final_path} ({len(source_blob)} bytes, exact source)")


def check(directory):
    final_path = frame_path(directory, 8)
    with open(final_path, "rb") as handle:
        final_blob = handle.read()
    digest = hashlib.sha256(final_blob).hexdigest()
    if digest != SOURCE_SHA256:
        raise ValueError(f"final frame SHA-256 {digest} != pinned {SOURCE_SHA256}")
    palette, source_rows = decode(final_blob)

    for frame in range(1, 9):
        path = frame_path(directory, frame)
        with open(path, "rb") as handle:
            blob = handle.read()
        got_palette, got_rows = decode(blob)
        if got_palette != palette:
            raise ValueError(f"{path}: palette differs from the canonical final frame")
        expected = reveal_rows(source_rows, frame)
        if got_rows != expected:
            unequal = sum(
                got != want
                for got_row, want_row in zip(got_rows, expected)
                for got, want in zip(got_row, want_row)
            )
            raise ValueError(f"{path}: {unequal} palette indices differ from reveal frame {frame}")
        visible = sum(index != 0 for row in got_rows for index in row)
        print(f"obscura-dissolve-bake: CLEAN {path} ({len(blob)} bytes, {visible} visible pixels)")


def main(argv):
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", metavar="DIR")
    mode.add_argument("--source", metavar="PNG")
    parser.add_argument("--out-dir", metavar="DIR")
    args = parser.parse_args(argv)
    try:
        if args.check is not None:
            if args.out_dir is not None:
                parser.error("--out-dir is only valid with --source")
            check(args.check)
        else:
            if args.out_dir is None:
                parser.error("--source requires --out-dir")
            generate(args.source, args.out_dir)
    except (OSError, ValueError) as error:
        print(f"obscura-dissolve-bake: FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
