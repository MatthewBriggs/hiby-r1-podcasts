#!/bin/sh
# init.sh — run at boot by the player supervisor (/usr/bin/hiby_player.sh).
#
# The rootfs is read-only squashfs, so the launcher's About tile keeps its stock
# icon and label unless they are shadowed. mount --bind does that without
# reflashing; the mounts do not survive a reboot, which is why this runs here.
# It must finish before hiby_player reads its resources — bind mounts take
# milliseconds and the player spends seconds starting up.

RES=/usr/data/podcast_res

for theme in theme1 theme2; do
    d=/usr/resource/litegui/$theme/launcher
    [ -f "$RES/about.png" ]   && [ -f "$d/about.png" ]   && mount --bind "$RES/about.png"   "$d/about.png"
    [ -f "$RES/about_s.png" ] && [ -f "$d/about_s.png" ] && mount --bind "$RES/about_s.png" "$d/about_s.png"
done

# Rename the tile: <abo_dev>About</abo_dev> becomes Podcasts.
[ -f "$RES/launcher.ini" ] && mount --bind "$RES/launcher.ini" /usr/resource/str/english/launcher.ini

exit 0
