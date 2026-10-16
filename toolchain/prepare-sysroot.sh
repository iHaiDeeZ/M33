#!/usr/bin/env bash
# Create $PS4_SYSROOT/libc-overlay: patched copies of OpenOrbis C headers that
# conflict with libc++ 21. The overlay is searched before $OPENORBIS/include.
set -euo pipefail
source "$(dirname "$0")/env.sh"

OVERLAY="$PS4_SYSROOT/libc-overlay"
mkdir -p "$OVERLAY/sys"

# math.h: OpenOrbis adds C++ overloads (isnan, signbit, isgreater, ...) under
# `#ifdef __cplusplus`; libc++ declares its own, so fall back to the plain C
# macros. The extern "C" { ... } wrappers are left alone.
awk '
    { line[NR] = $0 }
    END {
        for (i = 1; i <= NR; i++) {
            if (line[i] == "#ifdef __cplusplus" && line[i + 1] !~ /^(extern|})/)
                print "#if 0 /* PSChrome: libc++ provides the C++ overloads */"
            else
                print line[i]
        }
    }' "$OPENORBIS/include/math.h" > "$OVERLAY/math.h"

# ---------------------------------------------------------------------------
# ABI fixes. mmap, clock_gettime and sysconf resolve to the PS4 kernel library
# (FreeBSD numbering), but the OpenOrbis musl headers use Linux numbering for
# their constants. Override the constants with the FreeBSD values.

# time.h: clock ids.
awk '
    /^#define CLOCK_REALTIME / {
        print "/* PSChrome: FreeBSD clock ids (clock_gettime is the PS4 kernel'"'"'s). */"
        print "#define CLOCK_REALTIME           0"
        print "#define CLOCK_VIRTUAL            1"
        print "#define CLOCK_PROF               2"
        print "#define CLOCK_MONOTONIC          4"
        print "#define CLOCK_UPTIME             5"
        print "#define CLOCK_UPTIME_PRECISE     7"
        print "#define CLOCK_UPTIME_FAST        8"
        print "#define CLOCK_REALTIME_PRECISE   9"
        print "#define CLOCK_REALTIME_FAST      10"
        print "#define CLOCK_MONOTONIC_PRECISE  11"
        print "#define CLOCK_MONOTONIC_FAST     12"
        print "#define CLOCK_SECOND             13"
        print "#define CLOCK_THREAD_CPUTIME_ID  14"
        print "#define CLOCK_PROCESS_CPUTIME_ID 15"
        print "/* Linux names used by portable code. */"
        print "#define CLOCK_MONOTONIC_RAW      CLOCK_MONOTONIC_PRECISE"
        print "#define CLOCK_MONOTONIC_COARSE   CLOCK_MONOTONIC_FAST"
        print "#define CLOCK_REALTIME_COARSE    CLOCK_REALTIME_FAST"
        print "#define CLOCK_BOOTTIME           CLOCK_UPTIME"
        skipping = 1
        next
    }
    skipping && /^#define CLOCK_/ { next }
    { skipping = 0; print }
' "$OPENORBIS/include/time.h" > "$OVERLAY/time.h"

