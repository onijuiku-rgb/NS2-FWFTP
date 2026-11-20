// SPDX-License-Identifier: MIT
// Minimal read-only FTP server: passive mode only, binary transfers, one
// thread per client. Enough for FileZilla, lftp, wget and curl.
#include "common.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_SESSIONS 4
#define XFER_SIZE    (1024 * 1024)
#define PATH_LEN     1024
#define DATA_PORT_LO 5100
#define DATA_PORT_HI 5199

typedef struct {
    bool active;
    volatile bool done;
    Thread thread;
    int ctrl, pasv, data;
    bool quit;
    char cwd[PATH_LEN];
    s64 rest;
    char rbuf[4096];
    size_t rlen;
    char line[PATH_LEN + 64];
    u8 *xfer;
    char peer[INET_ADDRSTRLEN];
    u64 files, bytes;
} Session;

static Session g_sess[MAX_SESSIONS];
static int g_listen = -1;
static volatile bool g_stopping;
static u16 g_next_port = DATA_PORT_LO;
static Mutex g_port_lock;

// ---------------------------------------------------------------- io

static bool send_all(int fd, const void *buf, size_t len) {
    const u8 *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
}

static void reply(Session *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void reply(Session *s, const char *fmt, ...) {
    char buf[PATH_LEN + 256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n > sizeof buf - 3) n = sizeof buf - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    send_all(s->ctrl, buf, n);
}

static bool read_line(Session *s) {
    for (;;) {
        char *nl = memchr(s->rbuf, '\n', s->rlen);
        if (nl) {
            size_t len = nl - s->rbuf;
            size_t keep = len < sizeof s->line - 1 ? len : sizeof s->line - 1;
            memcpy(s->line, s->rbuf, keep);
            while (keep && (s->line[keep - 1] == '\r' || s->line[keep - 1] == '\0')) keep--;
            s->line[keep] = 0;
            s->rlen -= len + 1;
            memmove(s->rbuf, nl + 1, s->rlen);
            return true;
        }
        if (s->rlen == sizeof s->rbuf) s->rlen = 0; // overlong line: drop it
        ssize_t n = recv(s->ctrl, s->rbuf + s->rlen, sizeof s->rbuf - s->rlen, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        s->rlen += n;
    }
}

// Joins arg onto cwd and resolves "." and ".." into an absolute path.
static bool resolve(const char *cwd, const char *arg, char *out) {
    char tmp[PATH_LEN * 2 + 128];
    if (!arg || !*arg) arg = ".";
    if (arg[0] == '/') snprintf(tmp, sizeof tmp, "%s", arg);
    else snprintf(tmp, sizeof tmp, "%s/%s", cwd, arg);

    size_t len = 0, starts[256];
    int depth = 0;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) {
            if (depth) len = starts[--depth];
            continue;
        }
        size_t tl = strlen(tok);
        if (depth == 256 || len + 1 + tl >= PATH_LEN) return false;
        starts[depth++] = len;
        out[len++] = '/';
        memcpy(out + len, tok, tl);
        len += tl;
    }
    if (!len) out[len++] = '/';
    out[len] = 0;
    return true;
}

// ---------------------------------------------------------------- data connection

static void close_fd(int *fd) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

