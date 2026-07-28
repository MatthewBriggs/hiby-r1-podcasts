#!/usr/bin/env python3
"""Patch a HiBy R1 Audiobook Mod firmware image so it loads the Podcasts app.

The app is an LD_PRELOAD library, and what loads it is /usr/bin/hiby_player.sh.
That file lives on a read-only squashfs, so it cannot be pushed to the device —
the firmware has to be repacked. This takes *your own* .upt, applies one patch
to that script, and writes a new .upt. No one else's firmware is redistributed.

    ./patch_firmware.py r1-audiobooks-2.0.26.upt r1-podcast-2.0.26.upt

Needs pycdlib (pip install pycdlib) and squashfs-tools (mksquashfs/unsquashfs).
Verified against the mod's 2.0.25 and 2.0.26 releases.

Image layout, worked out by inspection:

    /D0000001/F0000002.BIN   md5 of each rootfs chunk, one per line
    /D0000001/F0000003.BIN   the same for the kernel chunks
    /D0000001/F0000004.TXT   manifest: img_name / img_size / img_md5 per image
    /D0000001/F0000005.TXT   empty
    /D0000001/F0000006...    rootfs.squashfs, then xImage, in 512 KiB chunks
    /F00000NN.TXT            current_version=0, numbered after the last chunk

Both the manifest digests and the per-chunk digests have to be regenerated or
the device rejects the update.
"""

import argparse
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

CHUNK = 524288
SCRIPT = "usr/bin/hiby_player.sh"

# Applied to the mod's stock supervisor. Each anchor is matched exactly; if one
# is missing the script has changed and we stop rather than guess, because a
# half-applied patch to init would be a brick rather than a bug.
ANCHOR_CONFIG = 'MAX_CRASHES=5\n'
INSERT_CONFIG = '''MAX_CRASHES=5

# --- podcast app additions -------------------------------------------------
# The rootfs is read-only squashfs, so there is otherwise no way to start user
# code at boot or to load a second preload without reflashing. Both hooks below
# read from /usr/data, which is writable and survives a firmware update.
#
# DEV_HOOK is dropped after DEV_HOOK_GIVEUP consecutive crashes so that a broken
# library degrades to a working player instead of a reboot loop.
DEV_HOOK="/usr/data/libpodcast_hook.so"
DEV_HOOK_GIVEUP=2
HEALTHY_RUN=60          # seconds up before a run counts as a success
USER_INIT="/usr/data/init.sh"

[ -f "$USER_INIT" ] && sh "$USER_INIT" >/dev/null 2>&1 &
# ---------------------------------------------------------------------------
'''

ANCHOR_LAUNCH = '''    if [ -f "$HOOK_LIB" ]; then
        LD_PRELOAD="$HOOK_LIB" "$PLAYER" &
    else
        "$PLAYER" &
    fi
'''
INSERT_LAUNCH = '''    PRELOAD=""
    if [ "$CRASH_COUNT" -lt "$DEV_HOOK_GIVEUP" ] && [ -f "$DEV_HOOK" ]; then
        PRELOAD="$DEV_HOOK"
    fi
    if [ -f "$HOOK_LIB" ]; then
        PRELOAD="${PRELOAD:+$PRELOAD }$HOOK_LIB"
    fi

    if [ -n "$PRELOAD" ]; then
        LD_PRELOAD="$PRELOAD" "$PLAYER" &
    else
        "$PLAYER" &
    fi
'''

ANCHOR_COUNT = '''    wait "$HP_PID" 2>/dev/null
    CRASH_COUNT=$((CRASH_COUNT + 1))
'''
INSERT_COUNT = '''    STARTED=$(date +%s)
    wait "$HP_PID" 2>/dev/null

    # The counter is meant to catch a hook that cannot survive startup, so it
    # has to measure *consecutive* failures. Incrementing unconditionally makes
    # it count every exit since boot instead, and two unrelated ones hours apart
    # were enough to silently drop DEV_HOOK for the rest of the session — the
    # Podcasts tile just reverts to About with nothing to say why. A player that
    # stayed up long enough to be useful is evidence the hook is fine.
    if [ $(( $(date +%s) - STARTED )) -ge "$HEALTHY_RUN" ]; then
        CRASH_COUNT=0
    else
        CRASH_COUNT=$((CRASH_COUNT + 1))
    fi
'''


def die(msg):
    sys.exit(f"error: {msg}")


def need(tool):
    if not shutil.which(tool):
        die(f"{tool} not found — install squashfs-tools")


