/* wps.c — see wps.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <ctype.h>

#include "wps.h"
#include "bmp.h"
#include "text.h"

/* ---- model --------------------------------------------------------------- */

enum { TOK_TEXT, TOK_TAG, TOK_COND };

typedef struct wps_tok wps_tok;
typedef struct { wps_tok *t; int n; } wps_toks;

struct wps_tok {
    int      type;
    char     tag[8];        /* "it", "xd", "pb", ... */
    char    *text;          /* TOK_TEXT literal, or a tag's raw (arg) string */
    wps_toks branch[WPS_MAX_BRANCH];   /* TOK_COND */
    int      nbranch;
};

typedef struct {
    wps_toks toks;
    int      vp;            /* index into theme->vp */
    int      align;         /* 'l', 'c', 'r' */
} wps_line;

typedef struct {
    char id;                /* 0 = the default viewport */
    int  x, y, w, h;
    int  conditional;       /* declared %Vl: only drawn when %Vd names it */
    int  shown;             /* set per-frame by %Vd */
} wps_vp;

typedef struct {
    char  id[4];
    bmp_t bm;
    int   x, y, frames;
    int   loaded;
} wps_img;

typedef struct { int x, y, w, h; char action[24]; int vp; } wps_touch;

struct wps_theme {
    char       name[64];
    bmp_t      backdrop;
    int        has_backdrop;
    wps_img    img[WPS_MAX_IMAGES];
    int        nimg;
    wps_vp     vp[WPS_MAX_VIEWPORTS];
    int        nvp;
    wps_line   line[WPS_MAX_LINES];
    int        nline;
    wps_touch  touch[WPS_MAX_TOUCH];
    int        ntouch;
    /* %Cl album-art placement */
    int        art_x, art_y, art_w, art_h, has_art_slot;
};

/* ---- parsing ------------------------------------------------------------- */

/* Reads a %tag's name: letters, plus the handful of two-letter forms the
 * language uses. Longest-match is not needed -- Rockbox tags are 1-3 chars
 * and always terminated by '(', '<', or a non-alnum. */
static const char *read_tag(const char *s, char *out, size_t n) {
    size_t i = 0;
    while (*s && (isalpha((unsigned char)*s) || *s == '?') && i < n - 1) {
        /* '?' only ever leads a conditional and is handled by the caller */
        if (*s == '?' && i > 0) break;
        out[i++] = *s++;
        /* Rockbox tag names are at most three letters; stopping here keeps
         * "%itSomething" from swallowing the literal text after the tag. */
        if (i >= 3) break;
    }
    out[i] = '\0';
    return s;
}

/* Copies a bracketed argument list, honouring the %( %) escapes the language
 * uses for literal parentheses inside arguments. */
static const char *read_args(const char *s, char **out) {
    if (*s != '(') { *out = NULL; return s; }
    s++;
    char buf[256];
    size_t i = 0;
    int depth = 1;
    while (*s && i < sizeof(buf) - 1) {
        if (*s == '%' && (s[1] == '(' || s[1] == ')')) { buf[i++] = s[1]; s += 2; continue; }
        if (*s == '(') depth++;
        if (*s == ')') { depth--; if (!depth) { s++; break; } }
        buf[i++] = *s++;
    }
    buf[i] = '\0';
    *out = strdup(buf);
    return s;
}

static const char *parse_toks(const char *s, wps_toks *out, int stop_at_branch);

/* %?tag<a|b|c> -- each branch is itself a token list. */
static const char *parse_cond(const char *s, wps_tok *tk) {
    tk->type = TOK_COND;
    s = read_tag(s, tk->tag, sizeof(tk->tag));
    s = read_args(s, &tk->text);          /* e.g. %?vg(show_vol)< */
    if (*s != '<') return s;
    s++;
    tk->nbranch = 0;
    while (*s && tk->nbranch < WPS_MAX_BRANCH) {
        s = parse_toks(s, &tk->branch[tk->nbranch], 1);
        tk->nbranch++;
        if (*s == '|') { s++; continue; }
        if (*s == '>') { s++; break; }
        break;
    }
    return s;
}