static void cmd_pasv(Session *s, bool extended) {
    close_fd(&s->pasv);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { reply(s, "425 Cannot open data socket."); return; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    u16 port = 0;
    for (int tries = 0; tries <= DATA_PORT_HI - DATA_PORT_LO && !port; tries++) {
        mutexLock(&g_port_lock);
        u16 p = g_next_port;
        g_next_port = p == DATA_PORT_HI ? DATA_PORT_LO : p + 1;
        mutexUnlock(&g_port_lock);
        a.sin_port = htons(p);
        if (!bind(fd, (struct sockaddr *)&a, sizeof a)) port = p;
    }
    if (!port || listen(fd, 1) < 0) {
        close(fd);
        reply(s, "425 Cannot listen for data.");
        return;
    }
    s->pasv = fd;

    struct sockaddr_in local;
    socklen_t ll = sizeof local;
    getsockname(s->ctrl, (struct sockaddr *)&local, &ll);
    u32 ip = ntohl(local.sin_addr.s_addr);
    if (extended)
        reply(s, "229 Entering Extended Passive Mode (|||%u|)", port);
    else
        reply(s, "227 Entering Passive Mode (%lu,%lu,%lu,%lu,%u,%u)", (unsigned long)(ip >> 24 & 255),
              (unsigned long)(ip >> 16 & 255), (unsigned long)(ip >> 8 & 255), (unsigned long)(ip & 255),
              port >> 8, port & 255);
}

static bool data_accept(Session *s) {
    if (s->pasv < 0) return false;
    struct pollfd p = {s->pasv, POLLIN, 0};
    if (poll(&p, 1, 15000) > 0) s->data = accept(s->pasv, NULL, NULL);
    close_fd(&s->pasv);
    return s->data >= 0;
}

// ---------------------------------------------------------------- listing

enum { LIST_UNIX, LIST_NAMES, LIST_MLSD };

typedef struct {
    Session *s;
    int mode; // LIST_*
    bool failed;
    size_t len;
    char buf[16384];
} ListCtx;

static void list_flush(ListCtx *c) {
    if (c->len && !c->failed && !send_all(c->s->data, c->buf, c->len)) c->failed = true;
    c->len = 0;
}

// RFC 3659 facts for MLSD/MLST.
static int mlsx_facts(char *out, size_t len, bool is_dir, s64 size) {
    return is_dir ? snprintf(out, len, "type=dir;perm=el;")
                  : snprintf(out, len, "type=file;size=%lld;perm=r;", (long long)size);
}

static void list_entry(void *ctx, const char *name, bool is_dir, s64 size) {
    ListCtx *c = ctx;
    char line[FS_MAX_PATH + 96], facts[64];
    int n;
    if (c->mode == LIST_NAMES) {
        n = snprintf(line, sizeof line, "%s\r\n", name);
    } else if (c->mode == LIST_MLSD) {
        mlsx_facts(facts, sizeof facts, is_dir, size);
        n = snprintf(line, sizeof line, "%s %s\r\n", facts, name);
    } else {
        n = snprintf(line, sizeof line, "%s 1 ftp ftp %13lld Jan  1  2026 %s\r\n",
                     is_dir ? "dr-xr-xr-x" : "-r--r--r--", (long long)size, name);
    }
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = sizeof line - 1;
    if (c->len + n > sizeof c->buf) list_flush(c);
    memcpy(c->buf + c->len, line, n);
    c->len += n;
}

static void cmd_list(Session *s, const char *arg, int mode) {
    while (arg && *arg == '-') { // skip "-a", "-la" ...
        arg = strchr(arg, ' ');
        while (arg && *arg == ' ') arg++;
    }
    char path[PATH_LEN], err[128] = "";
    bool is_dir;
    s64 size;
    if (!resolve(s->cwd, arg, path) || vfs_stat(path, &is_dir, &size, err, sizeof err) < 0) {
        close_fd(&s->pasv);
        reply(s, "550 %s", err[0] ? err : "Bad path.");
        return;
    }
    if (mode == LIST_MLSD && !is_dir) {
        close_fd(&s->pasv);
        reply(s, "501 Not a directory.");
        return;
    }
    if (s->pasv < 0) { reply(s, "425 Use PASV or EPSV first."); return; }
    reply(s, "150 Here comes the directory listing.");
    if (!data_accept(s)) { reply(s, "425 No data connection."); return; }

    ListCtx *c = calloc(1, sizeof *c);
    int ret = -1;
    if (c) {
        c->s = s;
        c->mode = mode;
        if (is_dir) {
            ret = vfs_list(path, list_entry, c, err, sizeof err);
        } else {
            list_entry(c, strrchr(path, '/') + 1, false, size);
            ret = 0;
        }
        list_flush(c);
        if (c->failed) ret = -1;
        free(c);
    }
    close_fd(&s->data);
    if (ret < 0) reply(s, "451 %s", err[0] ? err : "Listing failed.");
    else reply(s, "226 Directory send OK.");
}

// ---------------------------------------------------------------- transfers

static void cmd_retr(Session *s, const char *arg) {
    char path[PATH_LEN], err[128] = "";
    s64 size = 0, offset = s->rest;
    s->rest = 0;
    VFile *f = resolve(s->cwd, arg, path) ? vfs_open(path, &size, err, sizeof err) : NULL;
    if (!f) {
        close_fd(&s->pasv);
        reply(s, "550 %s", err[0] ? err : "Bad path.");
        return;
    }
    if (s->pasv < 0) { vfs_close(f); reply(s, "425 Use PASV or EPSV first."); return; }
    reply(s, "150 Opening BINARY mode data connection for %s (%lld bytes).", path, (long long)size);
    if (!data_accept(s)) { vfs_close(f); reply(s, "425 No data connection."); return; }

    bool ok = true;
    while (offset < size && !g_stopping) {
        s64 n = vfs_read(f, offset, s->xfer, XFER_SIZE);
        if (n <= 0) { ok = false; snprintf(err, sizeof err, "read failed at %lld", (long long)offset); break; }
        if (!send_all(s->data, s->xfer, n)) { ok = false; snprintf(err, sizeof err, "client closed"); break; }
        offset += n;
        s->bytes += n;
    }
    close_fd(&s->data);
    vfs_close(f);
    if (ok && offset >= size) {
        s->files++;
        reply(s, "226 Transfer complete.");
    } else {
        log_msg("%s: %s: %s\n", s->peer, path, err);
        reply(s, "451 %s", err);
    }
}

// ---------------------------------------------------------------- commands

static void handle(Session *s) {
    char *cmd = s->line, *arg = strchr(cmd, ' ');
    if (arg) {
        *arg++ = 0;
        while (*arg == ' ') arg++;
    } else {
        arg = cmd + strlen(cmd);
    }
    for (char *c = cmd; *c; c++)
        if (*c >= 'a' && *c <= 'z') *c -= 32;

    char path[PATH_LEN], err[128] = "";
    bool is_dir;
    s64 size;
#define IS(x) (!strcmp(cmd, x))
    if (IS("USER") || IS("PASS")) reply(s, "230 Logged in (read-only, any login).");
    else if (IS("SYST")) reply(s, "215 UNIX Type: L8");
    else if (IS("FEAT")) {
        static const char feat[] = "211-Features:\r\n PASV\r\n EPSV\r\n SIZE\r\n MLST type*;size*;perm*;\r\n REST STREAM\r\n UTF8\r\n211 End\r\n";
        send_all(s->ctrl, feat, sizeof feat - 1);
    }
    else if (IS("OPTS") || IS("NOOP") || IS("ALLO")) reply(s, "200 OK.");
    else if (IS("TYPE")) reply(s, "200 Type set (always binary).");
    else if (IS("MODE")) reply(s, toupper((unsigned char)*arg) == 'S' ? "200 OK." : "504 Only stream mode.");
    else if (IS("STRU")) reply(s, toupper((unsigned char)*arg) == 'F' ? "200 OK." : "504 Only file structure.");
    else if (IS("PWD") || IS("XPWD")) reply(s, "257 \"%s\" is the current directory.", s->cwd);
    else if (IS("CWD") || IS("XCWD") || IS("CDUP") || IS("XCUP")) {
        if (IS("CDUP") || IS("XCUP")) arg = "..";
        if (!resolve(s->cwd, arg, path) || vfs_stat(path, &is_dir, &size, err, sizeof err) < 0)
            reply(s, "550 %s", err[0] ? err : "Bad path.");
        else if (!is_dir) reply(s, "550 Not a directory.");
        else {
            snprintf(s->cwd, sizeof s->cwd, "%s", path);
            reply(s, "250 Directory changed to %s.", path);
        }
    }
    else if (IS("SIZE")) {
        if (!resolve(s->cwd, arg, path) || vfs_stat(path, &is_dir, &size, err, sizeof err) < 0)
            reply(s, "550 %s", err[0] ? err : "Bad path.");
        else if (is_dir) reply(s, "550 Is a directory.");
        else reply(s, "213 %lld", (long long)size);
    }
    else if (IS("REST")) {
        s->rest = strtoll(arg, NULL, 10);
        if (s->rest < 0) s->rest = 0;
        reply(s, "350 Restarting at %lld.", (long long)s->rest);
    }
    else if (IS("PASV")) cmd_pasv(s, false);
    else if (IS("EPSV")) {
        if (!strcasecmp(arg, "ALL") || *arg == '1' || !*arg) {
            if (*arg && toupper((unsigned char)*arg) == 'A') reply(s, "200 EPSV ALL accepted.");
            else cmd_pasv(s, true);
        } else reply(s, "522 Only IPv4 is supported (1).");
    }
    else if (IS("PORT") || IS("EPRT")) reply(s, "502 Active mode is not supported, use passive mode.");
    else if (IS("LIST")) cmd_list(s, arg, LIST_UNIX);
    else if (IS("NLST")) cmd_list(s, arg, LIST_NAMES);
    else if (IS("MLSD")) cmd_list(s, arg, LIST_MLSD);
    else if (IS("MLST")) {
        char facts[64];
        if (!resolve(s->cwd, arg, path) || vfs_stat(path, &is_dir, &size, err, sizeof err) < 0) {
            reply(s, "550 %s", err[0] ? err : "Bad path.");
        } else {
            mlsx_facts(facts, sizeof facts, is_dir, size);
            reply(s, "250-Listing %s\r\n %s %s\r\n250 End.", path, facts, path);
        }
    }
    else if (IS("RETR")) cmd_retr(s, arg);
    else if (IS("ABOR")) reply(s, "226 Nothing to abort.");
    else if (IS("QUIT")) { reply(s, "221 Bye."); s->quit = true; }
    else if (IS("STOR") || IS("APPE") || IS("STOU") || IS("DELE") || IS("MKD") || IS("XMKD") || IS("RMD") ||
             IS("XRMD") || IS("RNFR") || IS("RNTO") || IS("SITE"))
        reply(s, "550 This server is read-only.");
    else reply(s, "502 Command not implemented.");
#undef IS
}

static void session_main(void *arg) {
    Session *s = arg;
    reply(s, "220 ns2-fwftp " APP_VERSION " ready (read-only).");
    while (!g_stopping && !s->quit && read_line(s)) handle(s);
    close_fd(&s->pasv);
    close_fd(&s->data);
    close_fd(&s->ctrl);
    log_msg("%s disconnected: %llu files, %.1f MiB\n", s->peer, (unsigned long long)s->files,
            s->bytes / (1024.0 * 1024.0));
    s->done = true;
}

// ---------------------------------------------------------------- server

int ftp_start(u16 port) {
    mutexInit(&g_port_lock);
    g_stopping = false;
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0) { log_msg("ftp: socket failed (%d)\n", errno); return -1; }
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(g_listen, (struct sockaddr *)&a, sizeof a) < 0 || listen(g_listen, MAX_SESSIONS) < 0) {
        log_msg("ftp: cannot listen on port %u (%d)\n", port, errno);
        close_fd(&g_listen);
        return -1;
    }
    return 0;
}

