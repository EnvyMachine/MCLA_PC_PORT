"""Locate the statically linked D3D library and pointer tables into it.

Prints:
  - the address range of code that calls the Vd* video imports (D3D core)
  - runs of consecutive data words that all point into the D3D code range
    (render/sampler-state setter tables, internal dispatch tables)
  - functions that build PM4 DRAW_INDX / DRAW_INDX_2 headers

Usage: python d3d_tables.py [lo hi]   (hex range of D3D code; defaults below)
"""
import struct
import sys

sys.path.insert(0, __import__("os").path.dirname(__file__))
from ppcimage import Image  # noqa: E402

img = Image(sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("0x") else "extracted/default.pe")
lo = int(sys.argv[-2], 16) if len(sys.argv) >= 3 else 0x823F8000
hi = int(sys.argv[-1], 16) if len(sys.argv) >= 3 else 0x82470000

# 1. Pointer tables in data sections.
print(f"== pointer tables into {lo:08X}-{hi:08X} (>= 6 consecutive entries)")
for name, (va, size, flags) in img.sections.items():
    if flags & 0x20 or name in (".pdata", ".reloc"):
        continue
    words = struct.unpack_from(f">{size // 4}I", img.data, va - img.base)
    run = []
    for i, w in enumerate(words + (0,)):
        if lo <= w < hi and img.function_at(w) is not None and img.function_at(w)[0] == w:
            run.append((va + i * 4, w))
            continue
        if len(run) >= 6:
            print(f"  {name} {run[0][0]:08X}: {len(run)} entries, "
                  f"targets {min(t for _, t in run):08X}-{max(t for _, t in run):08X}")
        run = []

# 2. DRAW packet headers: type-3 opcode 0x22 / 0x36 built as immediates.
print("== code building DRAW_INDX/DRAW_INDX_2 headers")
for name, va, size in img.code_ranges():
    words = struct.unpack_from(f">{size // 4}I", img.data, va - img.base)
    for i, w in enumerate(words):
        op, imm = w >> 26, w & 0xFFFF
        # ori/addi with low half 0x2200/0x2201/0x3600/0x3601 after a lis 0xC0xx
        if op in (24, 14) and imm in (0x2200, 0x2201, 0x3600, 0x3601):
            prev = words[max(0, i - 6):i]
            if any(p >> 26 == 15 and (p & 0xFFFF) >> 8 == 0xC0 for p in prev):
                a = va + i * 4
                f = img.function_at(a)
                print(f"  {a:08X} in {('sub_%08X' % f[0]) if f else '?'} imm {imm:04X}")
