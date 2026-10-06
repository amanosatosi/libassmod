#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
"""Generate original, deterministic rectangle fonts using only Python's stdlib.

This is optional fixture authoring tooling, not a build/test dependency. The
committed TTFs contain no outlines or data copied from any third-party font.
"""
from pathlib import Path
import struct


def pack(fmt, *values):
    return struct.pack(">" + fmt, *values)


def checksum(data):
    data += bytes(-len(data) % 4)
    return sum(struct.unpack(">" + "I" * (len(data) // 4), data)) & 0xffffffff


def font(filename, family, em, asc, desc, underline, thickness, strike, strike_size,
         coverage, ink_bottom=0):
    # Every visible glyph has the same normalized cell advance (half a cell)
    # and ink rectangle (quarter a cell wide, half a cell above the baseline).
    cell = asc + desc
    advance, width, height = cell // 2, cell // 4, cell // 2
    chars = [32] + sorted(coverage)
    cmap = {ch: i + 1 for i, ch in enumerate(chars)}
    glyphs = [b"", b""]  # .notdef and space
    rect = (pack("hhhhh", 1, 0, ink_bottom, width, ink_bottom + height) + pack("HH", 3, 0) +
            bytes([1] * 4) + pack("hhhh", 0, 0, width, 0) +
            pack("hhhh", ink_bottom, height, 0, -height))
    glyphs += [rect] * len(coverage)
    offsets, glyf = [0], b""
    for glyph in glyphs:
        glyf += glyph + bytes(-len(glyph) % 4)
        offsets.append(len(glyf))
    n = len(glyphs)
    tables = {
        "glyf": glyf,
        "loca": pack("I" * len(offsets), *offsets),
        "hmtx": pack("Hh", advance, 0) * n,
        "head": pack("IIIIHHQQhhhhHHhhh", 0x10000, 0x10000, 0, 0x5f0f3cf5,
                     3, em, 0, 0, 0, ink_bottom, width, ink_bottom + height, 0, 8, 2, 1, 0),
        "hhea": (pack("IhhhH", 0x10000, asc, -desc, 0, advance) +
                 pack("h" * 11, 0, advance - width, width, 1, 0, 0, 0, 0, 0, 0, 0) +
                 pack("H", n)),
        "maxp": pack("IH" + "H" * 13, 0x10000, n,
                     4, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0),
        "post": pack("IihhIIIII", 0x30000, 0, underline, thickness, 0, 0, 0, 0, 0),
        "OS/2": (pack("HhHHH", 0, advance, 400, 5, 0) +
                 pack("h" * 11, 0, 0, 0, 0, 0, 0, 0, 0, strike_size, strike, 0) +
                 bytes(10) + pack("IIII", 1, 0, 0, 0) + b"TEST" +
                 pack("HHHhhhHH", 0x40, min(chars), max(chars), asc, -desc, 0, asc, desc)),
    }
    points = sorted(cmap) + [0xffff]
    segs = len(points)
    power = 1 << (segs.bit_length() - 1)
    sub = (pack("HHHHHHH", 4, 16 + 8 * segs, 0, 2 * segs,
                2 * power, power.bit_length() - 1, 2 * (segs - power)) +
           pack("H" * segs, *points) + pack("H", 0) +
           pack("H" * segs, *points) +
           pack("H" * segs, *[(cmap.get(ch, 0) - ch) & 0xffff for ch in points]) +
           bytes(2 * segs))
    tables["cmap"] = pack("HHHHI", 0, 1, 3, 1, 12) + sub
    names = {0: "Original libass decoration test fixture; ISC license",
             1: family, 2: "Regular", 3: family + "-1", 4: family,
             5: "Version 1.000", 6: family.replace(" ", ""), 16: family, 17: "Regular"}
    strings, records = b"", b""
    for name_id, text in sorted(names.items()):
        encoded = text.encode("utf-16-be")
        records += pack("HHHHHH", 3, 1, 0x409, name_id, len(encoded), len(strings))
        strings += encoded
    tables["name"] = pack("HHH", 0, len(names), 6 + len(records)) + records + strings
    count = len(tables)
    power = 1 << (count.bit_length() - 1)
    header = pack("IHHHH", 0x10000, count, power * 16,
                  power.bit_length() - 1, (count - power) * 16)
    directory, body = b"", b""
    head_offset = 0
    for tag, data in sorted(tables.items()):
        offset = 12 + count * 16 + len(body)
        if tag == "head":
            head_offset = offset
        directory += tag.encode("ascii") + pack("III", checksum(data), offset, len(data))
        body += data + bytes(-len(data) % 4)
    result = bytearray(header + directory + body)
    struct.pack_into(">I", result, head_offset + 8, (0xb1b0afba - checksum(result)) & 0xffffffff)
    Path(__file__).with_name(filename).write_bytes(result)


all_chars = {65, 66, 67, 0x3008, 0x3009}
font("decoration-primary-a.ttf", "Deco Primary A", 2048, 1800, 248, -900, 100, 600, 128, {65, 67})
font("decoration-primary-b.ttf", "Deco Primary B", 1000, 800, 200, -150, 240, 400, 200, {65, 67})
font("decoration-fallback.ttf", "Deco Fallback", 1000, 1000, 500, -100, 400, 40, 300, {66, 0x3008, 0x3009})
font("decoration-complete.ttf", "Deco Complete", 2048, 1800, 248, -900, 100, 600, 128, all_chars)

# Deliberately place the same rectangle ink outside the nominal ascent or
# descent. These probe ruby clearance against shaped geometry, not metrics.
furi_chars = {ord(c) for c in "WM漢字かんじ"}
font("furi-tight-ascent.ttf", "Furi Tight Ascent", 1000, 250, 750,
     -100, 50, 200, 50, furi_chars)
font("furi-tight-descent.ttf", "Furi Tight Descent", 1000, 900, 100,
     -100, 50, 200, 50, furi_chars, ink_bottom=-400)
