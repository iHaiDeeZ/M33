/*
 * PS4 implementation of Mesa's amdgpu kernel interface (ac_linux_drm.h), so that RADV's amdgpu
 * winsys runs unchanged on top of Sony's GNM driver.
 *
 * The PS4's GPU ("Liverpool", GCN gfx7) shares the process address space: a GPU virtual address
 * is the CPU virtual address. Buffers are direct memory mapped at the address the winsys
 * reserved for them, command buffers are submitted with sceGnmSubmitCommandBuffers, and each
 * submission ends with an end-of-pipe write of its sequence number, which fences and sync
 * objects compare against.
 *
 * Copyright 2026 DolphinPS4. SPDX-License-Identifier: MIT
 */

#include "ac_linux_drm.h"
#include "ac_gpu_info.h"
#include "addrlib/src/amdgpu_asic_addr.h"
#include "ac_debug.h"
#include "util/os_time.h"
#include "util/simple_mtx.h"
#include "util/u_math.h"
#include "util/u_sync_provider.h"
#include "util/vma.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

/* libkernel (declared here rather than through the OpenOrbis headers, which clash with Mesa's). */
int sceKernelAllocateDirectMemory(off_t search_start, off_t search_end, size_t len, size_t align,
                                  int type, off_t *phys);
int sceKernelReleaseDirectMemory(off_t start, size_t len);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, off_t phys,
                             size_t align);
int sceKernelReserveVirtualRange(void **addr, size_t len, int flags, size_t align);
int sceKernelMunmap(void *addr, size_t len);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelUsleep(unsigned int usec);
const char *sceKernelGetFsSandboxRandomWord(void);
int sceKernelLoadStartModule(const char *path, size_t args, const void *argp, unsigned flags,
                             void *opt, int *res);
int sceKernelDlsym(int handle, const char *symbol, void **address);
int sceKernelDebugOutText(int channel, const char *text);
int sceKernelCreateEqueue(void **eq, const char *name);
int sceKernelWaitEqueue(void *eq, void *events, int num, int *out, unsigned *timeout_us);

struct ps4_vq_info {
   void *start, *end;
   off_t offset;
   int protection, memory_type;
   unsigned is_flexible : 1, is_direct : 1, is_stack : 1, is_pooled : 1, is_committed : 1;
   char name[32];
};
int sceKernelVirtualQuery(const void *addr, int flags, struct ps4_vq_info *info, size_t size);

#define PS4_PROT_CPU_RW 0x03
#define PS4_PROT_GPU_RW 0x30
#define PS4_PROT_GPU_READ 0x10
#define PS4_MAP_FIXED 0x10
#define PS4_WB_ONION 0
#define PS4_WC_GARLIC 3
#define PS4_PAGE 0x4000ull /* direct memory and mapping granularity */
/* Every buffer gets this much zeroed memory of its own mapped right after it: something wrote
 * past the end of a buffer into the next one (a command buffer) and the system killed the app.
 * Writes past the end now land here, harmlessly, and are reported (ps4_check_tail). */
#define PS4_TAIL 0x10000ull

/* GPU virtual address windows: ranges of the process address space reserved at startup (the
 * kernel picks free ones - fixed addresses collided with the app heap), below the GPU's 40-bit
 * VM limit. The 32-bit window is 4 GiB aligned: address32_hi is its upper half. */
#define PS4_VA_SIZE 0x0400000000ull /* 16 GiB */
#define PS4_VA32_SIZE 0x0100000000ull
#define PS4_VA_HINT 0x4000000000ull /* search from 256 GiB */
#define PS4_GPU_VA_LIMIT 0x10000000000ull

/* Liverpool GB_TILE_MODE0..31 and GB_MACROTILE_MODE0..15, rebuilt from shadPS4's per-mode
 * attributes (8 pipes P8_32x32_16x16, 16 banks), and GB_ADDR_CONFIG: 8 pipes, 256 B pipe
 * interleave, 2 shader engines, 1 KiB DRAM rows. */
static const uint32_t ps4_tile_modes[32] = {
   0x00800310, 0x00800b10, 0x00801310, 0x00801b10, 0x00802310, 0x00800308, 0x00801318,
   0x00802318, 0x00000304, 0x00000308, 0x02000310, 0x02000294, 0x02000318, 0x00400308,
   0x02400310, 0x024002b0, 0x02400294, 0x02400318, 0x0240032c, 0x0100030c, 0x0100031c,
   0x010002b4, 0x010002a4, 0x01000328, 0x010002bc, 0x01000320, 0x010002b8, 0, 0, 0, 0, 0,
};
static const uint32_t ps4_macrotile_modes[16] = {
   0xe8, 0xd4, 0xd0, 0xd0, 0x80, 0x40, 0x00, 0x00, 0xec, 0xe8, 0xd4, 0xd0, 0x80, 0x40, 0x00, 0x00,
};
#define PS4_GB_ADDR_CONFIG 0x00011003
#define PS4_MC_ARB_RAMCFG 0x2 /* 16 banks */

struct amdgpu_bo {
   uint32_t handle;
   uint64_t size;
   off_t phys;
   void *cpu;     /* mapping address (= GPU VA) once mapped */
   bool cpu_owned; /* mapped by bo_cpu_map rather than a VA op */
   uint32_t heap;
   uint64_t flags;
   off_t pad_phys; /* RADEON_FLAG_VM_PAD_1PAGE page, when it lies past the buffer (else -1) */
   uint64_t last_va; /* last GPU address mapped at offset 0 (kept after unmap, for reports) */
   bool tail_mapped;   /* the overflow tail is mapped after the buffer (see PS4_TAIL) */
   bool tail_reported;
   bool gpu_ro; /* mapped GPU read-only (command buffers) */
};

struct amdgpu_va {
   struct ac_drm_device *dev;
   struct util_vma_heap *heap;
   uint64_t addr;
   uint64_t size;
};

/* A sync object: binary (point 0) or timeline. Each point is "signalled" once the submission
 * with sequence number `seq` has finished (seq 0 = signalled by the CPU). */
struct ps4_point {
   uint64_t point;
   uint64_t seq;
};
struct ps4_syncobj {
   bool used;
   struct ps4_point *points;
   unsigned num_points, max_points;
};

struct ac_drm_device {
   struct util_sync_provider p; /* first: the provider callbacks recover the device from it */
   simple_mtx_t lock;

   int (*submit)(uint32_t count, void *dcb[], uint32_t *dcb_sizes, void *ccb[],
                 uint32_t *ccb_sizes);
   int (*submit_done)(void);

   struct util_vma_heap va_heap, va32_heap;
   uint64_t va_start, va32_start;

   /* GPU end-of-pipe interrupts (sceGnmAddEqEvent): fence waits sleep on this queue. */
   int (*add_eq_event)(void *eq, uint64_t id, void *udata);
   int (*submit_and_flip)(uint32_t count, void *dcb[], uint32_t *dcb_sizes, void *ccb[],
                          uint32_t *ccb_sizes, uint32_t video, uint32_t buffer, uint32_t mode,
                          int64_t arg);
   int gnm_module;
   /* ac_ps4_submit_flip: small IBs ending in Gnm's prepareFlip packet, and the submission each
    * last used (a slot is reused once that has completed). */
   uint32_t *flip_ibs;
   uint64_t flip_seq[16];
   unsigned flip_count;
   void *eop_queue;

   /* Statistics for the app's monitor (ac_ps4_get_stats). */
   uint64_t bo_bytes, bo_count;

   /* Handle table. ps4_bo() reads it without the lock (from any thread, e.g. Dolphin's shader
    * compiler threads), so a full table is copied, never reallocated: old tables stay valid. */
   struct amdgpu_bo **bos;
   unsigned num_bos, bos_cap;

   struct ps4_syncobj *syncobjs;
   unsigned num_syncobjs;

   /* Fence page (WB onion, CPU-coherent): [0] = last completed sequence number. Followed by a
    * ring of fence IBs. */
   volatile uint64_t *fence;
   uint32_t *fence_ibs;
   /* GPU busy time (ac_ps4_get_gpu_busy): per sequence slot, the GPU clock when the command
    * processor started the submission (COPY_DATA in a small IB in front of it) and when all its
    * work had finished (bottom-of-pipe timestamp in the fence IB). Busy = the union of those
    * intervals; span = first start to last end. Both in GPU clock ticks, so the ratio needs no
    * clock frequency. */
   volatile uint64_t *ts_start, *ts_end;
   uint32_t *ts_ibs;
   uint64_t ts_accounted, ts_prev_end, ts_busy, ts_span_first, ts_span_last;
   uint64_t last_seq; /* last submitted */

   /* The last submissions' command buffers, for the GPU hang report. */
   struct {
      uint64_t seq;
      unsigned num_ibs;
      uint64_t va[8];
      uint32_t bytes[8];
   } recent[16];
   bool hang_reported;

   /* Unmaps, frees and address-range frees wait here until the GPU has finished every submission
    * made before them: the amdgpu kernel driver keeps a buffer alive while submissions use it,
    * and RADV relies on that (e.g. a queue's scratch or preamble buffers are replaced while the
    * GPU may still use the old ones). Freed at once, the GPU's late writes landed in whatever got
    * the memory next - command buffers. Guarded by `lock`, in order. */
   struct ps4_deferred {
      uint64_t seq;
      char op; /* 'U' unmap, 'F' free, 'V' address range free */
      struct amdgpu_bo *bo;
      uint32_t handle;
      uint64_t addr, size;
      struct amdgpu_va *va;
   } *deferred;
   unsigned num_deferred, deferred_cap;

   /* Recent unmaps and frees (GPU hang report: was a command buffer's memory reused?). */
   struct ps4_event {
      char what;
      uint32_t handle;
      uint64_t va, size, seq;
      off_t phys;
   } events[8192];
   unsigned num_events;
};

#define FENCE_IB_DW 24 /* user fence EOP + timestamp EOP + fence EOP (6 each), padded */
#define TS_IB_DW 8
#define FENCE_IB_COUNT 512

static void ps4_scan_tails(ac_drm_device *dev);
static bool ps4_mapped(const void *p, uint64_t len);
static void ps4_scan_writes(ac_drm_device *dev, uint32_t *ib, unsigned num_dw, uint64_t seq,
                            int depth);
struct ps4_deferred;
static void ps4_defer(ac_drm_device *dev, struct ps4_deferred d);
static void ps4_run_deferred(ac_drm_device *dev);
static int ps4_do_unmap(ac_drm_device *dev, struct amdgpu_bo *b, uint32_t bo_handle, uint64_t addr,
                        uint64_t size);
static void ps4_do_free(ac_drm_device *dev, struct amdgpu_bo *b);
static void ps4_do_va_free(amdgpu_va_handle va);

/* Guards the address-space heaps (va_range_free has no device argument). */
static simple_mtx_t ps4_va_lock = SIMPLE_MTX_INITIALIZER;

