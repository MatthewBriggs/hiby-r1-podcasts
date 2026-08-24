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
MOUNT_SCRIPT = "usr/bin/mount_ubifs.sh"
VERSION_FILE = "etc/r1_audiobook_version"
BT_INIT = "usr/bin/bt_init"

# Default stamp for a build. Override with --rom-version. It ends up in
# System -> About so a device can be identified without a laptop, which
# matters once several builds exist that differ only in what was patched in.
DEFAULT_ROM_VERSION = "pod1.0"

# System -> About renders config.json's `version`, and the field is cut to
# SEVEN characters — "2.0.26ABCDEFGHIJ" displays as "2.0.26A". The stock string
# is already six, so exactly one is left. A revision letter is what fits: a, b,
# c... The full build string goes in etc/r1_audiobook_version, which adb can
# read, but the letter is what identifies a device you are holding.
CONFIG_JSON = "usr/resource/config.json"
ABOUT_VERSION_MAX = 7

# Internet radio is already in the firmware — the strings are localised in every
# settings.ini and the icons ship in every theme — but the Stream media screen
# only lists it in HiBy's China-region layout. The plain layout has Tidal and
# Qobuz; the _cn one has those plus net_radio. So "unlocking" it is just using
# the layout HiBy already wrote, with no invented assets.
STREAM_LAYOUT_DIRS = (
    "usr/resource/layout/theme1",
    "usr/resource/layout/theme2",
    "usr/resource/layout/midi/theme1",
)


def enable_internet_radio(root):
    """Point each theme's Stream media screen at the layout with the radio tile."""
    changed = []
    for d in STREAM_LAYOUT_DIRS:
        plain = os.path.join(root, d, "hiby_stream_media.view")
        cn = os.path.join(root, d, "hiby_stream_media_cn.view")
        if not (os.path.exists(plain) and os.path.exists(cn)):
            continue
        with open(cn, "rb") as fh:
            want = fh.read()
        with open(plain, "rb") as fh:
            if fh.read() == want:
                continue                 # already swapped
        with open(plain, "wb") as fh:
            fh.write(want)
        changed.append(d)
    return changed


def stamp_config_json(text, rom_rev):
    """Append a one-character revision to the version About displays."""
    m = re.search(r'("version"\s*:\s*")([^"]+)(")', text)
    if not m:
        return None
    cur = m.group(2)
    if len(cur) >= ABOUT_VERSION_MAX:
        return None                      # already stamped, or no room left
    new = (cur + rom_rev)[:ABOUT_VERSION_MAX]
    return text[:m.start(2)] + new + text[m.end(2):]


def stamp_version_file(text, rom_version):
    """Stamp the build into the string System -> About displays.

    Neither about_dev.ini's <model> nor config.json's version drives that text —
    both were tried and neither showed. The Audiobook Mod renders `label` from
    this file, which is why About reads exactly "HiBy R1 2.0.26". Appending here
    is what actually reaches the screen. The extra keys are for scripts and adb.
    """
    if "podcast_rom=" in text:
        return None
    out = []
    for line in text.splitlines():
        if line.startswith("label=") and rom_version not in line:
            line = f"{line} {rom_version}"
        out.append(line)
    out.append(f"podcast_rom={rom_version}")
    out.append(f"podcast_rom_built="
               f"{__import__('datetime').date.today().isoformat()}")
    return "\n".join(out) + "\n"

# /usr/data — the writable UBIFS holding the library database, settings and the
# preload libraries — is mounted with -o sync, which makes every write hit NAND
# synchronously. That is a large part of why saving settings and progress feels
# slow, and it wears the flash unnecessarily. SQLite still calls fsync at commit
# on its own, so durability for the database does not depend on this flag.
# noatime additionally stops a metadata write every time a file is merely read.
# BlueALSA is started with A2DP only, which is all that playback needs — but it
# also means there is no RFCOMM link, and a headset's battery level arrives over
# HFP. Adding the Hands-Free Audio Gateway profile gives bluealsa-rfcomm
# something to report.
#
# The XAPL name is "iPhone" deliberately, and it is the difference between a
# battery reading and none. Apple's handshake has the headset send
# AT+XAPL=<vendor>-<product>-<version> and the gateway answer with an
# identifier and its feature bits. Answering "HiBy" was captured on the wire
# being accepted and then ignored — the WH-1000XM4 simply never sent the
# IPHONEACCEV that carries the level. Answering "iPhone" produced it
# immediately.
ANCHOR_BT = ('/usr/bin/bluealsa -p a2dp-source '
             '--a2dp-volume --sbc-quality=xq &')
