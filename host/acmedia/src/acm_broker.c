/* ACMedia's broker side (acm_broker.h). MIT, Copyright (c) 2026 AmiCradle Team. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "acm_broker.h"
#include "acm_msg.h"
#include "acm_codecs.h"

static void reap(struct acm_worker *w, int block)
{
    if (w->pid <= 0) return;
    int st;
    pid_t r = waitpid(w->pid, &st, block ? 0 : WNOHANG);
    if (r == w->pid) {
        if (WIFSIGNALED(st)) w->exit_signal = WTERMSIG(st);
        if (WIFEXITED(st)) w->exit_code = WEXITSTATUS(st);
        w->pid = 0;
    }
}

static int gone(struct acm_worker *w, const char *what)
{
    if (!w->dead) {
        w->dead = 1;
        if (w->pid > 0) {
            /* give it a moment to finish dying by itself (its signal says why) */
            for (int i = 0; i < 50 && w->pid > 0; i++) { reap(w, 0); if (w->pid > 0) usleep(2000); }
            if (w->pid > 0) { kill(w->pid, SIGKILL); reap(w, 1); }
        }
        if (w->exit_signal)
            snprintf(w->why, sizeof w->why, "%s: the worker ended by signal %d%s", what, w->exit_signal,
                     w->exit_signal == SIGSYS ? " (seccomp)" : w->exit_signal == SIGKILL ? " (killed)" : "");
        else
            snprintf(w->why, sizeof w->why, "%s: the worker exited (%d)", what, w->exit_code);
    }
    return ACM_E_DECODER;
}

static int lock_checked(pid_t pid)
{
    char p[64], line[256];
    snprintf(p, sizeof p, "/proc/%d/status", (int)pid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int sec = 0, nnp = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "Seccomp:", 8)) sec = atoi(line + 8);
        if (!strncmp(line, "NoNewPrivs:", 11)) nnp = atoi(line + 11);
    }
    fclose(f);
    return sec == 2 && nnp == 1;
}

int acm_ask(struct acm_worker *w, uint32_t type, const void *p, uint32_t n, const int *fds, int nfds,
            uint32_t *rtype, void *rbuf, uint32_t cap, uint32_t *rlen, int *rfds, int *rnfds, int timeout_ms)
{
    if (rnfds) *rnfds = 0;
    if (w->dead) return ACM_E_DECODER;
    if (acm_send(w->sock, type, p, n, NULL, 0, fds, nfds)) return gone(w, "send");
    struct pollfd pf = { w->sock, POLLIN, 0 };
    int r;
    do r = poll(&pf, 1, timeout_ms); while (r < 0 && errno == EINTR);
    if (r == 0) {
        kill(w->pid, SIGKILL);
        reap(w, 1);
        w->dead = 1;
        snprintf(w->why, sizeof w->why, "watchdog: no answer in %d ms; the worker was stopped", timeout_ms);
        return ACM_E_DECODER;
    }
    r = acm_recv(w->sock, rtype, rbuf, cap, rlen, rfds, rnfds);
    if (r <= 0) return gone(w, r == 0 ? "the worker went away" : "a bad answer");
    return 0;
}

void acm_hello_all(struct acm_hello *h, uint32_t flags)
{
    memset(h, 0, sizeof *h);
    h->version = ACM_PROTO_VERSION;
    h->flags = flags;
    for (unsigned i = 0; i < acm_all_tags_n && i < ACM_MAX_TAGS; i++) h->allow[h->nallow++] = acm_all_tags[i];
}

int acm_err(uint32_t rtype, const void *rbuf, int code)
{
    if (rtype == ACM_A_ERROR) return ((const struct acm_error *)rbuf)->code;
    return code;
}

