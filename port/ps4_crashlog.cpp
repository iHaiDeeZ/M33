// Crash diagnostics for PS4 test builds.
//
// PS4 coredumps are encrypted and klog is shared with other tools, so the app reports its own
// crashes: a constructor that runs before all others (priority 101) starts
// /data/March33/boot-trace.log and installs a handler for fatal signals that appends the
// signal, fault address, registers and the code addresses found on the stack to
// /data/March33/crash.log. Addresses are printed relative to the start of .text, i.e. as ELF
// virtual addresses: symbolize with `llvm-symbolizer --obj=<elf> 0x...`.
//
// Handlers installed later (Dolphin's fastmem handler) chain to this one for faults they don't
// handle.

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cxxabi.h>
#include <exception>
#include <typeinfo>

extern "C" char __text_start[];  // defined by the OpenOrbis link.x at the start of .text
extern "C" __attribute__((weak)) void ps4_crash_guest_info();  // the emulator's, if it has one

// FreeBSD's stack_t (the OpenOrbis musl header has Linux's field order, which the kernel misreads)
struct BsdSignalStack {
    void* sp;
    size_t size;
    int flags;
};
extern "C" int ps4_sigaltstack(const BsdSignalStack* stack, BsdSignalStack* old) __asm__("sigaltstack");

// The calling thread's own stack for the crash handler: a thread that runs out of stack faults on
// its guard page, and without another stack the handler can't run at all; the system then ends
// the app with error CE-34878-0 and nothing in crash.log. Every thread gets one
// (ps4_runtime.cpp: __wrap_pthread_create; the main thread: earlyInit below).
extern "C" void ps4_install_signal_stack() {
    constexpr size_t kSignalStackSize = 64 * 1024;
    void* stack = malloc(kSignalStackSize);
    if (!stack)
        return;
    const BsdSignalStack signalStack{stack, kSignalStackSize, 0};
    if (ps4_sigaltstack(&signalStack, nullptr) != 0)
        free(stack);
}
extern "C" void ps4_dump_thread_stacks(const char* path, const char* reason);  // ps4_sampler.cpp
// the system's (ps4_runtime.cpp wraps pthread_create with an allocation; a crash may hold the heap)
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

