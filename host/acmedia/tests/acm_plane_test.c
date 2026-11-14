/*
 * acm_plane.c against a stand-in acscreen (a thread on a SEQPACKET socket
 * speaking acvideo_proto.h): HELLO/WELCOME, frames shown and finished, a frame
 * sent back unshown comes back with its fds to composite, STATE off stops
 * sends, HIDE at the close, and no fd left open. No worker, FFmpeg or display.
 * Team 6, 11 Nov 2026. MIT, Copyright (c) 2026 AmiCradle Team.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "../src/acm_plane.h"

static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static char path[64];
static int lfd;
static int copies;

static int open_fds(void) {
    int n = 0; DIR *d = opendir("/proc/self/fd"); struct dirent *e;
    while (d && (e = readdir(d))) if (e->d_name[0] != '.') n++;
    if (d) closedir(d);
    return n;
}
static void ssend(int c, uint32_t type, const void *body, uint32_t len) {
    uint8_t b[ACV_MAX_PACKET]; struct acv_hdr h = { ACV_MAGIC, type, len, 0 };
    memcpy(b, &h, sizeof h); memcpy(b + sizeof h, body, len);
    if (send(c, b, sizeof h + len, MSG_NOSIGNAL) < 0) perror("send");
}
/* The stand-in: frame ids divisible by 3 come back unshown (NOT_SHOWN); the others are shown at once and the
 * one shown before them is DONE/OK; after frame 7, STATE off; HIDE takes the shown one off (DONE/HIDDEN). */
static void *acscreen(void *arg) {
    (void)arg;
    int c = accept(lfd, NULL, NULL);
    uint64_t on_screen = 0;
    for (;;) {
        uint8_t b[ACV_MAX_PACKET];
        union { struct cmsghdr h; char c[CMSG_SPACE(sizeof(int) * 4)]; } cm;
        struct iovec iov = { b, sizeof b };
        struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cm.c, .msg_controllen = sizeof cm.c };
        ssize_t n = recvmsg(c, &m, MSG_CMSG_CLOEXEC);
        if (n <= 0) break;
        for (struct cmsghdr *ch = CMSG_FIRSTHDR(&m); ch; ch = CMSG_NXTHDR(&m, ch))
            if (ch->cmsg_type == SCM_RIGHTS) for (size_t i = 0; i < (ch->cmsg_len - CMSG_LEN(0)) / sizeof(int); i++) { int fd; memcpy(&fd, CMSG_DATA(ch) + i * sizeof(int), sizeof fd); close(fd); }
        struct acv_hdr h; memcpy(&h, b, sizeof h);
        if (h.type == ACV_HELLO) { struct acv_welcome w = { ACV_VERSION, ACV_WF_COPY, 1920, 1080, 60000, 1280, 720, 1, 0, 0 }; ssend(c, ACV_WELCOME, &w, sizeof w); }
        else if (h.type == ACV_FRAME || h.type == ACV_COPY) {
            uint64_t id; memcpy(&id, b + sizeof h, sizeof id);
            if (h.type == ACV_COPY) copies++;
            if (id % 3 == 0) { struct acv_done d = { id, ACV_W_NOT_SHOWN, 0 }; ssend(c, ACV_DONE, &d, sizeof d); }
            else {
                struct acv_shown s = { id, 123456789ull + id, (uint32_t)id, 0 }; ssend(c, ACV_SHOWN, &s, sizeof s);
                if (on_screen) { struct acv_done d = { on_screen, ACV_W_OK, 0 }; ssend(c, ACV_DONE, &d, sizeof d); }
                on_screen = id;
            }
            if (id == 7) { struct acv_state st = { 0, ACV_W_NOT_SHOWN, 1920, 1080 }; ssend(c, ACV_STATE, &st, sizeof st); }
        } else if (h.type == ACV_HIDE && on_screen) { struct acv_done d = { on_screen, ACV_W_HIDDEN, 0 }; ssend(c, ACV_DONE, &d, sizeof d); on_screen = 0; }
    }
    close(c);
    return NULL;
}

