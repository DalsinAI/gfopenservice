/*
 * Random bytes on an Amiga, which has no random device (osrandom.h): the
 * E clock, the scheduler's counters, the time and some addresses, stirred
 * with SHA-256 into a pool that every call feeds. With slow set it also
 * waits a frame at a time 64 times and takes the E clock's jitter, for keys
 * that must stay secret; session nonces only need to differ.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include <string.h>
#include <exec/execbase.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include "oscrypto.h"
#include "osrandom.h"

struct Device *TimerBase;
static struct timerequest timerRequest;
static uint8_t pool[32];
static ULONG stirs;

static void stir(void)
{
    struct {
        struct EClockVal clock;
        ULONG idle, dispatches, task, free, stirs;
        struct DateStamp date;
        uint8_t pool[32];
    } sample;
    memset(&sample, 0, sizeof sample);
    if (!TimerBase && !OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&timerRequest, 0))
        TimerBase = timerRequest.tr_node.io_Device;
    if (TimerBase)
        ReadEClock(&sample.clock);
    sample.idle = SysBase->IdleCount;
    sample.dispatches = SysBase->DispCount;
    sample.task = (ULONG)FindTask(NULL);
    sample.free = AvailMem(MEMF_ANY);
    sample.stirs = ++stirs;
    if (DOSBase)
        DateStamp(&sample.date);
    memcpy(sample.pool, pool, sizeof pool);
    os_sha256(&sample, sizeof sample, pool);
}

void os_random(uint8_t *out, size_t length, int slow)
{
    uint8_t block[32], seed[36];
    ULONG counter = 0;
    int i;
    stir();
    for (i = 0; slow && i < 64; i++) {
        Delay(1);
        stir();
    }
    while (length) {
        size_t n = length < 32 ? length : 32;
        memcpy(seed, pool, 32);
        memcpy(seed + 32, &counter, 4);
        counter++;
        os_sha256(seed, sizeof seed, block);
        memcpy(out, block, n);
        out += n;
        length -= n;
    }
    stir();                               /* what was handed out never comes back */
}

void os_random_close(void)
{
    if (TimerBase)
        CloseDevice((struct IORequest *)&timerRequest);
    TimerBase = NULL;
}
