# Libra for the HiBy R1

One app that turns a stock HiBy R1 into a real platform: music, audiobooks,
parametric EQ, internet radio and podcasts, replacing the stock **About**
tile. It doesn't patch the stock binary — it `LD_PRELOAD`s into `hiby_player`
and re-points a launcher tile's callback at itself.

This used to be two separate apps sharing the device (Libra on Stream
media, a standalone Podcasts app chain-loaded onto About). Podcasts is built
into Libra directly now, so that second app was retired; Libra moved
onto the About tile in its place, and Stream media is untouched, back to its
stock behaviour.

## What you get

Browse by album artist, album, artist or genre straight from the stock media
index; gapless playback; FLAC, MP3, WAV, M4A, OGG Vorbis and Opus; a **Now
Playing** screen with full track detail and output route; a dedicated
**Audiobooks** section (folder-scoped, remembers position); a built-in
**parametric EQ** with EqualizerAPO profile support; **internet radio**
(direct streams and HLS); **podcasts** — subscriptions in a plain text file,
updated on demand, per-episode resume across reboots, scrollable show notes,
cover art per feed; output to jack, USB DAC or Bluetooth chosen
automatically; hardware key mapping that accounts for the unit's mislabeled
buttons; auto screen lock and **auto shutdown** after an idle period; quick
settings pulled down from the status strip; playlists as plain `.m3u` files.

## Requirements