def patch_script(text):
    """Return the patched supervisor, or None if it is already patched."""
    if "DEV_HOOK" in text:
        return None
    for name, anchor in (("config", ANCHOR_CONFIG),
                         ("launch block", ANCHOR_LAUNCH),
                         ("crash counter", ANCHOR_COUNT)):
        if text.count(anchor) != 1:
            die(f"{SCRIPT}: expected exactly one {name} anchor, found "
                f"{text.count(anchor)}. This firmware's supervisor differs from "
                f"the 2.0.25/2.0.26 releases this was written against; patch it "
                f"by hand using app/hiby_player.sh as the reference.")
    # STARTED has to be set before wait, so the counter anchor absorbs both.
    text = text.replace(ANCHOR_CONFIG, INSERT_CONFIG)
    text = text.replace(ANCHOR_LAUNCH, INSERT_LAUNCH)
    text = text.replace(ANCHOR_COUNT, INSERT_COUNT)
    return text


def read_images(iso):
    """Pull the manifest and reassemble each chunked image, verifying digests.

    Chunk order is *not* manifest order — 2.0.26 lists the kernel first but
    stores the rootfs first — so the layout is taken from the per-chunk digest
    lists, whose lengths give each image's chunk count, and each assembled
    image is then matched to its manifest entry by digest rather than position.
    """
    def rd(path):
        b = io.BytesIO()
        iso.get_file_from_iso_fp(b, iso_path=path)
        return b.getvalue()

    manifest = rd("/D0000001/F0000004.TXT;1").decode()
    entries = re.findall(r"img_type=(\S+)\s+img_name=(\S+)\s+"
                         r"img_size=(\d+)\s+img_md5=([0-9a-f]+)", manifest)
    if not entries:
        die("could not parse the image manifest — is this an R1 .upt?")

    counts = [len(rd(f"/D0000001/F{i:07d}.BIN;1").decode().split())
              for i in (2, 3)]
    by_md5 = {md5: (t, name, int(size)) for t, name, size, md5 in entries}

    images, n = [], 6
    for count in counts:
        data = b"".join(rd(f"/D0000001/F{i:07d}.BIN;1") for i in range(n, n + count))
        got = hashlib.md5(data).hexdigest()
        if got not in by_md5:
            die(f"chunks at F{n:07d} (md5 {got}) match no manifest entry — "
                f"image is corrupt or the layout is one this tool does not know")
        img_type, name, size = by_md5[got]
        if len(data) != size:
            die(f"{name}: reassembled {len(data)} bytes, manifest says {size}")
        images.append({"type": img_type, "name": name, "data": data, "first": n})
        n += count
    return manifest, entries, images, n


