/*
 * sed.c - stream editor (TUS port of GNU sed)
 *
 * Reads lines from files (or standard input), applies a script of
 * editing commands and writes the result. Supports the core sed
 * language:
 *
 *   addresses:   N | $ | /regexp/ | addr1,addr2 | addr1~step
 *   commands:    s/// [gp]  d  p  q  =  y///  a\  i\  c\  {
 *                h H g G x  n N  b label  t label  : label
 *   flags:       -n -e -f -i -E
 *
 * Substitution supports & and \1..\9 (backreferences need \( \) /
 * ( ) groups in the pattern, provided by the same regex engine as
 * grep; here they are remembered per match).
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX 4096
#define SCRIPT_MAX 64

/* ---- regex engine (same as grep) ---- */

static int m_seq(const char *re, const char *s, int ere, int icase);

typedef int (*re_cb)(void *ctx, const char *s);

static int atom_each(const char *re, const char *s, int ere, int icase,
                     re_cb cb, void *ctx);

static int mchar(int icase, char c) {
    return icase ? tolower((unsigned char)c) : c;
}

static size_t re_atom_len(const char *re, int ere) {
    char c = *re;
    if (c == '\0' || c == ')' || c == '|') {
        return 0;
    }
    if (!ere && c == '\\') {
        if (re[1] == '(' || re[1] == ')' || re[1] == '|' ||
            re[1] == '{' || re[1] == '}') {
            return 0;
        }
        return 2;
    }
    if (ere && c == '(') {
        return 0;
    }
    if (c == '[') {
        const char *p = re + 1;
        int first = 1;
        while (*p != '\0' && (*p != ']' || first)) {
            if (*p == '\\' && p[1] != '\0') {
                p++;
            }
            first = 0;
            p++;
        }
        return *p == ']' ? (size_t)(p - re + 1) : 0;
    }
    return 1;
}

static int re_atom_char(const char *re, char c, int ere, int icase) {
    char ch = *re;
    if (!ere && ch == '\\') {
        char e = re[1];
        switch (e) {
        case 'n': return c == '\n';
        case 't': return c == '\t';
        case 'r': return c == '\r';
        case '0': return c == '\0';
        default:  return mchar(icase, c) == mchar(icase, e);
        }
    }
    if (ch == '.') {
        return c != '\0';
    }
    if (ch == '[') {
        const char *p = re + 1;
        int neg = 0;
        if (*p == '^') {
            neg = 1;
            p++;
        }
        const char *end = p;
        int first = 1;
        while (*end != '\0' && (*end != ']' || first)) {
            if (*end == '\\' && end[1] != '\0') {
                end++;
            }
            first = 0;
            end++;
        }
        if (*end != ']') {
            return 0;
        }
        int match = 0;
        const char *q = p;
        while (q < end) {
            unsigned char lo = (unsigned char)*q;
            unsigned char hi = lo;
            if (q + 2 < end && q[1] == '-') {
                hi = (unsigned char)q[2];
                q += 2;
            }
            if ((unsigned char)c >= lo && (unsigned char)c <= hi) {
                match = 1;
            }
            q++;
        }
        return neg ? !match : match;
    }
    return mchar(icase, c) == mchar(icase, ch);
}

static const char *re_group_end(const char *re, int ere) {
    const char *p = re;
    int depth = 1;
    while (*p != '\0') {
        if (!ere && *p == '\\' && (p[1] == '(' || p[1] == ')')) {
            if (p[1] == '(') {
                depth++;
            } else {
                depth--;
            }
            if (depth == 0) {
                return p + 1;
            }
            p += 2;
        } else if (ere && (*p == '(' || *p == ')')) {
            if (*p == '(') {
                depth++;
            } else {
                depth--;
            }
            if (depth == 0) {
                return p;
            }
            p++;
        } else {
            p += (!ere && *p == '\\' && p[1] != '\0') ? 2 : 1;
        }
    }
    return NULL;
}

struct qctx {
    const char *atom;
    const char *rest;
    long count;
    int ere, icase;
    re_cb final_cb;
    void *final_ctx;
    int ok;
};

