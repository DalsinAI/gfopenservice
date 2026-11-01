/*
 * Random bytes on an Amiga (osrandom.c).
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef OSRANDOM_H
#define OSRANDOM_H

#include <stddef.h>
#include <stdint.h>

/* slow: also gather a second and more of timing jitter (needs DOS). */
void os_random(uint8_t *out, size_t length, int slow);
void os_random_close(void);

#endif