def write_upt(out_path, images, manifest, meta, version_blob, version_num):
    """Rebuild the .upt, matching the original's directory extensions exactly.

    The stock images carry both Joliet (level 3) and Rock Ridge 1.09. Omitting
    them still produces a valid ISO 9660 image that reads back perfectly on a
    host — every digest verifies — but the device's updater cannot navigate it:
    it displays "Upgrading..." and never progresses. The extensions shift where
    the payload starts (first chunk at extent 51 rather than 33), so this is not
    cosmetic. Anything changed here must be checked against a stock image with
    verify_firmware.py --against, not merely by confirming checksums.
    """
    import pycdlib
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=1, vol_ident="CDROM",
            joliet=3, rock_ridge="1.09")
    iso.add_directory("/D0000001", joliet_path="/ota_v0", rr_name="ota_v0")

    def add(data, iso_path, real):
        """Every file carries an 8.3 ISO name plus its real name in Joliet/RR."""
        iso.add_fp(io.BytesIO(data), len(data), iso_path,
                   joliet_path="/ota_v0/" + real if "/D0000001/" in iso_path
                   else "/" + real,
                   rr_name=real)

    def chunks(d):
        return [d[i:i + CHUNK] for i in range(0, len(d), CHUNK)]

    per_chunk, whole = [], []
    for img in images:
        per_chunk.append([hashlib.md5(c).hexdigest() for c in chunks(img["data"])])
        whole.append(hashlib.md5(img["data"]).hexdigest())

    add("".join(h + "\n" for h in per_chunk[0]).encode(), "/D0000001/F0000002.BIN;1",
        f"ota_md5_{images[0]['name']}.{whole[0]}")
    add("".join(h + "\n" for h in per_chunk[1]).encode(), "/D0000001/F0000003.BIN;1",
        f"ota_md5_{images[1]['name']}.{whole[1]}")
    add(manifest.encode(), "/D0000001/F0000004.TXT;1", "ota_update.in")
    add(meta, "/D0000001/F0000005.TXT;1", "ota_v0.ok")

    n = 6
    for i, img in enumerate(images):
        for k, c in enumerate(chunks(img["data"])):
            # The chunk names form a verification chain: index 0 carries the
            # digest of the whole image, and every later index carries the
            # digest of the chunk before it. Getting this wrong is invisible to
            # a checksum test and leaves the updater stuck on "Upgrading...".
            digest = whole[i] if k == 0 else per_chunk[i][k - 1]
            add(c, f"/D0000001/F{n:07d}.BIN;1", f"{img['name']}.{k:04d}.{digest}")
            n += 1
    add(version_blob, f"/F{n:07d}.TXT;1", "ota_config.in")
    iso.write(out_path)
    iso.close()
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="the mod's .upt, e.g. r1-audiobooks-2.0.26.upt")
    ap.add_argument("output", help="where to write the patched .upt")
    args = ap.parse_args()

    try:
        import pycdlib
    except ImportError:
        die("pycdlib not installed — pip install pycdlib")
    need("unsquashfs")
    need("mksquashfs")

    if not os.path.exists(args.input):
        die(f"{args.input} not found")
    if os.path.exists(args.output):
        die(f"{args.output} already exists — refusing to overwrite")

    iso = pycdlib.PyCdlib()
    iso.open(args.input)
    manifest, entries, images, last = read_images(iso)

    def rd(path):
        b = io.BytesIO()
        iso.get_file_from_iso_fp(b, iso_path=path)
        return b.getvalue()
    meta = rd("/D0000001/F0000005.TXT;1")
    version_blob = rd(f"/F{last:07d}.TXT;1")
    iso.close()

    print(f"{args.input}:")
    for img in images:
        print(f"  {img['type']:8s} {img['name']:18s} {len(img['data']):>9} bytes  verified")

    rootfs = next((i for i in images if i["type"] == "rootfs"), None)
    if rootfs is None:
        die("no rootfs image in this .upt")

    with tempfile.TemporaryDirectory(prefix="r1patch-") as tmp:
        sqfs = os.path.join(tmp, "rootfs.squashfs")
        root = os.path.join(tmp, "root")
        with open(sqfs, "wb") as fh:
            fh.write(rootfs["data"])

        print("\nunpacking rootfs...")
        subprocess.run(["unsquashfs", "-d", root, "-q", sqfs],
                       check=True, stdout=subprocess.DEVNULL)

        target = os.path.join(root, SCRIPT)
        if not os.path.exists(target):
            die(f"{SCRIPT} not found — this does not look like the Audiobook Mod. "
                f"The Podcasts app needs the mod's LD_PRELOAD supervisor.")

        with open(target, "r") as fh:
            original = fh.read()
        patched = patch_script(original)
        if patched is None:
            die(f"{SCRIPT} already contains DEV_HOOK — this image is already patched")
        with open(target, "w") as fh:
            fh.write(patched)
        print(f"patched {SCRIPT} "
              f"({len(original.splitlines())} -> {len(patched.splitlines())} lines)")

        print("repacking rootfs...")
        newsq = os.path.join(tmp, "rootfs.new.squashfs")
        subprocess.run(["mksquashfs", root, newsq, "-comp", "lzo", "-b", "131072",
                        "-no-exports", "-all-root", "-noappend", "-no-progress"],
                       check=True, stdout=subprocess.DEVNULL)
        with open(newsq, "rb") as fh:
            rootfs["data"] = fh.read()

    # The rootfs almost always changes size, so the manifest has to follow it.
    for img_type, name, size, md5 in entries:
        if img_type == "rootfs":
            manifest = manifest.replace(f"img_size={size}",
                                        f"img_size={len(rootfs['data'])}")
            manifest = manifest.replace(f"img_md5={md5}",
                                        f"img_md5={hashlib.md5(rootfs['data']).hexdigest()}")

    print("building .upt...")
    last = write_upt(args.output, images, manifest, meta, version_blob, last)
    print(f"\nwrote {args.output} ({os.path.getsize(args.output)} bytes, "
          f"last entry F{last:07d}.TXT)")
    print("\nVerify it before flashing:  ./tools/verify_firmware.py " + args.output)


if __name__ == "__main__":
    main()
