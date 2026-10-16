// Sampling profiler for PS4 test builds.
//
// PS4 has no perf/Razor access for homebrew, so the app samples itself: a helper thread sends
// SIGPROF to each registered thread every millisecond, and the handler records the interrupted
// instruction pointer. Every 10 seconds the samples are aggregated and appended to
// /data/March33/samples.log: per thread, the busiest eboot addresses (ELF virtual addresses,
// symbolize with `llvm-symbolizer --obj=<oelf> 0x...`), system module addresses ("sys") and JIT
// code addresses ("jit", 16-byte buckets). Every 60 s the busiest JIT code (64 KiB chunks) is
// copied to /data/March33/jitcode.bin and the emulated MEM1 to mem1.bin, so the generated x86
// and the guest code behind it can be disassembled offline (blocks: the jit-perf map written by
// Common/JitRegister).
//
// Started by ps4_sampler_start() (DolphinNoGUI, ps4.ini profile=on); threads register
// themselves with ps4_sampler_register_thread().

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>

extern "C" char __text_start[];  // defined by the OpenOrbis link.x at the start of .text
extern "C" int pthread_getthreadid_np(void);  // libkernel (FreeBSD libthr)
extern "C" void ps4_boot_trace(const char* stage);

namespace {

constexpr int kSigProf = 27;    // FreeBSD value
constexpr int kSaSiginfo = 0x40, kSaRestart = 0x0002;
constexpr int kMcontextOffset = 0x40;  // measured on hardware (see ps4_crashlog.cpp)
constexpr int kRipIndex = 20, kRspIndex = 23;
// Samples in system libraries (libkernel/libc at 0x800000000+) are attributed to the eboot
// function that called into them: the first eboot code address on the stack, tagged.
constexpr uint64_t kSystemLibs = 0x800000000ULL, kCallerTag = 1ULL << 62;
constexpr uintptr_t kTextWindow = 0x4000000;
constexpr int kMaxThreads = 4;
constexpr int kMaxSamples = 16384;  // per thread and window (10 s at 1 kHz fits)

struct ThreadSamples {
    std::atomic<pthread_t> thread{};
    const char* name = nullptr;
    uint64_t samples[kMaxSamples];
    std::atomic<int> count{0};
};

ThreadSamples g_threads[kMaxThreads];
std::atomic<int> g_thread_count{0};

void profHandler(int, siginfo_t*, void* context) {
    const pthread_t self = pthread_self();
    for (int i = 0; i < g_thread_count.load(std::memory_order_acquire); i++) {
        ThreadSamples& t = g_threads[i];
        if (t.thread.load(std::memory_order_relaxed) != self)
            continue;
        const auto* regs =
            reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
        const int n = t.count.load(std::memory_order_relaxed);
        if (n < kMaxSamples) {
            uint64_t sample = regs[kRipIndex];
            // Inside a system module: keep the exact address (tagged). The module list at the
            // top of samples.log maps it to a module offset, which the decrypted modules' symbol
            // tables name. (Blaming "the first eboot address on the stack" picked up stale
            // values and named the wrong callers.)
            if (sample >= kSystemLibs && sample < 2 * kSystemLibs)
                sample |= kCallerTag;
            t.samples[n] = sample;
            t.count.store(n + 1, std::memory_order_release);
        }
        return;
    }
}

void writeLine(int fd, const char* text) {
    size_t length = strlen(text);
    while (length > 0) {
        const ssize_t written = write(fd, text, length);
        if (written <= 0)
            return;
        text += written;
        length -= static_cast<size_t>(written);
    }
}

void noteHotJit(uint64_t address, int count);

// Aggregates one thread's window: sorts the samples in place and prints the top entries.
void report(int fd, ThreadSamples& t, double window_start) {
    // Copy first: the handler keeps writing into t.samples once count is reset.
    static uint64_t s[kMaxSamples];
    const int n = std::min(t.count.load(std::memory_order_acquire), kMaxSamples);
    memcpy(s, t.samples, sizeof(uint64_t) * n);
    t.count.store(0, std::memory_order_release);
    if (n == 0)
        return;
    const uintptr_t text = reinterpret_cast<uintptr_t>(__text_start);

    // Eboot and JIT code: bucket by 16 bytes (eboot addresses relative to .text; JIT addresses
    // tagged with bit 63).
    for (int i = 0; i < n; i++) {
        if (s[i] & kCallerTag) {
            s[i] &= ~0xFULL;  // system module address (see the module list)
            continue;
        }
        const bool eboot = s[i] >= text && s[i] < text + kTextWindow;
        s[i] = eboot ? ((s[i] - text) & ~0xFULL) : (s[i] | (1ULL << 63)) & ~0xFULL;
    }
    std::sort(s, s + n);
    struct Entry { uint64_t key; int count; };
    static Entry entries[kMaxSamples];
    int unique = 0, eboot_samples = 0;
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && s[j] == s[i])
            j++;
        entries[unique++] = {s[i], j - i};
        if (!(s[i] >> 62))
            eboot_samples += j - i;
        i = j;
    }
    std::sort(entries, entries + unique, [](const Entry& a, const Entry& b) { return a.count > b.count; });

    char line[256];
    snprintf(line, sizeof(line), "== %.0f s, %s: %d samples, %d in eboot code (%.0f%%)\n",
             window_start, t.name, n, eboot_samples, 100.0 * eboot_samples / n);
    writeLine(fd, line);
    // Every bucket seen at least twice (up to 1500): a function spread over many 16-byte buckets
    // fell below a top-60 cut, hiding about half of a busy thread. Aggregated per function
    // offline (symbolizer, JIT block map).
    for (int i = 0; i < unique && i < 1500 && entries[i].count >= 2; i++) {
        if (entries[i].key >> 63) {
            const uint64_t address = entries[i].key & ~(1ULL << 63);
            snprintf(line, sizeof(line), "  %5.1f%% jit 0x%llx\n", 100.0 * entries[i].count / n,
                     static_cast<unsigned long long>(address));
            noteHotJit(address, entries[i].count);
        }
        else if (entries[i].key & kCallerTag)
            snprintf(line, sizeof(line), "  %5.1f%% sys 0x%llx\n",
                     100.0 * entries[i].count / n,
                     static_cast<unsigned long long>(entries[i].key & ~kCallerTag));
        else
            snprintf(line, sizeof(line), "  %5.1f%% elf 0x%llx\n", 100.0 * entries[i].count / n,
                     static_cast<unsigned long long>(entries[i].key));
        writeLine(fd, line);
    }
}