int main(void) {
    snprintf(path, sizeof path, "/tmp/acm-plane-test-%d.sock", (int)getpid());
    lfd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    unlink(path);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 1)) { perror("bind"); return 2; }
    pthread_t th; pthread_create(&th, NULL, acscreen, NULL);
    int fds0 = open_fds();

    struct acm_plane p; char why[160];
    CHECK(acm_plane_open(&p, path, why, sizeof why) == 0, "open: %s", why);
    CHECK(p.on && p.welcome.mon_w == 1920 && p.welcome.rtg_w == 1280 && p.welcome.refresh_mhz == 60000, "WELCOME read");
    struct acv_rect r = { 0, 0, 0, 0, ACV_SPACE_FIT, 1 };
    CHECK(acm_plane_rect(&p, &r) == 0, "RECT sent");

    struct acm_frame fr; memset(&fr, 0, sizeof fr);
    fr.width = 64; fr.height = 64; fr.nplanes = 2; fr.nobj = 1; fr.flags = ACM_FF_DMABUF | ACM_FF_HW; fr.pitch[0] = fr.pitch[1] = 64; fr.offset[1] = 4096;
    struct acm_worker cw = { .pool_fd = memfd_create("pool", MFD_CLOEXEC), .pool_size = 8192, .slot_size = 8192, .nslots = 1 };
    if (cw.pool_fd >= 0 && ftruncate(cw.pool_fd, (off_t)cw.pool_size)) perror("ftruncate pool");
    struct acm_frame sf = fr; sf.flags = 0; sf.pixfmt = ACM_PIX_NV12; sf.nobj = 0;
    sf.rowbytes[0] = sf.rowbytes[1] = 64; sf.rows[0] = 64; sf.rows[1] = 32;
    p.welcome.flags &= ~ACV_WF_COPY;
    CHECK(acm_plane_send_copy(&p, &cw, &sf, 0, NULL) == 1, "an older acscreen gets software frames through the guest fallback");
    p.welcome.flags |= ACV_WF_COPY;
    int composited = 0, shown = 0, refused_sends = 0, comp_fd_ok = 1;
    for (int n = 1; n <= 10; n++) {
        int fd = -1;
        fr.slot = (uint32_t)n;
        uint64_t id = 0;
        int s;
        if (n == 2) {
            fr.slot = 0; fr.flags = 0; fr.pixfmt = ACM_PIX_NV12;
            fr.rowbytes[0] = fr.rowbytes[1] = 64; fr.rows[0] = 64; fr.rows[1] = 32;
            s = acm_plane_send_copy(&p, &cw, &fr, 0, &id);
            fr.flags = ACM_FF_DMABUF | ACM_FF_HW; fr.rowbytes[0] = fr.rowbytes[1] = fr.rows[0] = fr.rows[1] = 0;
        } else {
            fd = memfd_create("frame", MFD_CLOEXEC); if (ftruncate(fd, 8192)) perror("ftruncate");
            s = acm_plane_send(&p, &fr, &fd, 1, 0, &id);
        }
        if (s == 1) { refused_sends++; if (fd >= 0) close(fd); continue; }
        CHECK(s == 0 && id == (uint64_t)n - (uint64_t)refused_sends, "frame %d sent as id %llu", n, (unsigned long long)id);
        /* answers: SHOWN (+ DONE of the one before), or DONE unshown, and after 7 a STATE */
        for (int k = 0; k < 3; k++) {
            struct acm_plane_ev ev;
            int ty = acm_plane_pump(&p, NULL, 100, &ev);
            if (ty <= 0) break;
            if (ty == ACV_SHOWN) shown++;
            if (ev.composite) {
                composited++;
                if (ev.nfds != 1 || fcntl(ev.fds[0], F_GETFD) < 0 || ev.slot != (uint32_t)n) comp_fd_ok = 0;
                for (int i = 0; i < ev.nfds; i++) close(ev.fds[i]);
            }
        }
    }
    CHECK(shown == 5 && composited == 2, "1..7: five shown, two (3 and 6) sent back to composite: %d shown, %d composited", shown, composited);
    CHECK(comp_fd_ok, "a frame to composite comes back with its fd open and its slot");
    CHECK(copies == 1, "one software frame used COPY (%d)", copies);
    CHECK(!p.on && refused_sends == 3, "STATE off after 7: frames 8-10 not sent (%d)", refused_sends);
    CHECK(acm_plane_inflight(&p) == 1, "one frame on the screen (%d)", acm_plane_inflight(&p));
    fr.flags = ACM_FF_HW;                             /* a pool frame: never for the plane */
    p.on = 1;
    int dummy = 0;
    CHECK(acm_plane_send(&p, &fr, &dummy, 0, 0, NULL) == 1, "a frame with no dma-buf is not sent");
    acm_plane_close(&p, NULL, 500);
    if (cw.pool_fd >= 0) close(cw.pool_fd);
    CHECK(p.fd < 0 && acm_plane_inflight(&p) == 0, "close: HIDE took the last one off");
    CHECK(p.shown == 5 && p.composited == 2 && p.dropped == 0, "counts %u/%u/%u", p.shown, p.composited, p.dropped);
    pthread_join(th, NULL);
    CHECK(open_fds() == fds0, "no fd left open (%d before, %d after)", fds0, open_fds());
    CHECK(acm_plane_open(&p, "/tmp/no-such-acscreen.sock", why, sizeof why) == -1 && strstr(why, "no acscreen"), "no acscreen: %s", why);
    close(lfd); unlink(path);
    printf("acm_plane_test: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
