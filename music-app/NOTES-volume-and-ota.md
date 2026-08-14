# Two design notes

## R4 — one volume control instead of two

### Why there are two

There are genuinely two gain stages, and they are not the same thing:

1. **The firmware's own volume.** The stock UI's volume, driven by the side
   buttons. On the 3.5 mm output this does *not* reach the DAC: the CS43131's
   volume registers are not wired on this board — writes report success, read
   back zero and change nothing. The stock player therefore scales samples in
   software too.
2. **This app's volume.** Also software scaling, applied in the decode loop,
   with a squared curve so the low end is usable.

On Bluetooth neither of those applies: bluealsa owns a real mixer (0–127), and
that is what the headset's own buttons move.

So today: wired output is scaled twice, once by each. Bluetooth is scaled once,
by the mixer, and this app deliberately passes samples through unscaled.

### The options

**(a) Drop this app's software volume; use the firmware's.** Cleanest in
principle. The problem is that nothing in the app can *read* the firmware's
level — it lives in `hiby_player`'s own state, not in a mixer or a sysfs node
this code can see — so the app could no longer show a number or offer a slider.

**(b) Drop the firmware's; own it entirely.** The app already grabs the volume
keys while it is open, so the firmware's volume never moves during playback.
Its residual value is whatever it was left at, which is a fixed multiplier the
app cannot see or compensate for. Fixable only by forcing it to a known value
at startup — which means finding where it is stored.

**(c) Keep both but make one of them a no-op.** Set the firmware's volume to
maximum once at app entry, then use the app's alone. One control, one number,
no unreadable multiplier in the chain. Costs one write at startup and needs the
storage location; `/usr/data/user.ini` is the likely home.

**Recommendation: (c), then re-evaluate.** It gives a single number that means
something, keeps the slider, and does not depend on reading the firmware's
state continuously. The work is finding where the stock volume is persisted and
confirming that writing it does not upset the stock player.

**Worth noting:** on the bit-perfect path this matters more than it looks.
Software scaling is what stops the wired output being bit-perfect at anything
below 100% — the samples are multiplied before they reach the DAC. If
bit-perfect playback is the point, the honest answer is that volume should be
100% and the level set at the amplifier.

## R7 — OTA updates from GitHub

### What is being updated

Two quite different things, and they deserve different mechanisms:

- **The app** — `libmusic_hook.so`, one file in `/usr/data`. Swappable in
  place, takes effect on the next launch of `hiby_player`. Low risk: a bad
  file is caught before it is used (see below) and the old one can be kept.
- **The firmware** — a 40 MB `.upt`, flashed by the stock updater from the SD
  card root. High risk, slow, and already has a working manual path.

Only the first is worth automating. Firmware should stay a deliberate act.

### Sketch

1. **Feed.** `https://api.github.com/repos/<user>/<repo>/releases/latest`, over
   the static curl already on the card (the device's own TLS is too old).
   Compare the tag against a version baked into the build.
2. **Fetch** the `.so` asset to `/usr/data/update/` — never over the running
   file.
3. **Verify before trusting.** A SHA-256 published in the release body, checked
   against the download. Better still, a detached signature: the device holds a
   public key, and an unsigned or mis-signed asset is refused. Without this,
   an OTA channel is a remote-code-execution channel.
4. **Prove it loads.** `LD_PRELOAD=<new>.so /bin/true` before installing it —
   exactly the check that has caught a broken build by hand more than once
   today. A library that cannot link means `hiby_player` crash-loops and the
   supervisor reboots the device.
5. **Install and keep a way back.** Move the current file to `.prev`, move the
   new one into place. On startup, if the hook has crashed more than N times,
   the supervisor should restore `.prev`. The crash counter already exists in
   `hiby_player.sh`; it currently reboots after five, and could roll back
   instead.
6. **Tell the user.** A line in the app's own settings — "update available,
   install?" — rather than anything automatic. Downloads on a battery device
   over someone's tethered phone should be asked for.

### What I would not do

- No automatic firmware flashing. The stock updater needs the file at the card
  root as `r1.upt` and a manual menu action; leaving a `.upt` there also makes
  the device offer the update on every boot until it is deleted.
- No silent installs. The value here is fixing bugs quickly, not shipping
  changes someone did not ask for.
