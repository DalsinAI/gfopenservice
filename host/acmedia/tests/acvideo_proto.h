/*
 * acscreen's video socket: the wire format (ACMedia design 3.2, M4K step 3).
 *
 * A program that has decoded video frames as dma-bufs (ACMedia's broker in
 * the runtime, or gfopenservice's acm_planetest) hands them to acscreen, which
 * puts them on a KMS overlay plane over the monitor that shows the Amiga's RTG
 * screen. No pixel is copied. The socket is SOCK_SEQPACKET (one message per
 * packet), at $AC_VIDEO_SOCK, else /run/amicradle/acscreen-video-PORT.sock;
 * one client at a time, of acscreen's own user or root. Integers are the
 * host's own (both ends are on one machine).
 *
 * Each packet: struct acv_hdr, then the body named by type (len bytes).
 * ACV_FRAME carries its nobj dma-buf fds with SCM_RIGHTS, in object order.
 *
 * The client says HELLO and gets WELCOME (the monitor, the RTG screen, and
 * whether the plane can be used now). It sends RECT for where the video goes,
 * then FRAMEs. acscreen shows each frame at the first vblank on or after its
 * due time, answers SHOWN when it is on the screen, and DONE for every frame
 * it took once it no longer needs the buffer (replaced on the screen, dropped
 * for a newer one, refused, or taken off). Only after DONE may the client
 * give the buffer back to its decoder. STATE says when the plane stops or
 * starts being usable (the RTG screen not shown, the speed menu open, no
 * plane takes the format, the driver refused): while it is off, frames come
 * straight back as DONE/refused and the client composites into the RTG
 * surface instead (design 3.1's fallback). The client's rule is simple: send
 * every frame while STATE says on; a frame that comes back DONE without a
 * SHOWN before it, for any reason but DROPPED, is composited instead; while
 * STATE says off, composite and send nothing until STATE says on again.
 * Fences: none in v1. Decoder capture buffers are complete when the decoder
 * hands them out, and the kernel's implicit dma-buf sync covers the rest.
 *
 * This file is copied into gfopenservice host/acmedia/tests/ for
 * acm_planetest; keep the two the same. Team 6, 11 Nov 2026.
 */
#ifndef ACVIDEO_PROTO_H
#define ACVIDEO_PROTO_H
#include <stdint.h>

#define ACV_MAGIC   0x31564341u    /* "ACV1" */
#define ACV_VERSION 1
#define ACV_MAX_PACKET 256
#define ACV_MAX_OBJ 4

enum {
    ACV_HELLO = 1,          /* struct acv_hello */
    ACV_RECT = 2,           /* struct acv_rect: where the video goes, and whether it is visible */
    ACV_FRAME = 3,          /* struct acv_frame + nobj fds */
    ACV_HIDE = 4,           /* no body: take the plane off (frames on it come back DONE/hidden) */
    ACV_WELCOME = 0x81,     /* struct acv_welcome */
    ACV_SHOWN = 0x82,       /* struct acv_shown */
    ACV_DONE = 0x83,        /* struct acv_done */
    ACV_STATE = 0x84,       /* struct acv_state */
};

struct acv_hdr { uint32_t magic, type, len, serial; };
struct acv_hello { uint32_t version, flags; };

#define ACV_SPACE_RTG 0     /* x, y, w, h in the RTG screen's pixels (as the Amiga's window is) */
#define ACV_SPACE_FIT 1     /* the frame fitted to the monitor, its shape kept; the rectangle is ignored */
struct acv_rect { int32_t x, y; uint32_t w, h, space, visible; };

struct acv_frame {
    uint64_t id;            /* the client's, echoed in SHOWN and DONE */
    uint64_t due_ns;        /* CLOCK_MONOTONIC; 0: as soon as it can */
    uint32_t width, height, fourcc, nobj, nplanes, pad;
    uint64_t modifier;      /* DRM_FORMAT_MOD_*, the same for every plane */
    uint32_t obj[ACV_MAX_OBJ], offset[ACV_MAX_OBJ], pitch[ACV_MAX_OBJ];   /* per plane: its object, offset, pitch */
};

/* why the plane is not in use (STATE), or why a frame came back (DONE) */
enum {
    ACV_W_OK = 0,           /* STATE: the plane is usable / DONE: replaced on the screen by the next frame */
    ACV_W_NOT_SHOWN = 1,    /* the RTG screen is not on a monitor now (boot screen, chipset screen, a menu over it) */
    ACV_W_NO_PLANE = 2,     /* no overlay plane on that monitor takes the format and modifier */
    ACV_W_HIDDEN = 3,       /* the client said not visible, or HIDE, or it went */
    ACV_W_COMMIT = 4,       /* the driver refused the plane (scaling limits, bandwidth) */
    ACV_W_IMPORT = 5,       /* the buffer could not be imported (PRIME or AddFB2) */
    ACV_W_DROPPED = 6,      /* DONE: a newer frame came before this one was shown */
    ACV_W_BADMSG = 7,       /* DONE: the FRAME message was malformed */
};

struct acv_welcome { uint32_t version, flags, mon_w, mon_h, refresh_mhz, rtg_w, rtg_h, on_plane, why, pad; };
struct acv_shown { uint64_t id, vblank_ns; uint32_t seq, pad; };
struct acv_done { uint64_t id; uint32_t why, err; };
struct acv_state { uint32_t on_plane, why, mon_w, mon_h; };

#endif