static void
ps4_log(const char *fmt, ...)
{
   char line[256];
   va_list args;
   va_start(args, fmt);
   vsnprintf(line, sizeof(line), fmt, args);
   va_end(args);
   fputs(line, stderr);
   sceKernelDebugOutText(0, line);
   /* Also straight to a file: the app may not route stderr anywhere. */
   static int fd = -2;
   if (fd == -2)
      fd = open("/data/DolphinPS4/radv-ps4.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
   if (fd >= 0)
      write(fd, line, strlen(line));
}

#undef fprintf
int
ac_ps4_fprintf(FILE *f, const char *fmt, ...)
{
   char line[512];
   va_list args;
   va_start(args, fmt);
   int n = vsnprintf(line, sizeof(line), fmt, args);
   va_end(args);
   if (f == stderr || f == stdout)
      ps4_log("%s", line);
   else
      fputs(line, f);
   return n;
}

/* ---------------------------------------------------------------------------------------------
 * Fences
 */

static uint64_t
ps4_completed(ac_drm_device *dev)
{
   return __atomic_load_n(dev->fence, __ATOMIC_ACQUIRE);
}

/* Waits until submission `seq` has finished; returns false on timeout. */
/* GPU addresses are CPU addresses: the IB parser can follow chained IBs directly. */
static void
ps4_record(ac_drm_device *dev, char what, struct amdgpu_bo *b, uint64_t va, uint64_t size)
{
   struct ps4_event *e = &dev->events[dev->num_events++ % ARRAY_SIZE(dev->events)];
   e->what = what;
   e->handle = b ? b->handle : 0;
   e->va = va;
   e->size = size;
   e->phys = b ? b->phys : -1;
   e->seq = dev->last_seq;
}

/* Which live buffer maps `va`, and what happened recently around it. */
static void
ps4_describe_va(ac_drm_device *dev, FILE *f, uint64_t va)
{
   fprintf(f, "  address %#llx:\n", (unsigned long long)va);
   for (unsigned h = 1; h < dev->num_bos; h++) {
      struct amdgpu_bo *b = dev->bos[h];
      if (b && b->last_va && va >= b->last_va && va < b->last_va + b->size)
         fprintf(f, "    live bo %u: va %#llx size %#llx phys %#llx heap %#x flags %#llx mapped %s\n",
                 b->handle, (unsigned long long)b->last_va, (unsigned long long)b->size,
                 (unsigned long long)b->phys, b->heap, (unsigned long long)b->flags,
                 b->cpu ? "yes" : "NO");
   }
   for (unsigned i = 0; i < ARRAY_SIZE(dev->events) && i < dev->num_events; i++) {
      const struct ps4_event *e = &dev->events[i];
      if (va >= e->va && va < e->va + e->size)
         fprintf(f, "    event %c bo %u va %#llx size %#llx phys %#llx at submission %llu\n", e->what,
                 e->handle, (unsigned long long)e->va, (unsigned long long)e->size,
                 (unsigned long long)e->phys, (unsigned long long)e->seq);
   }
}

/* Live buffers sharing physical memory (with direct memory aliasing enabled for fastmem, a
 * stale or double allocation wouldn't fail any more - it would silently share memory). */
static void
ps4_check_overlaps(ac_drm_device *dev, FILE *f)
{
   unsigned found = 0;
   for (unsigned a = 1; a < dev->num_bos && found < 20; a++) {
      struct amdgpu_bo *x = dev->bos[a];
      if (!x)
         continue;
      for (unsigned c = a + 1; c < dev->num_bos && found < 20; c++) {
         struct amdgpu_bo *y = dev->bos[c];
         if (y && x->phys < y->phys + (off_t)y->size && y->phys < x->phys + (off_t)x->size) {
            fprintf(f, "  PHYS OVERLAP: bo %u [%#llx+%#llx] and bo %u [%#llx+%#llx]\n", x->handle,
                    (unsigned long long)x->phys, (unsigned long long)x->size, y->handle,
                    (unsigned long long)y->phys, (unsigned long long)y->size);
            found++;
         }
      }
   }
   fprintf(f, "  %u physical overlaps among %u buffer handles\n", found, dev->num_bos);
}

static void
ps4_ib_addr(void *data, uint64_t addr, struct ac_addr_info *info)
{
   ac_drm_device *dev = data;
   memset(info, 0, sizeof(*info));
   const bool in_window = (addr >= dev->va_start && addr < dev->va_start + PS4_VA_SIZE) ||
                          (addr >= dev->va32_start && addr < dev->va32_start + PS4_VA32_SIZE);
   if (in_window) {
      info->cpu_addr = (void *)(uintptr_t)addr;
      info->valid = true;
   }
}


/* Execution check for the hang report: walks the PM4 stream (following IB chains) and, for
 * every WRITE_DATA to memory and every EVENT_WRITE_EOP, tells whether the value is in memory -
 * i.e. whether the GPU got past it. With RADV_DEBUG=hang RADV writes an increasing trace id
 * after each draw, so the last "done" write marks where the GPU stopped. */
static unsigned ps4_walk_lines;

static void
ps4_walk_ib(ac_drm_device *dev, FILE *f, const uint32_t *ib, unsigned num_dw, int depth)
{
   struct ac_addr_info info;
   for (unsigned i = 0; i < num_dw && ps4_walk_lines < 4000;) {
      const uint32_t h = ib[i];
      const unsigned type = h >> 30;
      if (type == 2) {
         i++;
         continue;
      }
      if (type != 3) {
         fprintf(f, "%*s%p: non-type-3 header %#x, stop\n", depth * 2, "", (void *)&ib[i], h);
         ps4_describe_va(dev, f, (uintptr_t)&ib[i]);
         return;
      }
      const unsigned op = (h >> 8) & 0xff;
      const unsigned body = ((h >> 16) & 0x3fff) + 1;
      const uint32_t *b = &ib[i + 1];
      if ((op == 0x3f || op == 0x33) && body >= 3) { /* INDIRECT_BUFFER(_CONST) */
         const uint64_t va = b[0] | ((uint64_t)(b[1] & 0xffff) << 32);
         const unsigned size = b[2] & 0xfffff;
         const bool chain = b[2] & (1u << 20);
         ps4_ib_addr(dev, va, &info);
         fprintf(f, "%*s%p: INDIRECT_BUFFER %#llx, %u dwords%s%s\n", depth * 2, "", (void *)&ib[i],
                 (unsigned long long)va, size, chain ? " (chain)" : "", info.valid ? "" : " INVALID");
         ps4_walk_lines++;
         if (info.valid && depth < 4)
            ps4_walk_ib(dev, f, info.cpu_addr, size, chain ? depth : depth + 1);
         if (chain)
            return;
      } else if (op == 0x37 && body >= 4) { /* WRITE_DATA */
         const unsigned dst_sel = (b[0] >> 8) & 0xf;
         if (dst_sel == 5 || dst_sel == 2 || dst_sel == 1) {
            const uint64_t va = b[1] | ((uint64_t)b[2] << 32);
            ps4_ib_addr(dev, va, &info);
            const uint32_t now = info.valid ? *(volatile uint32_t *)info.cpu_addr : 0xdeadbeef;
            fprintf(f, "%*s%p: WRITE_DATA %#llx <- %#x, memory %#x %s\n", depth * 2, "",
                    (void *)&ib[i], (unsigned long long)va, b[3], now,
                    now == b[3] ? "done" : "NOT DONE");
            ps4_walk_lines++;
         }
      } else if (op == 0x47 && body >= 5) { /* EVENT_WRITE_EOP */
         const uint64_t va = b[1] | ((uint64_t)(b[2] & 0xffff) << 32);
         ps4_ib_addr(dev, va, &info);
         const uint32_t now = info.valid ? *(volatile uint32_t *)info.cpu_addr : 0xdeadbeef;
         fprintf(f, "%*s%p: EVENT_WRITE_EOP %#llx <- %#x, memory %#x %s\n", depth * 2, "",
                 (void *)&ib[i], (unsigned long long)va, b[3], now,
                 now == b[3] ? "done" : "NOT DONE");
         ps4_walk_lines++;
      } else if (op == 0x2d || op == 0x27 || op == 0x15 || op == 0x16) { /* draws, dispatch */
         fprintf(f, "%*s%p: %s\n", depth * 2, "", (void *)&ib[i],
                 op == 0x2d ? "DRAW_INDEX_AUTO" : op == 0x27 ? "DRAW_INDEX_2" : "DISPATCH");
         ps4_walk_lines++;
      }
      i += 1 + body;
   }
}

/* Writes the command buffers of the first submission the GPU hasn't finished, decoded by Mesa's
 * IB parser, to /data/DolphinPS4/gpu-hang.txt (once). */
static void
ps4_report_hang(ac_drm_device *dev, uint64_t waited_seq)
{
   if (dev->hang_reported)
      return;
   dev->hang_reported = true;
   const uint64_t completed = ps4_completed(dev);
   ps4_log("radv/ps4: GPU hang? waiting for %llu, completed %llu, submitted %llu - writing "
           "/data/DolphinPS4/gpu-hang.txt\n",
           (unsigned long long)waited_seq, (unsigned long long)completed,
           (unsigned long long)dev->last_seq);
   FILE *f = fopen("/data/DolphinPS4/gpu-hang.txt", "w");
   if (!f)
      return;
   fprintf(f, "waiting for %llu, GPU completed %llu, submitted %llu\n\n",
           (unsigned long long)waited_seq, (unsigned long long)completed,
           (unsigned long long)dev->last_seq);
   for (unsigned r = 0; r < ARRAY_SIZE(dev->recent); r++) {
      if (dev->recent[r].seq != completed + 1)
         continue;
      fprintf(f, "==== buffers ====\n");
      for (unsigned i = 0; i < dev->recent[r].num_ibs; i++)
         ps4_describe_va(dev, f, dev->recent[r].va[i]);
      ps4_check_overlaps(dev, f);
      simple_mtx_lock(&dev->lock);
      ps4_scan_tails(dev); /* logs to radv-ps4.log */
      simple_mtx_unlock(&dev->lock);
      fprintf(f, "==== execution check (memory writes the GPU did / didn't do) ====\n");
      for (unsigned i = 0; i < dev->recent[r].num_ibs; i++) {
         fprintf(f, "-- IB %u\n", i);
         ps4_walk_ib(dev, f, (const uint32_t *)(uintptr_t)dev->recent[r].va[i],
                     dev->recent[r].bytes[i] / 4, 0);
      }
      fprintf(f, "\n==== decoded command buffers ====\n");
      for (unsigned i = 0; i < dev->recent[r].num_ibs; i++) {
         char name[64];
         snprintf(name, sizeof(name), "submission %llu IB %u (%#llx, %u dwords)",
                  (unsigned long long)dev->recent[r].seq, i,
                  (unsigned long long)dev->recent[r].va[i], dev->recent[r].bytes[i] / 4);
         struct ac_ib_parser ib = {
            .f = f,
            .ib = (uint32_t *)(uintptr_t)dev->recent[r].va[i],
            .num_dw = dev->recent[r].bytes[i] / 4,
            .gfx_level = GFX7,
            .family = CHIP_BONAIRE,
            .ip_type = AMD_IP_GFX,
            .addr_callback = ps4_ib_addr,
            .addr_callback_data = dev,
         };
         ac_parse_ib(&ib, name);
      }
   }
   fclose(f);
   ps4_log("radv/ps4: hang report written\n");
}

/* Waits for the next GPU end-of-pipe interrupt (any submission's fence write), at most 1 ms. */
static void
ps4_sleep(ac_drm_device *dev)
{
   if (dev->eop_queue) {
      uint64_t events[8][4];
      int count = 0;
      unsigned timeout = 1000;
      sceKernelWaitEqueue(dev->eop_queue, events, 8, &count, &timeout);
   } else {
      sceKernelUsleep(50);
   }
}

static bool
ps4_wait_seq(ac_drm_device *dev, uint64_t seq, int64_t abs_timeout_ns)
{
   unsigned spins = 0;
   const int64_t start = os_time_get_nano();
   bool reported = false;
   while (ps4_completed(dev) < seq) {
      const int64_t now = os_time_get_nano();
      if (abs_timeout_ns != INT64_MAX && now >= abs_timeout_ns)
         return false;
      /* A wait this long means the GPU stopped (hang) or the fence write never happened. */
      if (!reported && now - start > 2000000000ll) {
         ps4_report_hang(dev, seq);
         reported = true;
      }
      if (++spins > 64)
         ps4_sleep(dev);
   }
   return true;
}

/* ---------------------------------------------------------------------------------------------
 * Device
 */

static bool
ps4_load_gnm(ac_drm_device *dev)
{
   char path[128];
   const char *word = sceKernelGetFsSandboxRandomWord();
   snprintf(path, sizeof(path), "/%s/common/lib/libSceGnmDriver.sprx", word ? word : "system");
   int module = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
   if (module < 0) {
      ps4_log("radv/ps4: loading %s failed (%#x)\n", path, module);
      return false;
   }
   if (sceKernelDlsym(module, "sceGnmAddEqEvent", (void **)&dev->add_eq_event))
      dev->add_eq_event = NULL;
   if (sceKernelDlsym(module, "sceGnmSubmitAndFlipCommandBuffers", (void **)&dev->submit_and_flip))
      dev->submit_and_flip = NULL;
   if (sceKernelDlsym(module, "sceGnmSubmitCommandBuffers", (void **)&dev->submit) ||
       sceKernelDlsym(module, "sceGnmSubmitDone", (void **)&dev->submit_done)) {
      ps4_log("radv/ps4: libSceGnmDriver symbols missing\n");
      return false;
   }
   dev->gnm_module = module;
   return true;
}

/* ---------------------------------------------------------------------------------------------
 * GPU fault capture. A GPU page/protection fault kills the app without a signal. Hooks found by
 * disassembling the system modules:
 *  - libkernel's libSceCoredump: sceCoredumpRegisterCoredumpHandler(handler, stack, arg) starts
 *    a thread that blocks in mdbg_service(0x14) until the kernel begins a core dump of this
 *    process, then calls `handler` - i.e. it runs on exactly that kill.
 *    sceCoredumpGetStopInfoCpu(buf, 16) / sceCoredumpGetStopInfoGpu(buf, 8) say why.
 *  - libSceGnmDriver maps 512 KiB of system memory named "SceGnmGpuInfo" and registers it with
 *    the kernel (sceKernelSetProcessProperty "Sce.Debug:Gnm"); the GPU driver records GPU state
 *    there (+0 valid, +0x90 protection fault timestamp, ...). Its pointer is GnmDriver data
 *    +0x10508 (firmware 12.00: sceGnmIsCoredumpValid at +0x690 reads it).
 *  - sceKernelAddGpuExceptionEvent(eq, udata): kevent filter -20 on GPU exceptions.
 * Everything is written to /data/DolphinPS4/gpu-fault.txt, with the recent submissions and every
 * live buffer, so a fault address can be mapped to its buffer.
 */
int sceKernelGetModuleList(int *handles, size_t num, size_t *actual);
static const uint8_t *ps4_gpu_info;
static int (*ps4_stop_info_cpu)(void *, size_t);
static int (*ps4_stop_info_gpu)(void *, size_t);
static ac_drm_device *ps4_fault_dev;

static void
ps4_dump_state(FILE *f, ac_drm_device *dev)
{
   uint64_t cpu[2] = {0}, gpu = 0;
   const int rc = ps4_stop_info_cpu ? ps4_stop_info_cpu(cpu, sizeof(cpu)) : -1;
   const int rg = ps4_stop_info_gpu ? ps4_stop_info_gpu(&gpu, sizeof(gpu)) : -1;
   fprintf(f, "stop info cpu %#x: %#llx %#llx; gpu %#x: %#llx\n", rc, (unsigned long long)cpu[0],
           (unsigned long long)cpu[1], rg, (unsigned long long)gpu);
   if (dev) {
      fprintf(f, "GPU completed %llu, submitted %llu\n", (unsigned long long)ps4_completed(dev),
              (unsigned long long)dev->last_seq);
      for (unsigned n = 0; n < ARRAY_SIZE(dev->recent); n++) {
         const unsigned r = (dev->last_seq + 1 + n) % ARRAY_SIZE(dev->recent);
         if (!dev->recent[r].seq)
            continue;
         fprintf(f, "submission %llu:", (unsigned long long)dev->recent[r].seq);
         for (unsigned i = 0; i < dev->recent[r].num_ibs; i++)
            fprintf(f, " %#llx+%u", (unsigned long long)dev->recent[r].va[i], dev->recent[r].bytes[i]);
         fprintf(f, "\n");
      }
      fprintf(f, "live buffers:\n");
      for (unsigned h = 1; h < dev->num_bos; h++) {
         const struct amdgpu_bo *b = dev->bos[h];
         if (b && b->last_va)
            fprintf(f, "  bo %u va %#llx..%#llx phys %#llx heap %#x flags %#llx%s%s\n", b->handle,
                    (unsigned long long)b->last_va, (unsigned long long)(b->last_va + b->size),
                    (unsigned long long)b->phys, b->heap, (unsigned long long)b->flags,
                    b->gpu_ro ? " gpu-read-only" : "", b->cpu ? "" : " (unmapped)");
      }
   }
   if (ps4_gpu_info) {
      fprintf(f, "SceGnmGpuInfo %p (non-zero 32-byte lines):\n", (const void *)ps4_gpu_info);
      unsigned lines = 0;
      for (unsigned off = 0; off < 0x80000 && lines < 3000; off += 32) {
         const uint32_t *w = (const uint32_t *)(ps4_gpu_info + off);
         if (!(w[0] | w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7]))
            continue;
         fprintf(f, "  +%#07x: %08x %08x %08x %08x %08x %08x %08x %08x\n", off, w[0], w[1], w[2],
                 w[3], w[4], w[5], w[6], w[7]);
         lines++;
      }
   }
}

static int
ps4_coredump_handler(void *arg)
{
   (void)arg;
   FILE *f = fopen("/data/DolphinPS4/gpu-fault.txt", "w");
   if (f) {
      fprintf(f, "==== core dump handler (the system is killing the app)\n");
      ps4_dump_state(f, ps4_fault_dev);
      fclose(f);
   }
   return 0;
}

static void *
ps4_gpu_exception_thread(void *eq)
{
   for (;;) {
      uint64_t ev[4][4];
      int n = 0;
      if (sceKernelWaitEqueue(eq, ev, 4, &n, NULL) || n <= 0)
         continue;
      FILE *f = fopen("/data/DolphinPS4/gpu-fault.txt", "a");
      for (int i = 0; i < n; i++) {
         ps4_log("radv/ps4: GPU EXCEPTION event: ident %#llx filter/flags/fflags %#llx data %#llx "
                 "udata %#llx\n", (unsigned long long)ev[i][0], (unsigned long long)ev[i][1],
                 (unsigned long long)ev[i][2], (unsigned long long)ev[i][3]);
         if (f)
            fprintf(f, "==== GPU exception event: ident %#llx filter/flags/fflags %#llx data %#llx\n",
                    (unsigned long long)ev[i][0], (unsigned long long)ev[i][1],
                    (unsigned long long)ev[i][2]);
      }
      if (f) {
         ps4_dump_state(f, ps4_fault_dev);
         fclose(f);
      }
   }
   return NULL;
}

/* A libkernel export not in the link stubs: look it up in every loaded module. */
static void *
ps4_find_symbol(const char *name)
{
   int handles[256];
   size_t count = 0;
   if (sceKernelGetModuleList(handles, ARRAY_SIZE(handles), &count))
      return NULL;
   for (size_t i = 0; i < count && i < ARRAY_SIZE(handles); i++) {
      void *p = NULL;
      if (!sceKernelDlsym(handles[i], name, &p) && p)
         return p;
   }
   return NULL;
}

/* ---------------------------------------------------------------------------------------------
 * Flight recorder (with RADV_PS4_TRACE, ps4.ini radv_trace=on). Nothing reports a GPU fault on a
 * retail PS4 - the app is just gone, and its core dump is encrypted - but data written with
 * write() sits in the kernel's page cache and survives the kill. So:
 *  - /data/DolphinPS4/flight/ib-N.bin: the complete command stream of each submission (every IB,
 *    chained and nested), in a ring of 8 files (N = submission % 8);
 *  - /data/DolphinPS4/flight/progress.log: submissions, the GPU's completed submission, and the
 *    last RADV trace id (written by the CP as it reaches each draw), every millisecond it changes.
 * After a kill, the last trace id locates the draw the GPU died on inside the saved stream.
 */
