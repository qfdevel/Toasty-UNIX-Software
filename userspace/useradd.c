/*
 * useradd.c - create a new user account (TUS port of the shadow-utils
 * useradd). Maintains /etc/passwd, /etc/shadow and /etc/group the way
 * the real tool does, including the standard exit codes:
 *
 *   0 success   1 can't update passwd   2 invalid syntax
 *   3 invalid argument   4 UID in use   6 group doesn't exist
 *   9 name in use   10 can't update group   12 can't create home
 *   19 invalid user/group name
 *
 * Supported options: -b -c -d -g -m -M -p -r -s -u -D (+ --help).
 */

#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define _PATH_PASSWD "/etc/passwd"
#define _PATH_SHADOW "/etc/shadow"
#define _PATH_GROUP  "/etc/group"
#define _PATH_DEF    "/etc/default/useradd"
#define _PATH_SKEL   "/etc/skel"

#define MAX_LINES 256
#define LINE_MAXLEN 512

/* ---- file helpers ---- */

struct lines {
    char *l[MAX_LINES];
    int n;
};

static void lines_free(struct lines *ls) {
    for (int i = 0; i < ls->n; i++) {
        free(ls->l[i]);
    }
    ls->n = 0;
}

static int lines_load(struct lines *ls, const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    char buf[LINE_MAXLEN];
    while (ls->n < MAX_LINES && fgets(buf, sizeof(buf), f) != NULL) {
        ls->l[ls->n] = strdup(buf);
        if (ls->l[ls->n] == NULL) {
            break;
        }
        ls->n++;
    }
    fclose(f);
    return 0;
}

static int lines_save(const struct lines *ls, const char *path) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return -1;
    }
    for (int i = 0; i < ls->n; i++) {
        fputs(ls->l[i], f);
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* Find the line whose first colon-field equals `name`. */
static int lines_find(const struct lines *ls, const char *name) {
    for (int i = 0; i < ls->n; i++) {
        char *c = strchr(ls->l[i], ':');
        if (c != NULL) {
            *c = '\0';
            int eq = strcmp(ls->l[i], name) == 0;
            *c = ':';
            if (eq) {
                return i;
            }
        }
    }
    return -1;
}

static int max_field(const struct lines *ls, int field) {
    /* field 0 = uid (passwd), 1 = gid (group) */
    int max = 0;
    for (int i = 0; i < ls->n; i++) {
        char *p = ls->l[i];
        int f = 0;
        while (p != NULL && *p != '\0' && f <= field) {
            char *c = strchr(p, ':');
            if (c != NULL) {
                *c = '\0';
            }
            if (f == field) {
                int v = atoi(p);
                if (v > max) {
                    max = v;
                }
            }
            if (c != NULL) {
                *c = ':';
                p = c + 1;
            } else {
                p = NULL;
            }
            f++;
        }
    }
    return max;
}

/* ---- user name validation ---- */

static int valid_name(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len > 32) {
        return 0;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    if (name[0] == '-' || name[0] == '.') {
        return 0;
    }
    int all_digits = 1;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) {
            return 0;
        }
        if (!isdigit((unsigned char)c)) {
            all_digits = 0;
        }
    }
    if (all_digits) {
        return 0;
    }
    return 1;
}

/* ---- defaults (-D) ---- */

static const char *def_base_dir = "/home";
static const char *def_shell = "/bin/tsh";
static const char *def_group = ""; /* empty: create a user group */

static void defaults_print(void) {
    printf("GROUP=%s\n", def_group[0] ? def_group : "100");
    printf("HOME=%s\n", def_base_dir);
    printf("SHELL=%s\n", def_shell);
    printf("SKEL=%s\n", _PATH_SKEL);
    printf("CREATE_MAIL_SPOOL=no\n");
}

/* ---- main ---- */

static void usage(void) {
    fprintf(stderr,
        "usage: useradd [options] LOGIN\n"
        "       useradd -D [options]\n"
        "options: -b BASE_DIR  -c COMMENT  -d HOME_DIR  -g GROUP\n"
        "         -m (create home)  -M (no home)  -p PASSWORD  -r (system)\n"
        "         -s SHELL  -u UID  -D (defaults)  --help\n");
    exit(2);
}

