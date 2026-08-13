/*
 * login.c - begin a session on the system (TUS port)
 *
 * Prompts for a user name (or takes one as an argument) and a
 * password with echoing disabled, verifies the credentials against
 * /etc/shadow (crypt) and, on success, prints the message of the day
 * and the account information - the beginning of a login session.
 * TUS has no fork/exec shell replacement yet, so the session itself
 * remains the calling tsh; login authenticates and reports.
 *
 * Only a number of password failures are permitted (LOGIN_RETRIES)
 * before login exits, like the real utility.
 */

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define _PATH_PASSWD "/etc/passwd"
#define _PATH_SHADOW "/etc/shadow"
#define _PATH_MOTD   "/etc/motd"
#define _PATH_TTY    "/dev/tty0"

#define LOGIN_RETRIES 3

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

/* Look up the shadow hash for `user`; returns NULL if absent. */
static const char *shadow_hash(const char *user) {
    FILE *f = fopen(_PATH_SHADOW, "r");
    if (f == NULL) {
        return NULL;
    }
    static char buf[512];
    const char *hit = NULL;
    while (fgets(buf, sizeof(buf), f) != NULL) {
        char *c1 = strchr(buf, ':');
        if (c1 == NULL) {
            continue;
        }
        *c1 = '\0';
        if (strcmp(buf, user) == 0) {
            char *h = c1 + 1;
            char *c2 = strchr(h, ':');
            if (c2 != NULL) {
                *c2 = '\0';
            }
            hit = h;
            break;
        }
    }
    fclose(f);
    return hit;
}

static int authenticate(const char *user, const char *pass) {
    const char *hash = shadow_hash(user);
    if (hash == NULL || hash[0] == '\0' || hash[0] == '!') {
        return 0; /* unknown, passwordless or locked account */
    }
    char *c = crypt(pass, hash);
    return c != NULL && strcmp(c, hash) == 0;
}

int main(int argc, char **argv) {
    const char *user = NULL;
    if (argc > 1) {
        user = argv[1];
    }

    char uname[64];
    char pass[128];
    int tries = 0;

    for (;;) {
        if (user == NULL) {
            if (read_line("login: ", uname, sizeof(uname), 0) != 0) {
                return 1;
            }
            user = uname;
        }
        if (read_line("Password: ", pass, sizeof(pass), 1) != 0) {
            return 1;
        }

        if (authenticate(user, pass)) {
            break;
        }
        fprintf(stderr, "Login incorrect\n");
        if (++tries >= LOGIN_RETRIES) {
            fprintf(stderr, "login: maximum login attempts exceeded\n");
            return 1;
        }
        user = NULL;
    }

    /* Successful login: account information. */
    struct passwd *pw = getpwnam(user);
    printf("Welcome to TUS, %s!\n", user);
    printf("  uid=%u  gid=%u  home=%s  shell=%s\n",
           pw != NULL ? pw->pw_uid : 0,
           pw != NULL ? pw->pw_gid : 0,
           pw != NULL ? pw->pw_dir : "/",
           pw != NULL ? pw->pw_shell : "/bin/tsh");
    printf("  PATH=/bin:/usr/bin\n");

    /* Message of the day, like a real login. */
    FILE *m = fopen(_PATH_MOTD, "r");
    if (m != NULL) {
        int c;
        while ((c = fgetc(m)) != EOF) {
            fputc(c, stdout);
        }
        fclose(m);
    }
    return 0;
}