# unistd.h: sysconf() numbering depends on which library provides it. PSChrome links it from
# a place that follows musl numbering; with love-ps4's link order (-lkernel first) it is the
# kernel's and follows FreeBSD numbering: the DolphinPS4 probe measured sysconf(47) = 16384
# (page size) and sysconf(58) = 8 (CPUs online), while the musl constants give -1 and 200112
# (so std::thread::hardware_concurrency() returned 200112). Use FreeBSD values.
cat > "$OVERLAY/unistd.h" <<'EOF'
/* DolphinPS4: FreeBSD sysconf() names; sysconf is the PS4 kernel's (FreeBSD numbering). */
#include_next <unistd.h>
#ifndef _DOLPHINPS4_UNISTD_H
#define _DOLPHINPS4_UNISTD_H
#undef _SC_ARG_MAX
#define _SC_ARG_MAX 1
#undef _SC_CHILD_MAX
#define _SC_CHILD_MAX 2
#undef _SC_CLK_TCK
#define _SC_CLK_TCK 3
#undef _SC_NGROUPS_MAX
#define _SC_NGROUPS_MAX 4
#undef _SC_OPEN_MAX
#define _SC_OPEN_MAX 5
#undef _SC_PAGESIZE
#define _SC_PAGESIZE 47
#undef _SC_PAGE_SIZE
#define _SC_PAGE_SIZE _SC_PAGESIZE
#undef _SC_NPROCESSORS_CONF
#define _SC_NPROCESSORS_CONF 57
#undef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 58
#undef _SC_GETGR_R_SIZE_MAX
#define _SC_GETGR_R_SIZE_MAX 70
#undef _SC_GETPW_R_SIZE_MAX
#define _SC_GETPW_R_SIZE_MAX 71
#undef _SC_PHYS_PAGES
#define _SC_PHYS_PAGES 121
#endif
EOF

# sys/mman.h: FreeBSD flags. Linux-only flags (MAP_NORESERVE, MAP_POPULATE,
# mremap, ...) are deliberately absent so portable code skips them.
cat > "$OVERLAY/sys/mman.h" <<'EOF'
/* PSChrome: FreeBSD <sys/mman.h> values for the PS4 kernel's mmap. */
#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H
#ifdef __cplusplus
extern "C" {
#endif

#include <features.h>

#define __NEED_mode_t
#define __NEED_size_t
#define __NEED_off_t
#include <bits/alltypes.h>

#define MAP_FAILED ((void *) -1)

#define PROT_NONE  0x00
#define PROT_READ  0x01
#define PROT_WRITE 0x02
#define PROT_EXEC  0x04

#define MAP_SHARED       0x0001
#define MAP_PRIVATE      0x0002
#define MAP_FIXED        0x0010
#define MAP_HASSEMAPHORE 0x0200
#define MAP_STACK        0x0400
#define MAP_NOSYNC       0x0800
#define MAP_FILE         0x0000
#define MAP_ANON         0x1000
#define MAP_ANONYMOUS    MAP_ANON
#define MAP_NOCORE       0x00020000
#define MAP_ALIGNMENT_SHIFT 24
#define MAP_ALIGNED(n)   ((n) << MAP_ALIGNMENT_SHIFT)

#define MS_SYNC       0x0000
#define MS_ASYNC      0x0001
#define MS_INVALIDATE 0x0002

#define MCL_CURRENT 0x0001
#define MCL_FUTURE  0x0002

#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       5
#define MADV_NOSYNC     6
#define MADV_AUTOSYNC   7
#define MADV_NOCORE     8
#define MADV_CORE       9

#define POSIX_MADV_NORMAL     MADV_NORMAL
#define POSIX_MADV_RANDOM     MADV_RANDOM
#define POSIX_MADV_SEQUENTIAL MADV_SEQUENTIAL
#define POSIX_MADV_WILLNEED   MADV_WILLNEED
#define POSIX_MADV_DONTNEED   MADV_DONTNEED

void *mmap(void *, size_t, int, int, int, off_t);
int munmap(void *, size_t);
int mprotect(void *, size_t, int);
int msync(void *, size_t, int);
int madvise(void *, size_t, int);
int mincore(const void *, size_t, char *);
int minherit(void *, size_t, int);
int mlock(const void *, size_t);
int munlock(const void *, size_t);
int mlockall(int);
int munlockall(void);
int posix_madvise(void *, size_t, int);
int shm_open(const char *, int, mode_t);
int shm_unlink(const char *);

#ifdef __cplusplus
}
#endif
#endif
EOF