#include <sys/stat.h>
static volatile const uint32_t *ps4_trace_id;
static int ps4_progress_fd = -1;

void ac_ps4_set_trace_va(uint64_t va);
void
ac_ps4_set_trace_va(uint64_t va)
{
   ps4_trace_id = (volatile const uint32_t *)(uintptr_t)va;
}

static void
ps4_progress(const char *fmt, ...)
{
   if (ps4_progress_fd < 0)
      return;
   char line[256];
   va_list ap;
   va_start(ap, fmt);
   const int n = vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   if (n > 0)
      write(ps4_progress_fd, line, MIN2(n, (int)sizeof(line) - 1));
}

static void *
ps4_progress_thread(void *arg)
{
   (void)arg;
   ac_drm_device *dev;
   uint64_t last_done = ~0ull;
   uint32_t last_trace = ~0u;
   int64_t last_write = 0;
   for (;;) {
      sceKernelUsleep(500);
      dev = ps4_fault_dev; /* the device in use (the last one created) */
      const uint64_t done = ps4_completed(dev);
      const uint32_t trace = ps4_trace_id ? *ps4_trace_id : 0;
      const int64_t now = os_time_get_nano();
      if (done == last_done && trace == last_trace)
         continue;
      if (done == last_done && now - last_write < 2000000)
         continue; /* trace ids alone: at most every 2 ms */
      ps4_progress("%lld.%06lld completed %llu submitted %llu trace %u\n",
                   (long long)(now / 1000000000), (long long)(now / 1000 % 1000000),
                   (unsigned long long)done, (unsigned long long)dev->last_seq, trace);
      last_done = done;
      last_trace = trace;
      last_write = now;
   }
   return NULL;
}

struct ps4_ib_record {
   uint32_t magic; /* 'IBRC' */
   uint32_t depth;
   uint64_t seq;
   uint64_t va;
   uint32_t num_dw;
   uint32_t top_index;
};

static void
ps4_record_ib(int fd, const uint32_t *ib, unsigned num_dw, uint64_t seq, unsigned top, int depth)
{
   for (unsigned link = 0; link < 256 && num_dw; link++) {
      if (!ps4_mapped(ib, num_dw * 4ull))
         return;
      const struct ps4_ib_record r = {0x43524249, depth, seq, (uintptr_t)ib, num_dw, top};
      write(fd, &r, sizeof(r));
      write(fd, ib, num_dw * 4ull);
      const uint32_t *next = NULL;
      unsigned next_dw = 0;
      for (unsigned i = 0; i < num_dw;) {
         const uint32_t h = ib[i];
         if (h >> 30 == 2) {
            i++;
            continue;
         }
         if (h >> 30 != 3)
            break;
         const unsigned op = (h >> 8) & 0xff;
         const unsigned body = ((h >> 16) & 0x3fff) + 1;
         if (i + 1 + body > num_dw)
            break;
         const uint32_t *b = &ib[i + 1];
         if ((op == 0x3f || op == 0x33) && body >= 3) {
            const uint32_t *t = (const uint32_t *)(uintptr_t)(b[0] | ((uint64_t)(b[1] & 0xffff) << 32));
            if (b[2] & (1u << 20)) {
               next = t;
               next_dw = b[2] & 0xfffff;
               break;
            }
            if (depth < 2)
               ps4_record_ib(fd, t, b[2] & 0xfffff, seq, top, depth + 1);
         }
         i += 1 + body;
      }
      if (!next)
         return;
      ib = next;
      num_dw = next_dw;
   }
}

static void
ps4_record_submission(void *const *dcb, const uint32_t *dcb_sizes, unsigned num_ibs, uint64_t seq)
{
   if (ps4_progress_fd < 0)
      return;
   char path[64];
   snprintf(path, sizeof(path), "/data/DolphinPS4/flight/ib-%u.bin", (unsigned)(seq % 8));
   const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
   if (fd < 0)
      return;
   for (unsigned i = 0; i < num_ibs; i++)
      ps4_record_ib(fd, dcb[i], dcb_sizes[i] / 4, seq, i, 0);
   close(fd);
   ps4_progress("submit %llu -> ib-%u.bin\n", (unsigned long long)seq, (unsigned)(seq % 8));
}

static void
ps4_start_flight_recorder(ac_drm_device *dev)
{
   if (!getenv("RADV_PS4_TRACE") || ps4_progress_fd >= 0)
      return;
   mkdir("/data/DolphinPS4/flight", 0777);
   ps4_progress_fd = open("/data/DolphinPS4/flight/progress.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
   if (ps4_progress_fd < 0)
      return;
   pthread_t t;
   pthread_create(&t, NULL, ps4_progress_thread, dev);
   ps4_log("radv/ps4: flight recorder on (/data/DolphinPS4/flight)\n");
}

static void
ps4_install_fault_capture(ac_drm_device *dev, int gnm_module)
{
   ps4_fault_dev = dev;
   void *valid = NULL;
   if (!sceKernelDlsym(gnm_module, "sceGnmIsCoredumpValid", &valid) && valid) {
      const uint8_t *const *slot = (const uint8_t *const *)((uintptr_t)valid - 0x690 + 0x10508);
      struct ps4_vq_info vq;
      memset(&vq, 0, sizeof(vq));
      if (!sceKernelVirtualQuery(slot, 0, &vq, sizeof(vq)) && *slot &&
          !sceKernelVirtualQuery(*slot, 0, &vq, sizeof(vq)) && !strncmp(vq.name, "SceGnmGpuInfo", 13))
         ps4_gpu_info = *slot;
      ps4_log("radv/ps4: SceGnmGpuInfo area %p ('%.32s')\n", (const void *)ps4_gpu_info, vq.name);
   }
   ps4_stop_info_cpu = ps4_find_symbol("sceCoredumpGetStopInfoCpu");
   ps4_stop_info_gpu = ps4_find_symbol("sceCoredumpGetStopInfoGpu");
   int (*reg)(int (*)(void *), size_t, void *) = ps4_find_symbol("sceCoredumpRegisterCoredumpHandler");
   const int rr = reg ? reg(ps4_coredump_handler, 0x10000, NULL) : -1;
   int (*add_exc)(void *, void *) = ps4_find_symbol("sceKernelAddGpuExceptionEvent");
   void *eq = NULL;
   int re = -1;
   if (add_exc && !sceKernelCreateEqueue(&eq, "radv gpu exception")) {
      re = add_exc(eq, NULL);
      if (!re) {
         pthread_t t;
         pthread_create(&t, NULL, ps4_gpu_exception_thread, eq);
      }
   }
   ps4_log("radv/ps4: fault capture: core dump handler %#x, GPU exception event %#x, stop info %s/%s\n",
           rr, re, ps4_stop_info_cpu ? "cpu" : "-", ps4_stop_info_gpu ? "gpu" : "-");
}

static void *
ps4_map_new(size_t size, int type, int prot, off_t *phys_out)
{
   off_t phys;
   size = align64(size, PS4_PAGE);
   if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, PS4_PAGE, type,
                                     &phys))
      return NULL;
   void *addr = NULL;
   if (sceKernelMapDirectMemory(&addr, size, prot, 0, phys, PS4_PAGE)) {
      sceKernelReleaseDirectMemory(phys, size);
      return NULL;
   }
   if (phys_out)
      *phys_out = phys;
   return addr;
}

/* Reserves a free range of the address space (kernel's choice, `align` aligned). */
static uint64_t
ps4_reserve_window(uint64_t size, uint64_t align)
{
   void *a = (void *)(uintptr_t)PS4_VA_HINT;
   int r = sceKernelReserveVirtualRange(&a, size, 0, align);
   if (r || !a || (uint64_t)(uintptr_t)a + size > PS4_GPU_VA_LIMIT) {
      ps4_log("radv/ps4: reserving %#llx bytes failed (%#x, %p)\n", (unsigned long long)size, r, a);
      return 0;
   }
   return (uint64_t)(uintptr_t)a;
}

static bool
ps4_reserve(uint64_t addr, uint64_t size)
{
   void *a = (void *)(uintptr_t)addr;
   return sceKernelReserveVirtualRange(&a, size, PS4_MAP_FIXED, 0) == 0 && a == (void *)(uintptr_t)addr;
}

/* Something other than our reservation inside [addr, addr + size) of a GPU VA window? (Then a
 * MAP_FIXED would silently replace someone else's memory, and their writes would land in ours.)
 * Logs the first few, returns true if found. */
static unsigned ps4_foreign_logged;
static bool ps4_vq_ok;
static bool
ps4_check_va_free(uint64_t addr, uint64_t size, const char *what)
{
   if (!ps4_vq_ok)
      return false;
   struct ps4_vq_info info;
   memset(&info, 0, sizeof(info));
   /* flags 1 = find next: the first mapping at or after addr */
   if (sceKernelVirtualQuery((void *)(uintptr_t)addr, 1, &info, sizeof(info)))
      return false;
   const uint64_t start = (uintptr_t)info.start, end = (uintptr_t)info.end;
   if (start >= addr + size || end <= addr)
      return false;
   if (!info.is_direct && !info.is_flexible && !info.is_committed && !info.is_stack)
      return false; /* a reservation */
   if (ps4_foreign_logged < 32) {
      ps4_foreign_logged++;
      ps4_log("radv/ps4: %s %#llx+%#llx: address range already holds a mapping [%#llx, %#llx) "
              "offset %#llx prot %#x type %d direct %u flexible %u stack %u committed %u '%.32s'\n",
              what, (unsigned long long)addr, (unsigned long long)size, (unsigned long long)start,
              (unsigned long long)end, (unsigned long long)info.offset, info.protection,
              info.memory_type, info.is_direct, info.is_flexible, info.is_stack, info.is_committed,
              info.name);
   }
   return true;
}

static struct util_sync_provider *ps4_sync_provider_init(ac_drm_device *dev);
static ac_drm_device *ps4_last_device;

/* Logs the first memory operations (bring-up). */
static unsigned ps4_verbose_left = 64;
#define PS4_VERBOSE(...) do { if (ps4_verbose_left) { ps4_verbose_left--; ps4_log(__VA_ARGS__); } } while (0)

int
ac_drm_device_initialize(int fd, bool is_virtio, uint32_t *major_version, uint32_t *minor_version,
                         ac_drm_device **out)
{
   (void)fd;
   (void)is_virtio;
   ac_drm_device *dev = calloc(1, sizeof(*dev));
   if (!dev)
      return -ENOMEM;
   simple_mtx_init(&dev->lock, mtx_plain);

   if (!ps4_load_gnm(dev))
      goto fail;
   dev->va_start = ps4_reserve_window(PS4_VA_SIZE, 0x200000);
   /* The kernel rejects a 4 GiB alignment (EINVAL): reserve twice the size and use the 4 GiB
    * aligned half inside it (the rest stays reserved, unused). */
   const uint64_t va32_range = ps4_reserve_window(PS4_VA32_SIZE * 2, 0x200000);
   dev->va32_start = va32_range ? align64(va32_range, PS4_VA32_SIZE) : 0;
   if (!dev->va_start || !dev->va32_start)
      goto fail;
   ps4_log("radv/ps4: GPU address windows %#llx (16 GiB), %#llx (32-bit)\n",
           (unsigned long long)dev->va_start, (unsigned long long)dev->va32_start);
   util_vma_heap_init(&dev->va_heap, dev->va_start, PS4_VA_SIZE);
   util_vma_heap_init(&dev->va32_heap, dev->va32_start, PS4_VA32_SIZE);

   off_t page_phys = -1;
   uint8_t *page =
      ps4_map_new(PS4_PAGE * 4, PS4_WB_ONION, PS4_PROT_CPU_RW | PS4_PROT_GPU_RW, &page_phys);
   if (!page)
      goto fail;
   {
      /* sceKernelVirtualQuery's struct is declared by hand: only trust it if it describes this
       * known mapping correctly. */
      struct ps4_vq_info vq;
      memset(&vq, 0, sizeof(vq));
      const int r = sceKernelVirtualQuery(page + 64, 0, &vq, sizeof(vq));
      ps4_vq_ok = r == 0 && vq.start == page && vq.end == page + PS4_PAGE * 4 && vq.is_direct &&
                  vq.offset == page_phys;
      ps4_log("radv/ps4: virtual query check %s (%#x: [%p, %p) offset %#llx direct %u, expected "
              "[%p, +%#llx) offset %#llx)\n", ps4_vq_ok ? "ok" : "FAILED - mapping checks off", r,
              vq.start, vq.end, (unsigned long long)vq.offset, vq.is_direct, page,
              (unsigned long long)(PS4_PAGE * 4), (unsigned long long)page_phys);
   }
   memset(page, 0, PS4_PAGE * 4);
   dev->fence = (volatile uint64_t *)page;
   if (dev->add_eq_event) {
      void *eq = NULL;
      int r = sceKernelCreateEqueue(&eq, "radv eop");
      int a = r == 0 ? dev->add_eq_event(eq, 0x40 /* GFX EOP */, NULL) : -1;
      ps4_log("radv/ps4: EOP event queue %#x, sceGnmAddEqEvent %#x\n", r, a);
      if (r == 0 && a == 0)
         dev->eop_queue = eq;
   }
   dev->fence_ibs = (uint32_t *)(page + 256);
   dev->flip_ibs = ps4_map_new(PS4_PAGE, PS4_WB_ONION, PS4_PROT_CPU_RW | PS4_PROT_GPU_RW, NULL);
   {
      /* Timestamps: [0, 4 KiB) start, [4, 8 KiB) end, then the start IBs (32 bytes each). */
      STATIC_ASSERT(FENCE_IB_COUNT * 8 * 2 + FENCE_IB_COUNT * TS_IB_DW * 4 <= PS4_PAGE * 2);
      uint8_t *ts = ps4_map_new(PS4_PAGE * 2, PS4_WB_ONION, PS4_PROT_CPU_RW | PS4_PROT_GPU_RW, NULL);
      if (ts) {
         memset(ts, 0, PS4_PAGE * 2);
         dev->ts_start = (volatile uint64_t *)ts;
         dev->ts_end = (volatile uint64_t *)(ts + FENCE_IB_COUNT * 8);
         dev->ts_ibs = (uint32_t *)(ts + FENCE_IB_COUNT * 16);
      }
   }
   STATIC_ASSERT(256 + FENCE_IB_COUNT * FENCE_IB_DW * 4 <= PS4_PAGE * 4);

   ps4_sync_provider_init(dev);

   /* Report a recent amdgpu kernel (3.61) so that Mesa enables its normal paths. */
   *major_version = 3;
   *minor_version = 61;
   *out = dev;
   ps4_last_device = dev;
   if (!ps4_fault_dev)
      ps4_install_fault_capture(dev, dev->gnm_module);
   ps4_fault_dev = dev; /* the last device created is the one in use */
   ps4_start_flight_recorder(dev);
   ps4_log("radv/ps4: device initialized\n");
   return 0;

fail:
   free(dev);
   return -ENODEV;
}

struct util_sync_provider *
ac_drm_device_get_sync_provider(ac_drm_device *dev)
{
   return &dev->p;
}

uintptr_t
ac_drm_device_get_cookie(ac_drm_device *dev)
{
   return (uintptr_t)dev;
}

void
ac_drm_device_deinitialize(ac_drm_device *dev)
{
   /* The device lives for the whole process. */
   (void)dev;
}

int
ac_drm_device_get_fd(ac_drm_device *dev)
{
   (void)dev;
   return -1;
}

/* ---------------------------------------------------------------------------------------------
 * Buffers and address space
 */