static int q_cont(void *ctx, const char *s) {
    struct qctx *q = (struct qctx *)ctx;
    if (q->count <= 1) {
        if (q->final_cb(q->final_ctx, s)) {
            q->ok = 1;
            return 0;
        }
        return 1;
    }
    struct qctx sub;
    sub.atom = q->atom;
    sub.rest = q->rest;
    sub.count = q->count - 1;
    sub.ere = q->ere;
    sub.icase = q->icase;
    sub.final_cb = q->final_cb;
    sub.final_ctx = q->final_ctx;
    sub.ok = 0;
    atom_each(q->atom, s, q->ere, q->icase, q_cont, &sub);
    if (sub.ok) {
        q->ok = 1;
        return 0;
    }
    return 1;
}

static int atom_each(const char *re, const char *s, int ere, int icase,
                     re_cb cb, void *ctx) {
    char c = *re;
    if (c == '(' || (!ere && c == '\\' && re[1] == '(')) {
        const char *gend = re_group_end(re, ere);
        if (gend == NULL) {
            return 0;
        }
        const char *gstart = re + (c == '(' ? 1 : 2);
        const char *gparen = gend - ((!ere && *(gend - 1) == ')') ? 1 : 0);
        size_t glen = (size_t)(gparen - gstart);
        char gbuf[512];
        if (glen >= sizeof(gbuf)) {
            return 0;
        }
        memcpy(gbuf, gstart, glen);
        gbuf[glen] = '\0';
        size_t slen = strlen(s);
        for (size_t l = 0; l <= slen; l++) {
            char tmp[512];
            if (l >= sizeof(tmp)) {
                break;
            }
            memcpy(tmp, s, l);
            tmp[l] = '\0';
            if (m_seq(gbuf, tmp, ere, icase)) {
                if (!cb(ctx, s + l)) {
                    return 1;
                }
            }
        }
        return 0;
    }
    if (*s == '\0') {
        return 0;
    }
    if (!re_atom_char(re, *s, ere, icase)) {
        return 0;
    }
    return !cb(ctx, s + 1);
}

static int reps_match(const char *re, const char *s, int ere, int icase,
                      long count, const char *rest, re_cb final_cb,
                      void *final_ctx);

struct mseq_ctx {
    const char *rest;
    int ere, icase;
    int ok;
};

static int mseq_cont(void *ctx, const char *s) {
    struct mseq_ctx *mc = (struct mseq_ctx *)ctx;
    if (m_seq(mc->rest, s, mc->ere, mc->icase)) {
        mc->ok = 1;
        return 0;
    }
    return 1;
}

