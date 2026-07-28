# Podcasts for the HiBy R1

A podcast client that runs **inside** the R1's own music player, on the launcher
tile where **About** used to be.

The R1 is a £70 MIPS DAP with 56 MB of usable RAM, a read-only squashfs rootfs
and no app model whatsoever. This adds one by `LD_PRELOAD`ing a shared object
into `hiby_player` and re-pointing a launcher tile's callback at it. Nothing is
patched on disk; the stock binary is untouched.

<p align="center">
  <img src="docs/screenshots/01-launcher-tiles.png" width="240" alt="Launcher with the Podcasts tile">
  <img src="docs/screenshots/02-feeds.png" width="240" alt="Feed list">
  <img src="docs/screenshots/05-playing.png" width="240" alt="Now playing">
</p>

## What it does

- **Subscriptions** in a plain text file on the SD card, one RSS URL per line.
- **Updates on demand** — tap `UPDATE FEEDS`. No daemon, nothing scheduled.
  The fetcher's log streams to the screen as it runs.
- **Plays MP3** with real transport: `-30 / -10 / play-pause / +10 / +30`.
- **Speed 1.0–2.0×** with pitch preserved (WSOLA time-stretching).
- **Resume positions** persist per episode, across reboots.
- **Show notes**, scrollable, pulled from the feed.
- **Cover art** per feed.
- **Volume** on the hardware keys, wired or Bluetooth.
- **Screen lock** with the power key while audio keeps playing.
- Full Latin text — accents, umlauts, the lot — rendered with a TrueType font
  already on the device.

<p align="center">
  <img src="docs/screenshots/03-episodes.png" width="190" alt="Episode list">
  <img src="docs/screenshots/08-speed.png" width="190" alt="Speed control">
  <img src="docs/screenshots/04-volume.png" width="190" alt="Volume overlay">
  <img src="docs/screenshots/06-update.png" width="190" alt="Update running">
</p>

## Requirements

