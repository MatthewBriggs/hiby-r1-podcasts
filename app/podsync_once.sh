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
DEST=/data/mnt/sd_0/Podcasts
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

# Check connectivity up front. Without this every feed fails with a bare
# "curl: (6) Could not resolve host", which looks like a curl problem rather
# than "the radio is off".
if ! ping -c1 -W3 1.1.1.1 >/dev/null 2>&1; then
    log "NO NETWORK"
    log "Turn on WiFi in Settings, then update again."
    log "__DONE__"
    exit 1
fi
if [ ! -s /etc/resolv.conf ]; then
    log "NO DNS"
    log "WiFi is up but has no DNS server yet. Wait a moment and retry."
    log "__DONE__"
    exit 1
fi

# Sanitise for FAT32 while staying readable: the app shows filenames verbatim.
# NBSP=$(printf '\302\240') — feeds sprinkle U+00A0 through titles and it ends
# up in filenames, where it looks like a space but breaks matching and quoting.
NBSP=$(printf '\302\240')

# busybox `cut -c` counts bytes, not characters — verified on the device — so a
# 70-byte cut can land inside a multi-byte character and leave a dangling
# fragment in the filename, which then draws as a stray '?'. These two rules
# remove only an *incomplete* trailing sequence: a lone lead byte, or a
# three-byte sequence that lost its final continuation. A complete character
# matches neither, because a continuation byte is never in the lead range.
UTF8_LEAD=$(printf '[\300-\377]')
UTF8_PART=$(printf '[\340-\377][\200-\277]')
# Same curly-quote folding the parser does, repeated here so that sanitize() is
# the single normaliser: running it over an already-sanitised name must be a
# no-op, which is what lets adopt_existing below recognise a renamed episode.
SQ=$(printf '\342\200\230\|\342\200\231')
DQ=$(printf '\342\200\234\|\342\200\235')

sanitize() {
    echo "$1" | sed -e "s/$NBSP/ /g" -e "s/$SQ/'/g" -e "s/$DQ/\"/g" \
                    -e 's/:/ -/g' -e 's/["<>|?*\/\\]//g' -e 's/[[:cntrl:]]//g' \
                    -e 's/  */ /g' -e 's/^[ .-]*//' -e 's/[ .]*$//' \
        | cut -c1-70 \
        | sed -e "s/$UTF8_PART\$//" -e "s/$UTF8_LEAD\$//" -e 's/[ .]*$//'
}

# Every change to sanitize() renames what future downloads are called, and an
# episode already on the card then stops being recognised and is fetched a
# second time under the new name — which is exactly what the NBSP fix did to
# Språket. If a file already here normalises to the name we are about to use,
# adopt it instead of downloading it again.
adopt_existing() {
    _dir=$1; _base=$2; _ext=$3
    for _old in "$_dir"/*."$_ext"; do
        [ -f "$_old" ] || continue
        _oldname=${_old##*/}
        _oldbase=${_oldname%.*}
        [ "$_oldbase" = "$_base" ] && return 1
        if [ "$(sanitize "$_oldbase")" = "$_base" ]; then
            mv "$_old" "$_dir/$_base.$_ext"
            [ -f "$_dir/$_oldbase.txt" ] && mv "$_dir/$_oldbase.txt" "$_dir/$_base.txt"
            log "  renamed: $_oldbase"
            return 0
        fi
    done
    return 1
}

get() { "$CURL" -fsSL --cacert "$CA" --connect-timeout 20 --max-time 900 -o "$2" "$1" 2>>"$LOG"; }

total_new=0

# Read the feed list from a file rather than a pipe. `... | while read` runs the
# loop in a subshell, so every count it kept was discarded when the loop ended
# and the run could never report what it had done.
grep -v '^[[:space:]]*#' "$FEEDS" 2>/dev/null | grep -v '^[[:space:]]*$' > "$TMP/feeds.list"

while read -r url; do
    url=$(echo "$url" | tr -d '\r')
    [ -z "$url" ] && continue

    rss="$TMP/feed.rss"
    rm -f "$rss"
    if ! get "$url" "$rss" || [ ! -s "$rss" ]; then
        log "FAILED to fetch feed"
        continue
    fi

    awk -v max="$EPISODES_PER_FEED" -f "$PARSER" "$rss" > "$TMP/parsed.txt" 2>>"$LOG"

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

    # Pair each episode with the notes line that follows it, so the download
    # loop can write show notes alongside the audio.
    awk -F'\t' -v n="$EPISODES_PER_FEED" '
        /^episode\t/ {
            if (have) { print title "\t" url "\t" date "\t" notes; have=0 }
            if (count >= n) { done=1; exit }
            title=$2; url=$3; date=$4; notes=""; have=1; count++
            next
        }
        /^notes\t/ { if (have) notes=$2; next }
        END { if (have) print title "\t" url "\t" date "\t" notes }
    ' "$TMP/parsed.txt" > "$TMP/eps.txt"

    while IFS='	' read -r title epurl pubdate notes; do
        [ -z "$epurl" ] && continue

        ext=$(echo "$epurl" | sed -e 's/[?#].*//' -e 's/.*\.//')
        case "$ext" in
            mp3|m4a|m4b|aac|ogg|oga|opus|wav|flac) : ;;
            *) ext=mp3 ;;
        esac

        base=$(sanitize "$title")
        [ -z "$base" ] && base=$(echo "$epurl" | sed -e 's/[?#].*//' -e 's/.*\///')
        out="$dir/$base.$ext"

        # Write show notes even when the audio is already present, so episodes
        # downloaded before this existed still get them. Rewrite rather than
        # skip when the file exists: the feed is the source of truth, and only
        # overwriting lets an episode fetched by an older, worse parser pick up
        # the corrected text — otherwise a library keeps its mangled entities
        # for as long as the episodes stay on the card.
        if [ -n "$notes" ]; then
            printf '%s\n' "$notes" > "$dir/$base.txt"
        fi

        [ -f "$out" ] && continue
        adopt_existing "$dir" "$base" "$ext" && continue

        log "  downloading $base"
        if get "$epurl" "$out.part" && [ -s "$out.part" ]; then
            mv "$out.part" "$out"
            # Stamp the publication date onto the file so the app can sort by it.
            # Download order is the reverse of publication order, so mtime would
            # otherwise put the newest episode last.
            [ -n "$pubdate" ] && touch -d "$pubdate" "$out" 2>/dev/null
            total_new=$((total_new + 1))
            log "  ok $base"
            # The player decodes MP3 only. Downloading anything else is still
            # worth doing — it can be copied off the card — but silently listing
            # an episode that will never play is worse than saying so.
            [ "$ext" = mp3 ] || log "  note - .$ext will not play, MP3 only"
        else
            rm -f "$out.part"
            log "  FAILED $base"
        fi
    done < "$TMP/eps.txt"
done < "$TMP/feeds.list"

if [ "$total_new" -eq 0 ]; then
    log "update complete - no new episodes"
elif [ "$total_new" -eq 1 ]; then
    log "update complete - 1 new episode"
else
    log "update complete - $total_new new episodes"
fi
log "__DONE__"
