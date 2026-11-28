#!/usr/bin/env python3
"""resolve_gadgets.py — Find IRETQ and RET gadgets in ntoskrnl.exe.
Zero external dependencies. Uses only struct + sys (stdlib).

Usage: python resolve_gadgets.py <path_to_ntoskrnl.exe>
Output: gadgets.cfg with RVA values for the mapper.
"""
import sys
import struct

def find_gadgets(path):
    with open(path, "rb") as f:
        data = f.read()

    if data[:2] != b"MZ":
        print("ERROR: not a PE file", file=sys.stderr); sys.exit(1)
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]

    sig = struct.unpack_from("<I", data, e_lfanew)[0]
    if sig != 0x4550:
        print("ERROR: invalid PE signature", file=sys.stderr); sys.exit(1)

    num_sections = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    opt_hdr_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    section_table = e_lfanew + 24 + opt_hdr_size

    text_rva = None
    text_raw_ptr = None
    text_raw_size = None
    text_vsize = None
    for i in range(num_sections):
        off = section_table + i * 40
        name = data[off:off+8].rstrip(b"\x00")
        if name == b".text":
            text_vsize = struct.unpack_from("<I", data, off + 8)[0]
            text_rva = struct.unpack_from("<I", data, off + 12)[0]
            text_raw_size = struct.unpack_from("<I", data, off + 16)[0]
            text_raw_ptr = struct.unpack_from("<I", data, off + 20)[0]
            break

    if text_rva is None:
        print("ERROR: no .text section found", file=sys.stderr); sys.exit(1)

    text_data = data[text_raw_ptr:text_raw_ptr + text_raw_size]

    irets = []
    rets = []
    for i in range(len(text_data)):
        if text_data[i] == 0xC3:
            rets.append(text_rva + i)
        if i + 1 < len(text_data) and text_data[i] == 0x48 and text_data[i+1] == 0xCF:
            irets.append(text_rva + i)

    if not irets:
        print("ERROR: no IRETQ gadget found", file=sys.stderr); sys.exit(1)
    if not rets:
        print("ERROR: no RET gadget found", file=sys.stderr); sys.exit(1)

    print(f"IRET_RVA=0x{irets[0]:X}")
    print(f"RET_RVA=0x{rets[0]:X}")
    print(f"TEXT_RVA=0x{text_rva:X}")
    print(f"TEXT_SIZE=0x{text_vsize:X}")
    print(f"# Found {len(irets)} IRETQ, {len(rets)} RET candidates")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <ntoskrnl.exe>", file=sys.stderr)
        sys.exit(1)
    find_gadgets(sys.argv[1])