INSERT_BT = ('/usr/bin/bluealsa -p a2dp-source -p hfp-ag '
             '--a2dp-volume --sbc-quality=xq --xapl-resp-name=iPhone &')


ANCHOR_BT_VANILLA = '/usr/bin/bluealsa -p a2dp-source --a2dp-volume &'
INSERT_BT_VANILLA = ('/usr/bin/bluealsa -p a2dp-source -p hfp-ag '
                     '--a2dp-volume --xapl-resp-name=iPhone &')


def patch_bt_init(text):
    """Add the HFP profile so a headset can report its battery.

    Two anchors: the mod's own bt_init already carries --sbc-quality=xq (a
    separate, earlier tweak this project did not introduce), vanilla 1.6's
    does not -- confirmed by reading it directly. Trying the vanilla anchor
    only when the mod one does not match keeps this working against either
    base without having to be told which one a given .upt actually is.
    """
    if "hfp-ag" in text:
        return None                      # already done
    if ANCHOR_BT in text:
        return text.replace(ANCHOR_BT, INSERT_BT, 1)
    if ANCHOR_BT_VANILLA in text:
        return text.replace(ANCHOR_BT_VANILLA, INSERT_BT_VANILLA, 1)
    return None


ANCHOR_MOUNT = 'mount -o sync -t ubifs /dev/${ubi_name}_0 $mount_path'
INSERT_MOUNT = 'mount -o noatime -t ubifs /dev/${ubi_name}_0 $mount_path'


def patch_mount_script(text):
    """Return the patched mount script, or None if it needs no change."""
    if ANCHOR_MOUNT not in text:
        return None
    return text.replace(ANCHOR_MOUNT, INSERT_MOUNT)

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

# Performance tuning, applied once at startup.
#
# Read-ahead: the card ships at 128 KB. Library scans and large FLAC reads are
# sequential, and on a slow SD card a bigger window is most of the difference
# between smooth and stuttering. 2 MB costs a little RAM for a lot of
# throughput.
for q in /sys/block/mmcblk*/queue/read_ahead_kb; do
    [ -w "$q" ] && echo 2048 > "$q"
done

# vfs_cache_pressure: at the default 100 the kernel reclaims dentries and
# inodes as eagerly as page cache. With 56 MB of RAM that means the library UI
# keeps re-reading the same directory metadata and cover paths. Halving it
# keeps them resident longer without pinning them outright.
[ -w /proc/sys/vm/vfs_cache_pressure ] && echo 50 > /proc/sys/vm/vfs_cache_pressure
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


# Vanilla 1.6's usr/bin/hiby_player.sh has none of the structure
# patch_script() patches -- no MAX_CRASHES, no LD_PRELOAD, no crash counting.
# It is thirteen bare lines: kill any stale hiby_player, start batd if
# present, run the player once, reboot when it exits. There is no anchor to
# key an incremental patch off, so build_vanilla_supervisor() below
# constructs the complete replacement directly instead, reusing the mod's
# own already-shipped supervisor loop (as it existed in a mod-based
# yetisoldier Audiobook Mod install) as the reference for the parts that are
# proven -- the crash-counting shape,
# DEV_HOOK_GIVEUP, HEALTHY_RUN -- with three differences: vanilla's own batd
# preamble is kept rather than introduced, HOOK_LIB is dropped entirely (the
# yetisoldier Audiobook Mod's own separate tile hijack has no reason to exist
# against a vanilla base -- Library has its own Audiobooks section, reached
# through the same DEV_HOOK), and the performance tuning that used to be a
# separate INSERT_CONFIG patch step is folded straight in rather than
# layered on afterward.
VANILLA_ANCHOR_PREAMBLE_END = '/usr/bin/batd -v -s -t5 -o /mnt/sd_0/batlog.txt &\nfi\n'