static struct amdgpu_bo *
ps4_bo(ac_drm_device *dev, uint32_t handle)
{
   const unsigned num = __atomic_load_n(&dev->num_bos, __ATOMIC_ACQUIRE);
   struct amdgpu_bo **bos = __atomic_load_n(&dev->bos, __ATOMIC_ACQUIRE);
   return handle && handle < num ? __atomic_load_n(&bos[handle], __ATOMIC_ACQUIRE) : NULL;
}

int
ac_drm_bo_alloc(ac_drm_device *dev, struct amdgpu_bo_alloc_request *req, ac_drm_bo *out)
{
   struct amdgpu_bo *bo = calloc(1, sizeof(*bo));
   if (!bo)
      return -ENOMEM;
   bo->size = align64(MAX2(req->alloc_size, 1), PS4_PAGE);
   bo->heap = req->preferred_heap;
   bo->flags = req->flags;
   bo->pad_phys = -1;

   /* VRAM: write-combined garlic (the GPU's fast path); GTT: CPU-cached onion, coherent. */
   const int type = (req->preferred_heap & AMDGPU_GEM_DOMAIN_VRAM) ||
                          (req->flags & AMDGPU_GEM_CREATE_CPU_GTT_USWC)
                       ? PS4_WC_GARLIC
                       : PS4_WB_ONION;
   const uint64_t align = MAX2(util_next_power_of_two64(MAX2(req->phys_alignment, 1)), PS4_PAGE);
   simple_mtx_lock(&dev->lock);
   ps4_run_deferred(dev);
   simple_mtx_unlock(&dev->lock);
   int r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bo->size + PS4_TAIL,
                                         align, type, &bo->phys);
   if (r && dev->num_deferred) {
      /* Memory still waiting for the GPU: let it finish, then try again. */
      ps4_wait_seq(dev, dev->last_seq, INT64_MAX);
      simple_mtx_lock(&dev->lock);
      ps4_run_deferred(dev);
      simple_mtx_unlock(&dev->lock);
      r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bo->size + PS4_TAIL,
                                        align, type, &bo->phys);
   }
   if (r) {
      ps4_log("radv/ps4: allocating %llu bytes of direct memory failed (%#x)\n",
              (unsigned long long)bo->size, r);
      free(bo);
      return -ENOMEM;
   }

   simple_mtx_lock(&dev->lock);
   if (dev->num_bos + 1 >= dev->bos_cap) {
      const unsigned cap = MAX2(dev->bos_cap * 2, 1024);
      struct amdgpu_bo **bigger = calloc(cap, sizeof(*bigger));
      if (dev->num_bos)
         memcpy(bigger, dev->bos, dev->num_bos * sizeof(*bigger));
      /* The old table is deliberately kept (see struct ac_drm_device). */
      __atomic_store_n(&dev->bos, bigger, __ATOMIC_RELEASE);
      dev->bos_cap = cap;
   }
   if (dev->num_bos == 0)
      dev->num_bos = 1; /* handle 0 is invalid */
   bo->handle = dev->num_bos;
   dev->bos[bo->handle] = bo;
   __atomic_store_n(&dev->num_bos, bo->handle + 1, __ATOMIC_RELEASE);
   simple_mtx_unlock(&dev->lock);

   out->abo = bo;
   __atomic_add_fetch(&dev->bo_bytes, bo->size, __ATOMIC_RELAXED);
   __atomic_add_fetch(&dev->bo_count, 1, __ATOMIC_RELAXED);
   PS4_VERBOSE("radv/ps4: bo %u: %llu bytes, heap %#x, flags %#llx, align %#llx, phys %#llx\n",
               bo->handle, (unsigned long long)bo->size, bo->heap, (unsigned long long)bo->flags,
               (unsigned long long)align, (unsigned long long)bo->phys);
   return 0;
}

int
ac_drm_bo_free(ac_drm_device *dev, ac_drm_bo bo)
{
   struct amdgpu_bo *b = bo.abo;
   if (!b)
      return 0;
   ps4_defer(dev, (struct ps4_deferred){.op = 'F', .bo = b});
   return 0;
}

/* A deferred free (dev->lock held). */
static void
ps4_do_free(ac_drm_device *dev, struct amdgpu_bo *b)
{
   if (b->cpu_owned)
      sceKernelMunmap(b->cpu, b->size);
   const int rr = sceKernelReleaseDirectMemory(b->phys, b->size + PS4_TAIL);
   if (rr) {
      static unsigned logged;
      if (logged++ < 16)
         ps4_log("radv/ps4: releasing bo %u (phys %#llx, %#llx bytes) failed (%#x)\n", b->handle,
                 (unsigned long long)b->phys, (unsigned long long)b->size, rr);
   }
   __atomic_sub_fetch(&dev->bo_bytes, b->size, __ATOMIC_RELAXED);
   __atomic_sub_fetch(&dev->bo_count, 1, __ATOMIC_RELAXED);
   if (b->pad_phys >= 0)
      sceKernelReleaseDirectMemory(b->pad_phys, PS4_PAGE);
   ps4_record(dev, 'F', b, b->last_va, b->size);
   dev->bos[b->handle] = NULL;
   free(b);
}

int
ac_drm_bo_export(ac_drm_device *dev, ac_drm_bo bo, enum amdgpu_bo_handle_type type,
                 uint32_t *shared_handle)
{
   (void)dev;
   if (type != amdgpu_bo_handle_type_kms && type != amdgpu_bo_handle_type_kms_noimport)
      return -ENOSYS;
   *shared_handle = bo.abo->handle;
   return 0;
}

int
ac_drm_bo_import(ac_drm_device *dev, enum amdgpu_bo_handle_type type, uint32_t shared_handle,
                 struct ac_drm_bo_import_result *output)
{
   return -ENOSYS;
}

int
ac_drm_create_bo_from_user_mem(ac_drm_device *dev, void *cpu, uint64_t size, ac_drm_bo *bo)
{
   /* GPU addresses are CPU addresses here, but user memory can't be mapped a second time at the
    * address the winsys picks. */
   return -ENOSYS;
}

int
ac_drm_bo_cpu_map(ac_drm_device *dev, ac_drm_bo bo, void **cpu)
{
   struct amdgpu_bo *b = bo.abo;
   if (!b->cpu) {
      void *addr = NULL;
      if (sceKernelMapDirectMemory(&addr, b->size, PS4_PROT_CPU_RW | PS4_PROT_GPU_RW, 0, b->phys,
                                   PS4_PAGE)) {
         ps4_log("radv/ps4: cpu map of bo %u failed\n", b->handle);
         return -ENOMEM;
      }
      b->cpu = addr;
      b->cpu_owned = true;
   }
   *cpu = b->cpu;
   return 0;
}

int
ac_drm_bo_cpu_unmap(ac_drm_device *dev, ac_drm_bo bo)
{
   return 0;
}

int
ac_drm_bo_set_metadata(ac_drm_device *dev, uint32_t bo_handle, struct amdgpu_bo_metadata *info)
{
   return 0;
}

int
ac_drm_bo_query_info(ac_drm_device *dev, uint32_t bo_handle, struct amdgpu_bo_info *info)
{
   struct amdgpu_bo *b = ps4_bo(dev, bo_handle);
   if (!b)
      return -EINVAL;
   memset(info, 0, sizeof(*info));
   info->alloc_size = b->size;
   info->phys_alignment = PS4_PAGE;
   info->preferred_heap = b->heap;
   info->alloc_flags = b->flags;
   return 0;
}

int
ac_drm_bo_wait_for_idle(ac_drm_device *dev, ac_drm_bo bo, uint64_t timeout_ns, bool *busy)
{
   /* No per-buffer tracking: wait for everything submitted so far. */
   *busy = !ps4_wait_seq(dev, dev->last_seq, os_time_get_absolute_timeout(timeout_ns));
   return 0;
}

/* Was anything written past the end of the buffer? `full`: look at the whole tail, else at the
 * first 64 bytes of each 4 KiB of it. Logs a buffer once. Called with dev->lock held. */
static void
ps4_check_tail(ac_drm_device *dev, struct amdgpu_bo *b, const char *when)
{
   if (!b->tail_mapped || b->tail_reported)
      return;
   const volatile uint32_t *t = (const volatile uint32_t *)(uintptr_t)(b->last_va + b->size);
   bool dirty = false;
   for (unsigned page = 0; page < PS4_TAIL / 4096 && !dirty; page++)
      for (unsigned i = 0; i < 16 && !dirty; i++)
         dirty = t[page * 1024 + i] != 0;
   if (!dirty)
      return;
   b->tail_reported = true;
   unsigned count = 0, first = ~0u, last = 0;
   char sample[512];
   int len = 0;
   for (unsigned i = 0; i < PS4_TAIL / 4; i++) {
      const uint32_t v = t[i];
      if (!v)
         continue;
      if (count < 24 && len < (int)sizeof(sample) - 24)
         len += snprintf(sample + len, sizeof(sample) - len, " +%#x:%08x", i * 4, v);
      count++;
      first = MIN2(first, i * 4);
      last = i * 4;
   }
   ps4_log("radv/ps4: WRITE PAST THE END of bo %u (%s): va %#llx size %#llx heap %#x flags %#llx "
           "phys %#llx, after submission %llu (GPU completed %llu): %u dwords written at "
           "+%#x..+%#x past the end:%s\n",
           b->handle, when, (unsigned long long)b->last_va, (unsigned long long)b->size, b->heap,
           (unsigned long long)b->flags, (unsigned long long)b->phys,
           (unsigned long long)dev->last_seq, (unsigned long long)ps4_completed(dev), count, first,
           last, sample);
}

/* All live buffers' tails (every 256 submissions; dev->lock held). */
static void
ps4_scan_tails(ac_drm_device *dev)
{
   for (unsigned h = 1; h < dev->num_bos; h++) {
      struct amdgpu_bo *b = dev->bos[h];
      if (b)
         ps4_check_tail(dev, b, "live");
   }
}

static int
ps4_va_op(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr,
          uint64_t flags, uint32_t ops)
{
   const uint64_t exact_size = size;
   size = align64(size, PS4_PAGE);
   if (flags & AMDGPU_VM_PAGE_PRT) {
      ps4_log("radv/ps4: sparse (PRT) mapping requested - unsupported\n");
      return -ENOSYS; /* no sparse residency */
   }

   switch (ops) {
   case AMDGPU_VA_OP_MAP: {
      struct amdgpu_bo *b = ps4_bo(dev, bo_handle);
      if (!b || offset + size > b->size) {
         ps4_log("radv/ps4: map of bo %u (+%#llx, %#llx bytes) out of range\n", bo_handle,
                 (unsigned long long)offset, (unsigned long long)size);
         return -EINVAL;
      }
      void *a = (void *)(uintptr_t)addr;
      const uint64_t cpu = (uintptr_t)b->cpu;
      if (b->cpu && addr != cpu) {
         /* A second mapping of the same memory: RADEON_FLAG_VM_PAD_1PAGE maps the first page
          * again after the buffer, so that prefetches past the end hit valid memory. The PS4
          * can't map direct memory twice (EBUSY). Inside the buffer's own (16 KiB rounded)
          * mapping it's already backed; past it, back it with a page of its own. */
         /* Starting inside the buffer's mapping: the pad page RADV wants (4 KiB) is covered;
          * the length has been rounded to the 16 KiB system page by then (getpagesize). */
         (void)exact_size;
         if (addr >= cpu && addr < cpu + b->size + (b->tail_mapped ? PS4_TAIL : 0))
            return 0;
         if (addr != cpu + b->size || size != PS4_PAGE || b->pad_phys >= 0) {
            ps4_log("radv/ps4: unsupported alias mapping of bo %u at %#llx (+%#llx, %#llx bytes)\n",
                    b->handle, (unsigned long long)addr, (unsigned long long)offset,
                    (unsigned long long)size);
            return -ENOSYS;
         }
         off_t pad;
         if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), PS4_PAGE, PS4_PAGE,
                                           PS4_WB_ONION, &pad))
            return -ENOMEM;
         if (sceKernelMapDirectMemory(&a, PS4_PAGE, PS4_PROT_CPU_RW | PS4_PROT_GPU_RW,
                                      PS4_MAP_FIXED, pad, PS4_PAGE) ||
             a != (void *)(uintptr_t)addr) {
            sceKernelReleaseDirectMemory(pad, PS4_PAGE);
            return -ENOMEM;
         }
         b->pad_phys = pad;
         return 0;
      }
      /* The whole buffer: map its overflow tail too. */
      const bool whole = offset == 0 && size == b->size && !b->cpu;
      const uint64_t map_size = whole ? size + PS4_TAIL : size;
      ps4_check_va_free(addr, map_size, "map");
      /* GPU read-only when RADV asks for it (command buffers), as amdgpu enforces on Linux: there
       * a stray GPU write into one is dropped, here it would land. */
      const bool gpu_ro = flags && !(flags & AMDGPU_VM_PAGE_WRITEABLE);
      if (gpu_ro) {
         static unsigned logged;
         if (logged++ < 3)
            ps4_log("radv/ps4: bo %u mapped GPU read-only at %#llx\n", b->handle,
                    (unsigned long long)addr);
      }
      int r = sceKernelMapDirectMemory(&a, map_size,
                                       PS4_PROT_CPU_RW | (gpu_ro ? PS4_PROT_GPU_READ : PS4_PROT_GPU_RW),
                                       PS4_MAP_FIXED, b->phys + offset, PS4_PAGE);
      if (!r && whole && a == (void *)(uintptr_t)addr) {
         memset((uint8_t *)a + size, 0, PS4_TAIL);
         b->tail_mapped = true;
      }
      if (r || a != (void *)(uintptr_t)addr) {
         ps4_log("radv/ps4: mapping %#llx+%#llx at %#llx failed (%#x)\n",
                 (unsigned long long)b->phys, (unsigned long long)offset,
                 (unsigned long long)addr, r);
         return -ENOMEM;
      }
      if (offset == 0 && !b->cpu)
         b->cpu = a;
      if (offset == 0) {
         b->last_va = addr;
         b->gpu_ro = gpu_ro;
      }
      ps4_record(dev, 'M', b, addr, size);
      PS4_VERBOSE("radv/ps4: map bo %u +%#llx at %#llx (%#llx bytes)\n", b->handle,
                  (unsigned long long)offset, (unsigned long long)addr, (unsigned long long)size);
      return 0;
   }
   case AMDGPU_VA_OP_UNMAP:
   case AMDGPU_VA_OP_CLEAR: {
      ps4_defer(dev, (struct ps4_deferred){.op = 'U', .bo = ps4_bo(dev, bo_handle),
                                           .handle = bo_handle, .addr = addr, .size = size});
      return 0;
   }
   default:
      ps4_log("radv/ps4: va op %u unsupported\n", ops);
      return -ENOSYS;
   }
}

/* A deferred unmap (dev->lock held). */
static int
ps4_do_unmap(ac_drm_device *dev, struct amdgpu_bo *b, uint32_t bo_handle, uint64_t addr,
             uint64_t size)
{
   {
      if (b && b->tail_mapped && addr == b->last_va) {
         ps4_check_tail(dev, b, "unmapped");
         b->tail_mapped = false;
         size = MAX2(size, b->size + PS4_TAIL);
      }
      if (b && b->cpu == (void *)(uintptr_t)addr && !b->cpu_owned)
         b->cpu = NULL;
      ps4_record(dev, 'U', b, addr, size);
      /* Unmap explicitly, then put the reservation back so the window stays ours. (Reserving
       * over a live mapping isn't guaranteed to replace it; a mapping left behind would vanish
       * when its direct memory is released, leaving a hole the kernel hands to other mappings
       * - which a later buffer at this address would then silently overwrite.) */
      const int ru = sceKernelMunmap((void *)(uintptr_t)addr, size);
      if (!ps4_reserve(addr, size)) {
         static unsigned logged;
         if (logged++ < 16)
            ps4_log("radv/ps4: unmap of bo %u at %#llx (%#llx bytes): munmap %#x, re-reserving "
                    "failed\n", bo_handle, (unsigned long long)addr, (unsigned long long)size, ru);
         return -EINVAL;
      }
      return 0;
   }
}