/* Parses until end-of-line, or (inside a conditional) until | or >. */
static const char *parse_toks(const char *s, wps_toks *out, int stop_at_branch) {
    wps_tok *list = calloc(WPS_MAX_TOKENS, sizeof(wps_tok));
    int n = 0;
    char lit[256];
    size_t li = 0;

    while (*s && *s != '\n' && n < WPS_MAX_TOKENS - 1) {
        if (stop_at_branch && (*s == '|' || *s == '>')) break;

        if (*s == '%') {
            /* escapes first: %% is a literal percent, %( and %) literal parens */
            if (s[1] == '%' || s[1] == '(' || s[1] == ')') {
                if (li < sizeof(lit) - 1) lit[li++] = s[1];
                s += 2;
                continue;
            }
            /* flush pending literal */
            if (li) {
                lit[li] = '\0';
                list[n].type = TOK_TEXT;
                list[n].text = strdup(lit);
                n++; li = 0;
                if (n >= WPS_MAX_TOKENS - 1) break;
            }
            s++;
            if (*s == '?') { s++; s = parse_cond(s, &list[n]); n++; continue; }
            list[n].type = TOK_TAG;
            s = read_tag(s, list[n].tag, sizeof(list[n].tag));
            s = read_args(s, &list[n].text);
            n++;
            continue;
        }
        if (li < sizeof(lit) - 1) lit[li++] = *s;
        s++;
    }
    if (li) {
        lit[li] = '\0';
        list[n].type = TOK_TEXT;
        list[n].text = strdup(lit);
        n++;
    }
    out->t = list;
    out->n = n;
    return s;
}

static int vp_find(wps_theme *t, char id) {
    for (int i = 0; i < t->nvp; i++) if (t->vp[i].id == id) return i;
    return -1;
}

static int img_find(wps_theme *t, const char *id) {
    for (int i = 0; i < t->nimg; i++) if (!strcmp(t->img[i].id, id)) return i;
    return -1;
}

/* Splits "a,b,c" into up to max fields. Rockbox uses '-' to mean "default",
 * which the caller distinguishes by the returned string, not a sentinel. */
static int split(char *s, char **f, int max) {
    int n = 0;
    if (!s) return 0;
    while (*s && n < max) {
        f[n++] = s;
        char *c = strchr(s, ',');
        if (!c) break;
        *c = '\0';
        s = c + 1;
    }
    return n;
}

static int num(const char *s, int dflt) {
    if (!s || !*s || *s == '-') {
        /* "-" means "inherit"; a real negative number is still a number */
        if (s && s[0] == '-' && isdigit((unsigned char)s[1])) return atoi(s);
        return dflt;
    }
    return atoi(s);
}

/* Resolves a theme-relative path. Rockbox writes them rooted at the card
 * ("/.rockbox/wps/x.bmp"); bare names are relative to the .wps's own image
 * directory, which is <wps dir>/<theme name>/. */
static void resolve(char *out, size_t n, const char *root,
                    const char *imgdir, const char *ref) {
    if (ref[0] == '/') snprintf(out, n, "%s%s", root, ref);
    else               snprintf(out, n, "%s/%s", imgdir, ref);
}

static void load_image(wps_theme *t, const char *root, const char *imgdir,
                       const char *args) {
    if (!args || t->nimg >= WPS_MAX_IMAGES) return;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", args);
    char *f[8];
    int n = split(tmp, f, 8);
    if (n < 4) return;

    wps_img *im = &t->img[t->nimg];
    snprintf(im->id, sizeof(im->id), "%s", f[0]);
    char path[512];
    resolve(path, sizeof(path), root, imgdir, f[1]);
    if (bmp_load(path, &im->bm) != 0) return;    /* not fatal: theme draws without it */
    im->x = num(f[2], 0);
    im->y = num(f[3], 0);
    im->frames = (n >= 5) ? num(f[4], 1) : 1;
    if (im->frames < 1) im->frames = 1;
    im->loaded = 1;
    t->nimg++;
}

static void add_viewport(wps_theme *t, const char *args, int conditional) {
    if (t->nvp >= WPS_MAX_VIEWPORTS) return;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", args ? args : "");
    char *f[8];
    int n = split(tmp, f, 8);
    wps_vp *v = &t->vp[t->nvp];
    memset(v, 0, sizeof(*v));
    int i = 0;
    if (conditional) { v->id = f[0][0]; i = 1; }
    v->x = (n > i)     ? num(f[i], 0)     : 0;
    v->y = (n > i + 1) ? num(f[i + 1], 0) : 0;
    v->w = (n > i + 2) ? num(f[i + 2], 0) : 0;
    v->h = (n > i + 3) ? num(f[i + 3], 0) : 0;
    v->conditional = conditional;
    t->nvp++;
}

