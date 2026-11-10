/*
 * ACMedia: the protocol between the broker (in the runtime, or the test
 * harness) and acmedia-worker, the sandboxed decoder process
 * (amicradle-teams designs/ACMEDIA-DESIGN-20261110.md, section 6).
 *
 * One SOCK_SEQPACKET socketpair per worker. Every message is a struct
 * acm_hdr then its payload, at most ACM_MSG_MAX bytes; file descriptors (the
 * source, the frame pool, dma-bufs) travel as SCM_RIGHTS beside it. The
 * broker asks, the worker answers each request with exactly one message.
 * Nothing in here is FFmpeg's: the guest-facing ABI (hostlib ids $00012000
 * up) is built on these, never on FFmpeg types.
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef ACM_PROTO_H
#define ACM_PROTO_H

#include <stdint.h>

#define ACM_PROTO_VERSION 1
#define ACM_MSG_MAX 65536
#define ACM_MAX_STREAMS 16
#define ACM_MAX_TAGS 96
#define ACM_MAX_FDS 4

/* Error codes: the design's ACM_E_* (section 2.2), as the guest sees them. */
#define ACM_OK 0
#define ACM_PENDING 1
#define ACM_E_NOTAVAIL (-1)
#define ACM_E_BADARG (-2)
#define ACM_E_NOSUCH (-3)
#define ACM_E_FORMAT (-4)
#define ACM_E_DENIED (-5)
#define ACM_E_NOCODEC (-6)
#define ACM_E_TOOBIG (-7)
#define ACM_E_DECODER (-8)
#define ACM_E_IO (-9)
#define ACM_E_LIMIT (-10)
#define ACM_E_EOF (-11)
#define ACM_E_AGAIN (-12)
#define ACM_E_VERSION (-13)

/* Requests (broker to worker) */
enum {
    ACM_Q_HELLO = 1,    /* acm_hello; answer ACM_A_OK with acm_ready */
    ACM_Q_SOURCE,       /* one fd (read-only); acm_source */
    ACM_Q_PROBE,        /* answer ACM_A_INFO */
    ACM_Q_OPEN,         /* acm_open; answer ACM_A_OPENED */
    ACM_Q_POOL,         /* one fd (the sealed memfd); acm_pool */
    ACM_Q_NEXT,         /* acm_next; answer ACM_A_FRAME, ACM_A_AUDIO or ACM_A_ERROR (EOF, AGAIN...) */
    ACM_Q_RELEASE,      /* acm_release: a pool slot or a mapped hardware frame is the worker's again */
    ACM_Q_SEEK,         /* acm_seek */
    ACM_Q_CAPS,         /* answer ACM_A_CAPS */
    ACM_Q_HOOK,         /* test builds only (ACM_TEST_HOOKS): acm_hook */
    ACM_Q_CLOSE,        /* the worker answers ACM_A_OK and exits */
};
/* Answers (worker to broker) */
enum {
    ACM_A_OK = 64,
    ACM_A_ERROR,        /* acm_error */
    ACM_A_INFO,         /* acm_info */
    ACM_A_OPENED,       /* acm_opened */
    ACM_A_FRAME,        /* acm_frame, plus dma-buf fds when it is a hardware frame given as such */
    ACM_A_AUDIO,        /* acm_audio then the samples */
    ACM_A_CAPS,         /* acm_caps */
};

struct acm_hdr { uint32_t type, len; };

/* HELLO: what this worker may do. Tags are ours (acm_codecs.h). */
#define ACM_HF_HW       1u   /* hardware decoders allowed */
#define ACM_HF_COPY     2u   /* never decode into the pool: copy every frame (for measuring) */
struct acm_hello {
    uint32_t version;
    uint32_t flags;
    uint32_t nallow, nhwonly;
    uint32_t allow[ACM_MAX_TAGS];    /* codecs the policy allows */
    uint32_t hwonly[ACM_MAX_TAGS];   /* codecs allowed only on a hardware decoder */
};
struct acm_ready {
    uint32_t version;
    uint32_t sandbox;                /* ACM_SB_* bits the worker has in force */
    uint32_t hw;                     /* ACM_PATH_* bits whose devices were opened before lock-down */
    uint32_t landlock_abi;
};
#define ACM_SB_SECCOMP  1u
#define ACM_SB_LANDLOCK 2u
#define ACM_SB_NNP      4u
#define ACM_SB_RLIMIT   8u

struct acm_source { uint64_t size; uint32_t flags, pad; };

/* Decode paths, per stream (ACM_INFO's "path in use") */
#define ACM_PATH_NONE        0
#define ACM_PATH_SW          1
#define ACM_PATH_HW_VAAPI    2
#define ACM_PATH_HW_V4L2REQ  4
#define ACM_PATH_HW_V4L2M2M  8

