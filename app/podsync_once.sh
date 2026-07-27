#!/bin/sh
# podsync_once.sh — fetch new episodes for every subscribed feed, once.
#
# Invoked by the Podcasts app's UPDATE button; there is no daemon. Feeds are one
# URL per line in feeds.txt on the SD card, so subscriptions can be edited by
# plugging the card in or using the player's WiFi transfer mode.
#
# busybox wget cannot be used: its TLS offers only legacy ciphers and every
# modern podcast host rejects the handshake. A static curl and a CA bundle live
# alongside this script.

BASE=/data/mnt/sd_0/.podsync
FEEDS="$BASE/feeds.txt"
CURL="$BASE/curl"
CA="$BASE/cacert.pem"
PARSER="$BASE/parse_rss.awk"
DEST=/data/mnt/sd_0/Audiobooks
LOG=/tmp/.podsync_run.log
TMP=/tmp/podsync

EPISODES_PER_FEED=3

echo "" > "$LOG"
log() { echo "$*" >> "$LOG"; }

log "starting update"

[ -f "$FEEDS" ]  || { log "no feeds.txt"; log "__DONE__"; exit 1; }
[ -f "$CURL" ]   || { log "curl missing"; log "__DONE__"; exit 1; }
chmod +x "$CURL" 2>/dev/null

mkdir -p "$TMP" "$DEST"

# Sanitise for FAT32 while staying readable: the app shows filenames verbatim.
sanitize() {
    echo "$1" | sed -e 's/:/ -/g' -e 's/["<>|?*\/\\]//g' -e 's/[[:cntrl:]]//g' \
                    -e 's/  */ /g' -e 's/^[ .-]*//' -e 's/[ .]*$//' | cut -c1-70
}

get() { "$CURL" -fsSL --cacert "$CA" --connect-timeout 20 --max-time 900 -o "$2" "$1" 2>>"$LOG"; }

total_new=0

grep -v '^[[:space:]]*#' "$FEEDS" 2>/dev/null | grep -v '^[[:space:]]*$' | while read -r url; do
    url=$(echo "$url" | tr -d '\r')
    [ -z "$url" ] && continue

    rss="$TMP/feed.rss"
    rm -f "$rss"
    if ! get "$url" "$rss" || [ ! -s "$rss" ]; then
        log "FAILED to fetch feed"
        continue
    fi

    awk -f "$PARSER" "$rss" > "$TMP/parsed.txt" 2>>"$LOG"

    raw=$(grep '^channel	' "$TMP/parsed.txt" | head -1 | cut -f2-)
    name=$(sanitize "$raw")
    [ -z "$name" ] && name="Unknown Podcast"
    dir="$DEST/$name"
    mkdir -p "$dir"
    log "$name"

    # Cover art, if the feed offers one and we do not already have it.
    img=$(grep '^image	' "$TMP/parsed.txt" | head -1 | cut -f2-)
    if [ -n "$img" ] && [ ! -f "$dir/cover.jpg" ]; then
        get "$img" "$dir/cover.jpg" || rm -f "$dir/cover.jpg"
    fi

    grep '^episode	' "$TMP/parsed.txt" | head -"$EPISODES_PER_FEED" > "$TMP/eps.txt"

    while IFS='	' read -r _tag title epurl pubdate; do
        [ -z "$epurl" ] && continue

        ext=$(echo "$epurl" | sed -e 's/[?#].*//' -e 's/.*\.//')
        case "$ext" in
            mp3|m4a|m4b|aac|ogg|oga|opus|wav|flac) : ;;
            *) ext=mp3 ;;
        esac

        base=$(sanitize "$title")
        [ -z "$base" ] && base=$(echo "$epurl" | sed -e 's/[?#].*//' -e 's/.*\///')
        out="$dir/$base.$ext"

        [ -f "$out" ] && continue

        log "  downloading $base"
        if get "$epurl" "$out.part" && [ -s "$out.part" ]; then
            mv "$out.part" "$out"
            # Stamp the publication date onto the file so the app can sort by it.
            # Download order is the reverse of publication order, so mtime would
            # otherwise put the newest episode last.
            [ -n "$pubdate" ] && touch -d "$pubdate" "$out" 2>/dev/null
            total_new=$((total_new + 1))
            log "  ok $base"
        else
            rm -f "$out.part"
            log "  FAILED $base"
        fi
    done < "$TMP/eps.txt"
done

log "update complete"
log "__DONE__"