namespace {

// FreeBSD values (the PS4 kernel's); the OpenOrbis musl headers may carry Linux ones.
constexpr int kSigIll = 4, kSigAbrt = 6, kSigFpe = 8, kSigBus = 10, kSigSegv = 11;
constexpr int kSaSiginfo = 0x40, kSaOnstack = 0x1;
constexpr int kMcontextOffset = 0x40;  // measured on hardware
// Registers in FreeBSD mcontext order (after mc_onstack).
constexpr const char* kRegNames[] = {"rdi", "rsi", "rdx", "rcx", "r8",  "r9",  "rax", "rbx",
                                     "rbp", "r10", "r11", "r12", "r13", "r14", "r15"};
constexpr int kRipIndex = 20, kRspIndex = 23;
constexpr uintptr_t kTextWindow = 0x4000000;  // 64 MiB covers the eboot's code

int g_traceFd = -1;
int g_probePipe[2] = {-1, -1};  // readability probe (readable() below)

// Whether 8 bytes at an address can be read, without touching them: the kernel copies them into
// a pipe and reports EFAULT instead of faulting. The crash handler's stack scan used to read past
// the top of a thread's stack, fault again inside the handler, and the system then ended the app
// with nothing in crash.log.
bool readable(const void* address) {
    if (g_probePipe[0] < 0)
        return false;
    if (write(g_probePipe[1], address, 8) != 8)
        return false;
    char drain[8];
    read(g_probePipe[0], drain, sizeof(drain));
    return true;
}

// Opened on first use: the heap may log before this file's constructor runs.
int traceFd() {
    if (g_traceFd < 0) {
        mkdir("/data/March33", 0777);
        g_traceFd = open("/data/March33/boot-trace.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
    return g_traceFd;
}

void writeAll(int fd, const char* text) {
    size_t length = strlen(text);
    while (length > 0) {
        ssize_t written = write(fd, text, length);
        if (written <= 0)
            return;
        text += written;
        length -= static_cast<size_t>(written);
    }
}

__attribute__((format(printf, 2, 3))) void writeFormat(int fd, const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    writeAll(fd, line);
}

bool isCode(uint64_t address) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(__text_start);
    return address >= base && address < base + kTextWindow;
}

uint64_t toElf(uint64_t address) { return address - reinterpret_cast<uintptr_t>(__text_start); }

void setHandler(int sig, void (*handler)(int, siginfo_t*, void*), int flags) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    // The handler union is the first member; its member names differ between headers.
    memcpy(&action, &handler, sizeof(handler));
    action.sa_flags = flags;
    sigemptyset(&action.sa_mask);
    sigaction(sig, &action, nullptr);
}

void crashHandler(int sig, siginfo_t* info, void* context) {
    static volatile int s_depth = 0;
    // Crashed while reporting (typically the stack scan running off the end of a small thread
    // stack): end the process now. Returning re-ran the faulting read forever, leaving the crashed
    // thread spinning and the app hanging on a black screen.
    if (s_depth++ > 0)
        _exit(128 + sig);

    const int fd = open("/data/March33/crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        const auto* regs = reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
        const uint64_t rip = regs[kRipIndex];
        const uint64_t rsp = regs[kRspIndex];
        const uint64_t address = *reinterpret_cast<const uint64_t*>(reinterpret_cast<char*>(info) + 24);
        writeFormat(fd, "\n=== signal %d (code %d), fault address 0x%llx\n", sig,
                    *reinterpret_cast<const int*>(reinterpret_cast<char*>(info) + 8),
                    static_cast<unsigned long long>(address));
        writeFormat(fd, "rip 0x%llx%s elf 0x%llx, rsp 0x%llx, text at %p\n",
                    static_cast<unsigned long long>(rip), isCode(rip) ? "" : " (not eboot code)",
                    static_cast<unsigned long long>(toElf(rip)), static_cast<unsigned long long>(rsp),
                    static_cast<void*>(__text_start));
        for (int i = 0; i < 15; i++)
            writeFormat(fd, "%s 0x%llx%s", kRegNames[i], static_cast<unsigned long long>(regs[1 + i]),
                        i % 4 == 3 ? "\n" : "  ");
        if (address < rsp && rsp - address < 0x10000)
            writeAll(fd, "\nthe fault is just below the stack pointer: this thread ran out of stack\n");
        writeAll(fd, "\ncode addresses on the stack (elf):\n");
        const auto* stack = reinterpret_cast<const uint64_t*>(rsp);
        int found = 0;
        for (int i = 0; i < 2048 && found < 48; i++) {
            // check each new 4 KiB page (and the first word) before reading it
            if ((i == 0 || ((reinterpret_cast<uintptr_t>(&stack[i]) & 0xfff) == 0)) && !readable(&stack[i]))
                break;
            if (isCode(stack[i])) {
                writeFormat(fd, "  [rsp+0x%x] 0x%llx\n", i * 8, static_cast<unsigned long long>(toElf(stack[i])));
                found++;
            }
        }
        close(fd);
    }
    if (traceFd() >= 0)
        writeAll(traceFd(), "crashed (see crash.log)\n");

    // What else to record takes locks and allocates, which a crash may have left held: a timer
    // thread ends the process if it doesn't finish.
    pthread_t timer;
    __real_pthread_create(&timer, nullptr, [](void*) -> void* {
        sleep(15);
        _exit(126);
    }, nullptr);
    // Where every other thread was (stacks, and the lock each one waits on), after the crash.
    ps4_dump_thread_stacks("/data/March33/crash.log", "every thread at the crash");
    // The emulator's view of the crash (M33: the PSP CPU registers, into crash.log).
    if (ps4_crash_guest_info) {
        ps4_crash_guest_info();
        if (traceFd() >= 0)
            writeAll(traceFd(), "PSP CPU state written to crash.log\n");
    }

    // End the process here rather than with the system's crash report: on FW 13.52 that
    // coredump of a homebrew app never finished and hung the console until a hard power-off
    // (DolphinPS4), and the report itself is encrypted.
    _exit(128 + sig);
}

// std::terminate (e.g. an uncaught exception): record what was thrown, then abort as usual.
[[noreturn]] void terminateHandler() {
    char message[512] = "std::terminate called";
    if (const std::type_info* type = abi::__cxa_current_exception_type()) {
        int status = 0;
        char* name = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
        const char* what = "";
        try {
            if (std::exception_ptr current = std::current_exception())
                std::rethrow_exception(current);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        snprintf(message, sizeof(message), "uncaught exception %s: %s", name ? name : type->name(), what);
    }
    const int fd = open("/data/March33/crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        writeFormat(fd, "\n=== %s\n", message);
        close(fd);
    }
    if (traceFd() >= 0)
        writeFormat(traceFd(), "%s\n", message);
    abort();
}

__attribute__((constructor(101))) void earlyInit() {
    if (traceFd() >= 0)
        writeFormat(traceFd(), "static init (text at %p)\n", static_cast<void*>(__text_start));
    std::set_terminate(terminateHandler);
    if (pipe(g_probePipe) == 0) {
        fcntl(g_probePipe[0], F_SETFL, O_NONBLOCK);
        fcntl(g_probePipe[1], F_SETFL, O_NONBLOCK);
    }
    ps4_install_signal_stack();
    const int signals[] = {kSigIll, kSigAbrt, kSigFpe, kSigBus, kSigSegv};
    for (int sig : signals)
        setHandler(sig, crashHandler, kSaSiginfo | kSaOnstack);
}

}  // namespace

// Appends "<what>: callers 0x.. 0x.." to the boot trace: code addresses found on the calling
// thread's stack (ELF addresses, symbolize like crash.log). A cheap "who calls this" without
// frame pointers; may include stale return addresses.
extern "C" void ps4_boot_trace(const char* stage);
extern "C" __attribute__((noinline)) void ps4_trace_callers(const char* what) {
    // Read from the stack pointer itself: walking past a local variable is undefined behaviour
    // the optimizer is free to drop (an earlier version printed nothing).
    const uint64_t* stack;
    asm volatile("mov %%rsp, %0" : "=r"(stack));
    char line[320];
    int len = snprintf(line, sizeof(line), "%s: callers", what);
    int found = 0;
    for (int i = 0; i < 512 && found < 10 && len < 280; i++) {
        if (isCode(stack[i])) {
            len += snprintf(line + len, sizeof(line) - len, " 0x%llx",
                            static_cast<unsigned long long>(toElf(stack[i])));
            found++;
        }
    }
    ps4_boot_trace(line);
}

// Appends a line to /data/March33/boot-trace.log (startup milestones), prefixed with the
// seconds since the first trace line.
extern "C" void ps4_boot_trace(const char* stage) {
    const int fd = traceFd();
    if (fd < 0)
        return;
    static timespec s_start;
    static bool s_started = false;
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!s_started) {
        s_start = now;
        s_started = true;
    }
    const double seconds = static_cast<double>(now.tv_sec - s_start.tv_sec) +
                           static_cast<double>(now.tv_nsec - s_start.tv_nsec) / 1e9;
    // One write per line, so lines from different threads don't interleave.
    // Room for the monitor's long lines; a cut line still ends with its newline (a truncated
    // monitor line used to run into the next one).
    char line[2048];
    const size_t length = strlen(stage);
    const bool newline = length == 0 || stage[length - 1] != '\n';
    const int written =
        snprintf(line, sizeof(line), "[%8.3f] %s%s", seconds, stage, newline ? "\n" : "");
    if (written >= static_cast<int>(sizeof(line)))
        line[sizeof(line) - 2] = '\n';
    writeAll(fd, line);
}

// The network game library's /net paths (ps4_netfs.cpp); everything else as before.
extern "C" int __real_open(const char* path, int flags, ...);
extern "C" int __real_close(int fd);
extern "C" int ps4_netfs_is_path(const char* path);
extern "C" int ps4_netfs_open(const char* path, int flags);
extern "C" int ps4_netfs_close(int fd);

extern "C" __attribute__((noinline)) int __wrap_open(const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    const int mode = va_arg(args, int);  // always passed on: harmless when the call had none
    va_end(args);
    if (ps4_netfs_is_path(path))
        return ps4_netfs_open(path, flags);
    return __real_open(path, flags, mode);
}

extern "C" int __wrap_close(int fd) {
    if (ps4_netfs_close(fd))
        return 0;
    return __real_close(fd);
}