# sys/sysctl.h: FreeBSD interface (sysctl/sysctlbyname are PS4 kernel calls).
cat > "$OVERLAY/sys/sysctl.h" <<'EOF'
/* PSChrome: minimal FreeBSD <sys/sysctl.h>. struct kinfo_proc is not
 * provided; code that needs it is patched for the PS4 instead. */
#ifndef _PSCHROME_SYS_SYSCTL_H
#define _PSCHROME_SYS_SYSCTL_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int sysctl(const int *name, unsigned int namelen, void *oldp, size_t *oldlenp, const void *newp, size_t newlen);
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, const void *newp, size_t newlen);
int sysctlnametomib(const char *name, int *mibp, size_t *sizep);
#ifdef __cplusplus
}
#endif
#define CTL_KERN 1
#define CTL_VM 2
#define CTL_HW 6
#define KERN_OSTYPE 1
#define KERN_OSRELEASE 2
#define KERN_PROC 14
#define KERN_PROC_PID 1
#define HW_MACHINE 1
#define HW_NCPU 3
#define HW_PHYSMEM 5
#define HW_USERMEM 6
#define HW_PAGESIZE 7
#define KERN_PROC_PATHNAME 12
#define HW_REALMEM 12
#endif
EOF

# signal.h: the PS4 kernel implements FreeBSD signals. OpenOrbis already uses
# FreeBSD's sigset_t / struct sigaction layout, but Linux signal numbers and
# flags, and names the sa_sigaction union member wrongly.
sed -E \
    -e 's/^#define SIG_BLOCK     0$/#define SIG_BLOCK     1/' \
    -e 's/^#define SIG_UNBLOCK   1$/#define SIG_UNBLOCK   2/' \
    -e 's/^#define SIG_SETMASK   2$/#define SIG_SETMASK   3/' \
    -e 's/void \(\*__sa_sigaction\)\(int, struct __siginfo \*, void \*\);/void (*sa_sigaction)(int, struct __siginfo *, void *);/' \
    -e 's/^#define SS_DISABLE    2$/#define SS_DISABLE    4/' \
    -e 's/^#define SIGRTMIN  \(__libc_current_sigrtmin\(\)\)$/#define SIGRTMIN  65/' \
    -e 's/^#define SIGRTMAX  \(__libc_current_sigrtmax\(\)\)$/#define SIGRTMAX  126/' \
    "$OPENORBIS/include/signal.h" > "$OVERLAY/signal.h"
for want in 'SIG_BLOCK     1' 'SIG_SETMASK   3' 'void (\*sa_sigaction)' 'SS_DISABLE    4' 'SIGRTMIN  65'; do
    grep -q "$want" "$OVERLAY/signal.h" || { echo "signal.h patch failed: $want" >&2; exit 1; }
done

mkdir -p "$OVERLAY/bits"
cat > "$OVERLAY/bits/signal.h" <<'EOF'
/* PSChrome: FreeBSD/amd64 signal numbers, flags and contexts (PS4 kernel). */
#if defined(_POSIX_SOURCE) || defined(_POSIX_C_SOURCE) \
 || defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) || defined(_BSD_SOURCE)

#define MINSIGSTKSZ 2048
#define SIGSTKSZ (MINSIGSTKSZ + 32768)

typedef long __pschrome_register_t;

typedef struct __mcontext {
	__pschrome_register_t mc_onstack;
	__pschrome_register_t mc_rdi, mc_rsi, mc_rdx, mc_rcx, mc_r8, mc_r9;
	__pschrome_register_t mc_rax, mc_rbx, mc_rbp, mc_r10, mc_r11, mc_r12;
	__pschrome_register_t mc_r13, mc_r14, mc_r15;
	unsigned int mc_trapno;
	unsigned short mc_fs, mc_gs;
	__pschrome_register_t mc_addr;
	unsigned int mc_flags;
	unsigned short mc_es, mc_ds;
	__pschrome_register_t mc_err, mc_rip, mc_cs, mc_rflags, mc_rsp, mc_ss;
	long mc_len;
	long mc_fpformat;
	long mc_ownedfp;
	long mc_fpstate[64] __attribute__((aligned(16)));
	__pschrome_register_t mc_fsbase, mc_gsbase;
	__pschrome_register_t mc_xfpustate, mc_xfpustate_len;
	long mc_spare[4];
} mcontext_t;

