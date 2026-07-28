# Extract podcast metadata from an RSS feed.
# Splitting on "<" makes each record a tag plus its text, which works whether or
# not the feed ships pretty-printed.
#
#   channel <TAB> <feed title>
#   image   <TAB> <artwork url>
#   episode <TAB> <episode title> <TAB> <enclosure url> <TAB> <YYYY-MM-DD HH:MM:SS>
#   notes   <TAB> <episode description, tags stripped, one line>
#
# A notes line follows the episode line it belongs to.
#
# Episodes come out in document order, i.e. newest first.

# Pass -v max=N to stop after N episodes. Feeds routinely carry 500-800 items
# and the caller only wants the newest few; parsing the rest costs minutes of
# string work on this device.
BEGIN {
    RS = "<"
    emitted = 0
    chan = ""; img = ""
    in_item = 0
    want_text = ""          # which field the next CDATA/text record belongs to
    ep_title = ""; ep_url = ""; ep_date = ""; ep_desc = ""
}

# Decodes once; see unescape_full for feeds that escape twice.
# Numeric character references, which some feeds use for nearly all punctuation
# — The Incomparable emits &#8217; several thousand times in one feed. Left alone
# they reach the screen verbatim. The renderer already carries the Latin and
# General Punctuation glyphs these decode to, so this was the missing half.
function cp_to_utf8(c) {
    if (c < 128)  return sprintf("%c", c)
    if (c < 2048) return sprintf("%c%c", 192 + int(c / 64), 128 + (c % 64))
    return sprintf("%c%c%c", 224 + int(c / 4096), 128 + int(c / 64) % 64, 128 + (c % 64))
}

function entity_value(e,   t, v, i, ch, d) {
    t = substr(e, 3, length(e) - 3)          # strip the leading &# and the ;
    if (t ~ /^[xX]/) {
        t = substr(t, 2)
        if (t == "") return 0
        v = 0
        for (i = 1; i <= length(t); i++) {
            ch = tolower(substr(t, i, 1))
            d = index("0123456789abcdef", ch) - 1
            if (d < 0) return 0
            v = v * 16 + d
        }
        return v
    }
    if (t ~ /^[0-9]+$/) return t + 0
    return 0
}

