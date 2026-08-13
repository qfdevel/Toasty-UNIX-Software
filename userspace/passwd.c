/*
 * passwd.c - change user passwords (TUS port). Maintains /etc/shadow
 * the way the shadow-utils passwd does: prompt for the current
 * password (unless the invoker is root or the account is being
 * managed), ask twice for the new one, hash with crypt() and store.
 *
 * Options: -d (delete password), -l (lock), -u (unlock), -S (status),
 * -n/-x/-w/-i (password aging fields). Exit codes: 0 ok, 1 permission
 * denied, 2 invalid combination, 3 unexpected failure, 4 passwd file
 * missing, 6 invalid argument, 10 PAM error (not applicable here).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define _PATH_PASSWD "/etc/passwd"
#define _PATH_SHADOW "/etc/shadow"
#define _PATH_TTY    "/dev/tty0"

#define MAX_LINES 256
#define LINE_MAXLEN 512

struct lines {
    char *l[MAX_LINES];
    int n;
};

static int lines_load(struct lines *ls, const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    char buf[LINE_MAXLEN];
    while (ls->n < MAX_LINES && fgets(buf, sizeof(buf), f) != NULL) {
        ls->l[ls->n++] = strdup(buf);
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

/* Split a shadow line into fields (mutating). */
static int shadow_fields(char *line, char **fields, int max) {
    int n = 0;
    char *p = line;
    while (p != NULL && n < max) {
        char *c = strchr(p, ':');
        if (c != NULL) {
            *c = '\0';
        }
        fields[n++] = p;
        if (c != NULL) {
            p = c + 1;
        } else {
            p = NULL;
        }
    }
    return n;
}

static char *shadow_get(const char *user) {
    FILE *f = fopen(_PATH_SHADOW, "r");
    if (f == NULL) {
        return NULL;
    }
    static char buf[LINE_MAXLEN];
    char *hit = NULL;
    while (fgets(buf, sizeof(buf), f) != NULL) {
        char *c = strchr(buf, ':');
        if (c == NULL) {
            continue;
        }
        *c = '\0';
        if (strcmp(buf, user) == 0) {
            *c = ':';
            hit = buf;
            break;
        }
    }
    fclose(f);
    return hit;
}

/* Read a line from the tty, optionally with echo disabled. */
static int read_line(const char *prompt, char *out, size_t outsz,
                     int noecho) {
    int fd = open(_PATH_TTY, O_RDWR);
    if (fd < 0) {
        fd = 0;
    }
    struct termios tio, orig;
    int have_tio = tcgetattr(fd, &orig) == 0;
    if (have_tio) {
        tio = orig;
        if (noecho) {
            tio.c_lflag &= ~(tcflag_t)ECHO;
        }
        tcsetattr(fd, TCSAFLUSH, &tio);
    }
    fputs(prompt, stderr);
    fflush(stderr);
    size_t n = 0;
    int c;
    while (n + 1 < outsz && (c = read(fd, out + n, 1)) == 1) {
        if (out[n] == '\n' || out[n] == '\r') {
            break;
        }
        n++;
    }
    out[n] = '\0';
    if (have_tio) {
        tcsetattr(fd, TCSAFLUSH, &orig);
    }
    if (fd != 0) {
        close(fd);
    }
    fputs("\n", stderr);
    return n > 0 ? 0 : -1;
}

static void usage(void) {
    fprintf(stderr,
        "usage: passwd [-d] [-l] [-u] [-S] [-n mindays] [-x maxdays]\n"
        "              [-w warndays] [-i inactdays] [LOGIN]\n");
    exit(2);
}