struct sigaltstack {
	void *ss_sp;
	size_t ss_size;
	int ss_flags;
};

typedef struct __ucontext {
	sigset_t uc_sigmask;
	mcontext_t uc_mcontext;
	struct __ucontext *uc_link;
	struct sigaltstack uc_stack;
	int uc_flags;
	int __spare__[4];
} ucontext_t;

#define SA_ONSTACK   0x0001
#define SA_RESTART   0x0002
#define SA_RESETHAND 0x0004
#define SA_NOCLDSTOP 0x0008
#define SA_NODEFER   0x0010
#define SA_NOCLDWAIT 0x0020
#define SA_SIGINFO   0x0040

#endif

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGIOT    SIGABRT
#define SIGEMT    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGBUS    10
#define SIGSEGV   11
#define SIGSYS    12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGURG    16
#define SIGSTOP   17
#define SIGTSTP   18
#define SIGCONT   19
#define SIGCHLD   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGIO     23
#define SIGPOLL   SIGIO
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGINFO   29
#define SIGUSR1   30
#define SIGUSR2   31
#define SIGTHR    32

#define _NSIG 128
EOF

# sys/stat.h: FreeBSD spells the birth time st_birthtime.
cat > "$OVERLAY/sys/stat.h" <<'EOF'
/* PSChrome: add FreeBSD field aliases on top of the OpenOrbis header. */
#include_next <sys/stat.h>
#ifndef st_birthtime
#define st_birthtime st_birthtim.tv_sec
#endif
EOF

# sys/endian.h: FreeBSD header that FreeBSD-targeting code expects; musl has
# <endian.h> instead. Provide the FreeBSD names on top of it.
cat > "$OVERLAY/sys/endian.h" <<'EOF'
/* PSChrome shim: FreeBSD <sys/endian.h> on top of musl <endian.h>. */
#ifndef _PSCHROME_SYS_ENDIAN_H
#define _PSCHROME_SYS_ENDIAN_H
#include <endian.h>
#include <stdint.h>
#ifndef _BYTE_ORDER
#define _BYTE_ORDER BYTE_ORDER
#define _LITTLE_ENDIAN LITTLE_ENDIAN
#define _BIG_ENDIAN BIG_ENDIAN
#define _PDP_ENDIAN PDP_ENDIAN
#endif
#ifndef bswap16
#define bswap16(x) __builtin_bswap16(x)
#define bswap32(x) __builtin_bswap32(x)
#define bswap64(x) __builtin_bswap64(x)
#endif
static __inline uint16_t be16dec(const void* p) { const uint8_t* b = (const uint8_t*)p; return (uint16_t)((b[0] << 8) | b[1]); }
static __inline uint32_t be32dec(const void* p) { const uint8_t* b = (const uint8_t*)p; return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; }
static __inline uint16_t le16dec(const void* p) { const uint8_t* b = (const uint8_t*)p; return (uint16_t)((b[1] << 8) | b[0]); }
static __inline uint32_t le32dec(const void* p) { const uint8_t* b = (const uint8_t*)p; return ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | b[0]; }
#endif
EOF

# ---------------------------------------------------------------------------
# FreeBSD headers the PS4 kernel implements but OpenOrbis doesn't ship (DolphinPS4).

# pthread_np.h: the FreeBSD thread extensions exported by libkernel.
cat > "$OVERLAY/pthread_np.h" <<'EOF'
/* DolphinPS4: FreeBSD <pthread_np.h> for functions exported by the PS4 libkernel. The OpenOrbis
 * <pthread.h> already declares most of them (pthread_attr_get_np, pthread_set_name_np, ...). */