static void reap(Session *s) {
    threadWaitForExit(&s->thread);
    threadClose(&s->thread);
    free(s->xfer);
    s->xfer = NULL;
    s->active = false;
}

void ftp_poll(void) {
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_sess[i].active && g_sess[i].done) reap(&g_sess[i]);
    if (g_listen < 0) return;

    struct pollfd p = {g_listen, POLLIN, 0};
    if (poll(&p, 1, 0) <= 0) return;
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    int fd = accept(g_listen, (struct sockaddr *)&peer, &pl);
    if (fd < 0) return;

    Session *s = NULL;
    for (int i = 0; i < MAX_SESSIONS && !s; i++)
        if (!g_sess[i].active) s = &g_sess[i];
    u8 *xfer = s ? malloc(XFER_SIZE) : NULL;
    if (!xfer) {
        static const char busy[] = "421 Too many connections.\r\n";
        send_all(fd, busy, sizeof busy - 1);
        close(fd);
        return;
    }
    memset(s, 0, sizeof *s);
    s->ctrl = fd;
    s->pasv = s->data = -1;
    s->xfer = xfer;
    strcpy(s->cwd, "/");
    inet_ntop(AF_INET, &peer.sin_addr, s->peer, sizeof s->peer);
    if (R_FAILED(threadCreate(&s->thread, session_main, s, NULL, 0x10000, 0x2C, -2)) ||
        R_FAILED(threadStart(&s->thread))) {
        close(fd);
        free(xfer);
        return;
    }
    s->active = true;
    log_msg("%s connected\n", s->peer);
}

void ftp_stop(void) {
    g_stopping = true;
    close_fd(&g_listen);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &g_sess[i];
        if (!s->active) continue;
        if (s->ctrl >= 0) shutdown(s->ctrl, SHUT_RDWR);
        if (s->data >= 0) shutdown(s->data, SHUT_RDWR);
        reap(s);
    }
}