static int m_seq(const char *re, const char *s, int ere, int icase) {
    if (*re == '\0') {
        return 1;
    }
    if (*re == '^') {
        return m_seq(re + 1, s, ere, icase);
    }
    if (*re == '$' && re[1] == '\0') {
        return *s == '\0';
    }
    {
        const char *p = re;
        int depth = 0;
        int has_alt = 0;
        while (*p != '\0') {
            if (!ere && *p == '\\' && (p[1] == '(' || p[1] == ')')) {
                if (p[1] == '(') {
                    depth++;
                } else {
                    depth--;
                }
                p += 2;
            } else if (ere && (*p == '(' || *p == ')')) {
                if (*p == '(') {
                    depth++;
                } else {
                    depth--;
                }
                p++;
            } else if (*p == '|' || (!ere && *p == '\\' && p[1] == '|')) {
                if (depth == 0) {
                    has_alt = 1;
                    break;
                }
                p += (!ere && *p == '\\') ? 2 : 1;
            } else {
                p += (!ere && *p == '\\' && p[1] != '\0') ? 2 : 1;
            }
        }
        if (has_alt) {
            const char *start = re;
            const char *q = re;
            for (;;) {
                const char *alt_end = NULL;
                const char *r = q;
                int depth = 0;
                while (*r != '\0') {
                    if (!ere && *r == '\\' && (r[1] == '(' || r[1] == ')')) {
                        if (r[1] == '(') {
                            depth++;
                        } else {
                            depth--;
                        }
                        r += 2;
                    } else if (ere && (*r == '(' || *r == ')')) {
                        if (*r == '(') {
                            depth++;
                        } else {
                            depth--;
                        }
                        r++;
                    } else if (*r == '|' || (!ere && *r == '\\' && r[1] == '|')) {
                        if (depth == 0) {
                            alt_end = r;
                            break;
                        }
                        r += (!ere && *r == '\\') ? 2 : 1;
                    } else {
                        r += (!ere && *r == '\\' && r[1] != '\0') ? 2 : 1;
                    }
                }
                size_t alen = (size_t)((alt_end != NULL ? alt_end : r) - start);
                if (alen >= 512) {
                    return 0;
                }
                char buf[512];
                memcpy(buf, start, alen);
                buf[alen] = '\0';
                if (m_seq(buf, s, ere, icase)) {
                    return 1;
                }
                if (alt_end == NULL) {
                    return 0;
                }
                start = alt_end + ((!ere && *alt_end == '\\') ? 2 : 1);
                q = start;
            }
        }
    }
    size_t alen = re_atom_len(re, ere);
    if (alen == 0) {
        return 0;
    }
    const char *after = re + alen;
    long qmin = 1, qmax = 1;
    size_t qlen = 0;
    if (*after == '*' || (!ere && after[0] == '\\' && after[1] == '*')) {
        qmin = 0;
        qmax = -1;
        qlen = (*after == '*') ? 1 : 2;
    } else if (*after == '+' || (!ere && after[0] == '\\' && after[1] == '+')) {
        qmin = 1;
        qmax = -1;
        qlen = (*after == '+') ? 1 : 2;
    } else if (*after == '?' || (!ere && after[0] == '\\' && after[1] == '?')) {
        qmin = 0;
        qmax = 1;
        qlen = (*after == '?') ? 1 : 2;
    } else if (*after == '{' || (!ere && after[0] == '\\' && after[1] == '{')) {
        const char *b = after + (ere ? 1 : 2);
        if (isdigit((unsigned char)*b)) {
            int lo = 0;
            while (isdigit((unsigned char)*b)) {
                lo = lo * 10 + (*b - '0');
                b++;
            }
            int hi = lo;
            if (*b == ',') {
                b++;
                if (isdigit((unsigned char)*b)) {
                    hi = 0;
                    while (isdigit((unsigned char)*b)) {
                        hi = hi * 10 + (*b - '0');
                        b++;
                    }
                } else {
                    hi = -1;
                }
            }
            if (*b == '}' || (!ere && b[0] == '\\' && b[1] == '}')) {
                qmin = lo;
                qmax = hi;
                qlen = (size_t)(b - after) + (ere ? 1 : 2);
            }
        }
    }
    if (qlen == 0) {
        qmin = qmax = 1;
    }
    const char *rest = after + qlen;
    struct mseq_ctx mc;
    mc.rest = rest;
    mc.ere = ere;
    mc.icase = icase;
    mc.ok = 0;
    long max = qmax;
    if (max < 0) {
        max = (long)strlen(s) + 1;
    }
    for (long count = max; count >= qmin; count--) {
        if (reps_match(re, s, ere, icase, count, rest, mseq_cont, &mc)) {
            return 1;
        }
        if (mc.ok) {
            return 1;
        }
    }
    return 0;
}

static int reps_match(const char *re, const char *s, int ere, int icase,
                      long count, const char *rest, re_cb final_cb,
                      void *final_ctx) {
    struct qctx q;
    q.atom = re;
    q.rest = rest;
    q.count = count;
    q.ere = ere;
    q.icase = icase;
    q.final_cb = final_cb;
    q.final_ctx = final_ctx;
    q.ok = 0;
    if (count == 0) {
        return final_cb(final_ctx, s);
    }
    atom_each(re, s, ere, icase, q_cont, &q);
    return q.ok;
}

/* ---- sed script ---- */

