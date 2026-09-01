# Extract podcast search results from the iTunes Search API's JSON response.
#
#   result <TAB> <collection name> <TAB> <artist name> <TAB> <feed URL>
#
# One line in, one line out: apple's API ships each element of "results" as
# a single complete JSON object on its own line (verified against a live
# response, not assumed) -- no brace-depth tracking needed, unlike
# parse_rss.awk's XML, which this otherwise mirrors the shape of.

# No strtonum() here -- a gawk extension the device's busybox awk does not
# have (same reason parse_rss.awk's entity_value() hand-rolls its own hex
# parse instead of using it).
function hex4(h,   i, d, v, ch) {
    v = 0
    for (i = 1; i <= 4; i++) {
        ch = tolower(substr(h, i, 1))
        d = index("0123456789abcdef", ch) - 1
        if (d < 0) return -1
        v = v * 16 + d
    }
    return v
}

function json_unescape(s,   out, i, c, n, hex, cp) {
    out = ""
    n = length(s)
    for (i = 1; i <= n; i++) {
        c = substr(s, i, 1)
        if (c == "\\" && i < n) {
            i++
            c = substr(s, i, 1)
            if (c == "n")      out = out " "
            else if (c == "t") out = out " "
            else if (c == "r") out = out ""
            else if (c == "u" && i + 4 <= n) {
                hex = substr(s, i + 1, 4)
                i += 4
                cp = hex4(hex)
                # Surrogate pairs and anything past the BMP are rare in a
                # podcast title -- drop rather than emit a broken sequence.
                if (cp >= 32 && cp < 128) out = out sprintf("%c", cp)
                else if (cp >= 128 && cp < 2048) out = out sprintf("%c%c", 192 + int(cp / 64), 128 + (cp % 64))
                else if (cp >= 2048 && cp < 55296) out = out sprintf("%c%c%c", 224 + int(cp / 4096), 128 + int(cp / 64) % 64, 128 + (cp % 64))
            }
            else out = out c    # \" \\ \/ and anything else: literal
        } else {
            out = out c
        }
    }
    return out
}

# Pull "key":"value" out of a line, honouring \" so a quote inside the
# value does not end the match early. Returns "" if the key is absent.
function json_field(line, key,   pat, start, i, c, prev, val) {
    pat = "\"" key "\":\""
    if (!match(line, pat)) return ""
    start = RSTART + RLENGTH
    val = ""
    prev = ""
    for (i = start; i <= length(line); i++) {
        c = substr(line, i, 1)
        if (c == "\"" && prev != "\\") break
        # A literal backslash-backslash must not make the next quote look
        # escaped -- track whether *this* backslash is itself escaped.
        if (c == "\\" && prev == "\\") prev = ""
        else prev = c
        val = val c
    }
    return json_unescape(val)
}

/"feedUrl"/ {
    name   = json_field($0, "collectionName")
    artist = json_field($0, "artistName")
    url    = json_field($0, "feedUrl")
    if (url == "") next
    if (name == "") name = "Untitled"
    print "result\t" name "\t" artist "\t" url
}
