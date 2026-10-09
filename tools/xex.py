"""XEX2 (Xbox 360 executable) inspector / unpacker.

Usage:
    python xex.py <file.xex> info               # dump header information
    python xex.py <file.xex> dump <out.pe>      # decrypt + decompress the PE image
    python xex.py <file.xex> imports <out.txt>  # resolve import ordinals (needs the image)
"""
import hashlib
import struct
import sys

# Public retail XEX2 key (the same constant every Xbox 360 emulator ships with).
RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
DEVKIT_KEY = bytes(16)

HEADER_NAMES = {
    0x000002FF: "RESOURCE_INFO",
    0x000003FF: "FILE_FORMAT_INFO",
    0x000005FF: "DELTA_PATCH_DESCRIPTOR",
    0x00000405: "BASE_REFERENCE",
    0x000080FF: "BOUNDING_PATH",
    0x00008105: "DEVICE_ID",
    0x00010001: "ORIGINAL_BASE_ADDRESS",
    0x00010100: "ENTRY_POINT",
    0x00010201: "IMAGE_BASE_ADDRESS",
    0x000103FF: "IMPORT_LIBRARIES",
    0x00018002: "CHECKSUM_TIMESTAMP",
    0x00018102: "ENABLED_FOR_CALLCAP",
    0x00018200: "ENABLED_FOR_FASTCAP",
    0x000183FF: "ORIGINAL_PE_NAME",
    0x000200FF: "STATIC_LIBRARIES",
    0x00020104: "TLS_INFO",
    0x00020200: "DEFAULT_STACK_SIZE",
    0x00020301: "DEFAULT_FILESYSTEM_CACHE_SIZE",
    0x00020401: "DEFAULT_HEAP_SIZE",
    0x00028002: "PAGE_HEAP_SIZE_AND_FLAGS",
    0x00030000: "SYSTEM_FLAGS",
    0x00040006: "EXECUTION_INFO",
    0x00040201: "TITLE_WORKSPACE_SIZE",
    0x00040310: "GAME_RATINGS",
    0x00040404: "LAN_KEY",
    0x000405FF: "XBOX360_LOGO",
    0x000406FF: "MULTIDISC_MEDIA_IDS",
    0x000407FF: "ALTERNATE_TITLE_IDS",
    0x00040801: "ADDITIONAL_TITLE_MEMORY",
    0x00E10402: "EXPORTS_BY_NAME",
}
NAME_TO_KEY = {v: k for k, v in HEADER_NAMES.items()}


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def be16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def version_str(v):
    return f"{v >> 28}.{(v >> 24) & 0xF}.{(v >> 8) & 0xFFFF}.{v & 0xFF}"