VANILLA_SUPERVISOR_BODY = '''PLAYER="/usr/bin/hiby_player"
CRASH_COUNT=0
MAX_CRASHES=5

# --- Library/Podcasts additions ---------------------------------------------
# The rootfs is read-only squashfs, so there is otherwise no way to start user
# code at boot or to load a preload without reflashing. DEV_HOOK reads from
# /usr/data, which is writable and survives a firmware update.
#
# DEV_HOOK is dropped after DEV_HOOK_GIVEUP consecutive crashes so that a
# broken library degrades to a working stock player instead of a reboot loop.
DEV_HOOK="/usr/data/libpodcast_hook.so"
DEV_HOOK_GIVEUP=2
HEALTHY_RUN=60          # seconds up before a run counts as a success
USER_INIT="/usr/data/init.sh"

[ -f "$USER_INIT" ] && sh "$USER_INIT" >/dev/null 2>&1 &

# Performance tuning, applied once at startup.
#
# Read-ahead: the card ships at 128 KB. Library scans and large FLAC reads are
# sequential, and on a slow SD card a bigger window is most of the difference
# between smooth and stuttering. 2 MB costs a little RAM for a lot of
# throughput.
for q in /sys/block/mmcblk*/queue/read_ahead_kb; do
    [ -w "$q" ] && echo 2048 > "$q"
done

# vfs_cache_pressure: at the default 100 the kernel reclaims dentries and
# inodes as eagerly as page cache. With 56 MB of RAM that means the library UI
# keeps re-reading the same directory metadata and cover paths. Halving it
# keeps them resident longer without pinning them outright.
[ -w /proc/sys/vm/vfs_cache_pressure ] && echo 50 > /proc/sys/vm/vfs_cache_pressure
# -----------------------------------------------------------------------------

while true; do
    if [ "$CRASH_COUNT" -lt "$DEV_HOOK_GIVEUP" ] && [ -f "$DEV_HOOK" ]; then
        LD_PRELOAD="$DEV_HOOK" "$PLAYER" &
    else
        "$PLAYER" &
    fi
    HP_PID=$!
    STARTED=$(date +%s)
    wait "$HP_PID" 2>/dev/null

    # The counter is meant to catch a hook that cannot survive startup, so it
    # has to measure *consecutive* failures. A player that stayed up long
    # enough to be useful is evidence the hook is fine.
    if [ $(( $(date +%s) - STARTED )) -ge "$HEALTHY_RUN" ]; then
        CRASH_COUNT=0
    else
        CRASH_COUNT=$((CRASH_COUNT + 1))
    fi
    if [ "$CRASH_COUNT" -ge "$MAX_CRASHES" ]; then
        reboot
    fi
    sleep 1
done
'''


STANDALONE_SUPERVISOR_BODY = '''BINARY="{binary_path}"
CRASH_COUNT=0
MAX_CRASHES=5
HEALTHY_RUN=60          # seconds up before a run counts as a success

# --- Library/Podcasts additions ---------------------------------------------
# RP1: no hiby_player at all. BINARY is a real, independent executable (not
# an LD_PRELOAD hook, so there is nothing to inject into another process's
# address space) living on /usr/data, which is writable and survives a
# firmware update -- the same reason the old preloaded hook .so used to live
# there too. A missing BINARY just means CRASH_COUNT climbs to MAX_CRASHES
# and the device reboots rather than spinning silently, the same failure
# shape the old hook-based supervisor already had for a missing hook.
#
# Mounting the SD card was never any init.d script's job or this script's
# own -- grepped the whole stock rootfs for it and came up empty. It happens
# inside the hiby_player *binary* itself at startup (confirmed live: without
# it, dmesg never shows "exFAT-fs ... mounted successfully", every path
# under /data/mnt/sd_0 stat()s as missing, and the library shows real album
# names -- index.c's own stale cache -- with every album reporting 0 tracks,
# since tracks_query() skips any row whose file doesn't actually exist).
# With hiby_player gone, nothing else will ever do this, so it has to happen
# here. -t exfat matches this card's real format exactly (confirmed via the
# same dmesg line); mount is idempotent enough in practice that a harmless
# "already mounted" on a crash-restart is fine to ignore.
mount -t exfat /dev/mmcblk0p1 /data/mnt/sd_0 2>/dev/null

USER_INIT="/usr/data/init.sh"

[ -f "$USER_INIT" ] && sh "$USER_INIT" >/dev/null 2>&1 &

# Performance tuning, applied once at startup -- same as the old hook-based
# supervisor's own copy of this block, kept verbatim since Library benefits
# from it regardless of what launches it.
for q in /sys/block/mmcblk*/queue/read_ahead_kb; do
    [ -w "$q" ] && echo 2048 > "$q"
done
[ -w /proc/sys/vm/vfs_cache_pressure ] && echo 50 > /proc/sys/vm/vfs_cache_pressure
# -----------------------------------------------------------------------------

while true; do
    if [ -x "$BINARY" ]; then
        "$BINARY" &
    else
        sleep 5 &   # nothing to run; still counts as a fast "crash" below
    fi
    B_PID=$!
    STARTED=$(date +%s)
    wait "$B_PID" 2>/dev/null

    if [ $(( $(date +%s) - STARTED )) -ge "$HEALTHY_RUN" ]; then
        CRASH_COUNT=0
    else
        CRASH_COUNT=$((CRASH_COUNT + 1))
    fi
    if [ "$CRASH_COUNT" -ge "$MAX_CRASHES" ]; then
        reboot
    fi
    sleep 1
done
'''