static void add_touch(wps_theme *t, const char *args, int vp) {
    if (!args || t->ntouch >= WPS_MAX_TOUCH) return;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", args);
    char *f[8];
    int n = split(tmp, f, 8);
    if (n < 5) return;
    /* Two forms: %T(x,y,w,h,action) and %T(label,x,y,w,h,action). The second
     * names a region so %Tl can test it later; the label is not a coordinate,
     * so the fields shift by one. */
    int i = isdigit((unsigned char)f[0][0]) ? 0 : 1;
    if (n < i + 5) return;
    wps_touch *tc = &t->touch[t->ntouch];
    tc->x = num(f[i], 0);
    tc->y = num(f[i + 1], 0);
    tc->w = num(f[i + 2], 0);
    tc->h = num(f[i + 3], 0);
    snprintf(tc->action, sizeof(tc->action), "%s", f[i + 4]);
    tc->vp = vp;
    t->ntouch++;
}

/* Tags that configure the theme rather than draw anything. Handled while
 * parsing so the render pass only ever walks drawing tokens. */
static int is_setup_tag(const char *tag) {
    return !strcmp(tag, "X")  || !strcmp(tag, "xl") || !strcmp(tag, "Vl") ||
           !strcmp(tag, "V")  || !strcmp(tag, "T")  || !strcmp(tag, "Cl") ||
           !strcmp(tag, "wd") || !strcmp(tag, "we") || !strcmp(tag, "Vi");
}

static void parse_wps(wps_theme *t, const char *path, const char *root,
                      const char *imgdir) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    /* Viewport 0 is the implicit full-screen one every line belongs to until
     * a %V or %Vl says otherwise. */
    t->vp[0].id = 0; t->vp[0].x = 0; t->vp[0].y = 0;
    t->vp[0].w = 0;  t->vp[0].h = 0; t->vp[0].conditional = 0;
    t->nvp = 1;
    int cur_vp = 0;

    char line[1024];
    while (fgets(line, sizeof(line), f) && t->nline < WPS_MAX_LINES) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(line, '\r'); if (cr) *cr = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        wps_toks toks;
        parse_toks(line, &toks, 0);

        /* Pull the setup tags out; whatever remains is a drawable line. */
        wps_toks keep;
        keep.t = calloc(WPS_MAX_TOKENS, sizeof(wps_tok));
        keep.n = 0;
        int align = 'l';

        for (int i = 0; i < toks.n; i++) {
            wps_tok *tk = &toks.t[i];
            if (tk->type == TOK_TAG && is_setup_tag(tk->tag)) {
                if (!strcmp(tk->tag, "X") && tk->text) {
                    char p[512];
                    resolve(p, sizeof(p), root, imgdir, tk->text);
                    if (bmp_load(p, &t->backdrop) == 0) t->has_backdrop = 1;
                } else if (!strcmp(tk->tag, "xl")) {
                    load_image(t, root, imgdir, tk->text);
                } else if (!strcmp(tk->tag, "Vl")) {
                    add_viewport(t, tk->text, 1);
                    cur_vp = t->nvp - 1;
                } else if (!strcmp(tk->tag, "V")) {
                    add_viewport(t, tk->text, 0);
                    cur_vp = t->nvp - 1;
                } else if (!strcmp(tk->tag, "T")) {
                    add_touch(t, tk->text, cur_vp);
                } else if (!strcmp(tk->tag, "Cl") && tk->text) {
                    char tmp[128]; snprintf(tmp, sizeof(tmp), "%s", tk->text);
                    char *fl[8]; int n = split(tmp, fl, 8);
                    if (n >= 4) {
                        t->art_x = num(fl[0], 0); t->art_y = num(fl[1], 0);
                        t->art_w = num(fl[2], 0); t->art_h = num(fl[3], 0);
                        t->has_art_slot = 1;
                    }
                }
                free(tk->text);
                continue;
            }
            /* Alignment tags stay in the token stream rather than setting a
             * whole-line flag: they can appear mid-line, and the playtime row
             * (%pc ... %ar%pr) depends on it -- elapsed left, remaining right,
             * one line. Collapsing them to a single per-line alignment ran the
             * two times together as one string. */
            if (tk->type == TOK_TAG && !strcmp(tk->tag, "s")) { free(tk->text); continue; }  /* scroll: static here */
            if (keep.n < WPS_MAX_TOKENS - 1) keep.t[keep.n++] = *tk;
        }
        free(toks.t);

        if (keep.n == 0) { free(keep.t); continue; }
        t->line[t->nline].toks = keep;
        t->line[t->nline].vp = cur_vp;
        t->line[t->nline].align = align;
        t->nline++;
    }
    fclose(f);
}