class Xex:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"XEX2":
            raise ValueError(f"not a XEX2 file (magic {d[:4]!r})")
        self.module_flags = be32(d, 4)
        self.pe_offset = be32(d, 8)
        self.security_offset = be32(d, 0x10)
        count = be32(d, 0x14)
        self.headers = {}  # key -> inline value or offset
        for i in range(count):
            key, value = struct.unpack_from(">II", d, 0x18 + i * 8)
            self.headers[key] = value
        self._parse_security()

    # ---- optional headers -------------------------------------------------
    def opt(self, name):
        """Inline value for inline headers, raw bytes for the rest, None if absent."""
        key = NAME_TO_KEY[name]
        if key not in self.headers:
            return None
        value = self.headers[key]
        size_code = key & 0xFF
        if size_code in (0x00, 0x01):
            return value
        if size_code == 0xFF:
            size = be32(self.data, value)
            return self.data[value: value + size]
        return self.data[value: value + size_code * 4]

    def _parse_security(self):
        d, s = self.data, self.security_offset
        self.image_size = be32(d, s + 0x4)
        self.image_flags = be32(d, s + 0x10C)
        self.load_address = be32(d, s + 0x110)
        self.import_table_count = be32(d, s + 0x128)
        self.encrypted_key = d[s + 0x150: s + 0x160]
        self.export_table = be32(d, s + 0x160)
        self.region = be32(d, s + 0x178)
        self.allowed_media = be32(d, s + 0x17C)
        n = be32(d, s + 0x180)
        self.page_size = 0x1000 if self.image_flags & 0x10000000 else 0x10000
        self.pages = []  # (page_count, info)
        for i in range(n):
            v = be32(d, s + 0x184 + i * 24)
            self.pages.append((v >> 4, v & 0xF))

    def file_format(self):
        ff = self.opt("FILE_FORMAT_INFO")
        enc, comp = struct.unpack_from(">HH", ff, 4)
        return enc, comp, ff

    def execution_info(self):
        b = self.opt("EXECUTION_INFO")
        if b is None:
            return None
        media_id, ver, base_ver, title_id = struct.unpack_from(">IIII", b, 0)
        platform, exe_type, disc, disc_count = b[16:20]
        return dict(media_id=media_id, version=ver, base_version=base_ver,
                    title_id=title_id, disc=disc, disc_count=disc_count)

    def static_libraries(self):
        b = self.opt("STATIC_LIBRARIES")
        out = []
        if b is None:
            return out
        for o in range(4, len(b), 16):
            name = b[o:o + 8].split(b"\0")[0].decode()
            major, minor, build, qfe = struct.unpack_from(">HHHH", b, o + 8)
            out.append((name, f"{major}.{minor}.{build}.{qfe & 0x3FFF}"))
        return out

    def import_libraries(self):
        """Return list of dicts: name, version, min_version, addresses."""
        b = self.opt("IMPORT_LIBRARIES")
        if b is None:
            return []
        str_size, str_count = struct.unpack_from(">II", b, 4)
        names = b[12:12 + str_size].split(b"\0")
        names = [n.decode() for n in names if n][:str_count]
        libs = []
        o = 12 + str_size
        while o + 0x28 <= len(b):
            size = be32(b, o)
            if size == 0:
                break
            ver, ver_min = be32(b, o + 0x1C), be32(b, o + 0x20)
            name_index, cnt = be16(b, o + 0x24), be16(b, o + 0x26)
            addrs = [be32(b, o + 0x28 + i * 4) for i in range(cnt)]
            libs.append(dict(name=names[name_index], version=version_str(ver),
                             min_version=version_str(ver_min), addresses=addrs))
            o += size
        return libs

    # ---- image extraction -------------------------------------------------
    def session_key(self, key=RETAIL_KEY):
        from Crypto.Cipher import AES
        return AES.new(key, AES.MODE_ECB).decrypt(self.encrypted_key)

    def payload(self, key=RETAIL_KEY):
        enc, comp, ff = self.file_format()
        raw = self.data[self.pe_offset:]
        if enc == 1:
            from Crypto.Cipher import AES
            raw = raw[: len(raw) // 16 * 16]
            raw = AES.new(self.session_key(key), AES.MODE_CBC, bytes(16)).decrypt(raw)
        return raw

    def image(self, key=RETAIL_KEY):
        enc, comp, ff = self.file_format()
        raw = self.payload(key)
        if comp == 0:  # none
            return raw[: self.image_size]
        if comp == 1:  # basic: runs of (data, zero padding)
            out = bytearray()
            pos = 0
            for o in range(8, len(ff), 8):
                data_size, zero_size = struct.unpack_from(">II", ff, o)
                out += raw[pos: pos + data_size]
                out += bytes(zero_size)
                pos += data_size
            return bytes(out)
        if comp == 2:  # normal: hashed block chain of LZX chunks
            window_size = be32(ff, 8)
            block_size = be32(ff, 12)
            block_hash = ff[16:36]
            pos = 0
            compressed = bytearray()
            while block_size:
                block = raw[pos: pos + block_size]
                if hashlib.sha1(block).digest() != block_hash:
                    raise ValueError(f"block hash mismatch at payload offset 0x{pos:X} "
                                     "(wrong key?)")
                next_size = be32(block, 0)
                next_hash = block[4:24]
                p = 24
                while True:
                    chunk = be16(block, p)
                    p += 2
                    if chunk == 0:
                        break
                    compressed += block[p: p + chunk]
                    p += chunk
                pos += block_size
                block_size, block_hash = next_size, next_hash
            from lzx import lzx_decompress
            return lzx_decompress(bytes(compressed), window_size, self.image_size)
        raise NotImplementedError(f"compression type {comp}")


def cmd_info(x):
    enc, comp, _ = x.file_format()
    ei = x.execution_info()
    print(f"module flags      0x{x.module_flags:08X}")
    print(f"load address      0x{x.load_address:08X}")
    print(f"image size        0x{x.image_size:X} ({x.image_size / 1048576:.2f} MiB)")
    print(f"entry point       0x{x.opt('ENTRY_POINT'):08X}")
    print(f"encryption        {['none', 'AES (normal)'][enc]}")
    print(f"compression       {['none', 'basic', 'normal (LZX)', 'delta'][comp]}")
    print(f"page size         0x{x.page_size:X}, {len(x.pages)} page descriptors")
    pe_name = x.opt("ORIGINAL_PE_NAME")
    if pe_name:
        pe_name = pe_name[4:].split(b"\0")[0].decode()
        print(f"original PE name  {pe_name}")
    if ei:
        print(f"title id          {ei['title_id']:08X}")
        print(f"version           {version_str(ei['version'])} (base {version_str(ei['base_version'])})")
        print(f"disc              {ei['disc']}/{ei['disc_count']}")
    for name in ("DEFAULT_STACK_SIZE", "DEFAULT_HEAP_SIZE", "TITLE_WORKSPACE_SIZE",
                 "SYSTEM_FLAGS", "IMAGE_BASE_ADDRESS"):
        v = x.opt(name)
        if v is not None:
            print(f"{name.lower():<17} 0x{v:X}")
    tls = x.opt("TLS_INFO")
    if tls:
        slots, raw_addr, data_size, raw_size = struct.unpack_from(">IIII", tls, 0)
        print(f"tls               {slots} slots, raw @0x{raw_addr:08X}, data 0x{data_size:X}, raw 0x{raw_size:X}")
    print("\noptional headers:")
    for k, v in x.headers.items():
        print(f"  {k:08X} {HEADER_NAMES.get(k, '?'):<28} 0x{v:08X}")
    print("\nstatic libraries (XDK build):")
    for name, ver in x.static_libraries():
        print(f"  {name:<10} {ver}")
    print("\nimport libraries:")
    for lib in x.import_libraries():
        print(f"  {lib['name']:<14} v{lib['version']} (min {lib['min_version']}) "
              f"- {len(lib['addresses'])} import records")
    print("\nsection pages:")
    addr = x.load_address
    kinds = {1: "code", 2: "data", 3: "read-only data"}
    run_start, run_kind = addr, None
    for count, info in x.pages + [(0, None)]:
        if info != run_kind:
            if run_kind is not None:
                print(f"  0x{run_start:08X}-0x{addr:08X}  {kinds.get(run_kind, run_kind)}")
            run_start, run_kind = addr, info
        addr += count * x.page_size


def load_export_tables():
    """Map library stem ('xboxkrnl', 'xam') -> {ordinal: name} from XenonUtils' tables."""
    import os
    import re
    root = os.path.join(os.path.dirname(__file__), "..", "thirdparty", "XenonRecomp",
                        "XenonUtils", "xbox")
    tables = {}
    for stem in ("xboxkrnl", "xam"):
        names = {}
        with open(os.path.join(root, f"{stem}_table.inc")) as f:
            for m in re.finditer(r"XE_EXPORT\(\w+,\s*(0x[0-9A-Fa-f]+),\s*(\w+),", f.read()):
                names[int(m.group(1), 16)] = m.group(2)
        tables[stem] = names
    return tables


def resolve_imports(x, img):
    """Yield (library, ordinal, name, kind, address) for every import record."""
    tables = load_export_tables()
    for lib in x.import_libraries():
        stem = lib["name"].split(".")[0]
        for addr in lib["addresses"]:
            value = be32(img, addr - x.load_address)
            kind = "thunk" if value >> 24 == 1 else "variable"
            ordinal = value & 0xFFFF
            name = tables.get(stem, {}).get(ordinal, f"{stem}_{ordinal:03X}")
            yield lib["name"], ordinal, name, kind, addr


def cmd_imports(x, out_path):
    img = x.image()
    rows = sorted(resolve_imports(x, img), key=lambda r: (r[0], r[1], r[3]))
    with open(out_path, "w") as f:
        for lib, ordinal, name, kind, addr in rows:
            f.write(f"{lib:<14} {ordinal:>4X} {kind:<8} 0x{addr:08X} {name}\n")
    funcs = {(r[0], r[1]) for r in rows if r[3] == "thunk"}
    vars_ = {(r[0], r[1]) for r in rows if r[3] == "variable"} - funcs
    print(f"{len(funcs)} imported functions, {len(vars_)} imported variables -> {out_path}")


def cmd_dump(x, out_path):
    img = x.image()
    with open(out_path, "wb") as f:
        f.write(img)
    print(f"wrote {len(img)} bytes to {out_path}; starts with {img[:2]!r}")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    x = Xex(sys.argv[1])
    if sys.argv[2] == "info":
        cmd_info(x)
    elif sys.argv[2] == "dump":
        cmd_dump(x, sys.argv[3])
    elif sys.argv[2] == "imports":
        cmd_imports(x, sys.argv[3])
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
