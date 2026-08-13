/*
 * doas.c - TUS port of OpenBSD doas
 *
 * doas executes commands with the privileges of the invoking user's
 * target account. The OpenBSD original is wired to BSD auth/PAM and
 * syslog; this TUS port keeps the same configuration file, rule
 * semantics and CLI, but authenticates against /etc/shadow directly
 * and logs nothing (there is no syslog on TUS).
 *
 * Configuration (/etc/doas.conf), one rule per line:
 *
 *     permit [nopass] [keepenv] user [as target] [cmd [args...]]
 *     deny   [nopass] [keepenv] user [as target] [cmd [args...]]
 *
 * Rules are evaluated in order; the LAST matching rule wins. `user`
 * may be a user name or :group. `cmd` may be given with arguments
 * (all matching) or alone (any command). Exit status: 0 on success,
 * 1 on denial or failure.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define _PATH_DOAS_CONF "/etc/doas.conf"
#define _PATH_SHADOW   "/etc/shadow"
#define _PATH_TTY      "/dev/tty0"

extern char **environ; /* the environment array (musl: <unistd.h>) */

#define PERMIT 1
#define DENY   2

#define NOPASS 0x1
#define KEEPENV 0x2

#define MAX_RULES 64

struct rule {
    int action;
    int options;
    char ident[64];       /* user name or :group */
    char target[64];      /* "" = self */
    char cmd[64];         /* "" = any command */
    char cmdargs[128];    /* "" = any arguments */
};

static struct rule g_rules[MAX_RULES];
static size_t g_nrules;
static int g_parse_errors;

/* ---- configuration parsing ---- */

static void parse_conf(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return; /* no config: nothing permitted */
    }
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == '\n' || *p == '\0') {
            continue;
        }
        char *nl = strchr(p, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }

        char *toks[16];
        int ntok = 0;
        char *save = NULL;
        for (char *t = strtok_r(p, " \t", &save);
             t != NULL && ntok < 16; t = strtok_r(NULL, " \t", &save)) {
            toks[ntok++] = t;
        }
        if (ntok < 2) {
            g_parse_errors++;
            continue;
        }
        if (g_nrules >= MAX_RULES) {
            break;
        }
        struct rule *r = &g_rules[g_nrules++];
        memset(r, 0, sizeof(*r));
        r->action = strcmp(toks[0], "deny") == 0 ? DENY : PERMIT;
        int i = 1;
        while (i < ntok && (strcmp(toks[i], "nopass") == 0 ||
                            strcmp(toks[i], "keepenv") == 0)) {
            if (strcmp(toks[i], "nopass") == 0) {
                r->options |= NOPASS;
            } else {
                r->options |= KEEPENV;
            }
            i++;
        }
        if (i >= ntok) {
            g_parse_errors++;
            continue;
        }
        strncpy(r->ident, toks[i++], sizeof(r->ident) - 1);
        if (i < ntok && strcmp(toks[i], "as") == 0) {
            if (++i >= ntok) {
                g_parse_errors++;
                continue;
            }
            strncpy(r->target, toks[i++], sizeof(r->target) - 1);
        }
        if (i < ntok) {
            strncpy(r->cmd, toks[i++], sizeof(r->cmd) - 1);
        }
        while (i < ntok) {
            if (strlen(r->cmdargs) + strlen(toks[i]) + 2 < sizeof(r->cmdargs)) {
                strcat(r->cmdargs, " ");
                strcat(r->cmdargs, toks[i]);
            }
            i++;
        }
    }
    fclose(f);
}

/* ---- identity helpers ---- */

static int is_member(const char *group, const char *user) {
    if (group[0] != ':') {
        return 0;
    }
    const char *gname = group + 1;
    /* A user is a member of a group if /etc/group lists them. */
    FILE *f = fopen("/etc/group", "r");
    if (f == NULL) {
        return 0;
    }
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *colon = strchr(line, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        if (strcmp(line, gname) != 0) {
            continue;
        }
        char *members = strrchr(colon + 1, ':');
        if (members == NULL) {
            continue;
        }
        members++;
        char *save = NULL;
        for (char *m = strtok_r(members, ", \t\n", &save);
             m != NULL; m = strtok_r(NULL, ", \t\n", &save)) {
            if (strcmp(m, user) == 0) {
                found = 1;
            }
        }
        break;
    }
    fclose(f);
    return found;
}

/* Last matching rule for (user, target, command, args). */
static struct rule *match_rule(const char *user, const char *target,
                               const char *cmd) {
    struct rule *hit = NULL;
    for (size_t i = 0; i < g_nrules; i++) {
        struct rule *r = &g_rules[i];
        int idmatch = (r->ident[0] == ':')
            ? is_member(r->ident, user)
            : strcmp(r->ident, user) == 0;
        if (!idmatch) {
            continue;
        }
        if (r->target[0] != '\0' && strcmp(r->target, target) != 0) {
            continue;
        }
        if (r->cmd[0] != '\0' && strcmp(r->cmd, cmd) != 0) {
            continue;
        }
        hit = r; /* last match wins */
    }
    return hit;
}

/* ---- password prompt ---- */