/* ---- loading ------------------------------------------------------------- */

wps_theme *wps_load(const char *cfg_path, const char *root) {
    FILE *f = fopen(cfg_path, "r");
    if (!f) return NULL;

    char wps_rel[256] = "";
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *c = strchr(line, '\n'); if (c) *c = '\0';
        if (!strncasecmp(line, "wps:", 4)) {
            char *v = line + 4;
            while (*v == ' ' || *v == '\t') v++;
            snprintf(wps_rel, sizeof(wps_rel), "%s", v);
        }
    }
    fclose(f);
    if (!wps_rel[0] || wps_rel[0] == '-') return NULL;

    wps_theme *t = calloc(1, sizeof(*t));
    if (!t) return NULL;

    const char *base = strrchr(cfg_path, '/');
    snprintf(t->name, sizeof(t->name), "%s", base ? base + 1 : cfg_path);
    char *dot = strrchr(t->name, '.'); if (dot) *dot = '\0';

    char wps_path[512];
    resolve(wps_path, sizeof(wps_path), root, root, wps_rel);

    /* Images referenced by bare name live in a directory beside the .wps,
     * named after the .wps itself -- cabbiev2.wps -> cabbiev2/. */
    char imgdir[512];
    snprintf(imgdir, sizeof(imgdir), "%s", wps_path);
    char *d = strrchr(imgdir, '.'); if (d) *d = '\0';

    parse_wps(t, wps_path, root, imgdir);
    if (t->nline == 0 && !t->has_backdrop) { wps_free(t); return NULL; }
    return t;
}

void wps_free(wps_theme *t) {
    if (!t) return;
    bmp_free(&t->backdrop);
    for (int i = 0; i < t->nimg; i++) bmp_free(&t->img[i].bm);
    for (int i = 0; i < t->nline; i++) {
        for (int j = 0; j < t->line[i].toks.n; j++) free(t->line[i].toks.t[j].text);
        free(t->line[i].toks.t);
    }
    free(t);
}

int wps_scan_themes(const char *dir, char names[][64], int max) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcasecmp(dot, ".cfg") != 0) continue;
        snprintf(names[n], 64, "%s", e->d_name);
        char *cut = strrchr(names[n], '.'); if (cut) *cut = '\0';
        n++;
    }
    closedir(d);
    /* Stable order: readdir's is not guaranteed and the picker indexes it. */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcasecmp(names[i], names[j]) > 0) {
                char tmp[64]; snprintf(tmp, sizeof(tmp), "%s", names[i]);
                snprintf(names[i], 64, "%s", names[j]);
                snprintf(names[j], 64, "%s", tmp);
            }
    return n;
}

/* ---- rendering ----------------------------------------------------------- */

static void blit(uint16_t *fb, int fw, int fh, const bmp_t *b,
                 int dx, int dy, int sy, int sh) {
    if (!b->px) return;
    for (int y = 0; y < sh; y++) {
        int fy = dy + y, by = sy + y;
        if (fy < 0 || fy >= fh || by < 0 || by >= b->h) continue;
        const uint16_t *src = b->px + (size_t)by * b->w;
        const uint8_t  *sa  = b->a ? b->a + (size_t)by * b->w : NULL;
        uint16_t *dst = fb + (size_t)fy * fw;
        for (int x = 0; x < b->w; x++) {
            int fx = dx + x;
            if (fx < 0 || fx >= fw) continue;
            if (!sa) { dst[fx] = src[x]; continue; }
            int a = sa[x];
            if (a == 0) continue;
            if (a == 255) { dst[fx] = src[x]; continue; }
            /* 565 blend, done per-channel in place */
            uint16_t s = src[x], dpx = dst[fx];
            int sr = (s >> 11) & 0x1F, sg = (s >> 5) & 0x3F, sb = s & 0x1F;
            int dr = (dpx >> 11) & 0x1F, dg = (dpx >> 5) & 0x3F, db = dpx & 0x1F;
            int r = (sr * a + dr * (255 - a)) / 255;
            int g = (sg * a + dg * (255 - a)) / 255;
            int bl = (sb * a + db * (255 - a)) / 255;
            dst[fx] = (uint16_t)((r << 11) | (g << 5) | bl);
        }
    }
}