// 64 KiB chunks of JIT code sampled since the last dump (dumpJitCode).
constexpr int kMaxHotChunks = 96;
uint64_t g_hot_chunks[kMaxHotChunks];
int g_hot_chunk_count = 0;

void noteHotJit(uint64_t address, int count) {
    if (count < 3)
        return;
    const uint64_t chunk = address & ~0xFFFFULL;
    for (int i = 0; i < g_hot_chunk_count; i++) {
        if (g_hot_chunks[i] == chunk)
            return;
    }
    if (g_hot_chunk_count < kMaxHotChunks)
        g_hot_chunks[g_hot_chunk_count++] = chunk;
}

double now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// Loaded modules and their segments, so "sys" samples can be mapped to module offsets.
struct ModuleSegment {
    void* address;
    uint32_t size;
    int32_t prot;
};
struct ModuleInfo {
    size_t size;
    char name[256];
    ModuleSegment segments[4];
    uint32_t segment_count;
    uint8_t fingerprint[20];
};
extern "C" int32_t sceKernelGetModuleList(int32_t* handles, size_t count, size_t* available);
extern "C" int32_t sceKernelGetModuleInfo(int32_t handle, ModuleInfo* info);

void writeModuleList(int fd) {
    int32_t handles[256];
    size_t count = 0;
    static size_t s_listed = 0;  // relisted when modules get loaded (e.g. RADV's GnmDriver)
    if (sceKernelGetModuleList(handles, 256, &count) != 0 || count == s_listed)
        return;
    s_listed = count;
    char line[512];
    for (size_t m = 0; m < count && m < 256; m++) {
        ModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(handles[m], &info) != 0)
            continue;
        int len = snprintf(line, sizeof(line), "module %s", info.name);
        for (uint32_t g = 0; g < info.segment_count && g < 4; g++)
            len += snprintf(line + len, sizeof(line) - len, " seg %p+0x%x/%d",
                            info.segments[g].address, info.segments[g].size, info.segments[g].prot);
        snprintf(line + len, sizeof(line) - len, "\n");
        writeLine(fd, line);
    }
}

}  // namespace