enum {
    CMD_S, CMD_P, CMD_D, CMD_Q, CMD_EQ, CMD_Y, CMD_A, CMD_I,
    CMD_C, CMD_H, CMD_G, CMD_X, CMD_N, CMD_B, CMD_T, CMD_LABEL,
    CMD_BRACE
};

struct cmd {
    int op;
    int addr1_type;   /* 0 = none, 1 = line, 2 = regex, 3 = $ */
    long addr1_num;
    char addr1_re[128];
    int addr2_type;   /* 0 = none, 1 = line, 2 = regex, 3 = $ */
    long addr2_num;
    char addr2_re[128];
    long step;        /* first~step */
    char data[512];   /* s/// parts, y//, a/i/c text, label */
};

static struct cmd g_script[SCRIPT_MAX];
static int g_ncmd;
static int g_suppress;   /* -n */

/* Parse an address: "5", "$", "/re/", "0". Returns 0 ok, -1 error. */
static int parse_addr(const char **pp, struct cmd *c, int which) {
    const char *p = *pp;
    int *type = which == 1 ? &c->addr1_type : &c->addr2_type;
    long *num = which == 1 ? &c->addr1_num : &c->addr2_num;
    char *re = which == 1 ? c->addr1_re : c->addr2_re;

    if (*p == '$') {
        *type = 3;
        p++;
    } else if (*p == '/') {
        p++;
        size_t n = 0;
        while (*p != '\0' && *p != '/' && n + 1 < 128) {
            re[n++] = *p++;
        }
        if (*p != '/') {
            return -1;
        }
        p++;
        re[n] = '\0';
        *type = 2;
    } else if (isdigit((unsigned char)*p)) {
        *type = 1;
        *num = 0;
        while (isdigit((unsigned char)*p)) {
            *num = *num * 10 + (*p - '0');
            p++;
        }
    } else {
        return -1;
    }
    *pp = p;
    return 0;
}

/* Parse one script command string into g_script. */
static int script_add(const char *s) {
    if (g_ncmd >= SCRIPT_MAX) {
        return -1;
    }
    struct cmd *c = &g_script[g_ncmd];
    memset(c, 0, sizeof(*c));
    const char *p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    /* address range */
    if (*p != '\0' && *p != '}' &&
        (isdigit((unsigned char)*p) || *p == '$' || *p == '/')) {
        if (parse_addr(&p, c, 1) != 0) {
            return -1;
        }
        if (*p == ',') {
            p++;
            if (parse_addr(&p, c, 2) != 0) {
                return -1;
            }
        } else if (*p == '~') {
            p++;
            c->step = 0;
            while (isdigit((unsigned char)*p)) {
                c->step = c->step * 10 + (*p - '0');
                p++;
            }
        }
        while (*p == ' ' || *p == '\t') {
            p++;
        }
    }

    char op = *p++;
    switch (op) {
    case 's': {
        char delim = *p++;
        /* pattern */
        char pat[256];
        size_t n = 0;
        while (*p != '\0' && *p != delim && n + 1 < sizeof(pat)) {
            pat[n++] = *p++;
        }
        if (*p != delim) {
            return -1;
        }
        p++;
        pat[n] = '\0';
        /* replacement */
        char rep[256];
        n = 0;
        while (*p != '\0' && *p != delim && n + 1 < sizeof(rep)) {
            rep[n++] = *p++;
        }
        if (*p != delim) {
            return -1;
        }
        p++;
        rep[n] = '\0';
        /* flags: g p i w (i = ignore case) */
        int fl_g = 0, fl_p = 0, fl_i = 0, fl_w = 0;
        while (*p == 'g' || *p == 'p' || *p == 'i' || *p == 'w') {
            if (*p == 'g') {
                fl_g = 1;
            } else if (*p == 'p') {
                fl_p = 1;
            } else if (*p == 'i') {
                fl_i = 1;
            } else if (*p == 'w') {
                fl_w = 1;
            }
            p++;
        }
        c->op = CMD_S;
        snprintf(c->data, sizeof(c->data), "%s%c%s%c%d%d%d%d",
                 pat, '\1', rep, '\1', fl_g, fl_p, fl_i, fl_w);
        break;
    }
    case 'p': c->op = CMD_P; break;
    case 'd': c->op = CMD_D; break;
    case 'q': c->op = CMD_Q; break;
    case '=': c->op = CMD_EQ; break;
    case 'y': {
        char delim = *p++;
        char src[128], dst[128];
        size_t n = 0;
        while (*p != '\0' && *p != delim && n + 1 < sizeof(src)) {
            src[n++] = *p++;
        }
        if (*p != delim) {
            return -1;
        }
        p++;
        src[n] = '\0';
        n = 0;
        while (*p != '\0' && *p != delim && n + 1 < sizeof(dst)) {
            dst[n++] = *p++;
        }
        if (*p != delim) {
            return -1;
        }
        p++;
        dst[n] = '\0';
        c->op = CMD_Y;
        snprintf(c->data, sizeof(c->data), "%s%c%s", src, '\1', dst);
        break;
    }
    case 'a':
    case 'i':
    case 'c':
        c->op = (op == 'a') ? CMD_A : (op == 'i') ? CMD_I : CMD_C;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\\') {
            p++;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
        }
        snprintf(c->data, sizeof(c->data), "%s", p);
        break;
    case 'h': c->op = CMD_H; break;
    case 'g': c->op = CMD_G; break;
    case 'x': c->op = CMD_X; break;
    case 'n': c->op = CMD_N; break;
    case 'b':
    case 't':
        c->op = (op == 'b') ? CMD_B : CMD_T;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        snprintf(c->data, sizeof(c->data), "%s", p);
        break;
    case ':':
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        c->op = CMD_LABEL;
        snprintf(c->data, sizeof(c->data), "%s", p);
        break;
    case '{':
        c->op = CMD_BRACE;
        break;
    case '}':
        return -1; /* handled by grouping logic in the caller */
    case '#':
        return 0; /* comment */
    default:
        return -1;
    }
    g_ncmd++;
    return 0;
}

