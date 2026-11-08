/*
 * Files kept on the host between calls (blob.h).
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include "blob.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct blob {
    uint32_t handle;           /* 0: the slot is free */
    uint8_t *data;
    uint32_t len;
    int refs;                  /* calls using it now */
    int dead;                  /* let go while in use: freed when the last call ends */
    uint64_t used;             /* for least recently used */
};

static struct blob blobs[BLOB_MAX];
static uint64_t clock_;
static uint32_t serial;
static uint64_t total;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* A handle names its slot (low 4 bits) and a serial (the rest, never 0), so
 * a handle kept by an Amiga never names a newer file in the same slot. */
static int slot_of(uint32_t handle)
{
    int s = (int)(handle & (BLOB_MAX - 1));
    if (!handle || blobs[s].handle != handle || blobs[s].dead)
        return -1;
    return s;
}

static void free_slot(struct blob *b)
{
    total -= b->len;
    free(b->data);
    memset(b, 0, sizeof *b);
}

static void let_go(struct blob *b)
{
    if (b->refs)
        b->dead = 1;
    else
        free_slot(b);
}

int blob_before(uint32_t extra3, const uint8_t **data, uint32_t *len, struct blob_use *u)
{
    uint32_t handle = extra3 >> BLOB_SHIFT;
    int s;

    u->slot = -1;
    if (*len || !handle)
        return 0;
    pthread_mutex_lock(&lock);
    if ((s = slot_of(handle)) < 0) {
        pthread_mutex_unlock(&lock);
        return BLOB_GONE;
    }
    blobs[s].refs++;
    blobs[s].used = ++clock_;
    *data = blobs[s].data;
    *len = blobs[s].len;
    u->slot = s;
    pthread_mutex_unlock(&lock);
    return 0;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* A new kept file: a copy of n bytes. Its handle, or 0. */
static uint32_t keep(const uint8_t *d, uint32_t n)
{
    uint8_t *copy;
    int s, i, oldest;

    if (!n || n > BLOB_BYTES || !(copy = malloc(n)))
        return 0;
    memcpy(copy, d, n);
    pthread_mutex_lock(&lock);
    for (;;) {
        /* A free slot, and room: the least recently used idle file goes. */
        s = -1;
        oldest = -1;
        for (i = 0; i < BLOB_MAX; i++) {
            if (!blobs[i].handle) {
                if (s < 0)
                    s = i;
            } else if (!blobs[i].refs && !blobs[i].dead && (oldest < 0 || blobs[i].used < blobs[oldest].used))
                oldest = i;
        }
        if (s >= 0 && total + n <= BLOB_BYTES)
            break;
        if (oldest < 0) {              /* everything in use: not kept */
            pthread_mutex_unlock(&lock);
            free(copy);
            return 0;
        }
        free_slot(&blobs[oldest]);
    }
    if (!++serial || serial >= (1u << (24 - 4)))
        serial = 1;
    blobs[s].handle = serial << 4 | (uint32_t)s;
    blobs[s].data = copy;
    blobs[s].len = n;
    blobs[s].used = ++clock_;
    total += n;
    pthread_mutex_unlock(&lock);
    return blobs[s].handle;
}

void blob_after(int ok, uint32_t extra3, const uint8_t *data, uint32_t len, uint8_t *out, uint32_t room,
                uint32_t *written, struct blob_use *u)
{
    uint32_t handle = 0;

    if (ok && (extra3 & BLOB_KEEP) && out && room >= 4) {
        if (u->slot >= 0) {
            pthread_mutex_lock(&lock);
            handle = blobs[u->slot].dead ? 0 : blobs[u->slot].handle;
            pthread_mutex_unlock(&lock);
        } else
            handle = keep(data, len);
        put32(out, handle);
        *written = 4;
    }
    pthread_mutex_lock(&lock);
    if (u->slot >= 0) {
        struct blob *b = &blobs[u->slot];
        b->refs--;
        if ((extra3 & BLOB_DROP) && !(handle && (extra3 & BLOB_KEEP)))
            b->dead = 1;
        if (b->dead && !b->refs)
            free_slot(b);
    }
    pthread_mutex_unlock(&lock);
    u->slot = -1;
}

int blob_release(uint32_t handle)
{
    int s;

    pthread_mutex_lock(&lock);
    if ((s = slot_of(handle)) >= 0)
        let_go(&blobs[s]);
    pthread_mutex_unlock(&lock);
    return s < 0 ? BLOB_GONE : 0;
}

void blob_stats(uint32_t *count, uint64_t *bytes)
{
    int i;

    pthread_mutex_lock(&lock);
    *count = 0;
    for (i = 0; i < BLOB_MAX; i++)
        if (blobs[i].handle && !blobs[i].dead)
            (*count)++;
    *bytes = total;
    pthread_mutex_unlock(&lock);
}
