#!/usr/bin/env python3
"""List the kernel symbols the prebuilt .ko modules import.

That set is the hard floor for any rebuilt kernel: every one of these must
still be exported, or the module won't load. Anything the kernel exports that
is NOT in this set (and not needed by userspace) is a candidate for trimming.
"""
import glob
import os
import struct
import sys


def sections(path):
    d = open(path, "rb").read()
    e = "<" if d[5] == 1 else ">"
    shoff, = struct.unpack_from(e + "I", d, 0x20)
    shent, = struct.unpack_from(e + "H", d, 0x2E)
    shnum, = struct.unpack_from(e + "H", d, 0x30)
    shstr, = struct.unpack_from(e + "H", d, 0x32)

    def sh(i):
        o = shoff + i * shent
        name, typ, fl, ad, off, size, link, info = struct.unpack_from(e + "8I", d, o)
        return dict(name=name, typ=typ, off=off, size=size, link=link)

    hdrs = [sh(i) for i in range(shnum)]
    stro = hdrs[shstr]["off"]
    out = {}
    for h in hdrs:
        end = d.index(b"\0", stro + h["name"])
        nm = d[stro + h["name"]:end].decode()
        out[nm] = (d[h["off"]:h["off"] + h["size"]], h)
    return out, d, e, hdrs, stro


def undefined_syms(path):
    secs, d, e, hdrs, stro = sections(path)
    if ".symtab" not in secs:
        return set()
    symdata, symhdr = secs[".symtab"]
    strdata = d[hdrs[symhdr["link"]]["off"]:
                hdrs[symhdr["link"]]["off"] + hdrs[symhdr["link"]]["size"]]
    out = set()
    n = len(symdata) // 16
    for i in range(n):
        nameoff, value, size, info, other, shndx = struct.unpack_from(e + "IIIBBH", symdata, i * 16)
        if shndx != 0:          # SHN_UNDEF == 0 -> imported from the kernel
            continue
        end = strdata.index(b"\0", nameoff)
        nm = strdata[nameoff:end].decode(errors="replace")
        if nm:
            out.add(nm)
    return out


def main():
    files = sorted(glob.glob(sys.argv[1] + "/*.ko"))
    provided = set()
    for f in files:                      # symbols the module set defines itself
        secs, d, e, hdrs, stro = sections(f)
        if ".symtab" not in secs:
            continue
        symdata, symhdr = secs[".symtab"]
        strdata = d[hdrs[symhdr["link"]]["off"]:
                    hdrs[symhdr["link"]]["off"] + hdrs[symhdr["link"]]["size"]]
        for i in range(len(symdata) // 16):
            nameoff, value, size, info, other, shndx = struct.unpack_from(
                e + "IIIBBH", symdata, i * 16)
            if shndx == 0:
                continue
            end = strdata.index(b"\0", nameoff)
            nm = strdata[nameoff:end].decode(errors="replace")
            if nm:
                provided.add(nm)

    allsyms = {}
    for f in files:
        u = undefined_syms(f)
        for s in u:
            allsyms.setdefault(s, []).append(os.path.basename(f)[:-3])

    # Imports satisfied inside the module set (e.g. utils.ko) aren't kernel deps
    kernel_needed = {s: m for s, m in allsyms.items() if s not in provided}
    print(f"modules scanned          : {len(files)}")
    print(f"distinct imported symbols: {len(allsyms)}")
    print(f"satisfied within the set : {len(allsyms) - len(kernel_needed)}")
    print(f"MUST come from the kernel: {len(kernel_needed)}")
    with open("kernel_needed_syms.txt", "w") as fh:
        for s in sorted(kernel_needed):
            fh.write(s + "\n")
    print("written: kernel_needed_syms.txt")


if __name__ == "__main__":
    main()
