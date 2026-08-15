#!/usr/bin/env python3
"""Read the .modinfo section out of MIPS .ko files (no cross-binutils needed)."""
import struct
import sys
import glob
import os


def sections(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF", path
    is64 = d[4] == 2
    little = d[5] == 1
    e = "<" if little else ">"
    assert not is64, "expected 32-bit"
    e_shoff = struct.unpack_from(e + "I", d, 0x20)[0]
    e_shentsize = struct.unpack_from(e + "H", d, 0x2E)[0]
    e_shnum = struct.unpack_from(e + "H", d, 0x30)[0]
    e_shstrndx = struct.unpack_from(e + "H", d, 0x32)[0]

    def sh(i):
        o = e_shoff + i * e_shentsize
        name, _typ, _fl, _ad, off, size = struct.unpack_from(e + "IIIIII", d, o)
        return name, off, size

    _, stroff, _ = sh(e_shstrndx)
    out = {}
    for i in range(e_shnum):
        nameoff, off, size = sh(i)
        end = d.index(b"\0", stroff + nameoff)
        nm = d[stroff + nameoff:end].decode()
        out[nm] = d[off:off + size]
    return out


def modinfo(path):
    secs = sections(path)
    raw = secs.get(".modinfo", b"")
    kv = []
    for item in raw.split(b"\0"):
        if b"=" in item:
            k, v = item.split(b"=", 1)
            kv.append((k.decode(errors="replace"), v.decode(errors="replace")))
    return kv


def main():
    files = sorted(glob.glob(sys.argv[1] + "/*.ko"))
    for f in files:
        info = dict()
        multi = {}
        for k, v in modinfo(f):
            if k in ("alias", "parm", "parmtype", "softdep"):
                multi.setdefault(k, []).append(v)
            else:
                info[k] = v
        name = os.path.basename(f)
        size = os.path.getsize(f)
        print(f"\n### {name}  ({size:,} bytes)")
        for key in ("description", "author", "license", "depends", "vermagic"):
            if info.get(key):
                print(f"    {key:12} {info[key]}")
        for key in ("alias", "parm"):
            for v in multi.get(key, [])[:6]:
                print(f"    {key:12} {v}")


if __name__ == "__main__":
    main()
