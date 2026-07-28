#!/usr/bin/env python3
"""Check a HiBy R1 .upt is internally consistent, and say what is in it.

Run this on anything patch_firmware.py produces before flashing it. A bad
image is not a bad build, it is a device that does not come back, so every
digest the updater checks is checked here first.

    ./verify_firmware.py r1-podcast-2.0.26.upt
    ./verify_firmware.py r1-podcast-2.0.26.upt --against r1-audiobooks-2.0.26.upt

With --against, the two images are compared file by file inside the rootfs, so
you can confirm the patch changed exactly what it claimed to and nothing else.
"""

import argparse
import hashlib
import io
import os
import re
import subprocess
import sys
import tempfile

CHUNK = 524288
SCRIPT = "usr/bin/hiby_player.sh"


def load(path):
    import pycdlib
    iso = pycdlib.PyCdlib()
    iso.open(path)

    def rd(p):
        b = io.BytesIO()
        iso.get_file_from_iso_fp(b, iso_path=p)
        return b.getvalue()

    manifest = rd("/D0000001/F0000004.TXT;1").decode()
    entries = re.findall(r"img_type=(\S+)\s+img_name=(\S+)\s+"
                         r"img_size=(\d+)\s+img_md5=([0-9a-f]+)", manifest)
    # Chunk order is not manifest order, so take the layout from the digest
    # lists and match each assembled image to its manifest entry by md5.
    listed = [rd("/D0000001/F0000002.BIN;1").decode().split(),
              rd("/D0000001/F0000003.BIN;1").decode().split()]
    by_md5 = {md5: (t, name, int(size)) for t, name, size, md5 in entries}

    images, n, chunk_digests = [], 6, []
    for lst in listed:
        parts = [rd(f"/D0000001/F{i:07d}.BIN;1") for i in range(n, n + len(lst))]
        data = b"".join(parts)
        chunk_digests.append([hashlib.md5(p).hexdigest() for p in parts])
        got = hashlib.md5(data).hexdigest()
        # The manifest is right about this image only if the bytes we actually
        # assembled hash to something it declares.
        img_type, name, size = by_md5.get(got, ("unknown", "unmatched", -1))
        images.append({"type": img_type, "name": name, "size": size,
                       "matched": got in by_md5, "data": data, "first": n})
        n += len(lst)
    version = rd(f"/F{n:07d}.TXT;1")
    iso.close()
    return images, chunk_digests, listed, version, n


def unpack_rootfs(data, dest):
    sq = os.path.join(dest, "r.squashfs")
    with open(sq, "wb") as fh:
        fh.write(data)
    root = os.path.join(dest, "root")
    subprocess.run(["unsquashfs", "-d", root, "-q", sq],
                   check=True, stdout=subprocess.DEVNULL)
    return root


def tree(root):
    out = {}
    for dp, _, fn in os.walk(root, followlinks=False):
        for f in fn:
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, root)
            if os.path.islink(p):
                out[rel] = ("link", os.readlink(p))
            else:
                with open(p, "rb") as fh:
                    out[rel] = ("file", hashlib.md5(fh.read()).hexdigest())
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--against", help="original .upt to diff the rootfs against")
    args = ap.parse_args()

    try:
        import pycdlib  # noqa: F401
    except ImportError:
        sys.exit("error: pycdlib not installed — pip install pycdlib")

    images, chunk_digests, listed, version, last = load(args.image)
    ok = True

    print(f"{args.image}  ({os.path.getsize(args.image)} bytes)\n")
    for i, img in enumerate(images):
        size_ok = len(img["data"]) == img["size"]
        chunks_ok = chunk_digests[i] == listed[i]
        ok &= size_ok and img["matched"] and chunks_ok
        print(f"  {img['type']:8s} {img['name']}")
        print(f"    size          {len(img['data'])} {'OK' if size_ok else 'MISMATCH'}")
        print(f"    manifest md5  {'OK' if img['matched'] else 'MISMATCH'}")
        print(f"    chunk digests {'OK' if chunks_ok else 'MISMATCH'} "
              f"({len(chunk_digests[i])} chunks from F{img['first']:07d})")
    print(f"\n  version entry   F{last:07d}.TXT = {version!r}")

    rootfs = next(i for i in images if i["type"] == "rootfs")
    with tempfile.TemporaryDirectory(prefix="r1verify-") as tmp:
        root = unpack_rootfs(rootfs["data"], tmp)
        script = os.path.join(root, SCRIPT)
        if os.path.exists(script):
            with open(script) as fh:
                body = fh.read()
            has = "DEV_HOOK" in body
            healthy = "HEALTHY_RUN" in body
            print(f"\n  {SCRIPT}")
            print(f"    loads libpodcast_hook.so   {'yes' if has else 'NO'}")
            print(f"    consecutive-crash fix      {'yes' if healthy else 'NO'}")
            ok &= has
        else:
            print(f"\n  {SCRIPT} MISSING")
            ok = False

        if args.against:
            other, *_ = load(args.against)
            other_rootfs = next(i for i in other if i["type"] == "rootfs")
            with tempfile.TemporaryDirectory(prefix="r1orig-") as tmp2:
                oroot = unpack_rootfs(other_rootfs["data"], tmp2)
                a, b = tree(oroot), tree(root)
                changed = sorted(k for k in set(a) & set(b) if a[k] != b[k])
                added = sorted(set(b) - set(a))
                removed = sorted(set(a) - set(b))
                print(f"\n  vs {args.against}:")
                print(f"    {len(a)} files -> {len(b)} files")
                for k in changed:
                    print(f"    ~ {k}")
                for k in added:
                    print(f"    + {k}")
                for k in removed:
                    print(f"    - {k}")
                if changed == [SCRIPT] and not added and not removed:
                    print("    exactly one file changed, as intended")
                else:
                    print("    UNEXPECTED: more than the supervisor differs")
                    ok = False

    print("\nRESULT:", "image verifies" if ok else "PROBLEM — do not flash")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
