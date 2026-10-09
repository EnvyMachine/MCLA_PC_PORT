"""Helpers for poking at a decrypted Xbox 360 PE image: sections, .pdata functions,
branch cross-references and disassembly.

Usage:
    python ppcimage.py <image.pe> stats
    python ppcimage.py <image.pe> callers <target_addr>
    python ppcimage.py <image.pe> dis <addr> [count]       # count in instructions
    python ppcimage.py <image.pe> func <addr>              # disassemble the containing function
"""
import bisect
import struct
import sys

BASE = 0x82000000


class Image:
    def __init__(self, path, base=BASE):
        self.data = open(path, "rb").read()
        self.base = base
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        sh = pe + 24 + optsz
        self.sections = {}
        for i in range(nsec):
            name, vsize, vaddr = struct.unpack_from("<8sII", d, sh + i * 40)
            flags = struct.unpack_from("<I", d, sh + i * 40 + 36)[0]
            self.sections[name.rstrip(b"\0").decode()] = (base + vaddr, vsize, flags)
        self.functions = self._parse_pdata()
        self._starts = [f[0] for f in self.functions]

    def _parse_pdata(self):
        start, size, _ = self.sections[".pdata"]
        off = start - self.base
        funcs = []
        for o in range(off, off + size - 7, 8):
            begin, packed = struct.unpack_from(">II", self.data, o)
            if begin == 0:
                continue
            length = ((packed >> 8) & 0x3FFFFF) * 4
            funcs.append((begin, length))
        funcs.sort()
        return funcs

    def code_ranges(self):
        for name, (va, size, flags) in self.sections.items():
            if flags & 0x20:  # IMAGE_SCN_CNT_CODE
                yield name, va, size

    def u32(self, addr):
        return struct.unpack_from(">I", self.data, addr - self.base)[0]

    def function_at(self, addr):
        i = bisect.bisect_right(self._starts, addr) - 1
        if i >= 0:
            begin, length = self.functions[i]
            if begin <= addr < begin + length:
                return begin, length
        return None

    def branch_xrefs(self, target):
        """Direct b/bl (I-form) instructions that reach `target`."""
        hits = []
        for _, va, size in self.code_ranges():
            off = va - self.base
            for o in range(off, off + size - 3, 4):
                ins = struct.unpack_from(">I", self.data, o)[0]
                if ins >> 26 != 18 or ins & 2:  # opcode 18, relative only
                    continue
                disp = ins & 0x03FFFFFC
                if disp & 0x02000000:
                    disp -= 0x04000000
                src = self.base + o
                if src + disp == target:
                    hits.append((src, "bl" if ins & 1 else "b"))
        return hits

    def disasm(self, addr, count):
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
        out = []
        for i in range(count):
            a = addr + i * 4
            raw = self.data[a - self.base: a - self.base + 4]
            ins = next(md.disasm(raw, a), None)
            text = f"{ins.mnemonic:<8} {ins.op_str}" if ins else f".long 0x{raw.hex()}"
            out.append(f"  {a:08X}:  {raw.hex()}  {text}")
        return "\n".join(out)


def main():
    img = Image(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "stats":
        for name, (va, size, flags) in img.sections.items():
            print(f"  {name:<9} 0x{va:08X} 0x{size:07X}")
        total = sum(l for _, l in img.functions)
        print(f".pdata functions: {len(img.functions)}, covering 0x{total:X} bytes")
        for name, va, size in img.code_ranges():
            covered = sum(l for b, l in img.functions if va <= b < va + size)
            print(f"  {name}: {covered / size:.1%} of 0x{size:X} bytes covered by .pdata")
    elif cmd == "callers":
        target = int(sys.argv[3], 16)
        for src, kind in img.branch_xrefs(target):
            f = img.function_at(src)
            where = f"in sub_{f[0]:08X} (size 0x{f[1]:X})" if f else "outside .pdata"
            print(f"  {kind:<2} from 0x{src:08X} {where}")
    elif cmd == "dis":
        addr = int(sys.argv[3], 16)
        print(img.disasm(addr, int(sys.argv[4]) if len(sys.argv) > 4 else 32))
    elif cmd == "func":
        f = img.function_at(int(sys.argv[3], 16))
        if not f:
            print("address not covered by .pdata")
            return
        print(f"sub_{f[0]:08X} size 0x{f[1]:X}")
        print(img.disasm(f[0], f[1] // 4))


if __name__ == "__main__":
    main()