- A HiBy R1 running
  [yetisoldier's Audiobook Mod](https://github.com/yetisoldier/Hiby-R1-Audiobook-Mod)
  — verified against **2.0.25** and **2.0.26**. That mod provides the
  `LD_PRELOAD` supervisor this app hooks into, and its `.upt` is the input to
  the patcher below. You need the `.upt` file itself, not just a modded device.
- An SD card.
- ADB over USB, and **willingness to reflash** — see [Install](#install).
- On the host: Python with `pycdlib`, and `squashfs-tools`, to patch the image.

> The tile addresses are specific to the `hiby_player` binary they were read
> out of. On a different firmware build they will point somewhere else, and the
> app declines to arm itself rather than guess — see
> [Porting](#porting-to-another-firmware-build).

## Install

There are two halves to this, and the first one is a firmware flash. The app is
an `LD_PRELOAD` library, and what loads it is `/usr/bin/hiby_player.sh` — which
lives on a read-only squashfs and **cannot be pushed to the device**. It has to
go in through a firmware image.

### 1. Patch your firmware

Take the `.upt` for whichever Audiobook Mod release you run and patch it. The
tool never redistributes anyone's firmware; it reads yours and writes a new one:

```bash
pip install pycdlib && brew install squashfs   # or your distro's squashfs-tools
```

```bash
./tools/patch_firmware.py r1-audiobooks-2.0.26.upt r1-podcast-2.0.26.upt
```

Check it before you flash anything. This verifies every digest the updater
checks, and diffs the rootfs against the original so you can see that exactly
one file changed:

```bash
./tools/verify_firmware.py r1-podcast-2.0.26.upt --against r1-audiobooks-2.0.26.upt
```

It must end with `RESULT: image verifies`. The one changed file is
`usr/bin/hiby_player.sh`; the patch applied is
[`app/hiby_player.sh`](app/hiby_player.sh), kept in the repo for reference.

Flash it the same way you flashed the mod — copy it to the SD card and use the
player's firmware update — then come back for the second half.

> The patcher matches the mod's supervisor exactly and stops if it does not
> recognise it, rather than guessing. If a future release changes that script,
> patch it by hand from `app/hiby_player.sh`.

### 2. Install the app

Build (or take `libpodcast_hook.so` from a release), then:

```bash
adb push libpodcast_hook.so /usr/data/libpodcast_hook.so && adb shell chmod 755 /usr/data/libpodcast_hook.so
```

```bash
adb shell mkdir -p /usr/data/podcast_res && adb push icon/out/about.png icon/out/about_s.png /usr/data/podcast_res/
```

The fetcher and its dependencies go on the card. `curl` is a **static mipsel
build you must supply** — busybox's `wget` offers only legacy TLS ciphers and
every modern podcast host rejects the handshake, which is the single most
annoying thing about this device:

```bash
adb shell mkdir -p /data/mnt/sd_0/.podsync && adb push app/podsync_once.sh app/parse_rss.awk /data/mnt/sd_0/.podsync/
```

The device has **no CA store at all** and `/etc` is read-only, so curl needs a
bundle on the card. Take Mozilla's, as published by the curl project — it is
dated, maintained, and freely redistributable, which a system trust store copied
off your own machine is not:

```bash
curl -fLo cacert.pem https://curl.se/ca/cacert.pem
```

```bash
adb push curl cacert.pem /data/mnt/sd_0/.podsync/ && adb shell chmod 755 /data/mnt/sd_0/.podsync/curl
```

Refresh it once in a while. Root certificates get rotated and a stale bundle
fails as a TLS error against one feed at a time, which reads like a broken feed
rather than an expired trust store.

Then write your subscriptions and reboot:

```bash
adb push app/feeds.txt /data/mnt/sd_0/.podsync/feeds.txt && adb shell sync && adb shell reboot
```

The tile comes back as **Podcasts**, with a microphone icon.

## Using it

Subscriptions live at `/data/mnt/sd_0/.podsync/feeds.txt` — one RSS URL per
line, `#` for comments. Edit it by pulling the card, over ADB, or through the
player's own WiFi transfer mode, then hit `UPDATE FEEDS`.

Episodes land in `/data/mnt/sd_0/Podcasts/<Feed Name>/`, three per feed by
default (`EPISODES_PER_FEED` in `podsync_once.sh`). Each episode's publication
date is stamped onto the file's mtime so the list sorts by publication rather
than download order — the fetcher walks a feed newest-first, so download order
is backwards.

Nothing is ever deleted automatically. Delete episodes yourself.

## Building

Needs [Zig](https://ziglang.org/) as the cross-compiler — no toolchain to
install, and it targets the R1's ancient glibc directly:

```bash
cd app && ./build.sh
```

The launcher icon is generated, not drawn by hand:

```bash
python3 icon/make_icon.py icon/out
```

## How it works

The short version; [STATUS.md](STATUS.md) has the full account.

- **Getting in.** A launcher tile's name string and its callback live in
  different 96-byte `.data` records — for a name at `S`, the callback is at
  `S + 0x48`. The callback may **not** point into the injected `.so` or the
  launcher silently fails to render at all, so a MIPS trampoline is written
  into a zeroed `.rodata` code cave and the tile is pointed there.
- **Owning the screen.** The app takes the framebuffer (`mmap /dev/fb0`, page
  flipping via `FBIOPAN_DISPLAY`) and `EVIOCGRAB`s the touch and key nodes, so
  input does not leak through to the launcher underneath. It runs its own frame
  loop rather than blocking the player's UI thread.
- **Renaming the tile.** Labels come from `settings.ini`, not `launcher.ini`.
  The rootfs is read-only, so the app rewrites one string in the firmware's own
  copy at startup and bind-mounts the result over the original. It derives that
  file rather than shipping one, so nothing of HiBy's is redistributed and a
  firmware update cannot leave a stale string table mounted over the new one.
- **Opening an MP3 fast.** `mp3dec_ex_open` scans every frame to compute
  duration — about 5 seconds for an episode. Everything needed is in the first
  few hundred bytes: a Xing/Info header carries the frame count and a seek
  table. Open time went from 4788 ms to 291 ms.
- **Volume.** The CS43131's volume registers are not wired on this device, so
  wired output scales samples in software. Bluetooth is different — bluealsa
  exposes a real mixer, and that gets driven instead.

## Known limitations

- **MP3 only.** No AAC, M4A/M4B, Opus or FLAC — the decoder is minimp3. The
  fetcher will download other formats but they will not play.
- **Very large progressive JPEG covers are skipped.** Baseline JPEGs stream
  cheaply at any size, but progressive decoding cannot stream: libjpeg builds
  the entire coefficient array before yielding a single scanline, and
  `scale_denom` shrinks only the output. A 3000×3000 progressive cover wants
  ~54 MB where roughly 18 MB is free, and this libjpeg is built with the
  `jmemnobs` stub so there is no spill-to-disk. The header is checked first and
  anything that will not fit is declined — that feed shows no art rather than
  taking the player down with it. Since 3000×3000 is Apple's cover requirement,
  expect this on a fair few feeds.
- **Long titles clip** at the right edge instead of ellipsing.
- English `settings.ini` only; other UI languages keep the "About" label.
- The tile addresses are hardcoded per firmware build.

## Porting to another firmware build

`ABOUT_CB_2`, `ABOUT_CB_ORIG` and `CAVE_ADDR` in
[`app/podcast_hook.c`](app/podcast_hook.c) are addresses read out of one
specific `hiby_player`. The constructor verifies the callback holds the value
it expects and refuses to patch anything if it does not, so a mismatched build
degrades to a stock **About** tile instead of a bricked launcher. Check
`/tmp/.podcast_hook.log` — it will say which value it found.

## Licence

MIT — see [LICENSE](LICENSE). Credits and vendored code in
[THIRD_PARTY.md](THIRD_PARTY.md). No HiBy resources are redistributed here.
