/*
 * The worker's lock-down (design section 6), applied by the worker itself
 * before it reads a single byte from the broker, so before any untrusted
 * byte: no new privileges, resource limits, Landlock (no filesystem but the
 * decoder devices, no TCP, no signals or abstract sockets outside), then a
 * seccomp-BPF allow-list that kills the process on anything else. The broker
 * reads /proc/PID/status before it sends the source.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include "acm_proto.h"
#include "acm_sandbox.h"

/* ---- Landlock ---- */

static int ll_create(const struct landlock_ruleset_attr *a, size_t n, uint32_t f)
{ return (int)syscall(__NR_landlock_create_ruleset, a, n, f); }
static int ll_add(int fd, const void *r)
{ return (int)syscall(__NR_landlock_add_rule, fd, LANDLOCK_RULE_PATH_BENEATH, r, 0); }
static int ll_self(int fd) { return (int)syscall(__NR_landlock_restrict_self, fd, 0); }

static int ll_allow(int rs, const char *path, uint64_t access, uint64_t handled)
{
    struct landlock_path_beneath_attr pb = { 0 };
    pb.parent_fd = open(path, O_PATH | O_CLOEXEC);
    if (pb.parent_fd < 0) return -1;
    struct stat st;
    if (fstat(pb.parent_fd, &st) == 0 && !S_ISDIR(st.st_mode))   /* a file takes file rights only */
        access &= LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE |
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
                  LANDLOCK_ACCESS_FS_IOCTL_DEV |
#endif
                  LANDLOCK_ACCESS_FS_EXECUTE;
    pb.allowed_access = access & handled;
    int r = ll_add(rs, &pb);
    close(pb.parent_fd);
    return r;
}

static int landlock_on(int hw, uint32_t *abi_out)
{
    int abi = ll_create(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    *abi_out = abi > 0 ? (uint32_t)abi : 0;
    if (abi < 1) return -1;
    uint64_t fs = (LANDLOCK_ACCESS_FS_MAKE_SYM << 1) - 1;       /* ABI 1: bits 0..12 */
    if (abi >= 2) fs |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3) fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    if (abi >= 5) fs |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
    struct landlock_ruleset_attr ra = { .handled_access_fs = fs };
    size_t rasz = offsetof(struct landlock_ruleset_attr, handled_access_fs) + sizeof(ra.handled_access_fs);
#ifdef LANDLOCK_ACCESS_NET_BIND_TCP
    if (abi >= 4) {
        ra.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
        rasz = offsetof(struct landlock_ruleset_attr, handled_access_net) + sizeof(ra.handled_access_net);
    }
#endif
#ifdef LANDLOCK_SCOPE_SIGNAL
    if (abi >= 6) {
        ra.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
        rasz = sizeof(ra);
    }
#endif
    int rs = ll_create(&ra, rasz, 0);
    if (rs < 0) return -1;
    if (hw) {
        const uint64_t ro = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
        uint64_t rw = ro | LANDLOCK_ACCESS_FS_WRITE_FILE;
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
        rw |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
        /* the decoder and display devices, read and write; nothing else in /dev */
        static const char *pat[] = { "/dev/video*", "/dev/media*", "/dev/dri", "/dev/dma_heap" };
        for (unsigned i = 0; i < sizeof(pat) / sizeof(pat[0]); i++) {
            glob_t g;
            if (glob(pat[i], 0, NULL, &g) == 0)
                for (size_t k = 0; k < g.gl_pathc; k++) ll_allow(rs, g.gl_pathv[k], rw, fs);
            globfree(&g);
        }
        /* the device scan (V4L2 request, m2m) reads sysfs and udev's database, read only */
        ll_allow(rs, "/sys", ro, fs);
        ll_allow(rs, "/run/udev", ro, fs);
    }
    int r = ll_self(rs);
    close(rs);
    return r;
}

/* ---- seccomp ---- */

#if defined(__x86_64__)
#define ACM_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define ACM_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#error "ACMedia's worker is for x86-64 and aarch64"
#endif

#define LD_NR  BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr))
#define LD_ARG(n) BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[n]))
#define RET(v) BPF_STMT(BPF_RET | BPF_K, (v))

