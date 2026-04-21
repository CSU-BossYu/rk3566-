#!/usr/bin/env python3
import re, subprocess, sys

bin_path = sys.argv[1]
sym_addr = int(sys.argv[2], 16)

txt = subprocess.check_output(["readelf", "-lW", bin_path], text=True)

loads = []
for line in txt.splitlines():
    line = line.strip()
    if not line.startswith("LOAD"):
        continue
    # 典型行: LOAD 0x000000 0x00400000 0x00400000 0x12345 0x12345 R E 0x1000
    parts = line.split()
    # readelf -lW 在不同版本下列顺序一致：LOAD Offset VirtAddr PhysAddr FileSiz MemSiz Flags Align
    try:
        off   = int(parts[1], 16)
        vaddr = int(parts[2], 16)
        filesz= int(parts[4], 16)
        memsz = int(parts[5], 16)
        loads.append((off, vaddr, filesz, memsz, line))
    except Exception:
        pass

for off, vaddr, filesz, memsz, raw in loads:
    if vaddr <= sym_addr < vaddr + memsz:
        file_off = sym_addr - vaddr + off
        print(f"[OK] sym=0x{sym_addr:x} in LOAD: {raw}")
        print(f"[OK] file_offset = 0x{file_off:x}")
        sys.exit(0)

print("[ERR] no LOAD segment contains sym address")
sys.exit(1)