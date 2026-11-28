#!/usr/bin/env python3
"""pe_diag.py — Diagnose why signtool rejects rr_exec.sys"""
import struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else "rr_exec.sys"
with open(path, "rb") as f:
    data = f.read()

print(f"File size: {len(data)} bytes")
if data[:2] != b"MZ": print("FATAL: Not PE"); sys.exit(1)

e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
sig = struct.unpack_from("<I", data, e_lfanew)[0]
machine = struct.unpack_from("<H", data, e_lfanew + 4)[0]
num_sec = struct.unpack_from("<H", data, e_lfanew + 6)[0]
opt_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
chars = struct.unpack_from("<H", data, e_lfanew + 22)[0]
magic = struct.unpack_from("<H", data, e_lfanew + 24)[0]
opt = e_lfanew + 24

print(f"PE sig: 0x{sig:X} | Machine: 0x{machine:X} | Sections: {num_sec}")
print(f"Characteristics: 0x{chars:X} | EXECUTABLE_IMAGE: {'YES' if chars & 2 else 'NO ← PROBLEM'}")
print(f"Magic: 0x{magic:X} ({'PE64' if magic==0x20B else 'BAD'})")

if magic == 0x20B:
    sec_align = struct.unpack_from("<I", data, opt+32)[0]
    file_align = struct.unpack_from("<I", data, opt+36)[0]
    size_image = struct.unpack_from("<I", data, opt+56)[0]
    size_headers = struct.unpack_from("<I", data, opt+60)[0]
    checksum = struct.unpack_from("<I", data, opt+64)[0]
    subsystem = struct.unpack_from("<H", data, opt+68)[0]
    num_rva = struct.unpack_from("<I", data, opt+108)[0]
    
    print(f"Subsystem: {subsystem} (NATIVE=1)")
    print(f"CheckSum: 0x{checksum:X} {'← ZERO' if checksum==0 else ''}")
    print(f"SizeOfImage: 0x{size_image:X} | SizeOfHeaders: 0x{size_headers:X}")
    print(f"SectionAlignment: 0x{sec_align:X} | FileAlignment: 0x{file_align:X}")
    print(f"NumberOfRvaAndSizes: {num_rva}")
    
    if file_align < 512 or (file_align & (file_align-1)):
        print(f"← ERROR: Invalid FileAlignment")
    if sec_align < file_align:
        print(f"← ERROR: SectionAlignment < FileAlignment")
    
    sec_table = e_lfanew + 24 + opt_size
    for i in range(num_sec):
        off = sec_table + i*40
        name = data[off:off+8].rstrip(b'\x00').decode('ascii', errors='replace')
        vsize = struct.unpack_from("<I", data, off+8)[0]
        rva = struct.unpack_from("<I", data, off+12)[0]
        raw_sz = struct.unpack_from("<I", data, off+16)[0]
        raw_ptr = struct.unpack_from("<I", data, off+20)[0]
        print(f"  [{i}] {name:8s} RVA=0x{rva:X} VSize=0x{vsize:X} Raw=0x{raw_ptr:X}+0x{raw_sz:X}")
        if raw_ptr + raw_sz > len(data): print(f"      ← PAST EOF!")
        if rva + vsize > size_image: print(f"      ← EXCEEDS SizeOfImage!")
        if raw_sz and raw_sz % file_align: print(f"      ← NOT FILE-ALIGNED!")
    
    hdr_end = sec_table + num_sec * 40
    if size_headers < hdr_end: print(f"← SizeOfHeaders too small! Need >= 0x{hdr_end:X}")

print("Done.")