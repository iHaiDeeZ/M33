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

/* Bytes in use (what mallinfo's uordblks says; mallinfo itself is left out, NO_MALLINFO): a walk
 * over every chunk, under the heap's lock - for a once-a-minute line, not more often. */
size_t ps4_heap_in_use_of(void* msp) {
  mstate m = (mstate)msp;
  size_t used = 0;
  if (!PREACTION(m)) {
    if (is_initialized(m)) {
      size_t mfree = m->topsize + TOP_FOOT_SIZE;
      msegmentptr s = &m->seg;
      while (s != 0) {
        mchunkptr q = align_as_chunk(s->base);
        while (segment_holds(s, q) && q != m->top && q->head != FENCEPOST_HEAD) {
          if (!is_inuse(q))
            mfree += chunksize(q);
          q = next_chunk(q);
        }
        s = s->next;
      }
      used = m->footprint - mfree;
    }
    POSTACTION(m);
  }
  return used;
}
