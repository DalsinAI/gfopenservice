/*
 * ACMedia's guest ABI, as libacmedia.so answers it: amicradle-guest
 * hostlib/acmedia/acmedia.h's numbers (designs/ACMEDIA-DESIGN-20261110.md
 * 2.2), plus the layouts of the blocks the calls write (CAPS, INFO and
 * RESULT), which are defined here first and go into acmedia.h with
 * akikoplus.device rung A. Everything the guest reads is big-endian longs.
 * Only what the backend needs is here; the error codes are acm_proto.h's
 * (the same numbers). Keep this and acmedia.h the same.
 * Team 6, 12 Nov 2026. MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef ACM_GUEST_H
#define ACM_GUEST_H
#include <stdint.h>

#define ACMG_ABI_VERSION 1u
#define ACMG_CALLS 12u                /* $00012000-$0001200B, each at version 1 */
#define ACMG_RANGE_MAX (16u << 20)
#define ACMG_SMALL_MAX 4096u
#define ACMG_TARGET_BYTES 32u

/* the source descriptor's kinds */
enum { ACMG_SRC_ACFS = 1, ACMG_SRC_FEED = 2, ACMG_SRC_CD = 3 };
/* ACM_OPEN 2: the streams wanted */
#define ACMG_ST_VIDEO 1u
#define ACMG_ST_AUDIO 2u
#define ACMG_OF_EVENTS_FRAME 1u
/* targets (long 0 of an ACM_TARGET_BYTES block) */
enum { ACMG_OUT_NONE, ACMG_OUT_PLANE, ACMG_OUT_RTG_SURFACE, ACMG_OUT_CHIPSET, ACMG_OUT_GUEST, ACMG_OUT_ACAHI };
/* ACM_CONTROL */
enum { ACMG_CTL_PLAY = 1, ACMG_CTL_PAUSE, ACMG_CTL_STOP, ACMG_CTL_SEEK, ACMG_CTL_RATE, ACMG_CTL_VOLUME,
       ACMG_CTL_SELECT, ACMG_CTL_SETRECT, ACMG_CTL_EVENTS, ACMG_CTL_TARGET };
#define ACMG_SEEK_EXACT 1u
/* ACM_FRAME formats */
enum { ACMG_PIX_CHUNKY8 = 1, ACMG_PIX_RGB16, ACMG_PIX_ARGB32, ACMG_PIX_HAM8, ACMG_PIX_PLANAR, ACMG_PIX_YUV420 };
/* ACM_AUDIO formats. The worker's native result is signed 16-bit,
 * interleaved, in host byte order. */
#define ACMG_SND_S16 1u
/* events */
enum { ACMG_EV_OPENED = 1, ACMG_EV_PROBED, ACMG_EV_FRAME, ACMG_EV_NEED, ACMG_EV_END, ACMG_EV_ERROR, ACMG_EV_PATH, ACMG_EV_RECT };
#define ACMG_EVM(ev) (1u << (ev))
/* paths and routes, as the guest numbers them */
enum { ACMG_PATH_SW = 0, ACMG_PATH_HW_V4L2_REQUEST, ACMG_PATH_HW_V4L2_M2M, ACMG_PATH_HW_VAAPI };
enum { ACMG_ROUTE_NONE = 0, ACMG_ROUTE_PLANE, ACMG_ROUTE_RTG_SURFACE, ACMG_ROUTE_CHIPSET, ACMG_ROUTE_GUEST, ACMG_ROUTE_ACAHI };

/* ---- the blocks the calls write (new: these go into acmedia.h) ----
 *
 * ACM_CAPS: ACM_CAPS_HEAD_LONGS longs, then one entry of ACM_CAPS_ENTRY_LONGS
 * longs for each codec tag, as many as len holds; returns the entries
 * written. len under the head is ACM_E_BADARG. */
#define ACM_CAPS_HEAD_LONGS 20
enum { ACM_CH_ABI, ACM_CH_ENTRIES /* all there are */, ACM_CH_SESSIONS, ACM_CH_RANGE_MAX, ACM_CH_SMALL_MAX,
       ACM_CH_FLAGS, ACM_CH_CALLS /* 12 longs: each call's version, ACM_CAPS's first */ };
#define ACM_CHF_HW 1u                 /* hardware decoding allowed here */
#define ACM_CHF_HWSCAN 2u             /* the host's decoders not known yet: no entry says hardware */
#define ACM_CAPS_ENTRY_LONGS 8
enum { ACM_CE_TAG, ACM_CE_FLAGS, ACM_CE_PATH /* ACM_PATH_*, with ACM_CEF_HW */, ACM_CE_MAXW, ACM_CE_MAXH,
       ACM_CE_DEPTHS /* bit 8: 8-bit, bit 10: 10-bit */ };
#define ACM_CEF_BUILT 1u              /* a decoder for it is in this build */
#define ACM_CEF_ALLOWED 2u            /* the policy allows it */
#define ACM_CEF_HWONLY 4u             /* the policy allows it on a hardware decoder only */
#define ACM_CEF_HW 8u                 /* a hardware decoder here takes it */

/* ACM_INFO and ACM_RESULT: ACM_INFO_HEAD_LONGS longs, then one entry of
 * ACM_INFO_STREAM_LONGS longs a stream, as many whole ones as len holds;
 * returns the bytes written. len under the head is ACM_E_BADARG. A
 * probe's (RESULT) has state 0 and no path or route. */
#define ACM_INFO_HEAD_LONGS 12
enum { ACM_IH_CONTAINER /* a tag, or 0 */, ACM_IH_STREAMS, ACM_IH_DURATION /* ms, -1 not known */, ACM_IH_STATE,
       ACM_IH_POSITION /* ms */, ACM_IH_VIDEO /* the stream open, -1 none */, ACM_IH_AUDIO, ACM_IH_VPATH,
       ACM_IH_VROUTE, ACM_IH_AROUTE, ACM_IH_SIZE /* the frames': width << 16 | height */, ACM_IH_ERROR /* the last ACM_E_, or 0 */ };
enum { ACM_STATE_PROBE = 0, ACM_STATE_OPENING, ACM_STATE_PAUSED, ACM_STATE_PLAYING, ACM_STATE_ENDED, ACM_STATE_ERROR };
#define ACM_INFO_STREAM_LONGS 12
enum { ACM_IS_KIND /* 1 video, 2 audio, 3 subtitles */, ACM_IS_TAG, ACM_IS_FLAGS /* acm_proto.h's ACM_SF_* */, ACM_IS_WIDTH,
       ACM_IS_HEIGHT, ACM_IS_DEPTH, ACM_IS_RATE_NUM, ACM_IS_RATE_DEN, ACM_IS_SAMPLE_RATE, ACM_IS_CHANNELS,
       ACM_IS_LANG /* four letters, as a long */ };

/* the events' a (and b) as this backend posts them:
 *   OPENED  a the duration in ms (-1 not known), b the video path
 *   PROBED  session 0, a the ticket
 *   FRAME   a the frame's pts in ms (only with ACM_OF_EVENTS_FRAME or CONTROL EVENTS)
 *   END     a the position in ms
 *   ERROR   a the ACM_E_ code
 *   PATH    a the new video path
 *   RECT    a why the plane is not used (acscreen's ACV_W_*, or 8: no plane here), b the route now */
#define ACM_RECT_NO_PLANE 8u
#endif