int
ac_drm_bo_va_op(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size,
                uint64_t addr, uint64_t flags, uint32_t ops)
{
   return ps4_va_op(dev, bo_handle, offset, size, addr, 0, ops);
}

int
ac_drm_bo_va_op_raw(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size,
                    uint64_t addr, uint64_t flags, uint32_t ops)
{
   return ps4_va_op(dev, bo_handle, offset, size, addr, flags, ops);
}

static int ps4_timeline_signal(struct util_sync_provider *p, const uint32_t *handles,
                               uint64_t *points, uint32_t count);

int
ac_drm_bo_va_op_raw2(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size,
                     uint64_t addr, uint64_t flags, uint32_t ops, uint32_t vm_timeline_syncobj_out,
                     uint64_t vm_timeline_point, uint64_t input_fence_syncobj_handles,
                     uint32_t num_syncobj_handles)
{
   int r = ps4_va_op(dev, bo_handle, offset, size, addr, flags, ops);
   /* Mapping is synchronous: signal the VM timeline point right away. */
   if (!r && vm_timeline_syncobj_out)
      ps4_timeline_signal(&dev->p, &vm_timeline_syncobj_out, &vm_timeline_point, 1);
   return r;
}

int
ac_drm_va_range_alloc(ac_drm_device *dev, enum amdgpu_gpu_va_range va_range_type, uint64_t size,
                      uint64_t va_base_alignment, uint64_t va_base_required,
                      uint64_t *va_base_allocated, amdgpu_va_handle *va_range_handle,
                      uint64_t flags)
{
   struct util_vma_heap *heap = flags & AMDGPU_VA_RANGE_32_BIT ? &dev->va32_heap : &dev->va_heap;
   size = align64(size, PS4_PAGE) + PS4_TAIL;
   const uint64_t align = MAX2(util_next_power_of_two64(MAX2(va_base_alignment, 1)), PS4_PAGE);

   simple_mtx_lock(&ps4_va_lock);
   uint64_t addr = 0;
   if (va_base_required) {
      if (util_vma_heap_alloc_addr(heap, va_base_required, size))
         addr = va_base_required;
   } else {
      addr = util_vma_heap_alloc(heap, size, align);
   }
   simple_mtx_unlock(&ps4_va_lock);
   if (!addr) {
      ps4_log("radv/ps4: va_range_alloc(%#llx, align %#llx, required %#llx, flags %#llx) failed\n",
              (unsigned long long)size, (unsigned long long)align,
              (unsigned long long)va_base_required, (unsigned long long)flags);
      return -ENOMEM;
   }
   PS4_VERBOSE("radv/ps4: va %#llx + %#llx (flags %#llx)\n", (unsigned long long)addr,
               (unsigned long long)size, (unsigned long long)flags);

   struct amdgpu_va *va = malloc(sizeof(*va));
   va->dev = dev;
   va->heap = heap;
   va->addr = addr;
   va->size = size;
   *va_base_allocated = addr;
   *va_range_handle = va;
   return 0;
}

int
ac_drm_va_range_free(amdgpu_va_handle va)
{
   if (!va)
      return 0;
   ps4_defer(va->dev, (struct ps4_deferred){.op = 'V', .va = va});
   return 0;
}

/* A deferred address range free. */
static void
ps4_do_va_free(amdgpu_va_handle va)
{
   simple_mtx_lock(&ps4_va_lock);
   util_vma_heap_free(va->heap, va->addr, va->size);
   simple_mtx_unlock(&ps4_va_lock);
   free(va);
}

/* Runs the deferred operations whose submissions have completed, in order. dev->lock held. */
static void
ps4_run_deferred(ac_drm_device *dev)
{
   const uint64_t done = ps4_completed(dev);
   unsigned n = 0;
   for (; n < dev->num_deferred && dev->deferred[n].seq <= done; n++) {
      struct ps4_deferred *d = &dev->deferred[n];
      switch (d->op) {
      case 'U':
         ps4_do_unmap(dev, d->bo, d->handle, d->addr, d->size);
         break;
      case 'F':
         ps4_do_free(dev, d->bo);
         break;
      case 'V':
         ps4_do_va_free(d->va);
         break;
      }
   }
   if (n) {
      dev->num_deferred -= n;
      memmove(dev->deferred, dev->deferred + n, dev->num_deferred * sizeof(*dev->deferred));
   }
}

static void
ps4_defer(ac_drm_device *dev, struct ps4_deferred d)
{
   simple_mtx_lock(&dev->lock);
   d.seq = dev->last_seq;
   if (dev->num_deferred == dev->deferred_cap) {
      dev->deferred_cap = MAX2(dev->deferred_cap * 2, 256);
      dev->deferred = realloc(dev->deferred, dev->deferred_cap * sizeof(*dev->deferred));
   }
   dev->deferred[dev->num_deferred++] = d;
   ps4_run_deferred(dev);
   simple_mtx_unlock(&dev->lock);
}

int
ac_drm_va_range_query(ac_drm_device *dev, enum amdgpu_gpu_va_range type, uint64_t *start,
                      uint64_t *end)
{
   *start = dev->va_start;
   *end = dev->va_start + PS4_VA_SIZE;
   return 0;
}

/* ---------------------------------------------------------------------------------------------
 * Contexts and submission
 */

int
ac_drm_cs_ctx_create2(ac_drm_device *dev, uint32_t priority, uint32_t *ctx_id)
{
   *ctx_id = 1;
   return 0;
}

int
ac_drm_cs_ctx_free(ac_drm_device *dev, uint32_t ctx_id)
{
   return 0;
}

int
ac_drm_cs_ctx_stable_pstate(ac_drm_device *dev, uint32_t ctx_id, uint32_t op, uint32_t flags,
                            uint32_t *out_flags)
{
   if (out_flags)
      *out_flags = 0;
   return 0;
}

int
ac_drm_cs_query_reset_state2(ac_drm_device *dev, uint32_t ctx_id, uint64_t *flags)
{
   *flags = 0;
   return 0;
}

int
ac_drm_cs_query_fence_status(ac_drm_device *dev, uint32_t ctx_id, uint32_t ip_type,
                             uint32_t ip_instance, uint32_t ring, uint64_t fence_seq_no,
                             uint64_t timeout_ns, uint64_t flags, uint32_t *expired)
{
   int64_t abs = flags & AMDGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE
                    ? (int64_t)timeout_ns
                    : os_time_get_absolute_timeout(timeout_ns);
   *expired = ps4_wait_seq(dev, fence_seq_no, abs);
   /* RADV_DEBUG=hang checks each submission with a 1 s wait: report before it gives up. */
   if (!*expired && timeout_ns >= 500000000ull)
      ps4_report_hang(dev, fence_seq_no);
   return 0;
}

void
ac_drm_cs_chunk_fence_info_to_data(uint32_t bo_handle, uint64_t offset,
                                   struct drm_amdgpu_cs_chunk_data *data)
{
   data->fence_data.handle = bo_handle;
   data->fence_data.offset = offset * sizeof(uint64_t);
}

static struct ps4_syncobj *ps4_syncobj(ac_drm_device *dev, uint32_t handle);
static bool ps4_point_submitted(struct ps4_syncobj *s, uint64_t point);
static void ps4_add_point(struct ps4_syncobj *s, uint64_t point, uint64_t seq);

/* EVENT_WRITE_EOP: flush and invalidate CB/DB and the texture caches, then write a 64-bit value
 * at end of pipe. */
static bool
ps4_mapped(const void *p, uint64_t len)
{
   struct ps4_vq_info vq;
   memset(&vq, 0, sizeof(vq));
   return ps4_vq_ok && !sceKernelVirtualQuery(p, 0, &vq, sizeof(vq)) && vq.is_direct &&
          (uintptr_t)vq.end >= (uintptr_t)p + len;
}

/* Who wrote into a buffer? Lists the live buffers mapped next to `target`, and every reference in
 * the recent submissions' command streams to [lo, hi): 64-bit addresses in any packet, and
 * 256-byte-aligned base registers (render target / depth / CMASK / FMASK / HTILE bases). */
static unsigned ps4_scan_hits;
static void
ps4_scan_ib(ac_drm_device *dev, FILE *f, const uint32_t *ib, unsigned num_dw, uint64_t lo,
            uint64_t hi, int depth)
{
   for (unsigned link = 0; link < 64; link++) {
      if (!ps4_mapped(ib, num_dw * 4ull)) {
         fprintf(f, "      IB %p (%u dwords) no longer mapped\n", (void *)ib, num_dw);
         return;
      }
      const uint32_t *next = NULL;
      unsigned next_dw = 0;
      for (unsigned i = 0; i < num_dw && ps4_scan_hits < 300;) {
         const uint32_t h = ib[i];
         if (h >> 30 == 2) {
            i++;
            continue;
         }
         if (h >> 30 != 3) {
            fprintf(f, "      %p: non-type-3 header %#x, stop\n", (void *)&ib[i], h);
            return;
         }
         const unsigned op = (h >> 8) & 0xff;
         const unsigned body = ((h >> 16) & 0x3fff) + 1;
         if (i + 1 + body > num_dw)
            break;
         const uint32_t *b = &ib[i + 1];
         for (unsigned k = 0; k + 1 < body; k++) {
            const uint64_t a = b[k] | ((uint64_t)(b[k + 1] & 0xffff) << 32);
            if (a >= lo && a < hi) {
               fprintf(f, "      %p: op %#x (%u dwords) dword %u: address %#llx\n", (void *)&ib[i],
                       op, body, k, (unsigned long long)a);
               ps4_scan_hits++;
            }
         }
         if (op == 0x69 && body >= 2) { /* SET_CONTEXT_REG */
            for (unsigned k = 1; k < body; k++) {
               const uint64_t a = (uint64_t)b[k] << 8;
               if (a >= lo && a < hi) {
                  fprintf(f, "      %p: context reg %#x = %#x (base %#llx)\n", (void *)&ib[i],
                          0xa000 + b[0] + k - 1, b[k], (unsigned long long)a);
                  ps4_scan_hits++;
               }
            }
         }
         if ((op == 0x3f || op == 0x33) && body >= 3) { /* INDIRECT_BUFFER */
            const uint32_t *t = (const uint32_t *)(uintptr_t)(b[0] | ((uint64_t)(b[1] & 0xffff) << 32));
            if (b[2] & (1u << 20)) {
               next = t;
               next_dw = b[2] & 0xfffff;
               break;
            }
            if (depth < 2)
               ps4_scan_ib(dev, f, t, b[2] & 0xfffff, lo, hi, depth + 1);
         }
         i += 1 + body;
      }
      if (!next)
         return;
      ib = next;
      num_dw = next_dw;
   }
}

static void
ps4_find_writer(ac_drm_device *dev, FILE *f, uint64_t target)
{
   struct amdgpu_bo *below = NULL, *above = NULL;
   for (unsigned h = 1; h < dev->num_bos; h++) {
      struct amdgpu_bo *b = dev->bos[h];
      if (!b || !b->last_va || !b->cpu)
         continue;
      if (b->last_va + b->size <= target && (!below || b->last_va > below->last_va))
         below = b;
      if (b->last_va > target && (!above || b->last_va < above->last_va))
         above = b;
   }
   fprintf(f, "  neighbours:\n");
   if (below) {
      fprintf(f, "    below: bo %u va %#llx size %#llx (ends %#llx) phys %#llx heap %#x flags %#llx\n",
              below->handle, (unsigned long long)below->last_va, (unsigned long long)below->size,
              (unsigned long long)(below->last_va + below->size), (unsigned long long)below->phys,
              below->heap, (unsigned long long)below->flags);
      ps4_describe_va(dev, f, below->last_va + below->size - 4);
   }
   if (above)
      fprintf(f, "    above: bo %u va %#llx size %#llx phys %#llx heap %#x flags %#llx\n",
              above->handle, (unsigned long long)above->last_va, (unsigned long long)above->size,
              (unsigned long long)above->phys, above->heap, (unsigned long long)above->flags);

   /* References into [start of the buffer below, end of the target's first page). */
   const uint64_t lo = below ? below->last_va : target - 0x100000, hi = target + PS4_PAGE;
   fprintf(f, "  references to [%#llx, %#llx) in recent submissions (GPU completed %llu):\n",
           (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)ps4_completed(dev));
   for (unsigned n = 0; n < ARRAY_SIZE(dev->recent); n++) {
      const unsigned r = (dev->last_seq + 1 + n) % ARRAY_SIZE(dev->recent);
      if (!dev->recent[r].seq)
         continue;
      fprintf(f, "    submission %llu:\n", (unsigned long long)dev->recent[r].seq);
      for (unsigned i = 0; i < dev->recent[r].num_ibs; i++)
         ps4_scan_ib(dev, f, (const uint32_t *)(uintptr_t)dev->recent[r].va[i],
                     dev->recent[r].bytes[i] / 4, lo, hi, 0);
   }
}

/* The live buffer whose memory holds `va` (dev->lock held), or NULL. */
static struct amdgpu_bo *
ps4_bo_at(ac_drm_device *dev, uint64_t va)
{
   for (unsigned h = 1; h < dev->num_bos; h++) {
      struct amdgpu_bo *b = dev->bos[h];
      if (b && b->cpu && !b->cpu_owned && va >= b->last_va && va < b->last_va + b->size)
         return b;
   }
   return NULL;
}

/* Every command that writes memory (WRITE_DATA, EVENT_WRITE(_EOP), COPY_DATA, DMA_DATA,
 * ATOMIC_MEM) is checked before the GPU sees it: a destination inside a command buffer (mapped
 * GPU read-only, so the write would fault and the system kill the app) or outside every buffer is
 * reported and the command turned into a NOP of the same length. Render target / depth bases in a
 * command buffer are reported. dev->lock held. */