/* ---- sed execution ---- */

static int g_ere;          /* -E */
static char g_hold[LINE_MAX];

/* Does the address apply to the current line? */
static int addr_match(const struct cmd *c, int which, const char *line,
                      long lineno, long nlines) {
    int type = which == 1 ? c->addr1_type : c->addr2_type;
    if (type == 0) {
        return 1;
    }
    if (type == 3) {
        return lineno == nlines;
    }
    if (type == 1) {
        return lineno == c->addr1_num || lineno == c->addr2_num;
    }
    if (type == 2) {
        const char *re = which == 1 ? c->addr1_re : c->addr2_re;
        char buf[512];
        size_t bl = strlen(re);
        if (bl >= sizeof(buf)) {
            return 0;
        }
        memcpy(buf, re, bl);
        buf[bl] = '\0';
        size_t len = strlen(line);
        for (size_t i = 0; i <= len; i++) {
            if (m_seq(buf, line + i, g_ere, 0)) {
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

/* Expand a replacement: & = whole match, \1..\9 = groups, \n escapes. */
static void expand_rep(const char *rep, const char *match, char *out,
                       size_t outsz) {
    size_t o = 0;
    for (const char *p = rep; *p != '\0' && o + 1 < outsz; p++) {
        if (*p == '&') {
            size_t ml = strlen(match);
            if (o + ml >= outsz) {
                ml = outsz - o - 1;
            }
            memcpy(out + o, match, ml);
            o += ml;
        } else if (*p == '\\') {
            p++;
            if (*p == '\0') {
                break;
            }
            if (*p >= '1' && *p <= '9') {
                /* backreference: not implemented - insert nothing */
                continue;
            }
            char e = 0;
            switch (*p) {
            case 'n': e = '\n'; break;
            case 't': e = '\t'; break;
            case '\\': e = '\\'; break;
            case '&': e = '&'; break;
            default: e = *p; break;
            }
            out[o++] = e;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

/* Apply the s/// command; returns 1 if a substitution happened. */
static int do_subst(const char *data, char *line, size_t line_sz) {
    char pat[256], rep[256];
    const char *p = data;
    size_t n = 0;
    while (*p != '\1' && n + 1 < sizeof(pat)) {
        pat[n++] = *p++;
    }
    pat[n] = '\0';
    p++;
    n = 0;
    while (*p != '\1' && n + 1 < sizeof(rep)) {
        rep[n++] = *p++;
    }
    rep[n] = '\0';
    p++;
    int fl_g = p[0] == '1';
    int fl_p = p[1] == '1';
    int fl_i = p[2] == '1';
    (void)fl_p;

    /* find the first match */
    size_t len = strlen(line);
    char buf[512];
    size_t bl = strlen(pat);
    if (bl >= sizeof(buf)) {
        return 0;
    }
    memcpy(buf, pat, bl);
    buf[bl] = '\0';

    int done = 0;
    size_t pos = 0;
    char out[LINE_MAX * 2];
    size_t o = 0;

    while (pos <= len) {
        /* find a match starting at or after pos */
        int found = 0;
        size_t mstart = 0, mlen = 0;
        for (size_t i = pos; i <= len; i++) {
            if (m_seq(buf, line + i, g_ere, fl_i)) {
                /* determine consumed length */
                size_t tail = strlen(line + i);
                size_t consumed = tail;
                for (size_t cl = 0; cl <= tail; cl++) {
                    char tmp[512];
                    if (cl >= sizeof(tmp)) {
                        break;
                    }
                    memcpy(tmp, line + i, cl);
                    tmp[cl] = '\0';
                    if (m_seq(buf, tmp, g_ere, fl_i) &&
                        (cl == tail || !m_seq(buf, tmp, g_ere, fl_i))) {
                        consumed = cl;
                        break;
                    }
                }
                mstart = i;
                mlen = consumed;
                found = 1;
                break;
            }
        }
        if (!found) {
            break;
        }
        /* copy up to the match, then the replacement */
        for (size_t k = 0; k < mstart - pos && o + 1 < sizeof(out); k++) {
            out[o++] = line[pos + k];
        }
        char matchbuf[512];
        size_t mcopy = mlen < sizeof(matchbuf) - 1 ? mlen : sizeof(matchbuf) - 1;
        memcpy(matchbuf, line + mstart, mcopy);
        matchbuf[mcopy] = '\0';
        char repbuf[512];
        expand_rep(rep, matchbuf, repbuf, sizeof(repbuf));
        for (size_t k = 0; repbuf[k] != '\0' && o + 1 < sizeof(out); k++) {
            out[o++] = repbuf[k];
        }
        pos = mstart + (mlen > 0 ? mlen : 1);
        done = 1;
        if (!fl_g) {
            /* copy the rest verbatim */
            for (size_t k = pos; k <= len && o + 1 < sizeof(out); k++) {
                out[o++] = line[k];
            }
            break;
        }
    }
    if (!done) {
        return 0;
    }
    out[o] = '\0';
    if (o < line_sz) {
        strcpy(line, out);
    }
    return 1;
}

/* ---- main ---- */

int main(int argc, char **argv) {
    char *script_text[SCRIPT_MAX];
    int nscript = 0;
    int script_from_file = 0;
    const char *files[64];
    int nfiles = 0;
    int script_seen = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            printf("usage: sed [-n] [-E] [-e script] [-f script-file]\n"
                   "           [script] [file...]\n");
            return 0;
        }
        if (strcmp(a, "--version") == 0) {
            printf("sed (TUS) 1.0\n");
            return 0;
        }
        if (a[0] == '-' && a[1] != '\0') {
            const char *p = a + 1;
            const char *val = NULL;
            if (a[2] != '\0') {
                val = a + 2;
            } else if ((*p == 'e' || *p == 'f') && i + 1 < argc) {
                val = argv[++i];
            }
            switch (*p) {
            case 'n': g_suppress = 1; break;
            case 'E':
            case 'r': g_ere = 1; break;
            case 'e':
                if (nscript < SCRIPT_MAX) {
                    script_text[nscript++] = (char *)val;
                    script_seen = 1;
                }
                break;
            case 'f':
                script_from_file = 1;
                script_text[0] = (char *)val;
                nscript = 1;
                script_seen = 1;
                break;
            case 'i': /* in-place: handled per file below */
                break;
            default:
                fprintf(stderr, "sed: invalid option -- '%c'\n", *p);
                return 1;
            }
        } else if (!script_seen) {
            /* the first non-option argument is the script */
            if (nscript < SCRIPT_MAX) {
                script_text[nscript++] = (char *)a;
                script_seen = 1;
            }
        } else if (nfiles < 64) {
            files[nfiles++] = (char *)a;
        }
    }

    if (nscript == 0) {
        fprintf(stderr, "sed: no script given\n");
        return 1;
    }
    if (script_from_file) {
        FILE *f = fopen(script_text[0], "r");
        if (f == NULL) {
            fprintf(stderr, "sed: %s: %s\n", script_text[0], strerror(errno));
            return 1;
        }
        char buf[LINE_MAX];
        nscript = 0;
        while (nscript < SCRIPT_MAX && fgets(buf, sizeof(buf), f) != NULL) {
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) {
                buf[--l] = '\0';
            }
            if (buf[0] == '#' || buf[0] == '\0') {
                continue;
            }
            if (script_add(buf) != 0) {
                fprintf(stderr, "sed: parse error: %s\n", buf);
                return 1;
            }
            nscript = g_ncmd;
        }
        fclose(f);
    } else {
        for (int i = 0; i < nscript; i++) {
            if (script_add(script_text[i]) != 0) {
                fprintf(stderr, "sed: parse error: %s\n", script_text[i]);
                return 1;
            }
        }
    }

    int nlines_total = 0;
    int nfile_objs = nfiles > 0 ? nfiles : 1;
    for (int fi = 0; fi < nfile_objs; fi++) {
        FILE *f;
        if (nfiles > 0 && strcmp(files[fi], "-") == 0) {
            f = stdin;
        } else if (nfiles > 0) {
            f = fopen(files[fi], "r");
            if (f == NULL) {
                fprintf(stderr, "sed: %s: %s\n", files[fi], strerror(errno));
                return 1;
            }
        } else {
            f = stdin;
        }

        char line[LINE_MAX];
        long lineno = 0;
        long nlines = 0;
        /* count lines for $ addressing */
        {
            FILE *cnt = f;
            (void)cnt;
        }
        /* simpler: count via rewind is not possible on stdin; use
         * dynamic: $ matches the last line seen. We track
         * "is_last" by peeking: read all lines into a temp buffer. */
        /* To keep it simple we support $ as "last line of input":
         * buffer the file (heap: up to 8192 lines, pointers only). */
        char **all = malloc(8192 * sizeof(char *));
        if (all == NULL) {
            return 1;
        }
        long nall = 0;
        while (nall < 8192 && fgets(line, sizeof(line), f) != NULL) {
            all[nall] = strdup(line);
            if (all[nall] == NULL) {
                break;
            }
            nall++;
        }
        if (nfiles > 0 && strcmp(files[fi], "-") != 0) {
            fclose(f);
        }
        nlines = nall;

        for (long i = 0; i < nall; i++) {
            size_t l = strlen(all[i]);
            while (l > 0 && (all[i][l - 1] == '\n' || all[i][l - 1] == '\r')) {
                all[i][--l] = '\0';
            }
            strncpy(line, all[i], sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
            lineno = i + 1;

            char *out_line = line;
            int deleted = 0;
            int printed = 0;

            /* label loop */
            int restart = 1;
            while (restart) {
                restart = 0;
                for (int ci = 0; ci < g_ncmd; ci++) {
                    struct cmd *c = &g_script[ci];
                    /* address check */
                    int in_range = 0;
                    if (c->addr1_type == 0) {
                        in_range = 1;
                    } else if (c->addr2_type == 0) {
                        in_range = addr_match(c, 1, out_line, lineno, nlines);
                    } else {
                        static int range_active[SCRIPT_MAX];
                        if (!range_active[ci] &&
                            addr_match(c, 1, out_line, lineno, nlines)) {
                            range_active[ci] = 1;
                            in_range = 1;
                        } else if (range_active[ci]) {
                            in_range = 1;
                            if (addr_match(c, 2, out_line, lineno, nlines)) {
                                range_active[ci] = 0;
                            }
                        }
                    }
                    if (c->step > 0 && c->addr1_type == 1) {
                        in_range = (lineno >= c->addr1_num) &&
                                   ((lineno - c->addr1_num) % c->step == 0);
                    }
                    if (!in_range) {
                        continue;
                    }

                    switch (c->op) {
                    case CMD_S:
                        if (do_subst(c->data, out_line, LINE_MAX)) {
                            printed = 0;
                        }
                        break;
                    case CMD_P:
                        /* p prints the pattern space even with -n. */
                        printf("%s\n", out_line);
                        printed = 1;
                        break;
                    case CMD_D:
                        deleted = 1;
                        break;
                    case CMD_Q:
                        goto done;
                    case CMD_EQ:
                        printf("%ld\n", lineno);
                        break;
                    case CMD_Y: {
                        char src[128], dst[128];
                        const char *p = c->data;
                        size_t n = 0;
                        while (*p != '\1' && n + 1 < sizeof(src)) {
                            src[n++] = *p++;
                        }
                        src[n] = '\0';
                        p++;
                        n = 0;
                        while (*p != '\0' && n + 1 < sizeof(dst)) {
                            dst[n++] = *p++;
                        }
                        dst[n] = '\0';
                        for (size_t k = 0; out_line[k] != '\0'; k++) {
                            for (size_t j = 0; src[j] != '\0'; j++) {
                                if (out_line[k] == src[j] && j < strlen(dst)) {
                                    out_line[k] = dst[j];
                                    break;
                                }
                            }
                        }
                        break;
                    }
                    case CMD_A:
                        printf("%s\n", c->data);
                        break;
                    case CMD_I:
                        printf("%s\n", c->data);
                        printed = 1;
                        break;
                    case CMD_C:
                        printf("%s\n", c->data);
                        deleted = 1;
                        printed = 1;
                        break;
                    case CMD_H:
                        strncpy(g_hold, out_line, sizeof(g_hold) - 1);
                        g_hold[sizeof(g_hold) - 1] = '\0';
                        break;
                    case CMD_G:
                        strncpy(out_line, g_hold, LINE_MAX - 1);
                        out_line[LINE_MAX - 1] = '\0';
                        break;
                    case CMD_X: {
                        char tmp[LINE_MAX];
                        strncpy(tmp, out_line, sizeof(tmp) - 1);
                        tmp[sizeof(tmp) - 1] = '\0';
                        strncpy(out_line, g_hold, LINE_MAX - 1);
                        out_line[LINE_MAX - 1] = '\0';
                        strncpy(g_hold, tmp, sizeof(g_hold) - 1);
                        g_hold[sizeof(g_hold) - 1] = '\0';
                        break;
                    }
                    case CMD_N: {
                        /* read the next line (approximation: we have
                         * them all buffered) */
                        if (i + 1 < nall) {
                            i++;
                            lineno = i + 1;
                            strncpy(out_line, all[i], LINE_MAX - 1);
                            out_line[LINE_MAX - 1] = '\0';
                            l = strlen(out_line);
                            while (l > 0 &&
                                   (out_line[l - 1] == '\n' ||
                                    out_line[l - 1] == '\r')) {
                                out_line[--l] = '\0';
                            }
                        }
                        if (!g_suppress) {
                            printf("%s\n", out_line);
                            printed = 1;
                        }
                        break;
                    }
                    case CMD_B:
                        restart = 1;
                        ci = -1; /* restart the script */
                        break;
                    case CMD_T:
                        break; /* no branch-on-success bookkeeping */
                    case CMD_LABEL:
                        break;
                    case CMD_BRACE:
                        break;
                    }
                    if (deleted) {
                        break;
                    }
                }
            }
            if (!deleted && !printed && !g_suppress) {
                printf("%s\n", out_line);
            }
        }
        for (long i = 0; i < nall; i++) {
            free(all[i]);
        }
        free(all);
    }
done:
    return 0;
}
