/*
 * ACMedia's plane route: see acm_plane.h. Team 6, 11 Nov 2026.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "acm_plane.h"

#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12 0x3231564e   /* fourcc_code('N','V','1','2') */
#endif

static int psend(struct acm_plane *p, uint32_t type, const void *body, uint32_t len, const int *fds, int nfds) {
    uint8_t b[ACV_MAX_PACKET]; struct acv_hdr h = { ACV_MAGIC, type, len, 0 };
    if (sizeof h + len > sizeof b || nfds > ACV_MAX_OBJ) return -1;
    memcpy(b, &h, sizeof h); if (len) memcpy(b + sizeof h, body, len);
    struct iovec iov = { b, sizeof h + len };
    union { struct cmsghdr h; char c[CMSG_SPACE(sizeof(int) * ACV_MAX_OBJ)]; } cm;
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (nfds > 0) {
        memset(&cm, 0, sizeof cm);
        m.msg_control = cm.c; m.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);
    }
    ssize_t r;
    do r = sendmsg(p->fd, &m, MSG_NOSIGNAL); while (r < 0 && errno == EINTR);
    return r < 0 ? -1 : 0;
}

static int precv(struct acm_plane *p, void *body, size_t cap, int ms) {
    struct pollfd pf = { p->fd, POLLIN, 0 };
    int r;
    do r = poll(&pf, 1, ms); while (r < 0 && errno == EINTR);
    if (r <= 0) return r;
    uint8_t b[ACV_MAX_PACKET];
    ssize_t n = recv(p->fd, b, sizeof b, MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    if (n < (ssize_t)sizeof(struct acv_hdr)) return -1;
    struct acv_hdr h; memcpy(&h, b, sizeof h);
    if (h.magic != ACV_MAGIC || h.len != (size_t)n - sizeof h) return -1;
    memset(body, 0, cap);
    memcpy(body, b + sizeof h, h.len < cap ? h.len : cap);
    return (int)h.type;
}

int acm_plane_open(struct acm_plane *p, const char *sock, char *why, size_t whylen) {
    memset(p, 0, sizeof *p);
    p->fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock);
    if (p->fd < 0 || connect(p->fd, (struct sockaddr *)&a, sizeof a)) {
        snprintf(why, whylen, "no acscreen at %s: %s", sock, strerror(errno));
        if (p->fd >= 0) close(p->fd);
        p->fd = -1; return -1;
    }
    struct acv_hello hello = { ACV_VERSION, 0 };
    union { struct acv_welcome w; uint8_t raw[ACV_MAX_PACKET]; } u;
    int ty = psend(p, ACV_HELLO, &hello, sizeof hello, NULL, 0) ? -1 : precv(p, &u, sizeof u, 2000);
    if (ty != ACV_WELCOME) {
        snprintf(why, whylen, "acscreen did not answer HELLO (%s)", ty == 0 ? "no answer in 2 s: another client on the socket?" : "it closed");
        close(p->fd); p->fd = -1; return -1;
    }
    p->welcome = u.w; p->on = (int)u.w.on_plane; p->why = u.w.why;
    return 0;
}

int acm_plane_rect(struct acm_plane *p, const struct acv_rect *r) {
    return p->fd < 0 ? -1 : psend(p, ACV_RECT, r, sizeof *r, NULL, 0);
}

int acm_plane_inflight(const struct acm_plane *p) {
    int n = 0; for (int i = 0; i < ACM_PLANE_INFLIGHT; i++) n += p->f[i].live;
    return n;
}

