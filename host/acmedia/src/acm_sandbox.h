/* The worker's lock-down (acm_sandbox.c). MIT, Copyright (c) 2026 AmiCradle Team. */
#ifndef ACM_SANDBOX_H
#define ACM_SANDBOX_H
#include <stdint.h>
/* hw: let the decoder devices through Landlock. mem_mb: RLIMIT_AS (0: none).
 * trap: SIGSYS with the syscall's number instead of a silent kill (for
 * finding what a new decoder path needs). 0 when seccomp and no-new-privs
 * are on; *bits says what is (ACM_SB_*). */
int acm_sandbox(int hw, unsigned mem_mb, int trap, uint32_t *bits, uint32_t *ll_abi);
int acm_seccomp_on(int trap);
#endif