def build_standalone_supervisor(original_text, binary_path):
    """RP1: full-replacement supervisor that launches `binary_path` (an
    on-device path, e.g. /usr/data/library_standalone) directly in a
    crash-counted loop -- no hiby_player, no DEV_HOOK/LD_PRELOAD at all.

    Reuses the same batd-preamble anchor build_vanilla_supervisor() keys
    off, since the input this is written against is the same bare vanilla
    hiby_player.sh either way -- battery monitoring has nothing to do with
    which player runs after it, so that preamble is kept unmodified.
    Returns None if the anchor isn't found, same "stop rather than guess"
    contract every other supervisor-shape function here follows.
    """
    if VANILLA_ANCHOR_PREAMBLE_END not in original_text:
        return None
    preamble = original_text.split(VANILLA_ANCHOR_PREAMBLE_END)[0] + VANILLA_ANCHOR_PREAMBLE_END
    body = STANDALONE_SUPERVISOR_BODY.format(binary_path=binary_path)
    return preamble + "\n" + body


def build_vanilla_supervisor(original_text):
    """Full-replacement supervisor for a bare vanilla hiby_player.sh.

    Returns None if this is not that script -- already has MAX_CRASHES (a
    mod-based image; patch_script() handles that case), or is missing the
    batd-preamble anchor this was written against, in which case the caller
    should stop rather than silently produce a half-built script.
    """
    if "MAX_CRASHES" in original_text:
        return None
    if VANILLA_ANCHOR_PREAMBLE_END not in original_text:
        return None
    preamble = original_text.split(VANILLA_ANCHOR_PREAMBLE_END)[0] + VANILLA_ANCHOR_PREAMBLE_END
    return preamble + "\n" + VANILLA_SUPERVISOR_BODY


# Settings -> About only shows because Podcasts took the top-level slot that
# used to say About; flipping this is what puts it back, one level in.
# Present verbatim on both stock 1.6 and the mod's 2.0.26 (the mod already
# flips it itself, which is why this is a no-op there -- confirmed by reading
# both directly), so this one function is correct against either base.
SET_FUNCTIONS_FILES = ("usr/resource/set_functions.json",
                       "usr/resource/midi_set_functions.json")


def enable_about_tile(root):
    """Flip about:0 -> 1 in every set_functions.json this rootfs has."""
    changed = []
    for rel in SET_FUNCTIONS_FILES:
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            continue
        with open(path) as fh:
            text = fh.read()
        if '"about":0' not in text:
            continue                     # already enabled, or key not present
        with open(path, "w") as fh:
            fh.write(text.replace('"about":0', '"about":1'))
        changed.append(rel)
    return changed


# Stock 1.6 ships /etc/init.d/T90adb (adbd start/stop, dispatched to S310adb
# or S440adb depending on the USB gadget interface) but rcS only runs S??*
# scripts at boot, so T90adb is never invoked and the device boots with no
# ADB at all -- confirmed empirically: the first vanilla-based flash of this
# project booted fine but never enumerated over USB. S90adb is a wrapper this
# project wrote (not HiBy's, not the mod's) that starts adb through T90adb's
# own S310adb/S440adb when USB working mode is Auto/Device, with retries for
# a host that didn't finish enumerating; it has shipped inside every mod-base
# .upt used by this project since before patch_firmware.py existed in its
# current form, which is why the mod code path never had to install it. S310
# and S440 themselves are confirmed byte-identical between stock 1.6 and the
# mod base, so only the wrapper is missing here, not any underlying support.
S90ADB_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "S90adb")


def install_boot_adb(root):
    """Add the boot-time ADB wrapper if this rootfs doesn't already have one."""
    dest = os.path.join(root, "etc/init.d/S90adb")
    if os.path.exists(dest):
        return False                     # mod-based image already carries it
    with open(S90ADB_SRC, "rb") as fh:
        data = fh.read()
    with open(dest, "wb") as fh:
        fh.write(data)
    os.chmod(dest, 0o755)
    return True


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
                f"by hand, using ANCHOR_CONFIG/ANCHOR_LAUNCH/ANCHOR_COUNT above "
                f"as the reference for what each anchor expects.")
    # STARTED has to be set before wait, so the counter anchor absorbs both.
    text = text.replace(ANCHOR_CONFIG, INSERT_CONFIG)
    text = text.replace(ANCHOR_LAUNCH, INSERT_LAUNCH)
    text = text.replace(ANCHOR_COUNT, INSERT_COUNT)
    return text