#ifndef _DOLPHINPS4_PTHREAD_NP_H
#define _DOLPHINPS4_PTHREAD_NP_H
#include <pthread.h>
#ifdef __cplusplus
extern "C" {
#endif
int pthread_getthreadid_np(void);
int pthread_main_np(void);
#ifdef __cplusplus
}
#endif
#endif
EOF

# machine/cpufunc.h: FreeBSD's cpuid helpers.
mkdir -p "$OVERLAY/machine"
cat > "$OVERLAY/machine/cpufunc.h" <<'EOF'
/* DolphinPS4: the cpuid helpers from FreeBSD's <machine/cpufunc.h>. */
#ifndef _DOLPHINPS4_MACHINE_CPUFUNC_H
#define _DOLPHINPS4_MACHINE_CPUFUNC_H
static __inline void do_cpuid(unsigned int ax, unsigned int *p) {
	__asm __volatile("cpuid" : "=a" (p[0]), "=b" (p[1]), "=c" (p[2]), "=d" (p[3]) : "0" (ax));
}
static __inline void cpuid_count(unsigned int ax, unsigned int cx, unsigned int *p) {
	__asm __volatile("cpuid" : "=a" (p[0]), "=b" (p[1]), "=c" (p[2]), "=d" (p[3]) : "0" (ax), "c" (cx));
}
#endif
EOF

# bits/alltypes.h: pthread types whose musl size is smaller than what the PS4 kernel's (FreeBSD
# libthr) functions write. FreeBSD's attribute and spinlock types are pointers to objects the
# kernel allocates; musl's are 4-byte structs, so pthread_mutexattr_init & co. overflowed by 4
# bytes (verified on hardware: std::recursive_mutex's stack attr clobbered a saved register).
# pthread_once_t stays 4 bytes because libc.a (newlocale, call_once) uses it compiled; the
# runtime wraps pthread_once instead (port/ps4_runtime.cpp). Other pthread types are larger
# than the kernel's pointer-sized ones and fine as they are.
cat > "$OVERLAY/bits/alltypes.h" <<'EOF'
/* DolphinPS4: FreeBSD-sized pthread attribute/spinlock types (the PS4 kernel implements them). */
#ifndef _DOLPHINPS4_PTHREAD_TYPES
#define _DOLPHINPS4_PTHREAD_TYPES
typedef struct { void *__p; } pthread_mutexattr_t;
#define __DEFINED_pthread_mutexattr_t
typedef struct { void *__p; } pthread_condattr_t;
#define __DEFINED_pthread_condattr_t
typedef struct { void *__p; } pthread_barrierattr_t;
#define __DEFINED_pthread_barrierattr_t
typedef void *pthread_spinlock_t;
#define __DEFINED_pthread_spinlock_t
#endif
#include_next <bits/alltypes.h>
EOF

# sys/ioctl.h: FreeBSD socket ioctls (OpenOrbis only has LINUX_FIONBIO).
cat > "$OVERLAY/sys/ioctl.h" <<'EOF'
/* DolphinPS4: FreeBSD additions on top of the OpenOrbis header. */
#include_next <sys/ioctl.h>
#ifndef FIONBIO
#define FIONBIO 0x8004667e  /* _IOW('f', 126, int) */
#endif
#ifndef FIONREAD
#define FIONREAD 0x4004667f /* _IOR('f', 127, int) */
#endif
EOF

# sys/socket.h: OpenOrbis already uses FreeBSD socket values; add the missing FreeBSD option.
cat > "$OVERLAY/sys/socket.h" <<'EOF'
/* DolphinPS4: FreeBSD additions on top of the OpenOrbis header. */
#include_next <sys/socket.h>
#ifndef SO_NOSIGPIPE
#define SO_NOSIGPIPE 0x0800
#endif
EOF

grep -n -A1 "__cplusplus\|PSChrome" "$OVERLAY/math.h"
