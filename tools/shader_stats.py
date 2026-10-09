"""Survey of captured D3D shader containers (shaders/dump, from MCLA_DUMP_SHADERS=1):
constant table register sets, control-flow features, texture fetch slots and
vertex element usages. Informs what the runtime shader translator must support.

    python tools/shader_stats.py [dump dir]
"""
import collections
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SETS = ["bool", "int4", "float4", "sampler"]
USAGES = ["POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
          "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"]


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def constants(b):
    ct = u32(b, 0x10)  # constantTableOffset -> ConstantTableContainer {size, ConstantTable}
    base = ct + 4
    count, info = u32(b, base + 12), u32(b, base + 16)
    out = []
    for i in range(count):
        o = base + info + i * 20
        name_off, rset, rindex, rcount = u32(b, o), u16(b, o + 4), u16(b, o + 6), u16(b, o + 8)
        name = b[base + name_off:b.index(b"\0", base + name_off)].decode("latin-1")
        out.append((name, SETS[rset] if rset < 4 else rset, rindex, rcount))
    return out


def program(b):
    vsize = u32(b, 4)
    so = u32(b, 0x18)
    phys_off, size = u32(b, so), u32(b, so + 4)
    start = vsize + phys_off
    return [u32(b, start + i * 4) for i in range(size // 4)], so


def walk(words):
    """Yields (kind, details) for control flow and fetches."""
    cf_end = len(words)
    at = 0
    while at + 3 <= cf_end:
        lo = [words[at], (words[at + 1] >> 16) | ((words[at + 2] << 16) & 0xFFFFFFFF)]
        hi = [words[at + 1] & 0xFFFF, words[at + 2] >> 16]
        for k in range(2):
            op = (hi[k] >> 12) & 0xF
            full = lo[k] | (hi[k] << 32)
            if op in (1, 2, 3, 4, 5, 6, 13, 14):
                address, count, seq = lo[k] & 0xFFF, (lo[k] >> 12) & 7, (lo[k] >> 16) & 0xFFF
                cf_end = min(cf_end, address * 3)
                if op in (3, 4):
                    yield "condexec_bool", (full >> 34) & 0xFF
                for i in range(count):
                    ins = (address + i) * 3
                    if (seq >> (i * 2)) & 1 and ins + 3 <= len(words):
                        w0 = words[ins]
                        fop = w0 & 0x1F
                        if fop == 0:
                            yield "vfetch", (w0 >> 20) & 0x1F  # constIndex
                        elif fop == 1:
                            yield "tfetch", (w0 >> 20) & 0x1F
                        else:
                            yield "fetchop", fop
                    elif not (seq >> (i * 2)) & 1 and ins + 3 <= len(words):
                        w1 = words[ins + 1]
                        if (w1 >> 29) & 3 or (w1 >> 31):
                            yield "alu_const_relative", 1
            elif op == 11:  # CondJmp
                uncond, pred = (lo[k] >> 13) & 1, (lo[k] >> 14) & 1
                if not uncond and not pred:
                    yield "condjmp_bool", (full >> 34) & 0xFF
                yield "condjmp", (uncond, pred, (full >> 33) & 1)
            elif op == 9:
                yield "condcall", 1
            elif op == 7:
                yield "loop", (lo[k] >> 16) & 0x1F
        at += 3


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "shaders", "dump")
    stats = collections.Counter()
    bool_regs = {"vs": collections.Counter(), "ps": collections.Counter()}
    cf_bools = {"vs": collections.Counter(), "ps": collections.Counter()}
    samplers_vs_slots = collections.Counter()
    usages = collections.Counter()
    max_float = {"vs": 0, "ps": 0}
    int_consts = collections.Counter()
    for fn in sorted(os.listdir(d)):
        b = open(os.path.join(d, fn), "rb").read()
        kind = "vs" if u32(b, 0) & 1 else "ps"
        stats[kind] += 1
        consts = constants(b)
        samplers = {}
        for name, rset, rindex, rcount in consts:
            if rset == "bool":
                bool_regs[kind][rindex] += 1
            elif rset == "float4":
                max_float[kind] = max(max_float[kind], rindex + rcount)
            elif rset == "int4":
                int_consts[(kind, rindex)] += 1
            elif rset == "sampler":
                samplers[rindex] = name
        words, so = program(b)
        tslots = set()
        for what, v in walk(words):
            if what in ("condexec_bool", "condjmp_bool"):
                cf_bools[kind][v] += 1
            elif what == "tfetch":
                tslots.add(v)
            elif what == "loop":
                stats[f"{kind} loops"] += 1
            elif what == "condjmp":
                stats[f"{kind} condjmp uncond={v[0]} pred={v[1]} dir={v[2]}"] += 1
            elif what == "condcall":
                stats[f"{kind} condcall"] += 1
            elif what == "fetchop":
                stats[f"{kind} fetch opcode {v}"] += 1
        for slot in tslots:
            if slot not in samplers:
                stats[f"{kind} tfetch slot without sampler register"] += 1
            samplers_vs_slots[(kind, slot, slot in samplers)] += 1
        if kind == "vs":
            vcount, f18 = u32(b, so + 0x1C), u32(b, so + 0x18)
            for i in range(vcount):
                e = u32(b, so + 0x24 + (f18 + i) * 4)
                usages[(USAGES[(e >> 12) & 0xF], (e >> 16) & 0xF)] += 1
    print("shaders:", dict(stats))
    print("max float4 register used:", max_float)
    print("bool registers in constant tables:", {k: dict(v) for k, v in bool_regs.items()})
    print("bool addresses in control flow:", {k: dict(v) for k, v in cf_bools.items()})
    print("int4 constants:", dict(int_consts))
    print("texture fetch slots (kind, slot, has sampler register):", sorted(samplers_vs_slots.items()))
    print("vertex element usages:", sorted(usages.items(), key=lambda x: -x[1]))


if __name__ == "__main__":
    main()