static void fmt_time(char *out, size_t n, int ms) {
    if (ms < 0) ms = 0;
    int s = ms / 1000;
    if (s >= 3600) snprintf(out, n, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else           snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

/* A tag's textual value. Unknown tags produce "" so an unsupported theme
 * loses a field rather than failing to draw. */
static void tag_text(const char *tag, const wps_state *st, char *out, size_t n) {
    out[0] = '\0';
    const char *v = NULL;
    if      (!strcmp(tag, "it")) v = st->title;
    else if (!strcmp(tag, "ia")) v = st->artist;
    else if (!strcmp(tag, "id")) v = st->album;
    else if (!strcmp(tag, "iA")) v = st->albumartist;
    else if (!strcmp(tag, "ig")) v = st->genre;
    else if (!strcmp(tag, "iy")) v = st->year;
    else if (!strcmp(tag, "fn")) v = st->filename;
    else if (!strcmp(tag, "fc")) v = st->codec;
    else if (!strcmp(tag, "pc")) { fmt_time(out, n, st->pos_ms); return; }
    else if (!strcmp(tag, "pt")) { fmt_time(out, n, st->dur_ms); return; }
    else if (!strcmp(tag, "pr")) { fmt_time(out, n, st->dur_ms - st->pos_ms); return; }
    else if (!strcmp(tag, "pp")) { snprintf(out, n, "%d", st->track_no); return; }
    else if (!strcmp(tag, "pe")) { snprintf(out, n, "%d", st->track_count); return; }
    else if (!strcmp(tag, "fb")) { snprintf(out, n, "%d", st->bitrate_kbps); return; }
    else if (!strcmp(tag, "pv")) { snprintf(out, n, "%d", st->volume_pct); return; }
    if (v) snprintf(out, n, "%s", v);
}

/* Truth of a conditional's tag. Same forgiving rule: unknown means false. */
static int tag_true(const char *tag, const wps_state *st) {
    if (!strcmp(tag, "C"))  return st->art != NULL;
    if (!strcmp(tag, "ps")) return st->shuffle;
    if (!strcmp(tag, "mp")) return st->playing ? 1 : 2;   /* 1 play, 2 pause */
    if (!strcmp(tag, "it")) return st->title && st->title[0];
    if (!strcmp(tag, "ia")) return st->artist && st->artist[0];
    if (!strcmp(tag, "id")) return st->album && st->album[0];
    if (!strcmp(tag, "iA")) return st->albumartist && st->albumartist[0];
    if (!strcmp(tag, "ig")) return st->genre && st->genre[0];
    if (!strcmp(tag, "iy")) return st->year && st->year[0];
    if (!strcmp(tag, "fv")) return 0;
    return 0;
}

typedef struct { char buf[256]; size_t len; int align; } seg_t;

static void emit(seg_t *seg, int nseg, const char *s) {
    seg_t *cur = &seg[nseg - 1];
    size_t l = strlen(s);
    if (cur->len + l < sizeof(cur->buf) - 1) {
        memcpy(cur->buf + cur->len, s, l);
        cur->len += l;
    }
}

static void render_tok(wps_theme *t, wps_tok *tk, uint16_t *fb, int fw, int fh,
                       const wps_state *st, const wps_vp *vp, int *pen_y,
                       seg_t *seg, int *nseg) {
    if (tk->type == TOK_TEXT) { emit(seg, *nseg, tk->text); return; }
    if (tk->type == TOK_COND) {
        int v = tag_true(tk->tag, st);
        /* Rockbox: a two-branch conditional is if/else on truthiness; a
         * multi-branch one selects by 1-based enumeration value. */
        int idx;
        if (tk->nbranch <= 2) idx = v ? 0 : 1;
        else                  idx = (v > 0 && v <= tk->nbranch) ? v - 1 : tk->nbranch - 1;
        if (idx >= 0 && idx < tk->nbranch) {
            for (int i = 0; i < tk->branch[idx].n; i++)
                render_tok(t, &tk->branch[idx].t[i], fb, fw, fh, st, vp, pen_y,
                           seg, nseg);
        }
        return;
    }

    /* Alignment: start a new segment on the same line. */
    if (!strcmp(tk->tag, "al") || !strcmp(tk->tag, "ac") || !strcmp(tk->tag, "ar")) {
        if (*nseg < 3) {
            seg[*nseg].len = 0; seg[*nseg].buf[0] = '\0';
            seg[*nseg].align = tk->tag[1];
            (*nseg)++;
        }
        return;
    }
    /* %Sx(text) is a translated string. There is no translation table here, so
     * the argument -- already English in every theme -- is the string. Without
     * this the word vanishes and "5 of 16" renders as "5  16". */
    if (!strcmp(tk->tag, "Sx")) { if (tk->text) emit(seg, *nseg, tk->text); return; }

    /* drawing tags */
    if (!strcmp(tk->tag, "Vd")) {
        if (tk->text) { int i = vp_find(t, tk->text[0]); if (i >= 0) t->vp[i].shown = 1; }
        return;
    }
    if (!strcmp(tk->tag, "xd")) {
        if (!tk->text) return;
        char id[4] = { tk->text[0], 0, 0, 0 };
        /* %xd(E, %mm, -1) and %xd(F, 1) select a sub-image; the plain form is
         * frame 0. Only the literal-number form is handled -- a tag-valued
         * index needs the enum tags this does not implement yet. */
        int frame = 0;
        const char *comma = strchr(tk->text, ',');
        if (comma) { while (*++comma == ' ') {} if (isdigit((unsigned char)*comma)) frame = atoi(comma) - 1; }
        /* Rockbox also allows the second letter of a two-char id to be the
         * frame: %xd(Ca) is image C frame 0, Cb frame 1, ... */
        if (isalpha((unsigned char)tk->text[1])) frame = tolower((unsigned char)tk->text[1]) - 'a';
        int i = img_find(t, id);
        if (i < 0 || !t->img[i].loaded) return;
        wps_img *im = &t->img[i];
        int fh_each = im->bm.h / im->frames;
        if (frame < 0 || frame >= im->frames) frame = 0;
        blit(fb, fw, fh, &im->bm, vp->x + im->x, vp->y + im->y,
             frame * fh_each, fh_each);
        return;
    }
    if (!strcmp(tk->tag, "Cd")) {
        if (!st->art || !t->has_art_slot) return;
        /* Nearest-neighbour into the theme's own art rectangle. */
        int dw = t->art_w, dh = t->art_h;
        for (int y = 0; y < dh; y++) {
            int fy = t->art_y + y;
            if (fy < 0 || fy >= fh) continue;
            const uint16_t *src = st->art + (size_t)(y * st->art_px / dh) * st->art_px;
            uint16_t *dst = fb + (size_t)fy * fw;
            for (int x = 0; x < dw; x++) {
                int fx = t->art_x + x;
                if (fx >= 0 && fx < fw) dst[fx] = src[x * st->art_px / dw];
            }
        }
        return;
    }
    if (!strcmp(tk->tag, "pb")) {
        /* %pb(x,y,w,h,image) -- the image is the filled portion. */
        int bx = vp->x, by = *pen_y, bw = vp->w ? vp->w : fw, bh = 6;
        char *fl[8]; char tmp[128];
        if (tk->text) {
            snprintf(tmp, sizeof(tmp), "%s", tk->text);
            int n = split(tmp, fl, 8);
            if (n >= 4) { bx = vp->x + num(fl[0], 0); by = vp->y + num(fl[1], 0);
                          bw = num(fl[2], bw); bh = num(fl[3], bh); }
            if (bh <= 0) bh = 6;
        }
        int filled = (st->dur_ms > 0) ? (int)((int64_t)bw * st->pos_ms / st->dur_ms) : 0;
        if (filled > bw) filled = bw;
        /* Track: a dim bar, so progress reads even when the theme's own fill
         * image is missing. */
        for (int y = by; y < by + bh && y < fh; y++)
            for (int x = bx; x < bx + bw && x < fw; x++)
                if (y >= 0 && x >= 0) fb[(size_t)y * fw + x] = 0x39E7;
        int im = -1;
        if (tk->text) {
            char tmp2[128]; snprintf(tmp2, sizeof(tmp2), "%s", tk->text);
            char *f2[8]; int n2 = split(tmp2, f2, 8);
            if (n2 >= 5) {
                /* the fill is named by file, not id: find a loaded image whose
                 * source file matched -- not tracked, so fall through to the
                 * flat fill below, which is honest rather than wrong */
                (void)f2;
            }
        }
        (void)im;
        for (int y = by; y < by + bh && y < fh; y++)
            for (int x = bx; x < bx + filled && x < fw; x++)
                if (y >= 0 && x >= 0) fb[(size_t)y * fw + x] = 0xFD20;
        *pen_y = by + bh + 2;
        return;
    }

    /* everything else contributes text */
    char val[256];
    tag_text(tk->tag, st, val, sizeof(val));
    emit(seg, *nseg, val);
}

static void render_toks(wps_theme *t, const wps_toks *toks, uint16_t *fb,
                        int fw, int fh, const wps_state *st,
                        const wps_vp *vp, int *pen_y, int align) {
    /* A line is a sequence of segments, each with its own alignment, because
     * %al/%ac/%ar can appear anywhere in it. Three is all the language can
     * actually produce for one line. */
    seg_t seg[3];
    int nseg = 1;
    seg[0].len = 0; seg[0].buf[0] = '\0'; seg[0].align = align;

    for (int i = 0; i < toks->n; i++)
        render_tok(t, &toks->t[i], fb, fw, fh, st, vp, pen_y, seg, &nseg);

    int drew = 0;
    int px = 22;
    int vw = vp->w ? vp->w : fw;
    for (int i = 0; i < nseg; i++) {
        char *sp = seg[i].buf;
        sp[seg[i].len] = '\0';
        while (*sp == ' ') sp++;
        size_t e = strlen(sp);
        while (e && sp[e - 1] == ' ') sp[--e] = '\0';
        if (!*sp) continue;

        int w = text_width(sp, px);
        int x = vp->x;
        if (seg[i].align == 'c')      x = vp->x + (vw - w) / 2;
        else if (seg[i].align == 'r') x = vp->x + vw - w;
        if (x < 0) x = 0;
        text_draw(fb, fw, fh, 0, x, *pen_y, sp, 0xFFFF, px, fw);
        drew = 1;
    }
    if (drew) *pen_y += px + 6;
}

void wps_render(wps_theme *t, uint16_t *fb, int fw, int fh, const wps_state *st) {
    if (t->has_backdrop) blit(fb, fw, fh, &t->backdrop, 0, 0, 0, t->backdrop.h);
    else memset(fb, 0, (size_t)fw * fh * sizeof(uint16_t));

    /* %Vd marks conditional viewports visible for this frame only. */
    for (int i = 0; i < t->nvp; i++) t->vp[i].shown = 0;

    /* Two passes: the first runs lines in unconditional viewports, which is
     * what executes the %Vd tags; the second draws whatever they turned on.
     * Doing it in one pass would depend on a %Vd appearing before the lines it
     * enables, which the format does not guarantee. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < t->nline; i++) {
            wps_vp *vp = &t->vp[t->line[i].vp];
            int want = pass == 0 ? !vp->conditional : (vp->conditional && vp->shown);
            if (!want) continue;
            int pen = vp->y;
            /* Lines share a viewport, so the pen has to carry between them. */
            for (int j = i; j < t->nline && t->line[j].vp == t->line[i].vp; j++) {
                render_toks(t, &t->line[j].toks, fb, fw, fh, st, vp, &pen,
                            t->line[j].align);
            }
            while (i + 1 < t->nline && t->line[i + 1].vp == t->line[i].vp) i++;
        }
    }
}

const char *wps_hit(const wps_theme *t, int x, int y) {
    /* Last match wins: later regions in the file sit on top, matching the
     * order the theme draws them. */
    const char *hit = NULL;
    for (int i = 0; i < t->ntouch; i++) {
        const wps_touch *tc = &t->touch[i];
        int ox = t->vp[tc->vp].x, oy = t->vp[tc->vp].y;
        if (x >= ox + tc->x && x < ox + tc->x + tc->w &&
            y >= oy + tc->y && y < oy + tc->y + tc->h)
            hit = tc->action;
    }
    return hit;
}