extern "C" int32_t sceKernelQueryMemoryProtection(void* address, void** start, void** end,
                                                  uint32_t* protection);
// Emulated MEM1 (set by Core/HW/Memmap.cpp), dumped with the JIT code.
extern "C" void* ps4_profile_guest_ram;
extern "C" uint32_t ps4_profile_guest_ram_size;
extern "C" {
void* ps4_profile_guest_ram = nullptr;
uint32_t ps4_profile_guest_ram_size = 0;
}

namespace {

bool isReadable(uint64_t address, uint64_t size) {
    void* start = nullptr;
    void* end = nullptr;
    uint32_t protection = 0;
    return sceKernelQueryMemoryProtection(reinterpret_cast<void*>(address), &start, &end,
                                          &protection) == 0 &&
           (protection & 1) && address + size <= reinterpret_cast<uintptr_t>(end);
}

// jitcode.bin: records of {u64 address, u64 size, bytes}. Copied through a buffer first so the
// kernel never reads JIT memory for the file write.
void dumpJitCode() {
    const int fd = open("/data/March33/jitcode.bin", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        static uint8_t buffer[0x10000];
        for (int i = 0; i < g_hot_chunk_count; i++) {
            if (!isReadable(g_hot_chunks[i], sizeof(buffer)))
                continue;
            memcpy(buffer, reinterpret_cast<const void*>(g_hot_chunks[i]), sizeof(buffer));
            const uint64_t header[2] = {g_hot_chunks[i], sizeof(buffer)};
            write(fd, header, sizeof(header));
            write(fd, buffer, sizeof(buffer));
        }
        close(fd);
    }
    g_hot_chunk_count = 0;
    if (ps4_profile_guest_ram && ps4_profile_guest_ram_size) {
        const int ram = open("/data/March33/mem1.bin", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (ram >= 0) {
            static uint8_t copy[0x100000];
            for (uint32_t offset = 0; offset < ps4_profile_guest_ram_size; offset += sizeof(copy)) {
                const uint32_t size =
                    std::min<uint32_t>(sizeof(copy), ps4_profile_guest_ram_size - offset);
                memcpy(copy, static_cast<const uint8_t*>(ps4_profile_guest_ram) + offset, size);
                write(ram, copy, size);
            }
            close(ram);
        }
    }
}

std::atomic<bool> g_sampler_on{false};

void* samplerThread(void*) {
    const int fd = open("/data/March33/samples.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return nullptr;
    writeModuleList(fd);
    const double start = now();
    double window_start = start, last_dump = start;
    const timespec period = {0, 1000000}, paused = {0, 100000000};
    for (;;) {
        // Profiler switched off in the menus: no signals, no new windows until it is back on.
        if (!g_sampler_on.load(std::memory_order_relaxed)) {
            nanosleep(&paused, nullptr);
            window_start = now();
            continue;
        }
        nanosleep(&period, nullptr);
        const int threads = g_thread_count.load(std::memory_order_acquire);
        for (int i = 0; i < threads; i++)
            pthread_kill(g_threads[i].thread.load(std::memory_order_relaxed), kSigProf);
        if (now() - window_start >= 10.0) {
            writeModuleList(fd);
            for (int i = 0; i < threads; i++)
                report(fd, g_threads[i], window_start - start);
            window_start = now();
            if (window_start - last_dump >= 60.0) {
                dumpJitCode();
                last_dump = window_start = now();
                writeLine(fd, "(jitcode.bin and mem1.bin updated)\n");
            }
        }
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Hang dumps: every thread that matters registers here (ps4_watch_thread, also done by
// ps4_sampler_register_thread). ps4_dump_thread_stacks() signals each one; the handler records
// its instruction pointer and every eboot code address on its stack (return addresses, newest
// first). Used by DolphinNoGUI's watchdog when emulation stops advancing.
namespace {
constexpr int kSigDump = 30;  // SIGUSR1 (FreeBSD)
constexpr int kMaxWatched = 96, kMaxFrames = 48;  // every thread of the app (pthread_create wrap)
// A thread away from its registered stack (Cemu's CPU threads run the emulated threads on fiber
// stacks): this much above rsp is scanned, within its mapping.
constexpr uintptr_t kForeignStackScan = 16 * 1024;

struct WatchedThread {
    std::atomic<pthread_t> thread{};
    char name[32] = {};  // copied: some callers pass temporaries
    // Highest stack address the dump may read: the thread's stack position when it registered
    // (plus its caller's frame). Reading a fixed amount above rsp ran off the top of small thread
    // stacks (the Wiimote scanner's) and crashed the app in the middle of a hang dump.
    uintptr_t stack_limit = 0;
    int tid = 0;  // kernel thread id (pthread_getthreadid_np): mutex objects record their owner's
    uint64_t rip = 0;
    uint64_t frames[kMaxFrames];
    int frame_count = 0;
    // Blocked in _umtx_op (the kernel's wait for mutexes, condition variables, semaphores): its
    // arguments - object, operation, value, second object (condition variable waits: the mutex) -
    // and the first 16 bytes of both objects (a FreeBSD mutex starts with its owner's thread id).
    bool in_umtx = false;
    uint64_t umtx_obj = 0, umtx_op = 0, umtx_val = 0, umtx_obj2 = 0;
    uint32_t umtx_words[4] = {}, umtx_words2[4] = {};
    std::atomic<int> done{0};
};
WatchedThread g_watched[kMaxWatched];
std::atomic<int> g_watched_count{0};

void dumpHandler(int, siginfo_t*, void* context) {
    const pthread_t self = pthread_self();
    const uintptr_t text = reinterpret_cast<uintptr_t>(__text_start);
    for (int i = 0; i < g_watched_count.load(std::memory_order_acquire); i++) {
        WatchedThread& t = g_watched[i];
        if (t.thread.load(std::memory_order_relaxed) != self)
            continue;
        const auto* regs =
            reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
        t.rip = regs[kRipIndex];
        // libkernel's _umtx_op stub: "mov $0x1c6, %rax; mov %rcx, %r10; syscall" (12 bytes), the
        // interrupted thread sits right after the syscall. FreeBSD mcontext: rdi 1, rsi 2, rdx 3,
        // r10 10. Only then are the argument registers known to be the wait's.
        t.in_umtx = false;
        if (t.rip >= kSystemLibs && t.rip < 2 * kSystemLibs) {
            const auto* code = reinterpret_cast<const uint8_t*>(t.rip - 12);
            static const uint8_t kUmtxStub[12] = {0x48, 0xc7, 0xc0, 0xc6, 0x01, 0x00,
                                                  0x00, 0x49, 0x89, 0xca, 0x0f, 0x05};
            if (memcmp(code, kUmtxStub, sizeof(kUmtxStub)) == 0) {
                // Registers only: the objects are read by the dumping thread, after checking that
                // they are mapped (a register that wasn't a pointer crashed v02.37 here).
                t.in_umtx = true;
                t.umtx_obj = regs[1];
                t.umtx_op = regs[2];
                t.umtx_val = regs[3];
                t.umtx_obj2 = regs[10];
            }
        }
        // Only scan a stack that is plausibly this registration's: a thread that registered and
        // exited can leave its pthread handle to a new thread, whose stack is elsewhere (a v02.34
        // hang dump crashed scanning from one thread's rsp to another's stack limit).
        const uint64_t rsp = regs[kRspIndex];
        const bool same_thread = pthread_getthreadid_np() == t.tid;
        const bool sane = t.stack_limit > rsp && t.stack_limit - rsp <= (1u << 20);
        const auto* stack = reinterpret_cast<const uint64_t*>(rsp);
        uintptr_t limit_address = rsp;
        if (same_thread && sane) {
            limit_address = t.stack_limit;
        } else if (same_thread) {
            void* start = nullptr;
            void* end = nullptr;
            uint32_t protection = 0;
            if (sceKernelQueryMemoryProtection(reinterpret_cast<void*>(rsp), &start, &end,
                                               &protection) == 0 && (protection & 1))
                limit_address = std::min<uintptr_t>(rsp + kForeignStackScan,
                                                    reinterpret_cast<uintptr_t>(end));
        }
        const auto* limit = reinterpret_cast<const uint64_t*>(limit_address);
        int n = 0;
        for (const uint64_t* p = stack; p < limit && n < kMaxFrames; p++) {
            if (*p >= text && *p < text + kTextWindow)
                t.frames[n++] = *p - text;
        }
        t.frame_count = n;
        t.done.store(1, std::memory_order_release);
        return;
    }
}

void installDumpHandler() {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true))
        return;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    void (*handler)(int, siginfo_t*, void*) = dumpHandler;
    memcpy(&action, &handler, sizeof(handler));
    action.sa_flags = kSaSiginfo | kSaRestart;
    sigemptyset(&action.sa_mask);
    sigaction(kSigDump, &action, nullptr);
}
}  // namespace

extern "C" void ps4_heap_private_arena(const char* name);  // ps4_runtime.cpp

extern "C" void ps4_watch_thread(const char* name) {
    installDumpHandler();
    ps4_heap_private_arena(name);
    const pthread_t self = pthread_self();
    for (int i = 0; i < std::min(g_watched_count.load(), kMaxWatched); i++) {
        if (g_watched[i].thread.load() == self) {
            // already watched (every thread is, from its start): the name it gives itself
            if (name)
                snprintf(g_watched[i].name, sizeof(g_watched[i].name), "%s", name);
            return;
        }
    }
    const int index = g_watched_count.fetch_add(1);
    if (index >= kMaxWatched)
        return;
    snprintf(g_watched[index].name, sizeof(g_watched[index].name), "%s", name ? name : "?");
    // Up to and including this function's own return address (frame + 16): threads that register
    // first thing (e.g. the analytics reporter) sit within 256 bytes of their stack's top, and a
    // larger margin read past it. Where a thread is stuck lies deeper than this anyway.
    g_watched[index].stack_limit = reinterpret_cast<uintptr_t>(__builtin_frame_address(0)) + 16;
    g_watched[index].tid = pthread_getthreadid_np();
    g_watched[index].thread.store(self);
}

extern "C" int32_t sceKernelQueryMemoryProtection(void* address, void** start, void** end,
                                                  uint32_t* protection);

// Copies 16 bytes at `address` if they lie in readable memory (else leaves zeros).
static void readIfMapped(uint64_t address, uint32_t (&words)[4]) {
    memset(words, 0, sizeof(words));
    void* start = nullptr;
    void* end = nullptr;
    uint32_t protection = 0;
    if (address < 0x10000 ||
        sceKernelQueryMemoryProtection(reinterpret_cast<void*>(address), &start, &end,
                                       &protection) != 0 ||
        !(protection & 1) || address + 16 > reinterpret_cast<uintptr_t>(end))
        return;
    memcpy(words, reinterpret_cast<const void*>(address), sizeof(words));
}

// Writes every watched thread's stack to `path` (appending), headed by `reason`.
extern "C" void ps4_dump_thread_stacks(const char* path, const char* reason) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;
    char line[256];
    snprintf(line, sizeof(line), "==== %s (eboot addresses relative to .text, symbolize with "
                                 "llvm-symbolizer --obj=<oelf>)\n", reason);
    writeLine(fd, line);
    const uintptr_t text = reinterpret_cast<uintptr_t>(__text_start);
    const int count = std::min(g_watched_count.load(std::memory_order_acquire), kMaxWatched);
    for (int i = 0; i < count; i++) {
        WatchedThread& t = g_watched[i];
        t.done.store(0);
        if (pthread_kill(t.thread.load(), kSigDump) != 0) {
            snprintf(line, sizeof(line), "-- %s: signal failed (thread gone?)\n", t.name);
            writeLine(fd, line);
            continue;
        }
        const timespec pause = {0, 1000000};
        for (int w = 0; w < 200 && !t.done.load(std::memory_order_acquire); w++)
            nanosleep(&pause, nullptr);
        if (!t.done.load(std::memory_order_acquire)) {
            snprintf(line, sizeof(line), "-- %s: no answer (signals blocked?)\n", t.name);
            writeLine(fd, line);
            continue;
        }
        const bool in_eboot = t.rip >= text && t.rip < text + kTextWindow;
        snprintf(line, sizeof(line), "-- %s (thread id %d): rip %s0x%llx\n", t.name, t.tid,
                 in_eboot ? "elf " : "(system/JIT) ",
                 static_cast<unsigned long long>(in_eboot ? t.rip - text : t.rip));
        writeLine(fd, line);
        if (t.in_umtx) {
            readIfMapped(t.umtx_obj, t.umtx_words);
            readIfMapped(t.umtx_obj2, t.umtx_words2);
            snprintf(line, sizeof(line),
                     "   blocked in _umtx_op(obj %#llx, op %llu, val %#llx, obj2 %#llx); obj: %08x "
                     "%08x %08x %08x; obj2: %08x %08x %08x %08x\n",
                     static_cast<unsigned long long>(t.umtx_obj),
                     static_cast<unsigned long long>(t.umtx_op),
                     static_cast<unsigned long long>(t.umtx_val),
                     static_cast<unsigned long long>(t.umtx_obj2), t.umtx_words[0], t.umtx_words[1],
                     t.umtx_words[2], t.umtx_words[3], t.umtx_words2[0], t.umtx_words2[1],
                     t.umtx_words2[2], t.umtx_words2[3]);
            writeLine(fd, line);
        }
        writeLine(fd, "   stack:");
        for (int f = 0; f < t.frame_count; f++) {
            snprintf(line, sizeof(line), " 0x%llx", static_cast<unsigned long long>(t.frames[f]));
            writeLine(fd, line);
        }
        writeLine(fd, "\n");
    }
    close(fd);
}

// Per-thread CPU time for the monitor: busy threads (CPU thread 0, video thread 1) read their own
// CPU clock now and then; the monitor divides the growth by wall time. Shows how close a thread
// is to its limit and whether something else shares its core (the sampler can't see that).
namespace {
constexpr clockid_t kThreadCpuClock = 14;  // FreeBSD CLOCK_THREAD_CPUTIME_ID
std::atomic<uint64_t> g_thread_cpu_ns[2];
}  // namespace

extern "C" void ps4_thread_cpu_sample(int slot) {
    timespec ts;
    if (slot < 0 || slot > 1 || clock_gettime(kThreadCpuClock, &ts) != 0)
        return;
    g_thread_cpu_ns[slot].store(static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                                    static_cast<uint64_t>(ts.tv_nsec),
                                std::memory_order_relaxed);
}

// Wait-time counters for the monitor (ns): 0 CPU thread waiting for the GPU thread (FlushGpu),
// 1 GPU loop sleeping for lack of work, 2 video thread waiting for the Vulkan recording thread,
// 3 vkWaitForFences.
namespace {
std::atomic<long long> g_wait_ns[6];  // 4: video-thread pipeline compile ns, 5: their count
}  // namespace

extern "C" void ps4_wait_add(int slot, long long ns) {
    if (slot >= 0 && slot < 6 && ns > 0)
        g_wait_ns[slot].fetch_add(ns, std::memory_order_relaxed);
}

extern "C" long long ps4_wait_total(int slot) {
    return slot >= 0 && slot < 6 ? g_wait_ns[slot].load(std::memory_order_relaxed) : 0;
}

extern "C" double ps4_thread_cpu_seconds(int slot) {
    if (slot < 0 || slot > 1)
        return 0;
    return g_thread_cpu_ns[slot].load(std::memory_order_relaxed) / 1e9;
}

// The PS4 doesn't let a thread read another thread's CPU clock (pthread_getcpuclockid fails), so
// busy threads report their own: ps4_thread_cpu_tick() in their loops stores the thread's CPU
// time in its watched slot, at most every ~10 ms (rdtsc check; the clock read is a system call).
namespace {
std::atomic<uint64_t> g_watched_cpu_ns[kMaxWatched];
}  // namespace

extern "C" void ps4_thread_cpu_tick() {
    static thread_local int t_slot = -2;
    static thread_local uint64_t t_last = 0;
    const uint64_t now = __builtin_ia32_rdtsc();
    if (now - t_last < 16000000)  // ~10 ms at 1.6 GHz
        return;
    t_last = now;
    if (t_slot == -2) {
        t_slot = -1;
        const pthread_t self = pthread_self();
        for (int i = 0; i < std::min(g_watched_count.load(), kMaxWatched); i++) {
            if (g_watched[i].thread.load() == self)
                t_slot = i;
        }
    }
    timespec ts;
    if (t_slot >= 0 && clock_gettime(kThreadCpuClock, &ts) == 0) {
        g_watched_cpu_ns[t_slot].store(static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec,
                                       std::memory_order_relaxed);
    }
}

// How busy each watched thread was since the previous call: its self-reported CPU time
// (ps4_thread_cpu_tick), else its CPU clock read from here (pthread_getcpuclockid, where the
// system allows it). "name N%" for those at or above min_percent, busiest first, into `out`.
// One caller (the monitor); no allocations.
extern "C" void ps4_thread_busy_report(char* out, size_t size, int min_percent) {
    static uint64_t s_prev_cpu[kMaxWatched];
    static uint64_t s_prev_wall = 0;
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t wall = static_cast<uint64_t>(now.tv_sec) * 1000000000ull + now.tv_nsec;
    const uint64_t elapsed = s_prev_wall ? wall - s_prev_wall : 0;
    s_prev_wall = wall;
    int percents[kMaxWatched];
    const int count = std::min(g_watched_count.load(std::memory_order_acquire), kMaxWatched);
    for (int i = 0; i < count; i++) {
        percents[i] = -1;
        uint64_t ns = g_watched_cpu_ns[i].load(std::memory_order_relaxed);
        if (ns == 0) {
            clockid_t clock;
            timespec cpu;
            if (pthread_getcpuclockid(g_watched[i].thread.load(), &clock) != 0 ||
                clock_gettime(clock, &cpu) != 0)
                continue;
            ns = static_cast<uint64_t>(cpu.tv_sec) * 1000000000ull + cpu.tv_nsec;
        }
        if (elapsed && s_prev_cpu[i] && ns >= s_prev_cpu[i])
            percents[i] = static_cast<int>((ns - s_prev_cpu[i]) * 100 / elapsed);
        s_prev_cpu[i] = ns;
    }
    size_t length = 0;
    out[0] = 0;
    for (;;) {
        int best = -1;
        for (int i = 0; i < count; i++)
            if (percents[i] >= min_percent && (best < 0 || percents[i] > percents[best]))
                best = i;
        if (best < 0 || length + 40 >= size)
            break;
        length += snprintf(out + length, size - length, "%s%s %d%%", length ? ", " : "",
                           g_watched[best].name, percents[best]);
        percents[best] = -1;
    }
}

extern "C" void ps4_sampler_register_thread(const char* name) {
    ps4_watch_thread(name);
    // a thread that is started again (M33: the emulation thread at every game start) takes
    // over its predecessor's slot instead of a new one
    for (int i = 0; i < g_thread_count.load(); i++) {
        if (g_threads[i].name && name && strcmp(g_threads[i].name, name) == 0) {
            g_threads[i].thread.store(pthread_self());
            return;
        }
    }
    const int index = g_thread_count.load();
    if (index >= kMaxThreads)
        return;
    g_threads[index].name = name;
    g_threads[index].thread.store(pthread_self());
    g_thread_count.store(index + 1, std::memory_order_release);
}

extern "C" void ps4_sampler_start() {
    static std::atomic<bool> s_started{false};
    g_sampler_on.store(true);
    if (s_started.exchange(true))
        return;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    void (*handler)(int, siginfo_t*, void*) = profHandler;
    memcpy(&action, &handler, sizeof(handler));  // the handler union's member names differ
    action.sa_flags = kSaSiginfo | kSaRestart;
    sigemptyset(&action.sa_mask);
    sigaction(kSigProf, &action, nullptr);

    pthread_t thread;
    if (pthread_create(&thread, nullptr, samplerThread, nullptr) == 0)
        ps4_boot_trace("sampler: started, writing /data/March33/samples.log every 10 s");
}

// The menus' Profiler switch: starts the sampler the first time, then pauses / resumes it.
extern "C" void ps4_sampler_set_enabled(int on) {
    if (on)
        ps4_sampler_start();
    else if (g_sampler_on.exchange(false))
        ps4_boot_trace("sampler: paused");
}
