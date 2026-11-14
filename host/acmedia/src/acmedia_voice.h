/* ACAudioMixer media voice v1: the cross-repository ACMedia audio clock. */
#ifndef ACMEDIA_VOICE_H
#define ACMEDIA_VOICE_H
#include <stdint.h>

/* Offsets, not a C structure, are the ABI. The Linux host is little endian. */
#define ACMV_MAGIC          0x31564d41u /* "AMV1" */
#define ACMV_VERSION        1u
#define ACMV_HEADER_BYTES   128u
#define ACMV_RATE           44100u
#define ACMV_CHANNELS       2u
#define ACMV_FORMAT_S16LE   1u
#define ACMV_O_MAGIC         0u /* u32 */
#define ACMV_O_VERSION       4u /* u32 */
#define ACMV_O_HEADER_BYTES  8u /* u32 */
#define ACMV_O_CAPACITY     12u /* u32 frames, power of two */
#define ACMV_O_RATE         16u /* u32 */
#define ACMV_O_CHANNELS     20u /* u32 */
#define ACMV_O_FORMAT       24u /* u32 */
#define ACMV_O_FLAGS        28u /* u32, producer */
#define ACMV_O_WRITE        32u /* u64 frames, producer/release */
#define ACMV_O_READ         40u /* u64 frames, mixer/acquire */
#define ACMV_O_PLAYED       48u /* u64 frames after latency, mixer/acquire */
#define ACMV_O_LATENCY      56u /* u64 frames, mixer/acquire */
#define ACMV_O_ORIGIN_NS    64u /* s64 media PTS of frame zero */
#define ACMV_O_GAIN_Q16     72u /* u32, CONTROL volume */
#define ACMV_O_GENERATION   76u /* u32, producer */
#define ACMV_F_PAUSED       (1u << 0)
#define ACMV_F_EOF          (1u << 1)

static inline uint32_t acmv_u32(const uint8_t *p, uint32_t o) {
    return __atomic_load_n((const uint32_t *)(const void *)(p + o), __ATOMIC_ACQUIRE);
}
static inline uint64_t acmv_u64(const uint8_t *p, uint32_t o) {
    return __atomic_load_n((const uint64_t *)(const void *)(p + o), __ATOMIC_ACQUIRE);
}
static inline void acmv_put_u32(uint8_t *p, uint32_t o, uint32_t v) {
    __atomic_store_n((uint32_t *)(void *)(p + o), v, __ATOMIC_RELEASE);
}
static inline void acmv_put_u64(uint8_t *p, uint32_t o, uint64_t v) {
    __atomic_store_n((uint64_t *)(void *)(p + o), v, __ATOMIC_RELEASE);
}

#endif