def detect_format(iso):
    """'mod' (the D0000001/... chunk layout every prior release used) or
    'stock' (the older OTA_V0/... layout vanilla 1.6 uses -- confirmed by
    extracting a real stock 1.6 image directly, not assumed)."""
    for path, name in (("/D0000001", "mod"), ("/OTA_V0", "stock")):
        try:
            next(iso.list_children(iso_path=path))
            return name
        except Exception:
            continue
    die("unrecognised .upt layout -- neither /D0000001 (mod) nor /OTA_V0 "
        "(stock) found. Is this really an R1 firmware image?")


def read_images_ota_v0(iso):
    """Stock/vanilla layout: OTA_V0/OTA_UPDA.IN manifest, OTA_V0/OTA_MD5_.<xxx>
    per-image ordered chunk-digest lists (xxx = the image's own md5, first 3
    hex chars, uppercase -- same convention read_images() already documents
    for the mod format's F0000002/3.BIN, just keyed by name here instead of
    position), ROOTFS_S.<xxx>/XIMAGE_0.<xxx> chunk files.

    xxx in a chunk's own filename is NOT its own digest -- confirmed
    empirically against a real stock 1.6 image: chunk 0's suffix is the whole
    image's own digest prefix, and chunk N>0's is chunk N-1's -- the same
    verification-chain idea write_upt() already implements for the other
    format, just discovered independently here rather than assumed to carry
    over. Chunks are matched to the ordered digest list by each chunk's own
    real md5, same as read_images() does; names are for the chain, not
    lookup.
    """
    def rd(path):
        b = io.BytesIO()
        iso.get_file_from_iso_fp(b, iso_path=path)
        return b.getvalue()

    def list_dir(path):
        return sorted(c.file_identifier().decode()
                     for c in iso.list_children(iso_path=path)
                     if c.file_identifier() not in (b'.', b'..'))

    manifest = rd("/OTA_V0/OTA_UPDA.IN;1").decode()
    entries = re.findall(r"img_type=(\S+)\s+img_name=(\S+)\s+"
                         r"img_size=(\d+)\s+img_md5=([0-9a-f]+)", manifest)
    if not entries:
        die("could not parse the image manifest — is this an R1 .upt?")

    names = list_dir("/OTA_V0")
    chunk_files = [n for n in names if n.startswith("ROOTFS_S.") or n.startswith("XIMAGE_0.")]
    by_own_md5 = {}
    for n in chunk_files:
        data = rd(f"/OTA_V0/{n}")
        by_own_md5[hashlib.md5(data).hexdigest()] = data

    images = []
    for img_type, name, size, md5 in entries:
        prefix = md5[:3].upper()
        dfile = next((n for n in names if n.upper().startswith(f"OTA_MD5_.{prefix}")), None)
        if dfile is None:
            die(f"no OTA_MD5_ digest list found for {name} (prefix {prefix}) "
                f"— image is corrupt or the layout has changed")
        digest_list = rd(f"/OTA_V0/{dfile}").decode().split()
        try:
            data = b"".join(by_own_md5[d] for d in digest_list)
        except KeyError as e:
            die(f"{name}: a chunk digest in {dfile} matches no chunk file "
                f"on disk ({e}) — image is corrupt or the layout has changed")
        got = hashlib.md5(data).hexdigest()
        if got != md5 or len(data) != int(size):
            die(f"{name}: reassembled {len(data)} bytes md5={got}, "
                f"manifest says {size} bytes md5={md5}")
        images.append({"type": img_type, "name": name, "data": data})
    return manifest, entries, images


# 8.3 base name per image type -- fixed strings, not derived from img_name,
# confirmed against the real stock image (every rootfs chunk is ROOTFS_S.xxx
# regardless of index, every kernel chunk XIMAGE_0.xxx).
OTA_V0_BASE_NAME = {"rootfs": "ROOTFS_S", "kernel": "XIMAGE_0"}