int acm_spawn(struct acm_worker *w, const char *exe, int hw, unsigned mem_mb, int quiet, const struct acm_hello *h)
{
    memset(w, 0, sizeof *w);
    w->sock = -1;
    w->pool_fd = -1;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return ACM_E_LIMIT;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 3);       /* dup2 clears CLOEXEC on 3 */
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (quiet) posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char mem[16];
    snprintf(mem, sizeof mem, "%u", mem_mb);
    char *argv[] = { (char *)exe, "--fd", "3", "--mem", mem, hw ? "--hw" : NULL, NULL };
    char *envp[] = { "MESA_SHADER_CACHE_DISABLE=true", "LIBVA_MESSAGING_LEVEL=0", NULL };
    int e = posix_spawn(&w->pid, exe, &fa, NULL, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    close(sv[1]);
    if (e) { close(sv[0]); w->pid = 0; snprintf(w->why, sizeof w->why, "spawn %s: %s", exe, strerror(e)); return ACM_E_NOTAVAIL; }
    w->sock = sv[0];
    uint32_t t, len;
    uint8_t buf[512];
    int r = acm_ask(w, ACM_Q_HELLO, h, sizeof *h, NULL, 0, &t, buf, sizeof buf, &len, NULL, NULL, 10000);
    if (r) return r;
    if (t != ACM_A_OK || len != sizeof(struct acm_ready)) {
        int code = acm_err(t, buf, ACM_E_VERSION);
        acm_end(w);
        snprintf(w->why, sizeof w->why, "HELLO refused (%d)", code);
        return code;
    }
    memcpy(&w->ready, buf, sizeof w->ready);
    /* the worker says it is locked down: the kernel must say so too */
    if (!lock_checked(w->pid) || !(w->ready.sandbox & ACM_SB_SECCOMP)) {
        acm_end(w);
        snprintf(w->why, sizeof w->why, "the worker is not locked down (sandbox %#x): refused", w->ready.sandbox);
        return ACM_E_NOTAVAIL;
    }
    return 0;
}

int acm_give_source(struct acm_worker *w, int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || (fl & O_ACCMODE) != O_RDONLY) return ACM_E_BADARG;
    struct acm_source s = { 0, 0, 0 };
    uint32_t t, len;
    uint8_t buf[256];
    int r = acm_ask(w, ACM_Q_SOURCE, &s, sizeof s, &fd, 1, &t, buf, sizeof buf, &len, NULL, NULL, 2000);
    if (r) return r;
    return t == ACM_A_OK ? 0 : acm_err(t, buf, ACM_E_BADARG);
}

int acm_give_pool(struct acm_worker *w, uint32_t slot_size, uint32_t nslots)
{
    size_t size = (size_t)slot_size * nslots;
    int fd = memfd_create("acmedia-pool", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return ACM_E_LIMIT;
    if (ftruncate(fd, (off_t)size) || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) {
        close(fd);
        return ACM_E_LIMIT;
    }
    void *m = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return ACM_E_LIMIT; }
    struct acm_pool p = { size, slot_size, nslots };
    uint32_t t, len;
    uint8_t buf[256];
    int r = acm_ask(w, ACM_Q_POOL, &p, sizeof p, &fd, 1, &t, buf, sizeof buf, &len, NULL, NULL, 2000);
    if (r || t != ACM_A_OK) { close(fd); munmap(m, size); return r ? r : acm_err(t, buf, ACM_E_LIMIT); }
    w->pool_fd = fd;
    w->pool = m;
    w->pool_size = size;
    w->slot_size = slot_size;
    w->nslots = nslots;
    return 0;
}

void acm_end(struct acm_worker *w)
{
    if (!w->dead && w->sock >= 0 && w->pid > 0) {
        uint32_t t, len;
        uint8_t buf[256];
        acm_ask(w, ACM_Q_CLOSE, NULL, 0, NULL, 0, &t, buf, sizeof buf, &len, NULL, NULL, 1000);
    }
    if (w->pid > 0) {
        for (int i = 0; i < 100 && w->pid > 0; i++) { reap(w, 0); if (w->pid > 0) usleep(1000); }
        if (w->pid > 0) { kill(w->pid, SIGKILL); reap(w, 1); }
    }
    if (w->sock >= 0) close(w->sock);
    w->sock = -1;
    if (w->pool_fd >= 0) close(w->pool_fd);
    w->pool_fd = -1;
    if (w->pool) munmap(w->pool, w->pool_size);
    w->pool = NULL;
    w->dead = 1;
}
