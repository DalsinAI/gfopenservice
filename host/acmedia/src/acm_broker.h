/*
 * ACMedia's broker side (design section 6): starts acmedia-worker, checks it
 * is locked down before giving it anything, asks it things with a watchdog,
 * and gives it the frame pool. The runtime's broker (stage b) and the test
 * harness both use this. Nothing here links FFmpeg.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef ACM_BROKER_H
#define ACM_BROKER_H
#include <stddef.h>
#include <sys/types.h>
#include "acm_proto.h"

struct acm_worker {
    pid_t pid;
    int sock;
    struct acm_ready ready;
    uint8_t *pool;
    size_t pool_size;
    uint32_t slot_size, nslots;
    int dead;                    /* the worker is gone: every request answers ACM_E_DECODER */
    int exit_signal;             /* what ended it (SIGSYS: seccomp), or 0 */
    int exit_code;
    char why[160];               /* one line for the log */
};

/* Starts a worker (exe) and says HELLO. hw: let it open the decoder
 * devices. mem_mb: its address-space limit. quiet: its stderr to
 * /dev/null. 0, or an ACM_E_ with w->why. */
int acm_spawn(struct acm_worker *w, const char *exe, int hw, unsigned mem_mb, int quiet,
              const struct acm_hello *h);
/* One request and its answer, within timeout_ms (the watchdog: past it the
 * worker is killed). 0 with the answer in *rtype/rbuf, or ACM_E_DECODER. */
int acm_ask(struct acm_worker *w, uint32_t type, const void *p, uint32_t n, const int *fds, int nfds,
            uint32_t *rtype, void *rbuf, uint32_t cap, uint32_t *rlen, int *rfds, int *rnfds, int timeout_ms);
/* The sealed memfd pool, mapped here read only and sent to the worker. */
int acm_give_pool(struct acm_worker *w, uint32_t slot_size, uint32_t nslots);
/* A read-only source fd for the worker. */
int acm_give_source(struct acm_worker *w, int fd);
void acm_end(struct acm_worker *w);                 /* CLOSE, else kill; always reaps */
/* The answer of an ACM_A_ERROR, or code when it was something else */
int acm_err(uint32_t rtype, const void *rbuf, int code);
/* An allow-everything hello */
void acm_hello_all(struct acm_hello *h, uint32_t flags);
#endif