int main(int argc, char **argv) {
    const char *base = def_base_dir;
    const char *comment = "";
    const char *home = NULL;
    const char *gid_opt = NULL;
    const char *pass_opt = NULL;
    const char *shell = def_shell;
    const char *name = NULL;
    int uid_opt = -1;
    int make_home = 0;
    int no_home = 0;
    int system_acct = 0;
    int defaults_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
        } else if (strcmp(argv[i], "-D") == 0) {
            defaults_mode = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0' && name == NULL) {
            const char *opt = argv[i] + 1;
            const char *val = NULL;
            if (argv[i][2] != '\0') {
                val = argv[i] + 2;
            } else if (strchr("bcdgpsu", *opt) != NULL && i + 1 < argc) {
                val = argv[++i];
            }
            switch (*opt) {
            case 'b': base = val; break;
            case 'c': comment = val; break;
            case 'd': home = val; break;
            case 'g': gid_opt = val; break;
            case 'm': make_home = 1; break;
            case 'M': no_home = 1; break;
            case 'p': pass_opt = val; break;
            case 'r': system_acct = 1; break;
            case 's': shell = val; break;
            case 'u': uid_opt = atoi(val); break;
            default:
                fprintf(stderr, "useradd: invalid option -- '%c'\n", *opt);
                return 3;
            }
        } else if (name == NULL) {
            name = argv[i];
        } else {
            usage();
        }
    }

    if (defaults_mode) {
        if (base != def_base_dir) {
            /* update defaults: persist to /etc/default/useradd */
            FILE *f = fopen(_PATH_DEF, "w");
            if (f != NULL) {
                fprintf(f, "HOME=%s\nSHELL=%s\nGROUP=%s\n",
                        base, shell, gid_opt ? gid_opt : "");
                fclose(f);
            }
        }
        defaults_print();
        return 0;
    }

    if (name == NULL || !valid_name(name)) {
        fprintf(stderr, "useradd: invalid user name '%s'\n",
                name != NULL ? name : "");
        return 19;
    }

    struct lines pw, sh, gr;
    lines_load(&pw, _PATH_PASSWD);
    lines_load(&sh, _PATH_SHADOW);
    lines_load(&gr, _PATH_GROUP);

    if (lines_find(&pw, name) >= 0) {
        fprintf(stderr, "useradd: user '%s' already exists\n", name);
        lines_free(&pw);
        lines_free(&sh);
        lines_free(&gr);
        return 9;
    }

    /* UID. */
    int uid;
    if (uid_opt >= 0) {
        uid = uid_opt;
        /* check it is not in use */
        struct lines all;
        lines_load(&all, _PATH_PASSWD);
        for (int i = 0; i < all.n; i++) {
            int f = 0;
            char *p = all.l[i];
            while (p != NULL && f <= 2) {
                char *c = strchr(p, ':');
                if (c != NULL) {
                    *c = '\0';
                }
                if (f == 2 && atoi(p) == uid) {
                    fprintf(stderr, "useradd: UID %d is already in use\n", uid);
                    lines_free(&all);
                    return 4;
                }
                if (c != NULL) {
                    *c = ':';
                    p = c + 1;
                } else {
                    p = NULL;
                }
                f++;
            }
        }
        lines_free(&all);
    } else {
        int min = system_acct ? 101 : 1000;
        uid = max_field(&pw, 0) + 1;
        if (uid < min) {
            uid = min;
        }
    }

    /* GID: explicit group name/number, or a new user group = uid. */
    int gid;
    char gid_name[64];
    if (gid_opt != NULL) {
        if (isdigit((unsigned char)gid_opt[0])) {
            gid = atoi(gid_opt);
        } else {
            int gi = lines_find(&gr, gid_opt);
            if (gi < 0) {
                fprintf(stderr, "useradd: group '%s' does not exist\n", gid_opt);
                return 6;
            }
            char *p = gr.l[gi];
            for (int f = 0; f < 2 && p != NULL; f++) {
                char *c = strchr(p, ':');
                if (c != NULL) {
                    *c = '\0';
                }
                if (f == 1) {
                    gid = atoi(p);
                }
                if (c != NULL) {
                    *c = ':';
                    p = c + 1;
                } else {
                    p = NULL;
                }
            }
        }
        snprintf(gid_name, sizeof(gid_name), "%s", gid_opt);
    } else {
        gid = uid;
        snprintf(gid_name, sizeof(gid_name), "%s", name);
        /* create the user group */
        char line[LINE_MAXLEN];
        snprintf(line, sizeof(line), "%s:x:%d:\n", name, gid);
        gr.l[gr.n++] = strdup(line);
        if (lines_save(&gr, _PATH_GROUP) != 0) {
            fprintf(stderr, "useradd: can't update group file\n");
            return 10;
        }
    }

    if (home == NULL) {
        char h[256];
        snprintf(h, sizeof(h), "%s/%s", base, name);
        home = strdup(h);
    }

    /* /etc/passwd entry. */
    char pwline[LINE_MAXLEN];
    snprintf(pwline, sizeof(pwline), "%s:x:%d:%d:%s:%s:%s\n",
             name, uid, gid, comment, home, shell);
    pw.l[pw.n++] = strdup(pwline);

    /* /etc/shadow entry: locked (no usable password) unless -p. */
    char shline[LINE_MAXLEN];
    if (pass_opt != NULL) {
        static char saltbuf[32];
        snprintf(saltbuf, sizeof(saltbuf), "$6$%ld%c%c$",
                 (long)time(NULL), 'a' + (rand() % 26), '0' + (rand() % 10));
        char *hash = crypt(pass_opt, saltbuf);
        snprintf(shline, sizeof(shline), "%s:%s:0:0:99999:7::\n",
                 name, hash != NULL ? hash : "!");
    } else {
        snprintf(shline, sizeof(shline), "%s:!:0:0:99999:7::\n", name);
    }
    sh.l[sh.n++] = strdup(shline);

    int rc = 0;
    if (lines_save(&pw, _PATH_PASSWD) != 0) {
        fprintf(stderr, "useradd: can't update password file\n");
        rc = 1;
    } else if (lines_save(&sh, _PATH_SHADOW) != 0) {
        fprintf(stderr, "useradd: can't update shadow file\n");
        rc = 1;
    }

    if (rc == 0 && make_home && !no_home) {
        /* Create the home directory (and its parents, e.g. /home). */
        char tmp[256];
        strncpy(tmp, home, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        for (char *p = tmp + 1; *p != '\0'; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(tmp, 0755);
                *p = '/';
            }
        }
        if (mkdir(home, 0700) != 0) {
            fprintf(stderr, "useradd: can't create home directory %s\n", home);
            rc = 12;
        }
    }

    if (rc == 0) {
        printf("useradd: user %s added (uid %d, gid %d, home %s)\n",
               name, uid, gid, home);
    }
    return rc;
}
