#!/bin/sh
# Install a build on the device.
#
# The supervisor loads DEV_HOOK, and DEV_HOOK is /usr/data/libpodcast_hook.so
# — not libmusic_hook.so, which is only what the file is called here. Pushing
# to the obvious name puts a perfectly good build somewhere nothing reads it,
# and the device then goes on running whatever it had before while every
# symptom looks like a bug in the new code. That cost three wasted debugging
# rounds on 2026-08-01. Always deploy through this script.
set -e

# adb is not on PATH in every shell here; fall back to where the Homebrew
# command-line tools put it.
ADB="${ADB:-adb}"
command -v "$ADB" >/dev/null 2>&1 ||
    ADB=/opt/homebrew/share/android-commandlinetools/platform-tools/adb
SO="${1:-$(dirname "$0")/../app/libmusic_hook.so}"
TARGET=/usr/data/libpodcast_hook.so
FONT="$(dirname "$0")/../fonts/font.ttf"
FONT_TARGET=/usr/data/podcast_res/font.ttf

[ -f "$SO" ] || { echo "no such build: $SO" >&2; exit 1; }

# R35: text.c looks for the UI font here first. Push it whenever it's out of
# date on the device, same md5-verify pattern as the .so, so a font update
# can't silently no-op.
if [ -f "$FONT" ]; then
    fwant=$(md5 -q "$FONT" 2>/dev/null || md5sum "$FONT" | cut -d' ' -f1)
    fgot=$($ADB shell "md5sum $FONT_TARGET 2>/dev/null" | tr -d '\r' | cut -d' ' -f1)
    if [ "$fwant" != "$fgot" ]; then
        $ADB shell "mkdir -p $(dirname "$FONT_TARGET")"
        $ADB push "$FONT" "$FONT_TARGET" >/dev/null
        echo "pushed font.ttf"
    fi
fi

$ADB push "$SO" "$TARGET.new" >/dev/null
echo "pushed $(basename "$SO")"

# A library that cannot link means hiby_player crash-loops and the supervisor
# drops the hook entirely after two short runs. Check before installing.
if ! $ADB shell "LD_PRELOAD=$TARGET.new /bin/true 2>&1; echo rc=\$?" | tr -d '\r' | grep -q '^rc=0$'; then
    echo "will not link — leaving the running build alone:" >&2
    $ADB shell "LD_PRELOAD=$TARGET.new /bin/true 2>&1" >&2
    $ADB shell "rm -f $TARGET.new"
    exit 1
fi
echo "links clean"

$ADB shell "mv -f $TARGET.new $TARGET && chmod 755 $TARGET"

# Prove what is on the device is what was built, rather than assuming the
# push landed.
want=$(md5 -q "$SO" 2>/dev/null || md5sum "$SO" | cut -d' ' -f1)
got=$($ADB shell "md5sum $TARGET" | tr -d '\r' | cut -d' ' -f1)
[ "$want" = "$got" ] || { echo "md5 mismatch: built $want, device $got" >&2; exit 1; }
echo "verified $got"

# Reboot, do not killall. The HGL graphics DMA pool is never reclaimed when
# hiby_player exits, so every restart leaks a slice of it. After enough of
# them allocation fails and the UI freezes with the process still alive and
# the screen still lit — taps simply stop working, which reads as a hang in
# the app rather than as the cost of the last deploy. Rebooting also clears
# CRASH_COUNT, which lives in the supervisor's shell and is the only thing
# that restores a hook it has given up on.
if [ "$2" = "--restart" ]; then
    $ADB shell 'killall hiby_player' || true
    echo "player restarted (leaks HGL DMA — reboot before testing properly)"
else
    $ADB shell 'sync; reboot'
    echo "rebooting"
fi