static unsigned ps4_bad_writes;
static unsigned long long ps4_writes_checked;
static void
ps4_scan_writes(ac_drm_device *dev, uint32_t *ib, unsigned num_dw, uint64_t seq, int depth)
{
   for (unsigned link = 0; link < 256 && num_dw; link++) {
      if (!ps4_mapped(ib, num_dw * 4ull))
         return;
      uint32_t *next = NULL;
      unsigned next_dw = 0;
      for (unsigned i = 0; i < num_dw;) {
         const uint32_t h = ib[i];
         if (h >> 30 == 2) {
            i++;
            continue;
         }
         if (h >> 30 != 3)
            return;
         const unsigned op = (h >> 8) & 0xff;
         const unsigned body = ((h >> 16) & 0x3fff) + 1;
         if (i + 1 + body > num_dw)
            return;
         uint32_t *b = &ib[i + 1];
         uint64_t dst = 0, len = 4;
         const char *name = NULL;
         switch (op) {
         case 0x37: /* WRITE_DATA */
            if (body >= 4 && (((b[0] >> 8) & 0xf) == 1 || ((b[0] >> 8) & 0xf) == 2 ||
                              ((b[0] >> 8) & 0xf) == 5)) {
               dst = b[1] | ((uint64_t)b[2] << 32);
               len = (body - 3) * 4ull;
               name = "WRITE_DATA";
            }
            break;
         case 0x47: /* EVENT_WRITE_EOP */
            if (body >= 5 && (b[2] >> 29)) {
               dst = b[1] | ((uint64_t)(b[2] & 0xffff) << 32);
               len = (b[2] >> 29) == 1 ? 4 : 8;
               name = "EVENT_WRITE_EOP";
            }
            break;
         case 0x46: /* EVENT_WRITE with an address (ZPASS_DONE, pipeline stats, ...) */
            if (body >= 3) {
               dst = b[1] | ((uint64_t)(b[2] & 0xffff) << 32);
               len = 8;
               name = "EVENT_WRITE";
            }
            break;
         case 0x40: /* COPY_DATA */
            if (body >= 5 && (((b[0] >> 8) & 0xf) == 1 || ((b[0] >> 8) & 0xf) == 2 ||
                              ((b[0] >> 8) & 0xf) == 5)) {
               dst = b[3] | ((uint64_t)b[4] << 32);
               len = (b[0] & (1u << 16)) ? 8 : 4;
               name = "COPY_DATA";
            }
            break;
         case 0x50: /* DMA_DATA */
            if (body >= 6 && (((b[0] >> 20) & 3) == 0 || ((b[0] >> 20) & 3) == 3)) {
               dst = b[3] | ((uint64_t)b[4] << 32);
               len = MAX2(b[5] & 0x1fffff, 1);
               name = "DMA_DATA";
            }
            break;
         case 0x1e: /* ATOMIC_MEM */
            if (body >= 3) {
               dst = b[1] | ((uint64_t)b[2] << 32);
               len = 8;
               name = "ATOMIC_MEM";
            }
            break;
         case 0x69: /* SET_CONTEXT_REG: CB_COLORn_BASE/CMASK/FMASK, DB bases, HTILE */
            for (unsigned k = 1; k < body; k++) {
               const unsigned reg = b[0] + k - 1;
               const bool base = (reg >= 0x318 && reg < 0x318 + 8 * 15 &&
                                  ((reg - 0x318) % 15 == 0 || (reg - 0x318) % 15 == 7 ||
                                   (reg - 0x318) % 15 == 9)) ||
                                 (reg >= 0x12 && reg <= 0x15) || reg == 0x5;
               if (!base || !b[k])
                  continue;
               struct amdgpu_bo *t = ps4_bo_at(dev, (uint64_t)b[k] << 8);
               if (t && t->gpu_ro && ps4_bad_writes++ < 40)
                  ps4_log("radv/ps4: submission %llu: %p: context reg %#x = %#x points into "
                          "command buffer bo %u (va %#llx)\n", (unsigned long long)seq,
                          (void *)&ib[i], 0xa000 + reg, b[k], t->handle,
                          (unsigned long long)t->last_va);
            }
            break;
         case 0x3f: { /* INDIRECT_BUFFER */
            uint32_t *t = (uint32_t *)(uintptr_t)(b[0] | ((uint64_t)(b[1] & 0xffff) << 32));
            if (body >= 3 && (b[2] & (1u << 20))) {
               next = t;
               next_dw = b[2] & 0xfffff;
            } else if (body >= 3 && depth < 3) {
               ps4_scan_writes(dev, t, b[2] & 0xfffff, seq, depth + 1);
            }
            break;
         }
         default:
            break;
         }
         if (name) {
            ps4_writes_checked++;
            struct amdgpu_bo *t = ps4_bo_at(dev, dst);
            struct amdgpu_bo *t_end = ps4_bo_at(dev, dst + len - 1);
            const char *why = !t ? "outside every buffer"
                              : t->gpu_ro ? "into a command buffer"
                              : t_end != t ? "past the end of its buffer"
                                           : NULL;
            if (why) {
               if (ps4_bad_writes++ < 40) {
                  char words[160];
                  int n = 0;
                  for (unsigned k = 0; k <= body && k < 10; k++)
                     n += snprintf(words + n, sizeof(words) - n, " %08x", ib[i + k]);
                  ps4_log("radv/ps4: submission %llu: %p: %s to %#llx (%llu bytes) %s - "
                          "disabled. packet:%s\n", (unsigned long long)seq, (void *)&ib[i], name,
                          (unsigned long long)dst, (unsigned long long)len, why, words);
                  if (t)
                     ps4_log("radv/ps4:   target bo %u: va %#llx size %#llx heap %#x flags %#llx\n",
                             t->handle, (unsigned long long)t->last_va,
                             (unsigned long long)t->size, t->heap, (unsigned long long)t->flags);
               }
               ib[i] = (h & ~0xff00u) | (0x10u << 8); /* NOP, same length */
            }
         }
         if (next)
            break;
         i += 1 + body;
      }
      if (!next)
         return;
      ib = next;
      num_dw = next_dw;
   }
}

/* Before submitting: does every IB of the chain still start with a packet header, and does every
 * chain link point into a live buffer? (A GPU hang showed a chained IB full of vertex-like float
 * data.) Follows only the chain link in each IB's last 4 dwords - a few reads per IB. On failure,
 * appends what it found to ib-corrupt.txt; returns false. */
static unsigned ps4_corrupt_reports;
static bool
ps4_check_ib_chain(ac_drm_device *dev, const uint32_t *ib, unsigned num_dw, uint64_t seq)
{
   const uint32_t *prev = NULL;
   unsigned prev_dw = 0;
   for (unsigned link = 0; link < 256 && num_dw; link++) {
      const char *problem = NULL;
      if (ib[0] >> 30 != 3 && ib[0] >> 30 != 2)
         problem = "does not start with a packet header";
      const uint32_t *tail = ib + num_dw - 4;
      uint64_t next = 0;
      unsigned next_dw = 0;
      if (!problem && num_dw >= 4 && tail[0] == 0xC0023F00 && (tail[3] & (1u << 20))) {
         next = tail[1] | ((uint64_t)(tail[2] & 0xffff) << 32);
         next_dw = tail[3] & 0xfffff;
         struct ac_addr_info info;
         ps4_ib_addr(dev, next, &info);
         struct ps4_vq_info vq;
         memset(&vq, 0, sizeof(vq));
         if (!info.valid)
            problem = "chains to an address outside the GPU windows";
         else if (ps4_vq_ok &&
                  (sceKernelVirtualQuery((void *)(uintptr_t)next, 0, &vq, sizeof(vq)) ||
                   !vq.is_direct || (uintptr_t)vq.end < next + next_dw * 4ull))
            problem = "chains to memory that isn't mapped";
      }
      if (problem) {
         if (ps4_corrupt_reports++ < 4) {
            ps4_log("radv/ps4: submission %llu: IB %p (%u dwords) %s (first dword %#x) - see "
                    "ib-corrupt.txt\n", (unsigned long long)seq, (void *)ib, num_dw, problem, ib[0]);
            FILE *f = fopen("/data/DolphinPS4/ib-corrupt.txt", "a");
            if (f) {
               fprintf(f, "==== submission %llu, chain link %u: IB %p, %u dwords, %s\n",
                       (unsigned long long)seq, link, (void *)ib, num_dw, problem);
               fprintf(f, "  first 48 dwords:");
               for (unsigned i = 0; i < 48 && i < num_dw; i++)
                  fprintf(f, "%s%08x", i % 8 ? " " : "\n    ", ib[i]);
               fprintf(f, "\n");
               if (prev && prev_dw >= 32) {
                  fprintf(f, "  last 32 dwords of the IB chaining to it (%p, %u dwords):", (void *)prev,
                          prev_dw);
                  for (unsigned i = prev_dw - 32; i < prev_dw; i++)
                     fprintf(f, "%s%08x", (i - (prev_dw - 32)) % 8 ? " " : "\n    ", prev[i]);
                  fprintf(f, "\n");
               }
               ps4_describe_va(dev, f, (uintptr_t)ib);
               if (next)
                  ps4_describe_va(dev, f, next);
               struct ps4_vq_info vq;
               memset(&vq, 0, sizeof(vq));
               if (!sceKernelVirtualQuery(ib, 0, &vq, sizeof(vq)))
                  fprintf(f, "  kernel: [%p, %p) offset %#llx prot %#x type %d direct %u flexible %u "
                             "'%.32s'\n", vq.start, vq.end, (unsigned long long)vq.offset,
                          vq.protection, vq.memory_type, vq.is_direct, vq.is_flexible, vq.name);
               ps4_check_overlaps(dev, f);
               ps4_find_writer(dev, f, (uintptr_t)ib);
               fclose(f);
            }
            simple_mtx_lock(&dev->lock);
            ps4_scan_tails(dev);
            simple_mtx_unlock(&dev->lock);
         }
         return false;
      }
      if (!next)
         return true;
      prev = ib;
      prev_dw = num_dw;
      ib = (const uint32_t *)(uintptr_t)next;
      num_dw = next_dw;
   }
   return true;
}

static uint32_t *
ps4_emit_eop(uint32_t *cs, uint64_t addr, uint64_t value, bool interrupt)
{
   *cs++ = 0xC0044700; /* PKT3(EVENT_WRITE_EOP, 4) */
   *cs++ = 0x14 | (5 << 8) | (1 << 16) | (1 << 17); /* CACHE_FLUSH_AND_INV_TS_EVENT, TCL1, TC */
   *cs++ = (uint32_t)addr;
   /* DATA_SEL: 64-bit value; INT_SEL 2: interrupt once the write is confirmed. */
   *cs++ = ((addr >> 32) & 0xffff) | (2u << 29) | (interrupt ? 2u << 24 : 0);
   *cs++ = (uint32_t)value;
   *cs++ = (uint32_t)(value >> 32);
   return cs;
}

/* Adds the finished submissions' timestamps to the busy time (called with dev->lock held). */
static void
ps4_account_gpu_time(ac_drm_device *dev)
{
   if (!dev->ts_start)
      return;
   const uint64_t done = ps4_completed(dev);
   if (dev->ts_accounted + FENCE_IB_COUNT < done)
      dev->ts_accounted = done - FENCE_IB_COUNT; /* slots already reused */
   while (dev->ts_accounted < done) {
      const unsigned i = ++dev->ts_accounted % FENCE_IB_COUNT;
      const uint64_t start = dev->ts_start[i], end = dev->ts_end[i];
      if (!start || end <= start)
         continue; /* a flip (no timestamps) or not written */
      const uint64_t from = MAX2(start, dev->ts_prev_end);
      if (end > from)
         dev->ts_busy += end - from;
      dev->ts_prev_end = MAX2(dev->ts_prev_end, end);
      if (!dev->ts_span_first)
         dev->ts_span_first = start;
      dev->ts_span_last = MAX2(dev->ts_span_last, end);
   }
}

void ac_ps4_get_gpu_busy(uint64_t *busy_ticks, uint64_t *span_ticks);
void
ac_ps4_get_gpu_busy(uint64_t *busy_ticks, uint64_t *span_ticks)
{
   ac_drm_device *dev = ps4_last_device;
   *busy_ticks = *span_ticks = 0;
   if (!dev)
      return;
   simple_mtx_lock(&dev->lock);
   ps4_account_gpu_time(dev);
   *busy_ticks = dev->ts_busy;
   *span_ticks = dev->ts_span_last - dev->ts_span_first;
   simple_mtx_unlock(&dev->lock);
}

int
ac_drm_cs_submit_raw2(ac_drm_device *dev, uint32_t ctx_id, uint32_t bo_list_handle,
                      int num_chunks, struct drm_amdgpu_cs_chunk *chunks, uint64_t *seq_no)
{
   void *dcb[64];
   uint32_t dcb_sizes[64];
   void *ccb[64] = {0};
   uint32_t ccb_sizes[64] = {0};
   unsigned num_ibs = 0;
   uint64_t user_fence_addr = 0;
   struct drm_amdgpu_cs_chunk *signal_chunk = NULL;

   for (int i = 0; i < num_chunks; i++) {
      struct drm_amdgpu_cs_chunk *c = &chunks[i];
      void *data = (void *)(uintptr_t)c->chunk_data;
      switch (c->chunk_id) {
      case AMDGPU_CHUNK_ID_IB: {
         struct drm_amdgpu_cs_chunk_ib *ib = data;
         if (ib->ip_type != AMDGPU_HW_IP_GFX || num_ibs >= ARRAY_SIZE(dcb) - 2)
            return -EINVAL;
         dcb[num_ibs] = (void *)(uintptr_t)ib->va_start;
         dcb_sizes[num_ibs] = ib->ib_bytes;
         num_ibs++;
         break;
      }
      case AMDGPU_CHUNK_ID_FENCE: {
         struct drm_amdgpu_cs_chunk_fence *f = data;
         struct amdgpu_bo *b = ps4_bo(dev, f->handle);
         if (b && b->cpu)
            user_fence_addr = (uintptr_t)b->cpu + f->offset;
         break;
      }
      case AMDGPU_CHUNK_ID_SYNCOBJ_IN:
      case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT: {
         /* One in-order queue: a wait only has to make sure the point was submitted (or
          * signalled by the CPU) before this submission; the GPU then runs them in order. */
         const bool timeline = c->chunk_id == AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT;
         const unsigned stride = timeline ? 4 : 1;
         for (unsigned j = 0; j + stride <= c->length_dw; j += stride) {
            const uint32_t *w = (const uint32_t *)data + j;
            const uint64_t point = timeline ? ((const struct drm_amdgpu_cs_chunk_syncobj *)w)->point : 0;
            for (;;) {
               simple_mtx_lock(&dev->lock);
               struct ps4_syncobj *s = ps4_syncobj(dev, w[0]);
               bool ready = !s || ps4_point_submitted(s, point);
               simple_mtx_unlock(&dev->lock);
               if (ready)
                  break;
               sceKernelUsleep(100);
            }
         }
         break;
      }
      case AMDGPU_CHUNK_ID_SYNCOBJ_OUT:
      case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL:
         signal_chunk = c;
         break;
      default:
         break;
      }
   }
   if (!num_ibs)
      return -EINVAL;

   /* A corrupted command buffer would hang the GPU (or fault it, and the system kills the app):
    * drop its work - the fences still signal - and report it once. */
   for (unsigned i = 0; i < num_ibs; i++) {
      if (!ps4_check_ib_chain(dev, dcb[i], dcb_sizes[i] / 4, dev->last_seq + 1)) {
         num_ibs = 0;
         break;
      }
   }
   ps4_record_submission(dcb, dcb_sizes, num_ibs, dev->last_seq + 1);

   /* Commands that would write where they mustn't: report and disable them (RADV_PS4_SCAN=1;
    * it found nothing in the boss-intro crash and costs CPU time). */
   static int scan = -1;
   if (scan < 0)
      scan = getenv("RADV_PS4_SCAN") != NULL;
   if (num_ibs && scan) {
      simple_mtx_lock(&dev->lock);
      for (unsigned i = 0; i < num_ibs; i++)
         ps4_scan_writes(dev, dcb[i], dcb_sizes[i] / 4, dev->last_seq + 1, 0);
      simple_mtx_unlock(&dev->lock);
   }

   simple_mtx_lock(&dev->lock);
   /* The fence IB slot for the next sequence number must be free (its previous user finished).
    * Waited for before taking the number, so submissions reach the GPU in sequence order (the
    * completed counter must never get ahead of work that hasn't been submitted yet). */
   while (dev->last_seq + 1 > FENCE_IB_COUNT &&
          ps4_completed(dev) < dev->last_seq + 1 - FENCE_IB_COUNT) {
      simple_mtx_unlock(&dev->lock);
      ps4_wait_seq(dev, dev->last_seq + 1 - FENCE_IB_COUNT, INT64_MAX);
      simple_mtx_lock(&dev->lock);
   }
   ps4_account_gpu_time(dev);
   const uint64_t seq = ++dev->last_seq;
   uint32_t *fence_ib = dev->fence_ibs + (seq % FENCE_IB_COUNT) * FENCE_IB_DW;
   uint32_t *cs = fence_ib;
   if (user_fence_addr)
      cs = ps4_emit_eop(cs, user_fence_addr, seq, false);
   const unsigned ts_slot = seq % FENCE_IB_COUNT;
   if (dev->ts_start) {
      dev->ts_start[ts_slot] = 0;
      dev->ts_end[ts_slot] = 0;
      /* PKT3(EVENT_WRITE_EOP): BOTTOM_OF_PIPE_TS, DATA_SEL 3 = the 64-bit GPU clock. */
      const uint64_t addr = (uintptr_t)&dev->ts_end[ts_slot];
      *cs++ = 0xC0044700;
      *cs++ = 0x28 | (5 << 8);
      *cs++ = (uint32_t)addr;
      *cs++ = ((addr >> 32) & 0xffff) | (3u << 29);
      *cs++ = 0;
      *cs++ = 0;
   }
   cs = ps4_emit_eop(cs, (uintptr_t)dev->fence, seq, dev->eop_queue != NULL);
   while ((cs - fence_ib) % 8)
      *cs++ = 0xFFFF1000; /* type-2 NOP padding */
   dcb[num_ibs] = fence_ib;
   dcb_sizes[num_ibs] = (cs - fence_ib) * 4;

   {
      const unsigned slot = seq % ARRAY_SIZE(dev->recent);
      dev->recent[slot].seq = seq;
      dev->recent[slot].num_ibs = MIN2(num_ibs, ARRAY_SIZE(dev->recent[slot].va));
      for (unsigned i = 0; i < dev->recent[slot].num_ibs; i++) {
         dev->recent[slot].va[i] = (uintptr_t)dcb[i];
         dev->recent[slot].bytes[i] = dcb_sizes[i];
      }
   }
   unsigned submit_ibs = num_ibs + 1;
   if (dev->ts_ibs) {
      /* In front: PKT3(COPY_DATA) of the GPU clock (SRC_SEL 9) to memory (DST_SEL 5), 64 bits,
       * write-confirmed - when the command processor reaches this submission. */
      uint32_t *ts_ib = dev->ts_ibs + ts_slot * TS_IB_DW, *t = ts_ib;
      const uint64_t addr = (uintptr_t)&dev->ts_start[ts_slot];
      *t++ = 0xC0044000;
      *t++ = 9 | (5 << 8) | (1 << 16) | (1 << 20);
      *t++ = 0;
      *t++ = 0;
      *t++ = (uint32_t)addr;
      *t++ = (uint32_t)(addr >> 32);
      while ((t - ts_ib) % 8)
         *t++ = 0xFFFF1000;
      memmove(dcb + 1, dcb, sizeof(dcb[0]) * submit_ibs);
      memmove(dcb_sizes + 1, dcb_sizes, sizeof(dcb_sizes[0]) * submit_ibs);
      dcb[0] = ts_ib;
      dcb_sizes[0] = (uint32_t)(t - ts_ib) * 4;
      submit_ibs++;
   }
   int r = dev->submit(submit_ibs, dcb, dcb_sizes, ccb, ccb_sizes);
   dev->submit_done();
   if (seq <= 40 || seq % 1000 == 0)
      ps4_log("radv/ps4: submit %llu: %u IBs (first %p, %u bytes) = %#x, GPU completed %llu, "
              "%llu memory writes checked, %u disabled\n",
              (unsigned long long)seq, num_ibs, dcb[0], dcb_sizes[0], r,
              (unsigned long long)ps4_completed(dev), ps4_writes_checked, ps4_bad_writes);
   if (r) {
      ps4_log("radv/ps4: sceGnmSubmitCommandBuffers(%u IBs) = %#x\n", num_ibs + 1, r);
      dev->last_seq--;
      simple_mtx_unlock(&dev->lock);
      return -EINVAL;
   }

   if (seq % 256 == 0)
      ps4_scan_tails(dev);
   ps4_run_deferred(dev);

   if (signal_chunk) {
      const bool timeline = signal_chunk->chunk_id == AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL;
      const unsigned stride = timeline ? 4 : 1;
      const uint32_t *data = (const uint32_t *)(uintptr_t)signal_chunk->chunk_data;
      for (unsigned j = 0; j + stride <= signal_chunk->length_dw; j += stride) {
         struct ps4_syncobj *s = ps4_syncobj(dev, data[j]);
         if (s)
            ps4_add_point(s, timeline ? ((const struct drm_amdgpu_cs_chunk_syncobj *)(data + j))->point : 0, seq);
      }
   }
   simple_mtx_unlock(&dev->lock);
   *seq_no = seq;
   return 0;
}

