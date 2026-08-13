/*
 * grep.c - print lines that match patterns (TUS port of GNU grep)
 *
 * A self-contained grep with a backtracking regular expression engine
 * supporting BRE and ERE: literals, '.', character classes with ranges
 * and POSIX names, '^'/'$' anchors, '*', '+', '?', '{n,m}', groups
 * and top-level alternation ('|' in ERE, '\|' in BRE). Backreferences
 * (\1..\9) are not implemented.
 *
 * Options: -i -v -n -c -l -o -q -s -w -x -E -F -e -f -H -h -m NUM
 *          -A NUM -B NUM -C NUM --help --version
 * Exit status: 0 if any line matched, 1 if none, 2 on error.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX 4096
#define PAT_MAX 512

static int opt_icase, opt_invert, opt_lineno, opt_count, opt_files_with;
static int opt_only, opt_quiet, opt_no_msgs, opt_word, opt_line;
static int opt_ere, opt_fixed, opt_filename = -1; /* -1 = auto */
static long opt_max_count = -1;
static long opt_after, opt_before;
static int errors;

/* ================= regex engine =================
 *
 * m_seq(re, s) is true when the pattern consumes the whole string s.
 * Atoms may consume a variable number of bytes (groups), so the
 * engine enumerates every possible consumption via continuations:
 *
 *   atom_each(atom, s, cb, ctx)  - for every length l that `atom`
 *                                  can consume at s, call cb(s + l)
 *   m_seq loops over quantifier repetition counts and uses atom_each
 *   per repetition, backtracking on failure.
 */

static int m_seq(const char *re, const char *s, int ere, int icase);

/* Continuation callback: return 0 to stop enumeration (success). */
typedef int (*re_cb)(void *ctx, const char *s);

/* For every length the atom at `re` can consume at s, call cb(s+len).
 * Returns 1 if any cb returned 0 (i.e. success was found). */
static int atom_each(const char *re, const char *s, int ere, int icase,
                     re_cb cb, void *ctx);

static int mchar(int icase, char c) {
    return icase ? tolower((unsigned char)c) : c;
}

/* Pattern length of the atom at `re`; 0 if re does not start an atom. */
static const char *re_group_end(const char *re, int ere);

