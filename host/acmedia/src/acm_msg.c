/* ACMedia's messages on the socketpair, with fds (acm_proto.h).
 * MIT, Copyright (c) 2026 AmiCradle Team. */
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include "acm_msg.h"

int acm_send(int sock, uint32_t type, const void *a, uint32_t alen, const void *b, uint32_t blen,
             const int *fds, int nfds)
{
    struct acm_hdr h = { type, alen + blen };
    if (sizeof h + h.len > ACM_MSG_MAX || nfds > ACM_MAX_FDS) return -1;
    struct iovec iov[3] = { { &h, sizeof h }, { (void *)a, alen }, { (void *)b, blen } };
    union { char buf[CMSG_SPACE(sizeof(int) * ACM_MAX_FDS)]; struct cmsghdr align; } u;
    struct msghdr m = { 0 };
    m.msg_iov = iov;
    m.msg_iovlen = 3;
    if (nfds > 0) {
        memset(&u, 0, sizeof u);
        m.msg_control = u.buf;
        m.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);
    }
    ssize_t r;
    do r = sendmsg(sock, &m, MSG_NOSIGNAL); while (r < 0 && errno == EINTR);
    return r == (ssize_t)(sizeof h + h.len) ? 0 : -1;
}

int acm_recv(int sock, uint32_t *type, void *buf, uint32_t cap, uint32_t *len, int *fds, int *nfds)
{
    struct acm_hdr h;
    struct iovec iov[2] = { { &h, sizeof h }, { buf, cap } };
    union { char buf[CMSG_SPACE(sizeof(int) * ACM_MAX_FDS)]; struct cmsghdr align; } u;
    struct msghdr m = { 0 };
    m.msg_iov = iov;
    m.msg_iovlen = 2;
    m.msg_control = u.buf;
    m.msg_controllen = sizeof u.buf;
    ssize_t r;
    do r = recvmsg(sock, &m, MSG_CMSG_CLOEXEC); while (r < 0 && errno == EINTR);
    if (nfds) *nfds = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        int n = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        int got[ACM_MAX_FDS];
        if (n > ACM_MAX_FDS) n = ACM_MAX_FDS;
        memcpy(got, CMSG_DATA(c), sizeof(int) * (size_t)n);
        for (int i = 0; i < n; i++) {
            if (fds && nfds && *nfds < ACM_MAX_FDS) fds[(*nfds)++] = got[i];
            else close(got[i]);
        }
    }
    if (r == 0) return 0;                                   /* the other side is gone */
    if (r < (ssize_t)sizeof h || (m.msg_flags & MSG_TRUNC) || h.len != (uint32_t)(r - (ssize_t)sizeof h)) {
        if (fds && nfds) { for (int i = 0; i < *nfds; i++) close(fds[i]); *nfds = 0; }
        return -1;
    }
    *type = h.type;
    *len = h.len;
    return 1;
}
