"""Locate the compiler helper routines XenonRecomp needs in a decrypted XEX image.

Usage: python find_helpers.py <image.pe> [base=0x82000000]
"""
import struct
import sys

PATTERNS = {
    "restgprlr_14_address": "e9c1ff68",
    "savegprlr_14_address": "f9c1ff68",
    "restfpr_14_address": "c9ccff70",
    "savefpr_14_address": "d9ccff70",
    "restvmx_14_address": "3960fee07dcb60ce",
    "savevmx_14_address": "3960fee07dcb61ce",
    "restvmx_64_address": "3960fc00100b60cb",
    "savevmx_64_address": "3960fc00100b61cb",
}


def find_all(data, needle):
    hits, i = [], data.find(needle)
    while i != -1:
        if i % 4 == 0:
            hits.append(i)
        i = data.find(needle, i + 1)
    return hits


def main():
    data = open(sys.argv[1], "rb").read()
    base = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x82000000
    for name, pat in PATTERNS.items():
        hits = find_all(data, bytes.fromhex(pat))
        shown = ", ".join(f"0x{base + h:08X}" for h in hits[:8])
        more = f" (+{len(hits) - 8} more)" if len(hits) > 8 else ""
        print(f"{name:<22} {len(hits):>3} hit(s): {shown}{more}")


if __name__ == "__main__":
    main()