/* Present without the CPU waiting: queues a flip of VideoOut buffer `buffer` behind everything
 * submitted so far (sceGnmSubmitAndFlipCommandBuffers: the flip happens when the GPU gets there).
 * `arg` comes back in sceVideoOutGetFlipStatus().flipArg once the flip is done. */
int ac_ps4_submit_flip(int video, unsigned buffer, unsigned mode, int64_t arg);
int
ac_ps4_submit_flip(int video, unsigned buffer, unsigned mode, int64_t arg)
{
   ac_drm_device *dev = ps4_fault_dev;
   if (!dev || !dev->submit_and_flip || !dev->flip_ibs)
      return -1;
   enum { FLIP_IB_DW = 128 };
   STATIC_ASSERT(ARRAY_SIZE(dev->flip_seq) * FLIP_IB_DW * 4 <= PS4_PAGE);

   simple_mtx_lock(&dev->lock);
   const unsigned slot = dev->flip_count % ARRAY_SIZE(dev->flip_seq);
   /* The slot's previous flip IB, and the fence IB slot for the next sequence number, must be
    * free - waited for before taking the number, as in ac_drm_cs_submit_raw2. */
   while (ps4_completed(dev) < dev->flip_seq[slot] ||
          (dev->last_seq + 1 > FENCE_IB_COUNT &&
           ps4_completed(dev) < dev->last_seq + 1 - FENCE_IB_COUNT)) {
      const uint64_t wait = MAX2(dev->flip_seq[slot], dev->last_seq + 1 > FENCE_IB_COUNT
                                                         ? dev->last_seq + 1 - FENCE_IB_COUNT
                                                         : 0);
      simple_mtx_unlock(&dev->lock);
      ps4_wait_seq(dev, wait, INT64_MAX);
      simple_mtx_lock(&dev->lock);
   }
   const uint64_t seq = ++dev->last_seq;
   uint32_t *ib = dev->flip_ibs + slot * FLIP_IB_DW, *cs = ib;
   cs = ps4_emit_eop(cs, (uintptr_t)dev->fence, seq, dev->eop_queue != NULL);
   while ((cs - ib) % 8)
      *cs++ = 0xFFFF1000;
   /* Gnm's prepareFlip: a 64-dword NOP with the flip label, rewritten by the driver. */
   *cs++ = 0xC03E1000;
   *cs++ = 0x68750777;
   for (int i = 0; i < 62; i++)
      *cs++ = 0;
   void *dcb[1] = {ib};
   uint32_t dcb_size[1] = {(uint32_t)(cs - ib) * 4};
   void *ccb[1] = {NULL};
   uint32_t ccb_size[1] = {0};
   const int r = dev->submit_and_flip(1, dcb, dcb_size, ccb, ccb_size, video, buffer, mode, arg);
   dev->submit_done();
   if (r) {
      dev->last_seq--;
      static unsigned logged;
      if (logged++ < 8)
         ps4_log("radv/ps4: sceGnmSubmitAndFlipCommandBuffers(buffer %u) = %#x\n", buffer, r);
   } else {
      dev->flip_seq[slot] = seq;
      dev->flip_count++;
      const unsigned rs = seq % ARRAY_SIZE(dev->recent);
      dev->recent[rs].seq = seq;
      dev->recent[rs].num_ibs = 1;
      dev->recent[rs].va[0] = (uintptr_t)ib;
      dev->recent[rs].bytes[0] = dcb_size[0];
      ps4_run_deferred(dev);
   }
   simple_mtx_unlock(&dev->lock);
   return r;
}

/* ---------------------------------------------------------------------------------------------
 * Sync objects (DRM syncobj semantics on top of sequence numbers)
 */

static struct ps4_syncobj *
ps4_syncobj(ac_drm_device *dev, uint32_t handle)
{
   return handle && handle < dev->num_syncobjs && dev->syncobjs[handle].used
             ? &dev->syncobjs[handle]
             : NULL;
}

static void
ps4_add_point(struct ps4_syncobj *s, uint64_t point, uint64_t seq)
{
   if (point == 0) {
      /* Binary: replace the fence. */
      s->num_points = 0;
   }
   if (s->num_points == s->max_points) {
      s->max_points = MAX2(s->max_points * 2, 4);
      s->points = realloc(s->points, s->max_points * sizeof(*s->points));
   }
   s->points[s->num_points++] = (struct ps4_point){point, seq};
}

static bool
ps4_point_submitted(struct ps4_syncobj *s, uint64_t point)
{
   for (unsigned i = 0; i < s->num_points; i++)
      if (s->points[i].point >= point)
         return true;
   return false;
}

/* Highest point whose submission has completed (timeline payload). */
static uint64_t
ps4_payload(ac_drm_device *dev, struct ps4_syncobj *s)
{
   const uint64_t done = ps4_completed(dev);
   uint64_t payload = 0;
   unsigned keep = 0;
   for (unsigned i = 0; i < s->num_points; i++) {
      if (s->points[i].seq <= done)
         payload = MAX2(payload, s->points[i].point);
   }
   /* Drop completed points below the payload (keep the latest one). */
   for (unsigned i = 0; i < s->num_points; i++) {
      if (s->points[i].seq > done || s->points[i].point == payload)
         s->points[keep++] = s->points[i];
   }
   s->num_points = keep;
   return payload;
}

static bool
ps4_binary_signalled(ac_drm_device *dev, struct ps4_syncobj *s)
{
   return s->num_points && s->points[s->num_points - 1].seq <= ps4_completed(dev);
}

static int
ps4_create(struct util_sync_provider *p, uint32_t flags, uint32_t *handle)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   unsigned h = 1;
   while (h < dev->num_syncobjs && dev->syncobjs[h].used)
      h++;
   if (h >= dev->num_syncobjs) {
      unsigned cap = MAX2(dev->num_syncobjs * 2, 64);
      dev->syncobjs = realloc(dev->syncobjs, cap * sizeof(*dev->syncobjs));
      memset(dev->syncobjs + dev->num_syncobjs, 0,
             (cap - dev->num_syncobjs) * sizeof(*dev->syncobjs));
      dev->num_syncobjs = cap;
   }
   struct ps4_syncobj *s = &dev->syncobjs[h];
   s->used = true;
   s->num_points = 0;
   if (flags & DRM_SYNCOBJ_CREATE_SIGNALED)
      ps4_add_point(s, 0, 0);
   simple_mtx_unlock(&dev->lock);
   *handle = h;
   return 0;
}

static int
ps4_destroy(struct util_sync_provider *p, uint32_t handle)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   struct ps4_syncobj *s = ps4_syncobj(dev, handle);
   if (s) {
      free(s->points);
      memset(s, 0, sizeof(*s));
   }
   simple_mtx_unlock(&dev->lock);
   return s ? 0 : -EINVAL;
}

static int
ps4_not_supported_fd(struct util_sync_provider *p, uint32_t handle, int *fd)
{
   return -ENOSYS;
}

static int
ps4_fd_to_handle(struct util_sync_provider *p, int fd, uint32_t *handle)
{
   return -ENOSYS;
}

static int
ps4_import_sync_file(struct util_sync_provider *p, uint32_t handle, int fd)
{
   return -ENOSYS;
}

/* Waits on handles/points; `points` NULL = binary. */
static int
ps4_wait_common(ac_drm_device *dev, uint32_t *handles, uint64_t *points, unsigned count,
                int64_t abs_timeout, unsigned flags, uint32_t *first)
{
   const bool all = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
   const bool available = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
   const bool for_submit = flags & (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
   unsigned spins = 0;
   int64_t wait_start = 0;
   for (;;) {
      unsigned done = 0, first_done = ~0u;
      simple_mtx_lock(&dev->lock);
      for (unsigned i = 0; i < count; i++) {
         struct ps4_syncobj *s = ps4_syncobj(dev, handles[i]);
         if (!s) {
            simple_mtx_unlock(&dev->lock);
            return -EINVAL;
         }
         bool ok;
         if (points && points[i]) {
            ok = available ? ps4_point_submitted(s, points[i]) : ps4_payload(dev, s) >= points[i];
            if (!ok && !for_submit && !ps4_point_submitted(s, points[i])) {
               simple_mtx_unlock(&dev->lock);
               return -EINVAL;
            }
         } else {
            if (!s->num_points && !for_submit) {
               simple_mtx_unlock(&dev->lock);
               return -EINVAL;
            }
            ok = available ? s->num_points > 0 : ps4_binary_signalled(dev, s);
         }
         if (ok) {
            done++;
            if (first_done == ~0u)
               first_done = i;
         }
      }
      simple_mtx_unlock(&dev->lock);
      if (all ? done == count : done > 0) {
         if (first)
            *first = first_done == ~0u ? 0 : first_done;
         return 0;
      }
      if (abs_timeout <= 0 || (abs_timeout != INT64_MAX && os_time_get_nano() >= abs_timeout))
         return -ETIME;
      if (!wait_start)
         wait_start = os_time_get_nano();
      else if (os_time_get_nano() - wait_start > 2000000000ll && ps4_completed(dev) < dev->last_seq)
         ps4_report_hang(dev, ps4_completed(dev) + 1);
      if (++spins > 64)
         ps4_sleep(dev);
   }
}

/* Failures also set errno, like the drmSyncobj ioctls: vk_drm_syncobj tells a timeout (fence not
 * signalled yet, e.g. vkGetFenceStatus) from an error by errno == ETIME, not by the return value. */
static int
ps4_wait(struct util_sync_provider *p, uint32_t *handles, unsigned count, int64_t timeout_nsec,
         unsigned flags, uint32_t *first)
{
   const int r = ps4_wait_common((ac_drm_device *)p, handles, NULL, count, timeout_nsec, flags, first);
   if (r)
      errno = -r;
   return r;
}

static int
ps4_timeline_wait(struct util_sync_provider *p, uint32_t *handles, uint64_t *points,
                  unsigned count, int64_t timeout_nsec, unsigned flags, uint32_t *first)
{
   const int r = ps4_wait_common((ac_drm_device *)p, handles, points, count, timeout_nsec, flags, first);
   if (r)
      errno = -r;
   return r;
}

static int
ps4_reset(struct util_sync_provider *p, const uint32_t *handles, uint32_t count)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct ps4_syncobj *s = ps4_syncobj(dev, handles[i]);
      if (s)
         s->num_points = 0;
   }
   simple_mtx_unlock(&dev->lock);
   return 0;
}

static int
ps4_signal(struct util_sync_provider *p, const uint32_t *handles, uint32_t count)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct ps4_syncobj *s = ps4_syncobj(dev, handles[i]);
      if (s)
         ps4_add_point(s, 0, 0);
   }
   simple_mtx_unlock(&dev->lock);
   return 0;
}

static int
ps4_timeline_signal(struct util_sync_provider *p, const uint32_t *handles, uint64_t *points,
                    uint32_t count)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct ps4_syncobj *s = ps4_syncobj(dev, handles[i]);
      if (s)
         ps4_add_point(s, points[i], 0);
   }
   simple_mtx_unlock(&dev->lock);
   return 0;
}