def write_upt_ota_v0(out_path, images):
    """Rebuild a stock-format .upt. See read_images_ota_v0() for the naming
    scheme this reproduces. Same Joliet(3)/Rock Ridge(1.09) requirement as
    the mod format's write_upt() -- confirmed present on the stock image too,
    by direct inspection (iso.has_joliet()/has_rock_ridge()) -- and the same
    reasoning applies: this is not cosmetic, the device's updater will not
    navigate an image missing them. Real names below (ota_update.in,
    ota_md5_..., ota_config.in, ...) are likewise taken from the stock
    image's own Rock Ridge records, not guessed.
    """
    import pycdlib
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=1, vol_ident="CDROM",
            joliet=3, rock_ridge="1.09")
    iso.add_directory("/OTA_V0", joliet_path="/ota_v0", rr_name="ota_v0")

    used_iso_paths = set()

    def add(data, iso_path, real, joliet_dir="/ota_v0/"):
        # The chunk suffix is 3 hex chars of a content digest -- only 4096
        # slots, and a real rootfs runs ~70 chunks, so a same-directory
        # collision is a real birthday-bound risk, not a hypothetical: it hit
        # on the very first build of a rootfs whose content differs from the
        # vendor's own (pycdlib doesn't error on a duplicate 8.3 name, it
        # silently drops the earlier record, corrupting the image). ISO9660
        # version numbers exist precisely to let two entries share a short
        # name, so bump the version on a collision rather than invent a
        # naming scheme of our own -- the Rock Ridge real name, which is what
        # actually carries the chain semantics, is untouched either way.
        base, _, version = iso_path.rpartition(";")
        v = int(version)
        while iso_path in used_iso_paths:
            v += 1
            iso_path = f"{base};{v}"
        used_iso_paths.add(iso_path)
        iso.add_fp(io.BytesIO(data), len(data), iso_path,
                   joliet_path=joliet_dir + real if joliet_dir else "/" + real,
                   rr_name=real)

    def chunks(d):
        return [d[i:i + CHUNK] for i in range(0, len(d), CHUNK)]

    lines = ["ota_version=0", ""]
    for img in images:
        lines.append(f"img_type={img['type']}")
        lines.append(f"img_name={img['name']}")
        lines.append(f"img_size={len(img['data'])}")
        lines.append(f"img_md5={hashlib.md5(img['data']).hexdigest()}")
        lines.append("")
    new_manifest = "\n".join(lines).rstrip("\n") + "\n"
    add(new_manifest.encode(), "/OTA_V0/OTA_UPDA.IN;1", "ota_update.in")

    for img in images:
        base = OTA_V0_BASE_NAME.get(img["type"])
        if base is None:
            die(f"unknown image type {img['type']!r} — no stock chunk-name "
                f"convention known for it; this tool has only ever seen "
                f"'rootfs' and 'kernel'")
        whole = hashlib.md5(img["data"]).hexdigest()
        pieces = chunks(img["data"])
        per_chunk = [hashlib.md5(c).hexdigest() for c in pieces]

        digest_list_text = "".join(d + "\n" for d in per_chunk)
        add(digest_list_text.encode(), f"/OTA_V0/OTA_MD5_.{whole[:3].upper()};1",
            f"ota_md5_{img['name']}.{whole}")

        for k, c in enumerate(pieces):
            chain_digest = whole if k == 0 else per_chunk[k - 1]
            suffix = chain_digest[:3].upper()
            add(c, f"/OTA_V0/{base}.{suffix};1",
                f"{img['name']}.{k:04d}.{chain_digest}")

    add(b"\n", "/OTA_V0/OTA_V0.OK;1", "ota_v0.ok")
    add(b"current_version=0\n", "/OTA_CONF.IN;1", "ota_config.in", joliet_dir="/")
    iso.write(out_path)
    iso.close()


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
    ap.add_argument("--rom-version", default=DEFAULT_ROM_VERSION,
                    help=f"full build string recorded in "
                         f"etc/r1_audiobook_version (default "
                         f"{DEFAULT_ROM_VERSION})")
    ap.add_argument("--no-radio", action="store_true",
                    help="do not surface the Internet radio tile on the "
                         "Stream media screen")
    ap.add_argument("--rom-rev", default="a", metavar="LETTER",
                    help="one character appended to the version shown in "
                         "System -> About; the field is cut to 7 chars and "
                         "'2.0.26' already uses 6 (default: a)")
    ap.add_argument("--kernel", metavar="XIMAGE",
                    help="replace the kernel image (uImage/xImage, u-boot "
                         "legacy header + payload) with this file. Swapped "
                         "in before rootfs patching, so a custom kernel and "
                         "the usual rootfs patches can be applied in one "
                         "pass. The stock (OTA_V0) manifest always rebuilds "
                         "fresh from each image's live data, so no extra "
                         "size/md5 bookkeeping is needed there; the mod "
                         "format's manifest is a text template that has to "
                         "be patched in place, same as the rootfs entry "
                         "already is below.")
    ap.add_argument("--standalone", metavar="DEVICE_PATH", nargs="?",
                    const="/usr/data/library_standalone",
                    help="RP1: replace hiby_player.sh entirely with a loop "
                         "that launches DEVICE_PATH (an on-device path, not "
                         "a local file -- nothing is embedded in the image) "
                         "directly, no hiby_player and no DEV_HOOK/LD_PRELOAD "
                         "at all. Defaults to /usr/data/library_standalone "
                         "if given with no value. Only works against a bare "
                         "vanilla hiby_player.sh -- see "
                         "build_standalone_supervisor()'s own comment.")
    ap.add_argument("--kernel-build-id", metavar="ID",
                    help="stamp usr/resource/kernel_build_id with this "
                         "string (e.g. '4.4.94_r1') -- read by the app's "
                         "About screen in preference to uname(), since a "
                         "custom kernel's real uname()/vermagic has to stay "
                         "exactly stock for the closed-source modules to "
                         "keep loading. Meaningless without --kernel, but "
                         "not required by it.")
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
    fmt = detect_format(iso)

    def rd(path):
        b = io.BytesIO()
        iso.get_file_from_iso_fp(b, iso_path=path)
        return b.getvalue()

    if fmt == "mod":
        manifest, entries, images, last = read_images(iso)
        meta = rd("/D0000001/F0000005.TXT;1")
        version_blob = rd(f"/F{last:07d}.TXT;1")
    else:
        manifest, entries, images = read_images_ota_v0(iso)
        last = None          # the mod format's own F{last}.TXT numbering doesn't apply
        meta = version_blob = None    # written directly by write_upt_ota_v0() instead
    iso.close()
    print(f"detected layout: {fmt}")

    print(f"{args.input}:")
    for img in images:
        print(f"  {img['type']:8s} {img['name']:18s} {len(img['data']):>9} bytes  verified")

    rootfs = next((i for i in images if i["type"] == "rootfs"), None)
    if rootfs is None:
        die("no rootfs image in this .upt")

    if args.kernel:
        kernel = next((i for i in images if i["type"] == "kernel"), None)
        if kernel is None:
            die("no kernel image in this .upt -- --kernel has nothing to replace")
        if not os.path.exists(args.kernel):
            die(f"{args.kernel} not found")
        with open(args.kernel, "rb") as fh:
            new_kernel = fh.read()
        if new_kernel[:4] != b"\x27\x05\x19\x56":   # u-boot legacy image magic
            die(f"{args.kernel} does not start with the u-boot legacy image "
                f"magic (0x27051956) -- this doesn't look like a real xImage/"
                f"uImage. Refusing to write something the device's bootloader "
                f"would reject.")
        print(f"replacing kernel: {len(kernel['data'])} -> {len(new_kernel)} bytes")
        kernel["data"] = new_kernel

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
            die(f"{SCRIPT} not found — this does not look like a real R1 "
                f"rootfs at all.")

        with open(target, "r") as fh:
            original = fh.read()

        if args.standalone:
            # A full replacement, not an incremental patch -- DEV_HOOK/
            # MAX_CRASHES already being present says nothing about whether
            # this can proceed, so that check is skipped entirely here.
            patched = build_standalone_supervisor(original, args.standalone)
            kind = f"standalone ({args.standalone})"
            if patched is None:
                die(f"{SCRIPT} does not match the bare vanilla shape "
                    f"--standalone was written against (missing the batd "
                    f"preamble anchor) -- see build_standalone_supervisor()'s "
                    f"own comment.")
        else:
            if "DEV_HOOK" in original:
                die(f"{SCRIPT} already contains DEV_HOOK — this image is already patched")

            # Two supervisor shapes, routed on whether this is a mod-based
            # image (has the mod's own MAX_CRASHES supervisor to patch
            # incrementally) or a vanilla one (bare stock script, no anchor
            # to patch -- build_vanilla_supervisor() writes the whole
            # replacement instead). Neither function guesses silently: each
            # returns None if the input does not match what it was written
            # against, and that is treated as a hard stop rather than a
            # best-effort patch, for the same reason patch_script() always
            # has -- a half-applied patch to init would be a brick rather
            # than a bug.
            if "MAX_CRASHES" in original:
                patched = patch_script(original)
                kind = "mod-based"
            else:
                patched = build_vanilla_supervisor(original)
                kind = "vanilla"
            if patched is None:
                die(f"{SCRIPT} matches neither the mod's supervisor shape nor "
                    f"vanilla's bare one — this firmware's {SCRIPT} has changed "
                    f"from what this tool knows. Patch it by hand, using "
                    f"patch_script() (mod-based) or build_vanilla_supervisor() "
                    f"(vanilla) above as the reference.")
        with open(target, "w") as fh:
            fh.write(patched)
        print(f"patched {SCRIPT} ({kind} base, "
              f"{len(original.splitlines())} -> {len(patched.splitlines())} lines)")

        mtarget = os.path.join(root, MOUNT_SCRIPT)
        if os.path.exists(mtarget):
            with open(mtarget, "r") as fh:
                mtext = fh.read()
            mpatched = patch_mount_script(mtext)
            if mpatched is None:
                print(f"note: {MOUNT_SCRIPT} not patched — no '-o sync' mount "
                      f"found, leaving it alone")
            else:
                with open(mtarget, "w") as fh:
                    fh.write(mpatched)
                print(f"patched {MOUNT_SCRIPT} (/usr/data: sync -> noatime)")

        btarget = os.path.join(root, BT_INIT)
        if os.path.exists(btarget):
            with open(btarget, "r") as fh:
                btext = fh.read()
            bpatched = patch_bt_init(btext)
            if bpatched is None:
                print(f"note: {BT_INIT} not patched — HFP already on, or the "
                      f"bluealsa line has moved")
            else:
                with open(btarget, "w") as fh:
                    fh.write(bpatched)
                print(f"patched {BT_INIT} (bluealsa: +hfp-ag for battery reporting)")

        about = enable_about_tile(root)
        if about:
            print(f"enabled Settings -> About in {len(about)} file(s)")
        else:
            print("note: About already enabled, or set_functions.json missing")

        if install_boot_adb(root):
            print("installed etc/init.d/S90adb (boot ADB — stock rcS never "
                  "runs T90adb, so a vanilla base boots with no ADB otherwise)")
        else:
            print("note: boot ADB already present (mod base carries its own)")

        # Version stamp, so the build is identifiable from the device itself.
        if not args.no_radio:
            radio = enable_internet_radio(root)
            if radio:
                print(f"enabled Internet radio in {len(radio)} theme layout(s)")
            else:
                print("note: Internet radio already enabled, or layouts missing")

        ctarget = os.path.join(root, CONFIG_JSON)
        if os.path.exists(ctarget):
            with open(ctarget) as fh:
                ctext = fh.read()
            stamped = stamp_config_json(ctext, args.rom_rev[:1])
            if stamped is None:
                print(f"note: {CONFIG_JSON} version not stamped (already "
                      f"stamped, or no room in {ABOUT_VERSION_MAX} chars)")
            else:
                with open(ctarget, "w") as fh:
                    fh.write(stamped)
                shown = re.search(r'"version"\s*:\s*"([^"]+)"', stamped).group(1)
                print(f"stamped About version -> '{shown}'")

        vtarget = os.path.join(root, VERSION_FILE)
        if os.path.exists(vtarget):
            with open(vtarget) as fh:
                vtext = fh.read()
            vnew = stamp_version_file(vtext, args.rom_version)
            if vnew is not None:
                with open(vtarget, "w") as fh:
                    fh.write(vnew)
                print(f"stamped {VERSION_FILE} with podcast_rom={args.rom_version}")

        if args.kernel_build_id:
            kbtarget = os.path.join(root, "usr/resource/kernel_build_id")
            with open(kbtarget, "w") as fh:
                fh.write(args.kernel_build_id + "\n")
            print(f"stamped usr/resource/kernel_build_id -> "
                  f"'{args.kernel_build_id}'")

        print("repacking rootfs...")
        newsq = os.path.join(tmp, "rootfs.new.squashfs")
        subprocess.run(["mksquashfs", root, newsq, "-comp", "lzo", "-b", "131072",
                        "-no-exports", "-all-root", "-noappend", "-no-progress"],
                       check=True, stdout=subprocess.DEVNULL)
        with open(newsq, "rb") as fh:
            rootfs["data"] = fh.read()

    print("building .upt...")
    if fmt == "mod":
        # The rootfs almost always changes size, so the manifest text has to
        # follow it -- write_upt() takes the manifest as text and patches it
        # in place, matching how it was read. A replaced kernel (--kernel)
        # needs exactly the same treatment, for the same reason.
        changed = {"rootfs": rootfs}
        if args.kernel:
            changed["kernel"] = kernel
        for img_type, name, size, md5 in entries:
            img = changed.get(img_type)
            if img is not None:
                manifest = manifest.replace(f"img_size={size}",
                                            f"img_size={len(img['data'])}")
                manifest = manifest.replace(f"img_md5={md5}",
                                            f"img_md5={hashlib.md5(img['data']).hexdigest()}")
        last = write_upt(args.output, images, manifest, meta, version_blob, last)
        print(f"\nwrote {args.output} ({os.path.getsize(args.output)} bytes, "
              f"last entry F{last:07d}.TXT)")
    else:
        # write_upt_ota_v0() rebuilds OTA_UPDA.IN fresh from each image's own
        # (possibly just-changed) data, so there is no old manifest text to
        # patch in place here -- unlike the mod format, size/md5 are never
        # stale to begin with.
        write_upt_ota_v0(args.output, images)
        print(f"\nwrote {args.output} ({os.path.getsize(args.output)} bytes)")
    print("\nVerify it before flashing:  ./tools/verify_firmware.py " + args.output)


if __name__ == "__main__":
    main()
