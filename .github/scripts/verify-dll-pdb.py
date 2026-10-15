"""Check the PE CodeView identity against its same-build MSF 7 PDB (stdlib only)."""
import math
from pathlib import Path
import struct
import sys
import uuid


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def dll_identity(path):
    data = path.read_bytes()
    pe = u32(data, 0x3C)
    if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE DLL")
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    if magic not in (0x10B, 0x20B):
        raise ValueError("unknown PE optional header")
    debug_rva, debug_size = struct.unpack_from(
        "<II", data, optional + (112 if magic == 0x20B else 96) + 6 * 8)
    sections = optional + struct.unpack_from("<H", data, pe + 20)[0]
    debug_offset = None
    for i in range(struct.unpack_from("<H", data, pe + 6)[0]):
        size, rva, raw_size, raw = struct.unpack_from("<IIII", data, sections + 40 * i + 8)
        if rva <= debug_rva < rva + max(size, raw_size):
            debug_offset = raw + debug_rva - rva
            break
    if debug_offset is None:
        raise ValueError("DLL has no mapped debug directory")
    for offset in range(debug_offset, debug_offset + debug_size, 28):
        if u32(data, offset + 12) != 2:  # IMAGE_DEBUG_TYPE_CODEVIEW
            continue
        size, _, raw = struct.unpack_from("<III", data, offset + 16)
        record = data[raw:raw + size]
        if record[:4] == b"RSDS" and len(record) >= 25:
            return record[4:20], u32(record, 20), record[24:].split(b"\0", 1)[0]
    raise ValueError("DLL has no RSDS PDB identity")


def pdb_identity(path):
    with path.open("rb") as file:
        header = file.read(56)
        if header[:32] != b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0":
            raise ValueError("unsupported PDB container")
        block_size, _, _, directory_size, _, block_map = struct.unpack_from("<6I", header, 32)
        if block_size not in (512, 1024, 2048, 4096, 8192, 16384, 32768):
            raise ValueError("invalid PDB block size")
        directory_blocks = math.ceil(directory_size / block_size)
        file.seek(block_map * block_size)
        blocks = struct.unpack(f"<{directory_blocks}I", file.read(directory_blocks * 4))
        directory = bytearray()
        for block in blocks:
            file.seek(block * block_size)
            directory.extend(file.read(block_size))
        streams = u32(directory, 0)
        if streams < 2 or u32(directory, 8) < 28 or u32(directory, 8) == 0xFFFFFFFF:
            raise ValueError("PDB info stream is absent")
        first_size = u32(directory, 4)
        first_blocks = 0 if first_size == 0xFFFFFFFF else math.ceil(first_size / block_size)
        info_block = u32(directory, 4 + streams * 4 + first_blocks * 4)
        file.seek(info_block * block_size)
        info = file.read(28)
        return info[12:28], u32(info, 8)


dll, pdb = map(Path, sys.argv[1:])
guid, age, name = dll_identity(dll)
if (guid, age) != pdb_identity(pdb) or Path(name.decode("utf-8")).name.lower() != pdb.name.lower():
    raise SystemExit("DLL and PDB GUID/age/name do not match")
print(f"Verified PDB GUID {uuid.UUID(bytes_le=guid)}, age {age}: {pdb.name}")