- A HiBy R1. This release targets **vanilla stock firmware 1.6**, unmodified
  — no third-party mod required first. (Earlier, Podcasts-only releases of
  this project targeted
  [yetisoldier's Audiobook Mod](https://github.com/yetisoldier/Hiby-R1-Audiobook-Mod)
  2.0.25/2.0.26; this combined release moves to vanilla so there is only one
  firmware lineage to track.)
- An SD card.
- ADB over USB, and **willingness to reflash** — see [Install](#install).
- On the host: Python with `pycdlib`, and `squashfs-tools`, to patch the
  image; [Zig](https://ziglang.org/) if you're building from source.

> The tile-hijack addresses are read out of one specific `hiby_player`
> binary. On a different firmware build they will point somewhere else, and
> the app declines to arm itself rather than guess — see
> [Porting](#porting-to-another-firmware-build).

## Install

Two halves: a firmware flash, then pushing the app itself. The hook is
loaded by `/usr/bin/hiby_player.sh`, which lives on a read-only squashfs and
**cannot be pushed to the device** — it has to go in through a firmware
image.

### 1. Patch your firmware

Take your `.upt` and patch it. The tool never redistributes anyone's
firmware; it reads yours and writes a new one:

```bash
pip install pycdlib && brew install squashfs   # or your distro's squashfs-tools
```

```bash
./tools/patch_firmware.py r1-1.6.upt r1-combined-0.1.upt
```

Check it before you flash anything. This verifies every digest the updater
checks, and diffs the rootfs against the original so you can see exactly
which files changed:

```bash
./tools/verify_firmware.py r1-combined-0.1.upt --against r1-1.6.upt
```

It must end with `RESULT: image verifies`. The changed files are the
supervisor script (rebuilt from a bare vanilla `hiby_player.sh` —
`build_vanilla_supervisor()` in `tools/patch_firmware.py` is the reference
for its shape), the mount script, `bt_init` (adds Bluetooth headset battery
reporting), the About-tile re-enable, and the internet-radio layout swap.

Flash it the way you'd flash any `.upt` — copy it to the SD card and use the
player's own firmware update — then come back for the second half.

> The patcher recognises a bare vanilla `hiby_player.sh` and a previously
> mod-patched one, and stops rather than guessing if it sees neither. If a
> future firmware release changes that script again, patch it by hand using
> `podcast-app/app/hiby_player.sh` as the reference.

### 2. Install the app

Build it (or take the `.so` file from a release):

```bash
cd music-app/app && ./build.sh
```

The firmware preloads exactly one library from `/usr/data`:

```bash
adb push music-app/app/libmusic_hook.so /usr/data/libpodcast_hook.so
adb shell chmod 755 /usr/data/libpodcast_hook.so
```

(`libpodcast_hook.so` predates this app absorbing Podcasts — the device only
looks at the path, so it's left as-is rather than renamed for cosmetics.)

Push the icon:

```bash
adb push music-app/icon/about.png music-app/icon/about_s.png /usr/data/music_res/
```

Always check the object loads before rebooting; a link error otherwise costs
a boot cycle:

```bash
adb shell 'LD_PRELOAD=/usr/data/libpodcast_hook.so /bin/true'
```

Podcasts needs its fetcher and dependencies on the card — `curl` is a
**static mipsel build you must supply**, since busybox's `wget` only offers
legacy TLS ciphers that every modern podcast host rejects:

```bash
adb shell mkdir -p /data/mnt/sd_0/.podsync
adb push music-app/podsync/podsync_once.sh music-app/podsync/parse_rss.awk /data/mnt/sd_0/.podsync/
adb push music-app/podsync/podsearch_once.sh music-app/podsync/parse_itunes.awk /data/mnt/sd_0/.podsync/
```

The last two are for in-app podcast search (the Search button on the Podcasts screen) — same `curl`/`cacert.pem` below, no separate setup.

The device has **no CA store at all** and `/etc` is read-only, so curl needs
a bundle on the card — Mozilla's, as published by the curl project, is dated,
maintained and freely redistributable, which a copy of your own system trust
store is not:

```bash
curl -fLo cacert.pem https://curl.se/ca/cacert.pem
adb push curl cacert.pem /data/mnt/sd_0/.podsync/
adb shell chmod 755 /data/mnt/sd_0/.podsync/curl
```

Refresh that bundle occasionally — root certificates rotate, and a stale one
fails as a TLS error against one feed at a time, which reads like a broken
feed rather than an expired trust store.

Write your subscriptions, then reboot:

```bash
adb push music-app/podsync/feeds.txt.example /data/mnt/sd_0/.podsync/feeds.txt
adb shell sync && adb shell reboot
```

**About** comes back as **Libra** (music note icon, same position as
stock); **Stream media** is untouched, back to its stock behaviour.

## Using it

Libra needs no configuration to start — point it at your existing music
library and it uses the stock media index. Two plain-text config files are
created on first run:

`/usr/data/music.conf`

```
idle_lock_seconds = 30      # 0 disables the automatic lock
```

`/usr/data/radio_stations.conf`

```
Name | https://example.com/stream.mp3
```

Podcast subscriptions live at `/data/mnt/sd_0/.podsync/feeds.txt` — one RSS
URL per line, `#` for comments. Edit it by pulling the card, over ADB, or
through the player's own WiFi transfer mode, then hit `UPDATE FEEDS`. Episodes
land in `/data/mnt/sd_0/Podcasts/<Feed Name>/`, three per feed by default;
nothing is ever deleted automatically.

## Building

Cross-compiles with [Zig](https://ziglang.org/) — no toolchain to install,
and it targets the R1's ancient glibc directly:

```bash
cd music-app/app && ./build.sh
```

The launcher icon is generated, not drawn by hand:

```bash
python3 music-app/icon/make_icon.py music-app/icon
```

## How it works

- **Getting in.** A launcher tile's name string and its callback live in
  different 96-byte `.data` records — for a name at `S`, the callback is at
  `S + 0x48`. The About tile shows up at two callback addresses in
  `hiby_player`'s data, both patched so it's caught wherever the launcher
  reads it from. The callback may **not** point into the injected `.so` or
  the launcher silently fails to render at all, so a MIPS trampoline is
  written into a zeroed `.rodata` code cave and the tile is pointed there.
- **Owning the screen.** The app takes the framebuffer (`mmap /dev/fb0`,
  page flipping via `FBIOPAN_DISPLAY`) and `EVIOCGRAB`s the touch and key
  nodes while active, so input does not leak through to the launcher
  underneath. It runs its own frame loop rather than blocking the player's
  UI thread.
- **Fail-safe hooking.** The hook checks that the tile callback it's about
  to overwrite still holds the value it expects before touching anything. A
  mismatched firmware build degrades to the stock tile rather than a
  bricked launcher.
- **Volume.** The CS43131's volume registers aren't wired on this device, so
  wired output scales samples in software; Bluetooth is different — bluealsa
  exposes a real mixer, and that gets driven instead.

## Known limitations

- Podcasts plays **MP3 only** — no AAC, M4A/M4B, Opus or FLAC episodes.
- Long titles clip at the right edge instead of ellipsing.
- English UI only; other languages fall back to stock labels where a string
  had to be rewritten (the tile label).
- Tile-hijack addresses are hardcoded per firmware build — see
  [Porting](#porting-to-another-firmware-build).

## Porting to another firmware build

The app reads its tile-hijack addresses out of one specific `hiby_player`:
`ABOUT_CB_1`/`ABOUT_CB_2`/`ABOUT_CB_ORIG`/`CAVE_ADDR` in
[`music-app/app/music_hook.c`](music-app/app/music_hook.c). The constructor
verifies the callback holds the value it expects and refuses to patch
anything if it does not, so a mismatched build degrades to the stock tile
instead of a bricked launcher.

## Licence

MIT — see [LICENSE](LICENSE). Credits and vendored code in
[THIRD_PARTY.md](THIRD_PARTY.md). No HiBy resources are redistributed here.
