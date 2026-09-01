#!/bin/sh
# podsearch_once.sh — search iTunes's podcast directory for a query typed on
# the app's own T9 keyboard, once. Same shape as podsync_once.sh: all
# network/parsing work happens here, in a subprocess, so the C side only
# ever polls a plain-text result file.
#
# iTunes Search is the source picked over Podcast Index/gpodder/fyyd (the
# other three AntennaPod itself searches) because it needs no API key or
# request signing -- a plain HTTPS GET returning JSON, matching the
# zero-config bar the bundled curl already sets for podsync_once.sh. See
# R65 in BACKLOG.md.

BASE=/data/mnt/sd_0/.podsync
CURL="$BASE/curl"
CA="$BASE/cacert.pem"
PARSER="$BASE/parse_itunes.awk"
LOG=/tmp/.podsearch_run.log
RESULTS=/tmp/.podsearch_results.tsv
TMP=/tmp/podsearch.json

RESULT_LIMIT=20

echo "" > "$LOG"
: > "$RESULTS"
log() { echo "$*" >> "$LOG"; }

QUERY="$1"
if [ -z "$QUERY" ]; then
    log "no query"
    log "__DONE__"
    exit 1
fi

[ -f "$CURL" ] || { log "curl missing"; log "__DONE__"; exit 1; }
chmod +x "$CURL" 2>/dev/null

if ! ping -c1 -W3 1.1.1.1 >/dev/null 2>&1; then
    log "NO NETWORK"
    log "Turn on WiFi in Settings, then search again."
    log "__DONE__"
    exit 1
fi

# curl itself does the percent-encoding for a value passed via --data-urlencode
# to a GET request (-G moves it onto the query string instead of a POST body) --
# simpler and more correct than hand-rolling it in shell for whatever the T9
# keyboard's own printable-ASCII set can produce (BG1's own note: nearly all of
# 0x20-0x7e is reachable, including '&' and '?', which a manual sed encode has
# historically gotten wrong somewhere).
"$CURL" -fsSL --cacert "$CA" --connect-timeout 10 --max-time 20 \
    -G "https://itunes.apple.com/search" \
    --data-urlencode "term=$QUERY" \
    --data-urlencode "media=podcast" \
    --data-urlencode "entity=podcast" \
    --data-urlencode "limit=$RESULT_LIMIT" \
    -o "$TMP" 2>>"$LOG"

if [ ! -s "$TMP" ]; then
    log "FAILED to reach iTunes Search"
    log "__DONE__"
    exit 1
fi

awk -f "$PARSER" "$TMP" > "$RESULTS" 2>>"$LOG"

n=$(wc -l < "$RESULTS" 2>/dev/null)
n=${n:-0}
if [ "$n" -eq 0 ]; then
    log "no results"
else
    log "$n result(s)"
fi
log "__DONE__"
