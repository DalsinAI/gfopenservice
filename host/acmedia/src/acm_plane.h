/*
 * ACMedia's plane route (design 3.2): the broker's side of acscreen's video
 * socket (acvideo_proto.h). Worker frames that came out as dma-bufs go to
 * acscreen, which shows them on a KMS overlay plane; a frame acscreen sends
 * back unshown comes back to the caller, with its fds, to composite into the
 * RTG surface instead. Frames acscreen has finished with go back to the
 * worker (ACM_Q_RELEASE). Nothing here links FFmpeg or libdrm.
 * Team 6, 11 Nov 2026. MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef ACM_PLANE_H
#define ACM_PLANE_H
#include <stdint.h>
#include "acm_broker.h"
#include "acvideo_proto.h"

#define ACM_PLANE_INFLIGHT 4         /* on the screen, committed, waiting, and one being answered */

struct acm_plane_frame { int live, shown, nfds; uint64_t id, due_ns; uint32_t slot; int fds[ACV_MAX_OBJ]; };
struct acm_plane {
    int fd;
    struct acv_welcome welcome;
    int on;                          /* acscreen says the plane is usable now */
    uint32_t why;                    /* and why not, when it is not */
    uint64_t next_id;
    struct acm_plane_frame f[ACM_PLANE_INFLIGHT];
    uint32_t shown, composited, dropped;   /* counts */
};

/* What acm_plane_pump saw */
struct acm_plane_ev {
    int type;                        /* 0 nothing within the time, ACV_SHOWN, ACV_DONE, ACV_STATE, -1 acscreen went */
    uint64_t id, vblank_ns, due_ns;  /* SHOWN: the frame and its vblank */
    uint32_t why;                    /* DONE or STATE: ACV_W_* */
    /* DONE of a frame never shown (not DROPPED): composite it. The caller owns these fds and the worker's slot
     * now, and gives the slot back with acm_plane_release once drawn. */
    int composite, nfds, fds[ACV_MAX_OBJ];
    uint32_t slot;
};

/* Connects and says HELLO. 0, or -1 with why. */
int acm_plane_open(struct acm_plane *p, const char *sock, char *why, size_t whylen);
int acm_plane_rect(struct acm_plane *p, const struct acv_rect *r);
/* Hands a worker frame to acscreen, due at due_ns (CLOCK_MONOTONIC, 0 now). On 0 the fds are the plane's.
 * 1: not sent (the plane is off, or ACM_PLANE_INFLIGHT frames are out): composite it; the fds stay the
 * caller's. -1: acscreen went. */
int acm_plane_send(struct acm_plane *p, const struct acm_frame *fr, const int *fds, int nfds, uint64_t due_ns, uint64_t *id);
/* One packet from acscreen within ms. Frames acscreen has finished with go back to w (if not NULL). */
int acm_plane_pump(struct acm_plane *p, struct acm_worker *w, int ms, struct acm_plane_ev *ev);
/* A worker slot back (a composited frame, drawn). */
void acm_plane_release(struct acm_worker *w, uint32_t slot);
/* Frames still out with acscreen */
int acm_plane_inflight(const struct acm_plane *p);
/* HIDE, wait up to ms for the frames, release them, close. */
void acm_plane_close(struct acm_plane *p, struct acm_worker *w, int ms);
#endif