static int
ps4_query(struct util_sync_provider *p, uint32_t *handles, uint64_t *points, uint32_t count,
          uint32_t flags)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct ps4_syncobj *s = ps4_syncobj(dev, handles[i]);
      if (!s) {
         points[i] = 0;
         continue;
      }
      if (flags & DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) {
         uint64_t last = 0;
         for (unsigned j = 0; j < s->num_points; j++)
            last = MAX2(last, s->points[j].point);
         points[i] = last;
      } else {
         points[i] = ps4_payload(dev, s);
      }
   }
   simple_mtx_unlock(&dev->lock);
   return 0;
}

static int
ps4_transfer(struct util_sync_provider *p, uint32_t dst_handle, uint64_t dst_point,
             uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
   ac_drm_device *dev = (ac_drm_device *)p;
   simple_mtx_lock(&dev->lock);
   struct ps4_syncobj *src = ps4_syncobj(dev, src_handle);
   struct ps4_syncobj *dst = ps4_syncobj(dev, dst_handle);
   int r = -EINVAL;
   if (src && dst) {
      /* The fence of the source: the first point >= src_point (binary: the current one). */
      uint64_t seq = 0;
      bool found = false;
      for (unsigned i = 0; i < src->num_points; i++) {
         if (src->points[i].point >= src_point && (!found || src->points[i].seq < seq)) {
            seq = src->points[i].seq;
            found = true;
         }
      }
      if (found) {
         ps4_add_point(dst, dst_point, seq);
         r = 0;
      }
   }
   simple_mtx_unlock(&dev->lock);
   return r;
}

static void
ps4_finalize(struct util_sync_provider *p)
{
}

static struct util_sync_provider *
ps4_clone(struct util_sync_provider *p)
{
   return p;
}

static struct util_sync_provider *
ps4_sync_provider_init(ac_drm_device *dev)
{
   dev->p = (struct util_sync_provider){
      .create = ps4_create,
      .destroy = ps4_destroy,
      .handle_to_fd = ps4_not_supported_fd,
      .fd_to_handle = ps4_fd_to_handle,
      .import_sync_file = ps4_import_sync_file,
      .export_sync_file = ps4_not_supported_fd,
      .wait = ps4_wait,
      .reset = ps4_reset,
      .signal = ps4_signal,
      .timeline_signal = ps4_timeline_signal,
      .timeline_wait = ps4_timeline_wait,
      .query = ps4_query,
      .transfer = ps4_transfer,
      .finalize = ps4_finalize,
      .clone = ps4_clone,
   };
   return &dev->p;
}

int
ac_drm_cs_create_syncobj2(ac_drm_device *dev, uint32_t flags, uint32_t *handle)
{
   return ps4_create(&dev->p, flags, handle);
}

int
ac_drm_cs_destroy_syncobj(ac_drm_device *dev, uint32_t handle)
{
   return ps4_destroy(&dev->p, handle);
}

int
ac_drm_cs_syncobj_wait(ac_drm_device *dev, uint32_t *handles, unsigned num_handles,
                       int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
   return ps4_wait(&dev->p, handles, num_handles, timeout_nsec, flags, first_signaled);
}

int
ac_drm_cs_syncobj_query2(ac_drm_device *dev, uint32_t *handles, uint64_t *points,
                         unsigned num_handles, uint32_t flags)
{
   return ps4_query(&dev->p, handles, points, num_handles, flags);
}

int
ac_drm_cs_import_syncobj(ac_drm_device *dev, int shared_fd, uint32_t *handle)
{
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_export_sync_file(ac_drm_device *dev, uint32_t syncobj, int *sync_file_fd)
{
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_import_sync_file(ac_drm_device *dev, uint32_t syncobj, int sync_file_fd)
{
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_export_sync_file2(ac_drm_device *dev, uint32_t syncobj, uint64_t point,
                                    uint32_t flags, int *sync_file_fd)
{
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_transfer(ac_drm_device *dev, uint32_t dst_handle, uint64_t dst_point,
                           uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
   return ps4_transfer(&dev->p, dst_handle, dst_point, src_handle, src_point, flags);
}

int
ac_drm_cs_syncobj_timeline_wait(ac_drm_device *dev, uint32_t *handles, uint64_t *points,
                                unsigned num_handles, int64_t timeout_nsec, unsigned flags,
                                uint32_t *first_signaled)
{
   return ps4_timeline_wait(&dev->p, handles, points, num_handles, timeout_nsec, flags,
                            first_signaled);
}

/* ---------------------------------------------------------------------------------------------
 * Queries: answer as the amdgpu kernel would for a Liverpool-like gfx7 APU
 */

#define PS4_VRAM_SIZE (2048ull << 20)
#define PS4_GTT_SIZE (1024ull << 20)

int
ac_drm_query_gpu_info(ac_drm_device *dev, struct amdgpu_gpu_info *info)
{
   memset(info, 0, sizeof(*info));
   info->asic_id = 0x9920; /* Liverpool */
   info->chip_external_rev = 0x14; /* Bonaire range: identified as CHIP_BONAIRE (gfx7) */
   info->family_id = FAMILY_CI;
   info->ids_flags = AMDGPU_IDS_FLAGS_FUSION;
   info->max_engine_clk = 800000;
   info->max_memory_clk = 1375000;
   info->num_shader_engines = 2;
   info->num_shader_arrays_per_engine = 1;
   info->rb_pipes = 8;
   info->enabled_rb_pipes_mask = 0xff;
   info->gpu_counter_freq = 100000; /* kHz */
   info->mc_arb_ramcfg = PS4_MC_ARB_RAMCFG;
   info->gb_addr_cfg = PS4_GB_ADDR_CONFIG;
   memcpy(info->gb_tile_mode, ps4_tile_modes, sizeof(ps4_tile_modes));
   memcpy(info->gb_macro_tile_mode, ps4_macrotile_modes, sizeof(ps4_macrotile_modes));
   info->cu_bitmap[0][0] = 0x1ff; /* 9 CUs per shader engine, 18 total */
   info->cu_bitmap[1][0] = 0x1ff;
   info->vram_type = AMDGPU_VRAM_TYPE_GDDR5;
   info->vram_bit_width = 256;
   return 0;
}

static void
ps4_dev_info(ac_drm_device *dev, struct drm_amdgpu_info_device *d)
{
   memset(d, 0, sizeof(*d));
   d->device_id = 0x9920;
   d->external_rev = 0x14;
   d->family = FAMILY_CI;
   d->num_shader_engines = 2;
   d->num_shader_arrays_per_engine = 1;
   d->gpu_counter_freq = 100000;
   d->max_engine_clock = 800000;
   d->max_memory_clock = 1375000;
   d->cu_active_number = 18;
   d->cu_bitmap[0][0] = 0x1ff;
   d->cu_bitmap[1][0] = 0x1ff;
   d->enabled_rb_pipes_mask = 0xff;
   d->num_rb_pipes = 8;
   d->num_hw_gfx_contexts = 8;
   d->ids_flags = AMDGPU_IDS_FLAGS_FUSION;
   d->virtual_address_offset = dev->va_start;
   d->virtual_address_max = dev->va_start + PS4_VA_SIZE;
   d->virtual_address_alignment = PS4_PAGE;
   d->pte_fragment_size = PS4_PAGE;
   d->gart_page_size = 4096;
   d->vram_type = AMDGPU_VRAM_TYPE_GDDR5;
   d->vram_bit_width = 256;
   d->wave_front_size = 64;
   d->num_shader_visible_vgprs = 256;
   d->num_cu_per_sh = 9;
   d->num_tcc_blocks = 8;
   d->gs_vgt_table_depth = 32;
   d->gs_prim_buffer_depth = 1792;
   d->max_gs_waves_per_vgt = 32;
}

int
ac_drm_query_info(ac_drm_device *dev, unsigned info_id, unsigned size, void *value)
{
   memset(value, 0, size);
   switch (info_id) {
   case AMDGPU_INFO_DEV_INFO: {
      struct drm_amdgpu_info_device d;
      ps4_dev_info(dev, &d);
      memcpy(value, &d, MIN2(size, sizeof(d)));
      return 0;
   }
   case AMDGPU_INFO_MEMORY: {
      struct drm_amdgpu_memory_info m = {0};
      m.vram.total_heap_size = m.vram.usable_heap_size = PS4_VRAM_SIZE;
      m.vram.max_allocation = PS4_VRAM_SIZE;
      m.cpu_accessible_vram = m.vram;
      m.gtt.total_heap_size = m.gtt.usable_heap_size = PS4_GTT_SIZE;
      m.gtt.max_allocation = PS4_GTT_SIZE;
      memcpy(value, &m, MIN2(size, sizeof(m)));
      return 0;
   }
   case AMDGPU_INFO_VRAM_GTT: {
      struct drm_amdgpu_info_vram_gtt v = {PS4_VRAM_SIZE, PS4_VRAM_SIZE, PS4_GTT_SIZE};
      memcpy(value, &v, MIN2(size, sizeof(v)));
      return 0;
   }
   case AMDGPU_INFO_MAX_IBS: {
      uint32_t *max = value;
      if (size >= 4)
         max[AMDGPU_HW_IP_GFX] = 32;
      return 0;
   }
   case AMDGPU_INFO_TIMESTAMP:
      if (size >= 8)
         *(uint64_t *)value = os_time_get_nano() / 10; /* 100 MHz */
      return 0;
   case AMDGPU_INFO_VRAM_USAGE:
   case AMDGPU_INFO_GTT_USAGE:
   case AMDGPU_INFO_VIS_VRAM_USAGE:
   case AMDGPU_INFO_NUM_EVICTIONS:
   case AMDGPU_INFO_NUM_BYTES_MOVED:
   case AMDGPU_INFO_NUM_VRAM_CPU_PAGE_FAULTS:
      return 0;
   default:
      return -EINVAL;
   }
}

int
ac_drm_read_mm_registers(ac_drm_device *dev, unsigned dword_offset, unsigned count,
                         uint32_t instance, uint32_t flags, uint32_t *values)
{
   for (unsigned i = 0; i < count; i++) {
      const unsigned reg = dword_offset + i;
      if (reg >= 0x2644 && reg < 0x2644 + 32)
         values[i] = ps4_tile_modes[reg - 0x2644];
      else if (reg >= 0x2664 && reg < 0x2664 + 16)
         values[i] = ps4_macrotile_modes[reg - 0x2664];
      else if (reg == 0x263e)
         values[i] = PS4_GB_ADDR_CONFIG;
      else
         return -EINVAL;
   }
   return 0;
}

int
ac_drm_query_hw_ip_count(ac_drm_device *dev, unsigned type, uint32_t *count)
{
   *count = type == AMDGPU_HW_IP_GFX ? 1 : 0;
   return 0;
}

int
ac_drm_query_hw_ip_info(ac_drm_device *dev, unsigned type, unsigned ip_instance,
                        struct drm_amdgpu_info_hw_ip *info)
{
   /* Only the graphics ring: GNM's compute queues and SDMA aren't exposed. */
   if (type != AMDGPU_HW_IP_GFX)
      return -EINVAL;
   memset(info, 0, sizeof(*info));
   info->hw_ip_version_major = 7;
   info->hw_ip_version_minor = 0;
   info->ib_start_alignment = 32;
   info->ib_size_alignment = 32;
   info->available_rings = 1;
   return 0;
}

int
ac_drm_query_firmware_version(ac_drm_device *dev, unsigned fw_type, unsigned ip_instance,
                              unsigned index, uint32_t *version, uint32_t *feature)
{
   /* Recent CIK microcode. */
   *version = 0x1000;
   *feature = 0x40;
   return 0;
}

int
ac_drm_query_uq_fw_area_info(ac_drm_device *dev, unsigned type, unsigned ip_instance,
                             struct drm_amdgpu_info_uq_metadata *info)
{
   return -EINVAL;
}

int
ac_drm_query_heap_info(ac_drm_device *dev, uint32_t heap, uint32_t flags,
                       struct amdgpu_heap_info *info)
{
   memset(info, 0, sizeof(*info));
   info->heap_size = heap == AMDGPU_GEM_DOMAIN_VRAM ? PS4_VRAM_SIZE : PS4_GTT_SIZE;
   info->max_allocation = info->heap_size;
   return 0;
}

int
ac_drm_query_sensor_info(ac_drm_device *dev, unsigned sensor_type, unsigned size, void *value)
{
   return -EINVAL;
}

int
ac_drm_query_video_caps_info(ac_drm_device *dev, unsigned cap_type, unsigned size, void *value)
{
   return -EINVAL;
}

int
ac_drm_query_gpuvm_fault_info(ac_drm_device *dev, unsigned size, void *value)
{
   return -EINVAL;
}

int
ac_drm_vm_reserve_vmid(ac_drm_device *dev, uint32_t flags)
{
   return 0;
}

int
ac_drm_vm_unreserve_vmid(ac_drm_device *dev, uint32_t flags)
{
   return 0;
}

const char *
ac_drm_get_marketing_name(ac_drm_device *device)
{
   return "AMD Liverpool (PS4)";
}

int
ac_drm_query_sw_info(ac_drm_device *dev, enum amdgpu_sw_info info, void *value)
{
   if (info != amdgpu_sw_info_address32_hi)
      return -EINVAL;
   *(uint32_t *)value = dev->va32_start >> 32;
   return 0;
}

int
ac_drm_create_userqueue(ac_drm_device *dev, uint32_t ip_type, uint32_t doorbell_handle,
                        uint32_t doorbell_offset, uint64_t queue_va, uint64_t queue_size,
                        uint64_t wptr_va, uint64_t rptr_va, void *mqd_in, uint32_t flags,
                        uint32_t *queue_id)
{
   return -ENOSYS;
}

int
ac_drm_free_userqueue(ac_drm_device *dev, uint32_t queue_id)
{
   return -ENOSYS;
}

int
ac_drm_userq_signal(ac_drm_device *dev, struct drm_amdgpu_userq_signal *signal_data)
{
   return -ENOSYS;
}

int
ac_drm_userq_wait(ac_drm_device *dev, struct drm_amdgpu_userq_wait *wait_data)
{
   return -ENOSYS;
}

int
ac_drm_query_pci_bus_info(ac_drm_device *dev, struct radeon_info *info)
{
   info->pci.domain = 0;
   info->pci.bus = 0;
   info->pci.dev = 1;
   info->pci.func = 0;
   return 0;
}

void
ac_drm_query_has_vm_always_valid(ac_drm_device *dev, struct radeon_info *info)
{
   info->has_vm_always_valid = true;
}

/* ---------------------------------------------------------------------------------------------
 * libdrm entry points referenced by RADV's DRM-device paths, which never run on the PS4.
 */

#include <xf86drm.h>

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
   return -EINVAL;
}

drmVersionPtr
drmGetVersion(int fd)
{
   return NULL;
}

void
drmFreeVersion(drmVersionPtr version)
{
}

char *
drmGetFormatModifierName(uint64_t modifier)
{
   return NULL;
}

/* Trace points in RADV's startup (PS4_TRACE in the patched Mesa sources). */
void
ac_ps4_trace(const char *fmt, ...)
{
   char line[256];
   va_list args;
   va_start(args, fmt);
   vsnprintf(line, sizeof(line), fmt, args);
   va_end(args);
   ps4_log("%s", line);
}

/* For the app's performance monitor: the last device's submissions and GPU memory. */

void
ac_ps4_get_stats(uint64_t *submitted, uint64_t *completed, uint64_t *bo_bytes, uint64_t *bo_count)
{
   ac_drm_device *dev = ps4_last_device;
   *submitted = dev ? dev->last_seq : 0;
   *completed = dev ? ps4_completed(dev) : 0;
   *bo_bytes = dev ? dev->bo_bytes : 0;
   *bo_count = dev ? dev->bo_count : 0;
}
