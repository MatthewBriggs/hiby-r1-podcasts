# Extract podcast metadata from an RSS feed.
# Splitting on "<" makes each record a tag plus its text, which works whether or
# not the feed ships pretty-printed.
#
#   channel <TAB> <feed title>
#   image   <TAB> <artwork url>
#   episode <TAB> <episode title> <TAB> <enclosure url> <TAB> <YYYY-MM-DD HH:MM:SS>
#
# Episodes come out in document order, i.e. newest first.

BEGIN {
    RS = "<"
    chan = ""; img = ""
    in_item = 0
    want_text = ""          # which field the next CDATA/text record belongs to
    ep_title = ""; ep_url = ""; ep_date = ""
}

# Decodes once; see unescape_full for feeds that escape twice.
function unescape(s) {
    gsub(/&quot;/,  "\"", s)
    gsub(/&apos;/,  "'",  s)
    gsub(/&#39;/,   "'",  s)
    gsub(/&#34;/,   "\"", s)
    gsub(/&lt;/,    "(",  s)
    gsub(/&gt;/,    ")",  s)
    gsub(/&nbsp;/,  " ",  s)
    gsub(/&amp;/,   "\\&", s)   # last, so other entities decode first
    return s
}

function unescape_full(s) {
    s = unescape(s)
    if (s ~ /&(amp|quot|apos|lt|gt|#3[49]);/) s = unescape(s)
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
    }
    ep_title = ""; ep_url = ""; ep_date = ""
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
    if (in_item) flush_item()
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