int acm_plane_send(struct acm_plane *p, const struct acm_frame *fr, const int *fds, int nfds, uint64_t due_ns, uint64_t *id) {
    if (p->fd < 0) return -1;
    if (!p->on || !(fr->flags & ACM_FF_DMABUF) || nfds <= 0 || nfds > ACV_MAX_OBJ) return 1;
    int k = -1; for (int i = 0; i < ACM_PLANE_INFLIGHT; i++) if (!p->f[i].live) { k = i; break; }
    if (k < 0) return 1;
    struct acv_frame af = { .id = ++p->next_id, .due_ns = due_ns, .width = fr->width, .height = fr->height,
                            .fourcc = fr->drm_fourcc ? fr->drm_fourcc : DRM_FORMAT_NV12, .nobj = (uint32_t)nfds,
                            .nplanes = fr->nplanes, .modifier = fr->drm_modifier };
    for (uint32_t i = 0; i < fr->nplanes && i < ACV_MAX_OBJ; i++) { af.obj[i] = fr->obj[i]; af.offset[i] = fr->offset[i]; af.pitch[i] = fr->pitch[i]; }
    if (psend(p, ACV_FRAME, &af, sizeof af, fds, nfds)) return -1;
    struct acm_plane_frame *f = &p->f[k];
    memset(f, 0, sizeof *f);
    f->live = 1; f->id = af.id; f->due_ns = due_ns; f->slot = fr->slot; f->nfds = nfds;
    memcpy(f->fds, fds, sizeof(int) * (size_t)nfds);   /* kept until DONE: a frame sent back is composited from them */
    if (id) *id = af.id;
    return 0;
}

void acm_plane_release(struct acm_worker *w, uint32_t slot) {
    if (!w) return;
    uint8_t rb[256]; uint32_t rt, rl;
    acm_ask(w, ACM_Q_RELEASE, &(struct acm_release){ slot, 0 }, sizeof(struct acm_release), NULL, 0, &rt, rb, sizeof rb, &rl, NULL, NULL, 2000);
}

int acm_plane_pump(struct acm_plane *p, struct acm_worker *w, int ms, struct acm_plane_ev *ev) {
    memset(ev, 0, sizeof *ev);
    if (p->fd < 0) { ev->type = -1; return -1; }
    union { struct acv_shown s; struct acv_done d; struct acv_state st; uint8_t raw[ACV_MAX_PACKET]; } u;
    int ty = precv(p, &u, sizeof u, ms);
    ev->type = ty;
    if (ty < 0) return -1;
    if (ty == ACV_STATE) { p->on = (int)u.st.on_plane; p->why = u.st.why; ev->why = u.st.why; return ty; }
    for (int i = 0; i < ACM_PLANE_INFLIGHT; i++) {
        struct acm_plane_frame *f = &p->f[i];
        if (!f->live) continue;
        if (ty == ACV_SHOWN && u.s.id == f->id) {
            f->shown = 1; p->shown++;
            ev->id = f->id; ev->vblank_ns = u.s.vblank_ns; ev->due_ns = f->due_ns;
        } else if (ty == ACV_DONE && u.d.id == f->id) {
            ev->id = f->id; ev->why = u.d.why; ev->slot = f->slot;
            if (!f->shown && u.d.why != ACV_W_DROPPED) {          /* the caller composites this one */
                p->composited++;
                ev->composite = 1; ev->nfds = f->nfds; memcpy(ev->fds, f->fds, sizeof(int) * (size_t)f->nfds);
            } else {
                if (!f->shown) p->dropped++;
                for (int k = 0; k < f->nfds; k++) close(f->fds[k]);
                acm_plane_release(w, f->slot);
            }
            f->live = 0;
        }
    }
    return ty;
}

void acm_plane_close(struct acm_plane *p, struct acm_worker *w, int ms) {
    if (p->fd >= 0) {
        if (acm_plane_inflight(p)) psend(p, ACV_HIDE, NULL, 0, NULL, 0);
        for (int waited = 0; acm_plane_inflight(p) && waited < ms; waited += 20) {
            struct acm_plane_ev ev;
            if (acm_plane_pump(p, w, 20, &ev) < 0) break;
            if (ev.composite) { for (int k = 0; k < ev.nfds; k++) close(ev.fds[k]); acm_plane_release(w, ev.slot); }
        }
        close(p->fd); p->fd = -1;
    }
    for (int i = 0; i < ACM_PLANE_INFLIGHT; i++) if (p->f[i].live) {   /* acscreen went with frames out: they are ours again */
        for (int k = 0; k < p->f[i].nfds; k++) close(p->f[i].fds[k]);
        acm_plane_release(w, p->f[i].slot);
        p->f[i].live = 0;
    }
}
