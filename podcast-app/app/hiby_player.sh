#!/bin/sh
# r1_player_supervisor.sh ??? replaces stock /usr/bin/hiby_player.sh
#
# LD_PRELOAD in-process audiobook hook supervisor.
#
# Boot ??? supervisor launches stock hiby_player with LD_PRELOAD set to
# libaudiobook_hook.so. The hook .so installs in-process trampolines
# (Hook A: hgl_fb_display suppressor, Hook B: tile-cave trampoline ???
# hook_b). When the user taps the Audiobooks tile, hook_b runs inside
# hiby_player's process, taking over the framebuffer while hiby_player
# stays alive (keeping fb0 + DMA active). No process killing, no fb DMA
# conflict, no touch-screen death.
#
# The supervisor only handles crashes: if hiby_player exits, count and
# reboot after MAX_CRASHES consecutive crashes.

PLAYER="/usr/bin/hiby_player"
HOOK_LIB="/usr/lib/libaudiobook_hook.so"
CRASH_COUNT=0
MAX_CRASHES=5

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

while true; do
    PRELOAD=""
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
    HP_PID=$!
    STARTED=$(date +%s)
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
    if [ "$CRASH_COUNT" -ge "$MAX_CRASHES" ]; then
        reboot
    fi
    sleep 1
done