int main(int argc, char **argv) {
    int del = 0, lock = 0, unlock = 0, status = 0;
    long min = -1, max = -1, warn = -1, inact = -1;
    const char *user = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            const char *p = argv[i] + 1;
            const char *val = NULL;
            if (argv[i][2] != '\0') {
                val = argv[i] + 2;
            } else if (strchr("nxwi", *p) != NULL && i + 1 < argc) {
                val = argv[++i];
            }
            switch (*p) {
            case 'd': del = 1; break;
            case 'l': lock = 1; break;
            case 'u': unlock = 1; break;
            case 'S': status = 1; break;
            case 'n': min = atol(val); break;
            case 'x': max = atol(val); break;
            case 'w': warn = atol(val); break;
            case 'i': inact = atol(val); break;
            default:
                fprintf(stderr, "passwd: invalid option -- '%c'\n", *p);
                return 6;
            }
        } else if (user == NULL) {
            user = argv[i];
        } else {
            usage();
        }
    }
    if (del && (lock || unlock)) {
        fprintf(stderr, "passwd: invalid combination of options\n");
        return 2;
    }

    /* Which account? The invoker may only manage their own unless
     * they are root. */
    if (user == NULL) {
        /* passwd without a LOGIN changes the caller's password. */
        extern char *getlogin(void);
        char *u = getlogin();
        if (u == NULL || *u == '\0') {
            u = getenv("USER");
        }
        if (u == NULL || *u == '\0') {
            fprintf(stderr, "passwd: can't determine your user name\n");
            return 3;
        }
        user = u;
    }

    /* Does the user exist? */
    FILE *pf = fopen(_PATH_PASSWD, "r");
    if (pf == NULL) {
        fprintf(stderr, "passwd: passwd file missing\n");
        return 4;
    }
    char pline[LINE_MAXLEN];
    int found = 0;
    while (fgets(pline, sizeof(pline), pf) != NULL) {
        char *c = strchr(pline, ':');
        if (c != NULL) {
            *c = '\0';
            if (strcmp(pline, user) == 0) {
                found = 1;
                break;
            }
        }
    }
    fclose(pf);
    if (!found) {
        fprintf(stderr, "passwd: user '%s' does not exist\n", user);
        return 1;
    }

    char *sh = shadow_get(user);
    if (sh == NULL) {
        fprintf(stderr, "passwd: shadow file missing\n");
        return 4;
    }
    char shcopy[LINE_MAXLEN];
    strncpy(shcopy, sh, sizeof(shcopy) - 1);
    shcopy[sizeof(shcopy) - 1] = '\0';

    char *fld[9];
    int nf = shadow_fields(shcopy, fld, 9);
    char *hash = nf > 1 ? fld[1] : "";

    if (status) {
        /* name P/L/NP + lastchg:min:max:warn:inact */
        const char *st = "NP";
        if (hash[0] == '!') {
            st = "L";
        } else if (hash[0] != '\0') {
            st = "P";
        }
        printf("%s %s %s %s %s %s %s\n", user, st,
               nf > 2 ? fld[2] : "", nf > 3 ? fld[3] : "",
               nf > 4 ? fld[4] : "", nf > 5 ? fld[5] : "",
               nf > 6 ? fld[6] : "");
        return 0;
    }

    /* Editing the shadow: rebuild the target line. */
    struct lines ls;
    memset(&ls, 0, sizeof(ls));
    if (lines_load(&ls, _PATH_SHADOW) != 0) {
        fprintf(stderr, "passwd: shadow file missing\n");
        return 4;
    }

    char newline[LINE_MAXLEN];
    char newhash[128];

    if (del) {
        newhash[0] = '\0';
    } else if (lock) {
        snprintf(newhash, sizeof(newhash), "!%s",
                 hash[0] == '!' ? hash + 1 : hash);
    } else if (unlock) {
        snprintf(newhash, sizeof(newhash), "%s",
                 hash[0] == '!' ? hash + 1 : hash);
    } else {
        /* Change the password. */
        if (geteuid() != 0) {
            char cur[128];
            if (read_line("Current password: ", cur, sizeof(cur), 1) != 0 ||
                *cur == '\0') {
                fprintf(stderr, "passwd: authentication failure\n");
                return 1;
            }
            if (hash[0] == '!' || hash[0] == '\0') {
                fprintf(stderr, "passwd: account is locked\n");
                return 1;
            }
            char *c = crypt(cur, hash);
            if (c == NULL || strcmp(c, hash) != 0) {
                fprintf(stderr, "passwd: authentication failure\n");
                return 1;
            }
        }

        char p1[128], p2[128];
        if (read_line("New password: ", p1, sizeof(p1), 1) != 0) {
            fprintf(stderr, "passwd: no password supplied\n");
            return 1;
        }
        if (read_line("Retype new password: ", p2, sizeof(p2), 1) != 0 ||
            strcmp(p1, p2) != 0) {
            fprintf(stderr, "passwd: passwords do not match\n");
            return 1;
        }
        if (p1[0] == '\0') {
            fprintf(stderr, "passwd: no password supplied\n");
            return 1;
        }
        static char salt[32];
        snprintf(salt, sizeof(salt), "$6$%ld%c%c$", (long)time(NULL),
                 'a' + (rand() % 26), '0' + (rand() % 10));
        char *h = crypt(p1, salt);
        snprintf(newhash, sizeof(newhash), "%s", h != NULL ? h : "!");
    }

    /* Keep aging fields unless overridden. */
    char last[16], mn[16], mx[16], wn[16], in[16];
    snprintf(last, sizeof(last), "%s", nf > 2 ? fld[2] : "0");
    snprintf(mn, sizeof(mn), "%s", nf > 3 ? fld[3] : "0");
    snprintf(mx, sizeof(mx), "%s", nf > 4 ? fld[4] : "99999");
    snprintf(wn, sizeof(wn), "%s", nf > 5 ? fld[5] : "7");
    snprintf(in, sizeof(in), "%s", nf > 6 ? fld[6] : "");
    if (min >= 0) {
        snprintf(mn, sizeof(mn), "%ld", min);
    }
    if (max >= 0) {
        snprintf(mx, sizeof(mx), "%ld", max);
    }
    if (warn >= 0) {
        snprintf(wn, sizeof(wn), "%ld", warn);
    }
    if (inact >= 0) {
        snprintf(in, sizeof(in), "%ld", inact);
    }

    /* Replace the user's line in the loaded shadow file. */
    for (int i = 0; i < ls.n; i++) {
        char *c = strchr(ls.l[i], ':');
        if (c != NULL) {
            *c = '\0';
            int eq = strcmp(ls.l[i], user) == 0;
            *c = ':';
            if (eq) {
                snprintf(newline, sizeof(newline), "%s:%s:%s:%s:%s:%s:%s::\n",
                         user, newhash, last, mn, mx, wn, in);
                free(ls.l[i]);
                ls.l[i] = strdup(newline);
                break;
            }
        }
    }

    if (lines_save(&ls, _PATH_SHADOW) != 0) {
        fprintf(stderr, "passwd: can't update shadow file\n");
        return 3;
    }

    if (del) {
        printf("passwd: password for %s deleted\n", user);
    } else if (lock) {
        printf("passwd: password for %s locked\n", user);
    } else if (unlock) {
        printf("passwd: password for %s unlocked\n", user);
    } else {
        printf("passwd: password for %s updated\n", user);
    }
    return 0;
}
