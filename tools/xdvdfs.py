"""Minimal XDVDFS (Xbox / Xbox 360 disc filesystem) reader.

Usage:
    python xdvdfs.py <iso> list                 # print the full file tree
    python xdvdfs.py <iso> extract <out_dir> [path ...]
        # extract the given paths (e.g. default.xex), or everything if none given
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets: plain XISO, XGD3, XGD2, XGD1
PARTITION_OFFSETS = [0x0, 0x2080000, 0xFD90000, 0x18300000]
ATTR_DIRECTORY = 0x10


class XdvdfsImage:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.base = None
        for off in PARTITION_OFFSETS:
            self.f.seek(off + 32 * SECTOR)
            if self.f.read(len(MAGIC)) == MAGIC:
                self.base = off
                break
        if self.base is None:
            raise ValueError("no XDVDFS volume descriptor found")
        root_sector, root_size = struct.unpack("<II", self.f.read(8))
        self.root = (root_sector, root_size)

    def _read(self, sector, size):
        self.f.seek(self.base + sector * SECTOR)
        return self.f.read(size)

    def read_dir(self, sector, size):
        """Return list of (name, attrs, start_sector, size) for a directory table."""
        if size == 0:
            return []
        table = self._read(sector, size)
        entries = []
        seen = set()
        stack = [0]
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(table):
                continue
            seen.add(off)
            left, right, start, fsize, attrs, nlen = struct.unpack_from("<HHIIBB", table, off)
            if left == 0xFFFF and right == 0xFFFF:
                continue  # padding
            name = table[off + 14: off + 14 + nlen].decode("ascii", "replace")
            entries.append((name, attrs, start, fsize))
            if left:
                stack.append(left * 4)
            if right:
                stack.append(right * 4)
        entries.sort(key=lambda e: e[0].lower())
        return entries

    def walk(self, sector=None, size=None, prefix=""):
        if sector is None:
            sector, size = self.root
        for name, attrs, start, fsize in self.read_dir(sector, size):
            path = prefix + name
            if attrs & ATTR_DIRECTORY:
                yield path + "/", attrs, start, fsize
                yield from self.walk(start, fsize, path + "/")
            else:
                yield path, attrs, start, fsize

    def extract(self, start, size, out_path):
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        self.f.seek(self.base + start * SECTOR)
        remaining = size
        with open(out_path, "wb") as out:
            while remaining:
                chunk = self.f.read(min(remaining, 8 << 20))
                if not chunk:
                    raise IOError("unexpected end of image")
                out.write(chunk)
                remaining -= len(chunk)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    img = XdvdfsImage(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "list":
        print(f"# partition offset 0x{img.base:X}")
        total = 0
        for path, attrs, start, size in img.walk():
            if not path.endswith("/"):
                total += size
            print(f"{size:>12}  sector {start:>8}  {path}")
        print(f"# total file bytes: {total}")
    elif cmd == "extract":
        out_dir = sys.argv[3]
        wanted = {p.lower().replace("\\", "/") for p in sys.argv[4:]}
        for path, attrs, start, size in img.walk():
            if path.endswith("/"):
                continue
            if wanted and path.lower() not in wanted:
                continue
            dest = os.path.join(out_dir, *path.split("/"))
            print(f"extracting {path} ({size} bytes)")
            img.extract(start, size, dest)
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
