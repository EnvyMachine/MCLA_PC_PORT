"""Recover C++ classes and vtables from MSVC RTTI in a decrypted Xbox 360 image.

Usage: python rtti.py <image.pe> <out.txt> [filter]

MSVC RTTI layout (32-bit, big-endian on Xenon):
  vtable[-1]            -> CompleteObjectLocator
  COL  {sig, offset, cdOffset, pTypeDescriptor, pClassHierarchyDescriptor}
  TypeDescriptor {pVFTable, spare, char name[]}    name = ".?AVfoo@rage@@"
  CHD  {sig, attributes, numBaseClasses, pBaseClassArray}
  BCD  {pTypeDescriptor, numContainedBases, PMD(mdisp, pdisp, vdisp), attributes}
"""
import re
import struct
import sys

sys.path.insert(0, __import__("os").path.dirname(__file__))
from ppcimage import Image  # noqa: E402


def demangle(raw):
    """'grcTexture@rage' -> 'rage::grcTexture' (good enough for simple names)."""
    parts = [p for p in raw.split("@") if p]
    return "::".join(reversed(parts))


def main():
    img = Image(sys.argv[1])
    out_path = sys.argv[2]
    flt = sys.argv[3].lower() if len(sys.argv) > 3 else None
    d, base = img.data, img.base
    code = [(va, va + size) for _, va, size in img.code_ranges()]

    def in_code(a):
        return any(lo <= a < hi for lo, hi in code)

    def u32(a):
        return struct.unpack_from(">I", d, a - base)[0]

    # TypeDescriptors: the name string sits 8 bytes into the descriptor.
    tds = {}
    for m in re.finditer(rb"\.\?AV([A-Za-z0-9_@?$]+)@@\x00", d):
        tds[base + m.start() - 8] = demangle(m.group(1).decode())

    # Index every aligned big-endian word -> addresses holding it (data sections only).
    words = {}
    for name, (va, size, flags) in img.sections.items():
        if flags & 0x20 or name in (".pdata", ".reloc"):
            continue
        off = va - base
        for i, w in enumerate(struct.unpack_from(f">{size // 4}I", d, off)):
            words.setdefault(w, []).append(va + i * 4)

    def base_classes(col):
        chd = u32(col + 16)
        n, arr = u32(chd + 8), u32(chd + 12)
        return [tds.get(u32(u32(arr + 4 * i)), "?") for i in range(1, n)]

    rows = []
    for td, cls in tds.items():
        for col_field in words.get(td, []):
            col = col_field - 12
            if u32(col) != 0:  # signature must be 0 on 32-bit
                continue
            offset = u32(col + 4)
            for ref in words.get(col, []):
                vt = ref + 4
                funcs = []
                a = vt
                while a - base + 4 <= len(d) and in_code(u32(a)):
                    funcs.append(u32(a))
                    a += 4
                if funcs:
                    rows.append((cls, offset, vt, funcs, base_classes(col)))

    rows.sort(key=lambda r: (r[0], r[1]))
    with open(out_path, "w") as f:
        for cls, offset, vt, funcs, bases in rows:
            if flt and flt not in cls.lower():
                continue
            sub = f" (subobject +0x{offset:X})" if offset else ""
            f.write(f"{cls}{sub}  vtable 0x{vt:08X}  {len(funcs)} virtuals"
                    f"{'  : ' + ', '.join(bases) if bases else ''}\n")
            for i, fn in enumerate(funcs):
                f.write(f"    [{i:3}] +0x{i * 4:03X}  sub_{fn:08X}\n")
    classes = {r[0] for r in rows}
    print(f"{len(tds)} type descriptors, {len(classes)} classes with vtables, "
          f"{len(rows)} vtables, {sum(len(r[3]) for r in rows)} virtual slots -> {out_path}")


if __name__ == "__main__":
    main()