function decode_numeric(s,   out, pre, body, c) {
    out = ""
    while (match(s, /&#[0-9]+;|&#[xX][0-9a-fA-F]+;/)) {
        pre  = substr(s, 1, RSTART - 1)
        body = substr(s, RSTART, RLENGTH)
        s    = substr(s, RSTART + RLENGTH)
        c = entity_value(body)
        # Control characters get stripped downstream anyway, and anything past
        # the BMP needs a fourth byte this does not emit. Leave both as they
        # came rather than writing something malformed into a filename.
        #
        # Spelled out rather than folded into a ternary inside the concatenation:
        # busybox awk fails to resolve a user function called from there and
        # aborts the whole feed with "Call to undefined function".
        if (c >= 32 && c <= 65535) body = cp_to_utf8(c)
        out = out pre body
    }
    return out s
}

function unescape(s) {
    gsub(/&quot;/,  "\"", s)
    gsub(/&apos;/,  "'",  s)
    gsub(/&#39;/,   "'",  s)     # kept explicit so the two commonest survive
    gsub(/&#34;/,   "\"", s)     # even where sprintf("%c") cannot be trusted
    s = decode_numeric(s)
    gsub(/&lt;/,    "(",  s)
    gsub(/&gt;/,    ")",  s)
    gsub(/&nbsp;/,  " ",  s)
    gsub(/&amp;/,   "\\&", s)   # last, so other entities decode first
    return s
}

function unescape_full(s) {
    s = unescape(s)
    if (s ~ /&(amp|quot|apos|lt|gt|nbsp);|&#[0-9xX]/) s = unescape(s)
    return s
}

function trim(s) {
    sub(/^[ \t\r\n]+/, "", s)
    sub(/[ \t\r\n]+$/, "", s)
    return s
}

function strip_cdata(s) {
    sub(/^!\[CDATA\[/, "", s)
    sub(/\]\]>.*$/, "", s)
    return s
}

# Emit a finished episode. Requires a URL; the title is optional.
function flush_item() {
    if (ep_url != "") {
        print "episode\t" ep_title "\t" ep_url "\t" ep_date
        if (ep_desc != "") print "notes\t" ep_desc
        emitted++
    }
    ep_title = ""; ep_url = ""; ep_date = ""; ep_desc = ""
    if (max > 0 && emitted >= max) {
        # Channel details always precede the items, so they are known by now.
        print "channel\t" (chan == "" ? "Unknown Podcast" : chan)
        if (img != "") print "image\t" img
        exit
    }
}

/^item>/ {
    if (in_item) flush_item()      # tolerate feeds that omit </item>
    in_item = 1
    want_text = ""
    next
}

/^\/item>/ {
    if (in_item) flush_item()
    in_item = 0
    want_text = ""
    next
}

/^title>/ {
    v = trim(substr($0, 7))
    if (in_item) {
        if (ep_title == "") {
            if (v != "") { ep_title = unescape_full(v) } else { want_text = "ep" }
        }
    } else if (chan == "") {
        if (v != "") { chan = unescape_full(v) } else { want_text = "chan" }
    }
    next
}

# CDATA and bare text arrive as their own record after an empty-looking tag.
/^!\[CDATA\[/ {
    v = trim(strip_cdata($0))
    if (v != "") {
        if (want_text == "ep" && ep_title == "") ep_title = unescape_full(v)
        else if (want_text == "chan" && chan == "") chan = unescape_full(v)
        else if (want_text == "desc" && ep_desc == "") ep_desc = clean_html(v)
    }
    want_text = ""
    next
}

# Channel artwork: <itunes:image href="..."> or <image><url>...</url></image>.
/^itunes:image/ {
    if (img == "" && !in_item && match($0, /href[ \t]*=[ \t]*"[^"]*"/)) {
        u = substr($0, RSTART, RLENGTH)
        sub(/^href[ \t]*=[ \t]*"/, "", u)
        sub(/"$/, "", u)
        img = unescape(u)
    }
    next
}

/^url>/ {
    if (img == "" && !in_item) {
        v = trim(substr($0, 5))
        if (v ~ /^https?:\/\/.*\.(jpg|jpeg|png)/) img = unescape(v)
    }
    next
}

# Attribute order varies between feeds, so match the attribute, not a position.
/^enclosure/ {
    if (ep_url == "") {
        if (match($0, /url[ \t]*=[ \t]*"[^"]*"/)) {
            u = substr($0, RSTART, RLENGTH)
            sub(/^url[ \t]*=[ \t]*"/, "", u)
            sub(/"$/, "", u)
        } else if (match($0, /url[ \t]*=[ \t]*'[^']*'/)) {
            u = substr($0, RSTART, RLENGTH)
            sub(/^url[ \t]*=[ \t]*'/, "", u)
            sub(/'$/, "", u)
        } else {
            u = ""
        }
        if (u != "") ep_url = unescape(u)
    }
    next
}

END {
    if (max > 0 && emitted >= max) exit      # already printed on the way out
    if (in_item) flush_item()
    if (max > 0 && emitted >= max) exit
    print "channel\t" (chan == "" ? "Unknown Podcast" : chan)
    if (img != "") print "image\t" img
}

# RFC-822 ("Mon, 21 Jul 2026 05:00:00 GMT") to a form busybox `touch -d` accepts.
# The leading day name is optional and the zone is ignored; only ordering matters.
function to_iso(v,   parts, n, i, mon, day, year, tm, mi) {
    n = split(v, parts, /[ \t]+/)
    mi = 0
    for (i = 1; i <= n; i++)
        if (parts[i] ~ /^(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)$/) { mi = i; break }
    if (mi < 2 || mi + 2 > n) return ""
    mon = int(index("JanFebMarAprMayJunJulAugSepOctNovDec", parts[mi]) / 3) + 1
    day  = parts[mi - 1] + 0
    year = parts[mi + 1] + 0
    tm   = parts[mi + 2]
    if (tm !~ /^[0-9][0-9]:[0-9][0-9]/) tm = "00:00:00"
    if (length(tm) == 5) tm = tm ":00"
    if (year < 1970 || day < 1 || day > 31) return ""
    return sprintf("%04d-%02d-%02d %s", year, mon, day, tm)
}

/^pubDate>/ {
    if (in_item && ep_date == "") ep_date = to_iso(trim(substr($0, 9)))
    next
}

# Show notes. Feeds put them in <description> or <itunes:summary>, usually as
# escaped or CDATA-wrapped HTML, so strip tags and collapse whitespace.
function clean_html(v) {
    gsub(/\]\]>/, " ", v)          # CDATA terminator, left by the collector
    gsub(/<!\[CDATA\[/, " ", v)
    gsub(/<[^>]*>/, " ", v)
    v = unescape_full(v)
    gsub(/<[^>]*>/, " ", v)        # entities can reveal a second layer of tags
    gsub(/[\r\n\t]+/, " ", v)
    gsub(/  +/, " ", v)
    return trim(v)
}


# Descriptions span several records: splitting on "<" cuts the CDATA marker away
# from its content whenever the content itself starts with a tag, which is the
# normal case (<![CDATA[<p>...). So accumulate from the opening tag to the
# closing one and clean the lot at the end.
collecting != "" {
    # index() against a precomputed string, not a dynamic regex: RS="<" turns a
    # 2 MB feed into ~100k records and recompiling a regex for each one is
    # pathologically slow on this device.
    if (index($0, close_tag) == 1) {
        if (in_item && ep_desc == "") ep_desc = clean_html(desc_buf)
        collecting = ""; close_tag = ""; desc_buf = ""
    } else if (length(desc_buf) < 8000) {
        desc_buf = desc_buf "<" $0
    }
    next
}

/^description>/ {
    if (in_item && ep_desc == "") {
        collecting = "description"; close_tag = "/description>"
        desc_buf = substr($0, 13)
    }
    next
}

/^itunes:summary>/ {
    if (in_item && ep_desc == "") {
        collecting = "itunes:summary"; close_tag = "/itunes:summary>"
        desc_buf = substr($0, 16)
    }
    next
}