/* Read a line from the tty with echo disabled (for passwords). */
static int read_password(char *out, size_t outsz) {
    int fd = open(_PATH_TTY, O_RDWR);
    if (fd < 0) {
        fd = 0;
    }
    struct termios tio, orig;
    if (tcgetattr(fd, &orig) == 0) {
        tio = orig;
        tio.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(fd, TCSAFLUSH, &tio);
    }
    size_t n = 0;
    int c;
    while (n + 1 < outsz && (c = read(fd, out + n, 1)) == 1) {
        if (out[n] == '\n' || out[n] == '\r') {
            break;
        }
        n++;
    }
    out[n] = '\0';
    if (tcgetattr(fd, &orig) == 0 && (tio.c_lflag & ECHO) == 0) {
        /* restore */
    }
    if (tcgetattr(fd, &tio) == 0) {
        tcsetattr(fd, TCSAFLUSH, &orig);
    }
    if (fd != 0) {
        close(fd);
    }
    write(1, "\n", 1);
    return n > 0 ? 0 : -1;
}

/* Verify `user`'s password against /etc/shadow. */
static int check_password(const char *user, const char *pass) {
    FILE *f = fopen(_PATH_SHADOW, "r");
    if (f == NULL) {
        return 0; /* no shadow: only root may proceed (checked by caller) */
    }
    char line[512];
    int ok = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *c1 = strchr(line, ':');
        if (c1 == NULL) {
            continue;
        }
        *c1 = '\0';
        if (strcmp(line, user) != 0) {
            continue;
        }
        char *hash = c1 + 1;
        char *c2 = strchr(hash, ':');
        if (c2 != NULL) {
            *c2 = '\0';
        }
        if (hash[0] == '\0' || hash[0] == '!') {
            ok = 0; /* locked or passwordless */
        } else {
            char *crypted = crypt(pass, hash);
            ok = crypted != NULL && strcmp(crypted, hash) == 0;
        }
        break;
    }
    fclose(f);
    return ok;
}

/* ---- main ---- */

static void usage(void) {
    fprintf(stderr, "usage: doas [-nSs] [-u user] [-C config] command [args]\n");
    exit(1);
}

int main(int argc, char **argv) {
    int noninteractive = 0;
    int shellmode = 0;
    int check_config = 0;
    const char *target_user = NULL;
    const char *conf = _PATH_DOAS_CONF;

    int ch;
    while ((ch = getopt(argc, argv, "nSsu:C:")) != -1) {
        switch (ch) {
        case 'n': noninteractive = 1; break;
        case 's': shellmode = 1; break;
        case 'u': target_user = optarg; break;
        case 'C': check_config = 1; conf = optarg; break;
        default: usage();
        }
    }

    /* Identity of the invoker. */
    uid_t uid = getuid();
    struct passwd *pw = getpwuid(uid);
    const char *user = target_user != NULL
        ? target_user : (pw != NULL ? pw->pw_name : "root");

    parse_conf(conf);
    if (g_parse_errors > 0) {
        fprintf(stderr, "doas: parse error in %s\n", conf);
        if (check_config) {
            return 1;
        }
    }
    if (check_config) {
        return g_parse_errors ? 1 : 0;
    }

    /* Build the command line. */
    if (shellmode) {
        argv[0] = getenv("SHELL");
        if (argv[0] == NULL) {
            argv[0] = pw != NULL && pw->pw_shell[0] != '\0'
                ? pw->pw_shell : "/bin/sh";
        }
        argc = 1;
        argv[1] = NULL;
    } else if (optind >= argc) {
        usage();
    } else {
        argv += optind;
        argc -= optind;
    }

    const char *cmd = argv[0];
    struct rule *r = match_rule(user, target_user != NULL ? target_user : user,
                                cmd);
    if (r == NULL || r->action == DENY) {
        fprintf(stderr, "doas: permission denied\n");
        return 1;
    }

    /* Password, unless the rule says nopass or we are root. */
    if (!(r->options & NOPASS) && geteuid() != 0) {
        if (noninteractive) {
            fprintf(stderr, "doas: no password allowed for non-interactive use\n");
            return 1;
        }
        char pass[128];
        fprintf(stderr, "doas (%s@tus) password: ", user);
        fflush(stderr);
        if (read_password(pass, sizeof(pass)) != 0 ||
            !check_password(user, pass)) {
            fprintf(stderr, "doas: authentication failed\n");
            return 1;
        }
    }

    /* Switch to the target identity (everything starts as root, so a
     * setuid is allowed) and run the command. */
    if (target_user != NULL) {
        struct passwd *tpw = getpwnam(target_user);
        if (tpw == NULL) {
            fprintf(stderr, "doas: unknown user %s\n", target_user);
            return 1;
        }
        setgid(tpw->pw_gid);
        setuid(tpw->pw_uid);
    }

    /* Resolve the command: a bare name is searched in PATH (TUS uses
     * /bin), like a real shell would. */
    if (strchr(argv[0], '/') == NULL) {
        static char pbuf[128];
        snprintf(pbuf, sizeof(pbuf), "/bin/%s", argv[0]);
        int fd = open(pbuf, O_RDONLY);
        if (fd >= 0) {
            close(fd);
            argv[0] = pbuf;
        }
    }

    execve(argv[0], argv, environ);
    fprintf(stderr, "doas: unable to execute %s: %s\n", argv[0],
            strerror(errno));
    return 1;
}
