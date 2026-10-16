/* dlmalloc 2.8.6 (Doug Lea, MIT-0) configured as the PS4 app heap: only the mspace API (no
 * global malloc symbols, so nothing clashes with the OpenOrbis libc), over a memory block we
 * map ourselves (no mmap/sbrk), with locks and footers. Corruption and bad frees are reported
 * through ps4_heap_error() in ps4_runtime.cpp with the operation and caller that hit them.
 *
 * Replaces Sony's sceLibcMspace, which stopped returning memory after ~800 small allocations
 * in Dolphin's static initialisation with 49 KB in use on a 1 GiB heap. */

#define ONLY_MSPACES 1
#define MSPACES 1
#define USE_LOCKS 1
#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define HAVE_MORECORE 0
#define FOOTERS 1
#define INSECURE 0
#define USE_DEV_RANDOM 0
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
#define LACKS_SCHED_H 0
/* The PS4 page size; sysconf numbering differs between headers, so don't ask. */
#define malloc_getpagesize ((size_t)16384U)
#define DEFAULT_GRANULARITY ((size_t)64U * (size_t)1024U)

void ps4_heap_error(void* mspace, void* chunk, int corruption);
#define CORRUPTION_ERROR_ACTION(m) ps4_heap_error((m), 0, 1)
#define USAGE_ERROR_ACTION(m, p) ps4_heap_error((m), (p), 0)

#include "third_party/dlmalloc/malloc.c"

/* Integrity check for diagnostics: is the mspace's state still intact? */
int ps4_heap_ok(void* msp) {
  mstate ms = (mstate)msp;
  return ok_magic(ms);
}