static const int allowed[] = {
    __NR_read, __NR_write, __NR_readv, __NR_writev, __NR_pread64, __NR_preadv, __NR_lseek,
    __NR_close, __NR_fstat, __NR_newfstatat, __NR_statx, __NR_fstatfs, __NR_statfs,
    __NR_getdents64, __NR_readlinkat, __NR_faccessat, __NR_faccessat2, __NR_openat,
    __NR_fcntl, __NR_dup, __NR_dup3, __NR_close_range,
    __NR_munmap, __NR_mremap, __NR_madvise, __NR_brk, __NR_mlock, __NR_munlock,
    __NR_futex, __NR_set_robust_list, __NR_get_robust_list, __NR_rseq, __NR_set_tid_address,
    __NR_exit, __NR_exit_group, __NR_restart_syscall,
    __NR_rt_sigaction, __NR_rt_sigprocmask, __NR_rt_sigreturn, __NR_sigaltstack, __NR_tgkill,
    __NR_getpid, __NR_gettid, __NR_getppid, __NR_getuid, __NR_geteuid, __NR_getgid, __NR_getegid,
    __NR_clock_gettime, __NR_clock_getres, __NR_clock_nanosleep, __NR_nanosleep, __NR_gettimeofday,
    __NR_sched_yield, __NR_sched_getaffinity, __NR_getrandom, __NR_getrusage, __NR_uname,
    __NR_sysinfo, __NR_prctl, __NR_membarrier, __NR_times,
    __NR_sendmsg, __NR_recvmsg, __NR_sendto, __NR_recvfrom,
    __NR_ppoll, __NR_pselect6, __NR_epoll_create1, __NR_epoll_ctl, __NR_epoll_pwait, __NR_eventfd2,
    __NR_pipe2,
#ifdef __NR_open
    __NR_open,
#endif
#ifdef __NR_stat
    __NR_stat, __NR_lstat, __NR_access, __NR_readlink, __NR_poll, __NR_select,
    __NR_epoll_wait, __NR_pipe, __NR_dup2, __NR_arch_prctl, __NR_getdents,
#endif
#ifdef __NR_epoll_pwait2
    __NR_epoll_pwait2,
#endif
#ifdef __NR_futex_waitv
    __NR_futex_waitv,
#endif
};

int acm_seccomp_on(int trap)
{
    struct sock_filter f[256];
    unsigned n = 0;
    const uint32_t deny = trap ? SECCOMP_RET_TRAP : SECCOMP_RET_KILL_PROCESS;
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ACM_AUDIT_ARCH, 1, 0);
    f[n++] = (struct sock_filter)RET(SECCOMP_RET_KILL_PROCESS);
    f[n++] = (struct sock_filter)LD_NR;
#if defined(__x86_64__)
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1);   /* x32 */
    f[n++] = (struct sock_filter)RET(deny);
#endif
    /* clone3: "not here", so glibc's pthread_create falls back to clone */
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone3, 0, 1);
    f[n++] = (struct sock_filter)RET(SECCOMP_RET_ERRNO | ENOSYS);
    /* clone: threads only */
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 4);
    f[n++] = (struct sock_filter)LD_ARG(0);
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD, 1, 0);
    f[n++] = (struct sock_filter)RET(deny);
    f[n++] = (struct sock_filter)RET(SECCOMP_RET_ALLOW);
    /* mmap, mprotect: never executable */
    f[n++] = (struct sock_filter)LD_NR;
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mmap, 1, 0);
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mprotect, 0, 4);
    f[n++] = (struct sock_filter)LD_ARG(2);
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 0, 1);
    f[n++] = (struct sock_filter)RET(deny);
    f[n++] = (struct sock_filter)RET(SECCOMP_RET_ALLOW);
    /* ioctl: V4L2, media, DRM, dma-buf, dma-heap and the tty's (isatty) only */
    f[n++] = (struct sock_filter)LD_NR;
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_ioctl, 0, 11);
    f[n++] = (struct sock_filter)LD_ARG(1);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 8);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0xff);
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 'V', 5, 0);
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, '|', 4, 0);
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 'd', 3, 0);
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 'b', 2, 0);
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 'H', 1, 0);
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 'T', 0, 1);
    f[n++] = (struct sock_filter)RET(SECCOMP_RET_ALLOW);
    f[n++] = (struct sock_filter)RET(deny);
    /* the rest: the list */
    f[n++] = (struct sock_filter)LD_NR;
    unsigned na = sizeof(allowed) / sizeof(allowed[0]);
    for (unsigned i = 0; i < na; i++)
        f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)allowed[i], (uint8_t)(na - i), 0);
    f[n++] = (struct sock_filter)RET(deny);
    f[n++] = (struct sock_filter)RET(SECCOMP_RET_ALLOW);
    struct sock_fprog prog = { .len = (unsigned short)n, .filter = f };
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
}

static void on_sigsys(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    char b[64];
    int len = snprintf(b, sizeof b, "acmedia-worker: seccomp refused syscall %d\n", si->si_syscall);
    if (write(2, b, (size_t)len) < 0) { /* nothing more to do */ }
    _exit(99);
}

int acm_sandbox(int hw, unsigned mem_mb, int trap, uint32_t *bits, uint32_t *ll_abi)
{
    *bits = 0;
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() == 1) return -1;                      /* the broker is already gone */
    struct rlimit core = { 0, 0 }, nofile = { 64, 64 };
    struct rlimit as = { (rlim_t)mem_mb << 20, (rlim_t)mem_mb << 20 };
    if (!setrlimit(RLIMIT_CORE, &core) && !setrlimit(RLIMIT_NOFILE, &nofile) &&
        (!mem_mb || !setrlimit(RLIMIT_AS, &as)))
        *bits |= ACM_SB_RLIMIT;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0) *bits |= ACM_SB_NNP;
    if (landlock_on(hw, ll_abi) == 0) *bits |= ACM_SB_LANDLOCK;
    if (trap) {
        struct sigaction sa = { 0 };
        sa.sa_sigaction = on_sigsys;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSYS, &sa, NULL);
    }
    if (acm_seccomp_on(trap) == 0) *bits |= ACM_SB_SECCOMP;
    return (*bits & (ACM_SB_SECCOMP | ACM_SB_NNP)) == (ACM_SB_SECCOMP | ACM_SB_NNP) ? 0 : -1;
}