#define ACM_KIND_VIDEO 1
#define ACM_KIND_AUDIO 2
#define ACM_KIND_SUB   3

#define ACM_SF_DENIED  1u   /* the policy refuses this stream's codec */
#define ACM_SF_HWOK    2u   /* a hardware decoder here takes it */
#define ACM_SF_NOCODEC 4u   /* no tag, or not in our build */
struct acm_stream {
    uint32_t kind, tag, flags;
    uint32_t width, height, bitdepth;
    uint32_t rate_num, rate_den;     /* frames a second, video */
    uint32_t sample_rate, channels;
    char lang[4];
};
struct acm_info {
    uint32_t container;              /* a tag ('MP4 ', 'MKV ', ...) or 0 */
    uint32_t nstreams;
    int64_t duration_ms;             /* -1: not known */
    char container_name[24];         /* the demuxer's, for logs only */
    struct acm_stream s[ACM_MAX_STREAMS];
};

/* OPEN: which streams (-1 the best one, -2 none) */
struct acm_open { int32_t video, audio; uint32_t flags, pad; };
#define ACM_PIX_YUV420P  1
#define ACM_PIX_NV12     2
#define ACM_PIX_YUV420P10 3
#define ACM_PIX_P010     4
#define ACM_PIX_OTHER    15
struct acm_opened {
    int32_t video, audio;            /* the streams opened, or -1 */
    uint32_t vpath, apath;           /* ACM_PATH_* */
    uint32_t width, height, pixfmt;  /* the frames in the pool */
    uint32_t slot_size, nslots;      /* the pool the broker should make (0: no video) */
    uint32_t sample_rate, channels;  /* audio, always S16 interleaved */
    struct acm_info info;
};
struct acm_pool { uint64_t size; uint32_t slot_size, nslots; };

#define ACM_WANT_VIDEO   1u
#define ACM_WANT_AUDIO   2u
#define ACM_WANT_DMABUF  4u   /* a hardware frame as dma-buf fds, not copied into the pool */
struct acm_next { uint32_t want, pad; };

#define ACM_FF_COPIED   1u   /* the frame was copied into the pool (not decoded there) */
#define ACM_FF_HW       2u   /* a hardware decoder made it */
#define ACM_FF_PATH     4u   /* the path changed before this frame (hardware fell to software) */
#define ACM_FF_DMABUF   8u   /* the planes are the fds sent with it, not a pool slot */
#define ACM_FF_KEY     16u
struct acm_frame {
    uint32_t slot;                   /* pool slot, or a hardware frame's handle (ACM_FF_DMABUF) */
    uint32_t flags, path;
    int64_t pts_ms;
    uint32_t width, height, pixfmt;
    uint32_t nplanes;
    uint32_t offset[4], pitch[4];    /* in the slot, or in the dma-buf object (plane i's object: obj[i]) */
    uint32_t drm_fourcc, nobj;
    uint64_t drm_modifier;
    uint32_t obj[4];                 /* plane -> object index */
    uint32_t objsize[4];
    uint32_t rowbytes[4], rows[4];   /* the visible bytes of each plane: what a checksum covers */
};
struct acm_audio { int64_t pts_ms; uint32_t frames, sample_rate, channels, pad; /* then S16 samples */ };

struct acm_release { uint32_t slot, pad; };
struct acm_seek { int64_t ms; uint32_t flags, pad; };

struct acm_error { int32_t code; uint32_t detail; char text[120]; };

/* CAPS: what this host's decoders can do */
#define ACM_MAX_HWCAPS 24
struct acm_hwcap {
    uint32_t tag, path;
    uint32_t max_width, max_height;
    uint32_t depths;                 /* bit 8: 8-bit, bit 10: 10-bit */
    char device[24];
};
struct acm_caps {
    uint32_t n;
    uint32_t pad;
    struct acm_hwcap c[ACM_MAX_HWCAPS];
    char note[160];
};

/* Test hooks (only a worker built with ACM_TEST_HOOKS answers ACM_Q_HOOK) */
enum {
    ACM_HOOK_CRASH = 1,     /* SIGSEGV after arg frames */
    ACM_HOOK_HANG,          /* loop for ever after arg frames */
    ACM_HOOK_LEAK,          /* allocate without end after arg frames */
    ACM_HOOK_FAIL_HW,       /* the hardware decoder "fails" after arg frames */
    ACM_HOOK_ESCAPE,        /* try what the sandbox forbids; answer which ones worked */
};
struct acm_hook { uint32_t hook, arg; };
/* ESCAPE's answer (in acm_error.detail): a bit set for each that WORKED */
#define ACM_ESC_OPEN_PASSWD   1u
#define ACM_ESC_WRITE_TMP     2u
#define ACM_ESC_LIST_HOME     4u
/* socket, execve and fork are tried last, each in a thread of its own; a
 * worker killed by seccomp there is the pass. */

#endif
