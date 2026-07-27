# Extract podcast metadata from an RSS feed.
# Splitting on "<" makes each record a tag plus its text, which works whether or
# not the feed ships pretty-printed.
#
#   channel <TAB> <feed title>
#   image   <TAB> <artwork url>
#   episode <TAB> <episode title> <TAB> <enclosure url>
#
# Episodes come out in document order, i.e. newest first.

BEGIN {
    RS = "<"
    chan = ""; img = ""
    in_item = 0
    want_text = ""          # which field the next CDATA/text record belongs to
    ep_title = ""; ep_url = ""
}

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
        print "episode\t" ep_title "\t" ep_url
    }
    ep_title = ""; ep_url = ""
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
            if (v != "") { ep_title = unescape(v) } else { want_text = "ep" }
        }
    } else if (chan == "") {
        if (v != "") { chan = unescape(v) } else { want_text = "chan" }
    }
    next
}

# CDATA and bare text arrive as their own record after an empty-looking tag.
/^!\[CDATA\[/ {
    v = trim(strip_cdata($0))
    if (v != "") {
        if (want_text == "ep" && ep_title == "") ep_title = unescape(v)
        else if (want_text == "chan" && chan == "") chan = unescape(v)
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
        img = u
    }
    next
}

/^url>/ {
    if (img == "" && !in_item) {
        v = trim(substr($0, 5))
        if (v ~ /^https?:\/\/.*\.(jpg|jpeg|png)/) img = v
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