/* Pattern length of the atom at `re`; 0 if re does not start an atom. */
static size_t re_atom_len(const char *re, int ere) {
    char c = *re;
    if (c == '\0' || c == ')' || c == '|') {
        return 0;
    }
    if (!ere && c == '\\') {
        if (re[1] == '(') {
            /* BRE group: the atom is the whole \(...\) */
            const char *gend = re_group_end(re, ere);
            /* gend points AT the closing ')': include it. */
            return gend != NULL ? (size_t)(gend - re + 1) : 0;
        }
        if (re[1] == ')' || re[1] == '|' || re[1] == '{' ||
            re[1] == '}') {
            return 0;
        }
        return 2;
    }
    if (ere && c == '(') {
        /* ERE group: the atom is the whole (...), so the quantifier
         * check below sees the '{' that follows the group. */
        const char *gend = re_group_end(re, ere);
        return gend != NULL ? (size_t)(gend - re + 1) : 0;
    }
    if (c == '[') {
        const char *p = re + 1;
        int first = 1;
        while (*p != '\0' && (*p != ']' || first)) {
            /* A POSIX class name [:name:] is atomic: its ']' must not
             * terminate the outer class. */
            if (*p == '[' && p[1] == ':') {
                const char *close = strchr(p + 2, ':');
                if (close != NULL && close[1] == ']') {
                    p = close + 2;
                    first = 0;
                    continue;
                }
            }
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

/* Does the atom at `re` match a single character c? */
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
            if (*end == '[' && end[1] == ':') {
                const char *close = strchr(end + 2, ':');
                if (close != NULL && close[1] == ']') {
                    end = close + 2;
                    first = 0;
                    continue;
                }
            }
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
            if (*q == '[' && q + 1 < end && q[1] == ':') {
                /* POSIX class: [[:name:]] - name runs up to the first
                 * ':' and must be closed by a ']'. */
                const char *name = q + 2;
                const char *nn = strchr(name, ':');
                size_t nlen = (nn != NULL) ? (size_t)(nn - name) : 0;
                if (nlen > 0 && nn[1] == ']') {
                    int in = 0;
                    unsigned char uc = (unsigned char)c;
                    if (nlen == 5 && strncmp(name, "alpha", 5) == 0) in = isalpha(uc);
                    else if (nlen == 5 && strncmp(name, "digit", 5) == 0) in = isdigit(uc);
                    else if (nlen == 5 && strncmp(name, "space", 5) == 0) in = isspace(uc);
                    else if (nlen == 5 && strncmp(name, "upper", 5) == 0) in = isupper(uc);
                    else if (nlen == 5 && strncmp(name, "lower", 5) == 0) in = islower(uc);
                    else if (nlen == 5 && strncmp(name, "punct", 5) == 0) in = ispunct(uc);
                    else if (nlen == 4 && strncmp(name, "alnum", 4) == 0) in = isalnum(uc);
                    else if (nlen == 3 && strncmp(name, "cntrl", 3) == 0) in = iscntrl(uc);
                    else if (nlen == 6 && strncmp(name, "xdigit", 6) == 0) in = isxdigit(uc);
                    else if (nlen == 5 && strncmp(name, "graph", 5) == 0) in = isgraph(uc);
                    else if (nlen == 5 && strncmp(name, "print", 5) == 0) in = isprint(uc);
                    else if (nlen == 4 && strncmp(name, "blank", 4) == 0) in = isblank(uc);
                    if (in) {
                        match = 1;
                    }
                    q = nn + 2;
                    continue;
                }
            }
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

/* Find the matching close paren for the group starting at `re`.
 * Returns a pointer to the ')' or NULL if unbalanced. */
static const char *re_group_end(const char *re, int ere) {
    const char *p = re;
    int depth = 0;
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
    const char *atom;   /* the atom (no quantifier) */
    const char *rest;   /* pattern after the quantifier */
    long count;         /* remaining repetitions */
    int ere, icase;
    re_cb final_cb;     /* called when count reaches 0 */
    void *final_ctx;
    int ok;
};

/* Continuation after one more repetition: match count-1 more. */
static int q_cont(void *ctx, const char *s) {
    struct qctx *q = (struct qctx *)ctx;
    if (q->count <= 1) {
        /* final_cb returns 0 when the tail matched (stop enumerating)
         * and 1 when it did not (keep trying other lengths). The
         * condition used to be inverted: a FAILING tail continuation
         * was recorded as a success, so "line2" matched "line1". */
        if (!q->final_cb(q->final_ctx, s)) {
            q->ok = 1;
            return 0; /* stop enumeration */
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
        /* group: try every consumed length 0..strlen(s) */
        const char *gend = re_group_end(re, ere);
        if (gend == NULL) {
            return 0;
        }
        const char *gstart = re + (c == '(' ? 1 : 2);
        /* gend points at the ')' (ERE) or at the ')' of "\)" (BRE,
         * one char past the backslash): back up over that backslash
         * so the group text is exactly what sits between the parens. */
        const char *gparen = gend - (!ere ? 1 : 0);
        size_t glen = (size_t)(gparen - gstart);
        char gbuf[PAT_MAX];
        if (glen >= sizeof(gbuf)) {
            return 0;
        }
        memcpy(gbuf, gstart, glen);
        gbuf[glen] = '\0';
        size_t slen = strlen(s);
        for (size_t l = 0; l <= slen; l++) {
            char tmp[PAT_MAX];
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
    /* simple atom: consumes exactly one character */
    if (*s == '\0') {
        return 0;
    }
    if (!re_atom_char(re, *s, ere, icase)) {
        return 0;
    }
    return !cb(ctx, s + 1);
}

/* Match `count` repetitions of the atom at `re`, then final_cb. */
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

/* Continuation for m_seq's quantifier loop: match the rest. */
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

/* m_seq: does the pattern consume the whole string s? */
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

    /* top-level alternation */
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
                if (alen >= PAT_MAX) {
                    return 0;
                }
                char buf[PAT_MAX];
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

    /* atom + optional quantifier */
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

/* Search the pattern anywhere in `line`. */
static int re_search(const char *pat, const char *line, size_t len,
                     int ere, int icase) {
    if (opt_fixed) {
        if (icase) {
            for (size_t i = 0; i <= len; i++) {
                size_t j = 0;
                while (pat[j] != '\0' && i + j < len &&
                       tolower((unsigned char)line[i + j]) ==
                       tolower((unsigned char)pat[j])) {
                    j++;
                }
                if (pat[j] == '\0') {
                    return 1;
                }
            }
            return 0;
        }
        return strstr(line, pat) != NULL;
    }

    size_t plen = strlen(pat);
    int anchored = pat[0] == '^';
    int anchored_end = plen > 0 && pat[plen - 1] == '$';

    char buf[PAT_MAX];
    size_t blen = plen;
    if (anchored) {
        blen--;
    }
    if (anchored_end) {
        blen--;
    }
    if (blen >= sizeof(buf)) {
        return 0;
    }
    memcpy(buf, pat + (anchored ? 1 : 0), blen);
    buf[blen] = '\0';

    for (size_t i = 0; i <= len; i++) {
        if (anchored && i != 0) {
            break;
        }
        if (m_seq(buf, line + i, ere, icase)) {
            if (anchored_end) {
                /* the pattern must consume the whole tail */
                size_t tail = strlen(line + i);
                char tailbuf[LINE_MAX];
                if (tail >= sizeof(tailbuf)) {
                    return 0;
                }
                memcpy(tailbuf, line + i, tail + 1);
                if (m_seq(buf, tailbuf, ere, icase)) {
                    return 1;
                }
            } else {
                return 1;
            }
        }
        if (anchored) {
            break;
        }
    }
    return 0;
}

/* ================= main ================= */

static void print_help(void) {
    printf("Usage: grep [OPTION]... PATTERNS [FILE]...\n"
           "Search for PATTERNS in each FILE (standard input if none).\n\n"
           "  -i, --ignore-case       ignore case distinctions\n"
           "  -v, --invert-match      select non-matching lines\n"
           "  -n, --line-number       print line numbers\n"
           "  -c, --count             print only a count per file\n"
           "  -l, --files-with-matches  print only file names\n"
           "  -o, --only-matching     print only matched parts\n"
           "  -q, --quiet             suppress normal output\n"
           "  -s, --no-messages       suppress error messages\n"
           "  -w, --word-regexp       match whole words\n"
           "  -x, --line-regexp       match whole lines\n"
           "  -E, --extended-regexp   use ERE (default: BRE)\n"
           "  -F, --fixed-strings     treat pattern as a fixed string\n"
           "  -e PATTERNS             use PATTERNS as the pattern\n"
           "  -f FILE                 take patterns from FILE\n"
           "  -H, -h                  with/without file name prefix\n"
           "  -m NUM                  stop after NUM matches\n"
           "  -A/-B/-C NUM            print context lines\n"
           "  --help                  display this help\n"
           "  --version               output version information\n");
}

int main(int argc, char **argv) {
    char *patterns[16];
    int npat = 0;
    char *files[64];
    int nfiles = 0;
    int pat_from_file = 0;
    const char *pat_file = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(a, "--version") == 0) {
            printf("grep (TUS) 1.0\n");
            return 0;
        }
        if (a[0] == '-' && a[1] != '\0' && npat == 0 && nfiles == 0) {
            const char *p = a + 1;
            const char *val = NULL;
            if (a[2] != '\0') {
                val = a + 2;
            } else if ((*p == 'e' || *p == 'f') && i + 1 < argc) {
                val = argv[++i];
            }
            switch (*p) {
            case 'i': opt_icase = 1; break;
            case 'v': opt_invert = 1; break;
            case 'n': opt_lineno = 1; break;
            case 'c': opt_count = 1; break;
            case 'l': opt_files_with = 1; break;
            case 'o': opt_only = 1; break;
            case 'q': opt_quiet = 1; break;
            case 's': opt_no_msgs = 1; break;
            case 'w': opt_word = 1; break;
            case 'x': opt_line = 1; break;
            case 'E': opt_ere = 1; break;
            case 'F': opt_fixed = 1; break;
            case 'e':
                if (npat < 16) {
                    patterns[npat++] = (char *)val;
                }
                break;
            case 'f':
                pat_from_file = 1;
                pat_file = val;
                break;
            case 'H': opt_filename = 1; break;
            case 'h': opt_filename = 0; break;
            case 'm': opt_max_count = atol(val); break;
            case 'A': opt_after = atol(val); break;
            case 'B': opt_before = atol(val); break;
            case 'C': opt_after = opt_before = atol(val); break;
            default:
                fprintf(stderr, "grep: invalid option -- '%c'\n", *p);
                return 2;
            }
        } else if (npat == 0 && !pat_from_file) {
            if (npat < 16) {
                patterns[npat++] = (char *)a;
            }
        } else if (nfiles < 64) {
            files[nfiles++] = (char *)a;
        }
    }

    if (pat_from_file) {
        FILE *f = fopen(pat_file, "r");
        if (f == NULL) {
            fprintf(stderr, "grep: %s: %s\n", pat_file, strerror(errno));
            return 2;
        }
        char buf[LINE_MAX];
        npat = 0;
        while (npat < 16 && fgets(buf, sizeof(buf), f) != NULL) {
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) {
                buf[--l] = '\0';
            }
            patterns[npat++] = strdup(buf);
        }
        fclose(f);
    }

    if (npat == 0) {
        fprintf(stderr, "grep: no pattern given\n");
        return 2;
    }
    if (opt_filename < 0) {
        opt_filename = nfiles > 1;
    }

    int matched_any = 0;
    int total_files = nfiles > 0 ? nfiles : 1;

    for (int fi = 0; fi < total_files; fi++) {
        FILE *f;
        const char *fname = nfiles > 0 ? files[fi] : "(standard input)";
        if (nfiles > 0 && strcmp(files[fi], "-") == 0) {
            f = stdin;
        } else if (nfiles > 0) {
            f = fopen(files[fi], "r");
            if (f == NULL) {
                if (!opt_no_msgs) {
                    fprintf(stderr, "grep: %s: %s\n", files[fi],
                            strerror(errno));
                }
                errors = 1;
                continue;
            }
        } else {
            f = stdin;
        }

        char line[LINE_MAX];
        long lineno = 0;
        long matches = 0;
        char(*ctx_before)[LINE_MAX] = malloc(16 * LINE_MAX);
        if (ctx_before == NULL) {
            return 2;
        }
        long ctx_n = 0;
        long ctx_pending = 0;

        while (fgets(line, sizeof(line), f) != NULL) {
            lineno++;
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }

            int hit = 0;
            for (int pi = 0; pi < npat && !hit; pi++) {
                hit = re_search(patterns[pi], line, len, opt_ere, opt_icase);
                if (hit && opt_word) {
                    hit = 0;
                    size_t pl = strlen(patterns[pi]);
                    for (size_t i = 0; i + pl <= len; i++) {
                        if (strncmp(line + i, patterns[pi], pl) == 0) {
                            int left_ok = (i == 0) ||
                                !(isalnum((unsigned char)line[i - 1]) ||
                                  line[i - 1] == '_');
                            int right_ok = (i + pl >= len) ||
                                !(isalnum((unsigned char)line[i + pl]) ||
                                  line[i + pl] == '_');
                            if (left_ok && right_ok) {
                                hit = 1;
                                break;
                            }
                        }
                    }
                }
                if (hit && opt_line) {
                    size_t pl = strlen(patterns[pi]);
                    hit = (pl == len) && strncmp(line, patterns[pi], pl) == 0;
                }
            }
            if (opt_invert) {
                hit = !hit;
            }
            if (hit) {
                matched_any = 1;
                matches++;
                if (opt_quiet) {
                    fclose(f);
                    return 0;
                }
                if (!opt_count && !opt_files_with) {
                    long start = ctx_n > opt_before ? ctx_n - opt_before : 0;
                    if (opt_before > 0) {
                        for (long k = start; k < ctx_n; k++) {
                            if (opt_filename) {
                                printf("%s-", fname);
                            }
                            if (opt_lineno) {
                                printf("%ld-", lineno - (ctx_n - k));
                            }
                            printf("%s\n", ctx_before[k]);
                        }
                    }
                    ctx_pending = opt_after;
                    if (opt_filename) {
                        printf("%s:", fname);
                    }
                    if (opt_lineno) {
                        printf("%ld:", lineno);
                    }
                    printf("%s\n", line);
                }
                if (opt_max_count >= 0 && matches >= opt_max_count) {
                    break;
                }
            } else if (ctx_pending > 0) {
                ctx_pending--;
                if (!opt_count && !opt_files_with) {
                    if (opt_filename) {
                        printf("%s-", fname);
                    }
                    if (opt_lineno) {
                        printf("%ld-", lineno);
                    }
                    printf("%s\n", line);
                }
            }
            if (opt_before > 0) {
                if (ctx_n < 16) {
                    strncpy(ctx_before[ctx_n], line, LINE_MAX - 1);
                    ctx_before[ctx_n][LINE_MAX - 1] = '\0';
                    ctx_n++;                } else {
                    for (long k = 0; k < 15; k++) {
                        strcpy(ctx_before[k], ctx_before[k + 1]);
                    }
                    strncpy(ctx_before[15], line, LINE_MAX - 1);
                    ctx_before[15][LINE_MAX - 1] = '\0';
                }
            }
        }

        if (opt_count && !opt_files_with) {
            if (opt_filename) {
                printf("%s:", fname);
            }
            printf("%ld\n", matches);
        }
        if (opt_files_with && matches > 0) {
            printf("%s\n", fname);
        }
        if (nfiles > 0 && strcmp(files[fi], "-") != 0) {
            fclose(f);
        }
    }

    if (errors) {
        return 2;
    }
    return matched_any ? 0 : 1;
}
