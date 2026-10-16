// PSChrome replacement for the OpenOrbis malloc.
//
// The OpenOrbis version maps one fixed 2.5 GB block of flexible memory on the
// first allocation and, when that fails (it does on FW 12.00/13.52), calls
// Sony's allocator with a NULL heap and crashes. This version sizes the heap
// from whatever memory pool the process can actually get and fails cleanly
// (NULL + ENOMEM) instead of crashing.
//
// Every block carries a 16-byte header (base pointer + requested size) so that
// aligned allocations, realloc and malloc_usable_size work without Sony's
// non-exported sceLibcMspaceMallocUsableSize.

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

typedef void* SceLibcMspace;
SceLibcMspace sceLibcMspaceCreate(const char* name, void* base, size_t capacity, unsigned int flags);
void* sceLibcMspaceMalloc(SceLibcMspace msp, size_t size);
int sceLibcMspaceFree(SceLibcMspace msp, void* ptr);

size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t searchStart, int64_t searchEnd, size_t alignment, int64_t* physAddrOut, size_t* sizeOut);
int sceKernelAllocateDirectMemory(int64_t searchStart, int64_t searchEnd, size_t len, size_t alignment, int memoryType, int64_t* physAddrOut);
int sceKernelMapDirectMemory(void** addr, size_t len, int prot, int flags, int64_t directMemoryStart, size_t alignment);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
int sceKernelAvailableFlexibleMemorySize(size_t* sizeOut);
int sceKernelMapNamedFlexibleMemory(void** addr, size_t len, int prot, int flags, const char* name);

#define MB ((size_t)1024 * 1024)
enum { kProtCpuReadWrite = 0x03, kMemoryTypeWbOnion = 0 };

// Direct memory left untouched for the GPU (Piglet compositor needs 512 MB+),
// audio, the system and WebKit's own mappings.
static const size_t kDirectReserve = 2560 * MB;
static const size_t kMaxHeap = 2048 * MB;
static const size_t kMinHeap = 64 * MB;

static SceLibcMspace g_mspace;
static volatile int g_initState;  // 0 = not started, 1 = running, 2 = done
static const char* g_heapSource = "none";
static size_t g_heapSize;
static size_t g_directAvailable;
static size_t g_flexibleAvailable;

// Header placed immediately before every pointer we hand out.
typedef struct {
    void* base;
    size_t size;
} BlockHeader;
_Static_assert(sizeof(BlockHeader) == 16, "header keeps 16-byte alignment");

static int tryDirectMemory(void** base, size_t* size) {
    size_t total = sceKernelGetDirectMemorySize();
    int64_t phys = 0;
    size_t largest = 0;
    if (sceKernelAvailableDirectMemorySize(0, (int64_t)total, 2 * MB, &phys, &largest) != 0)
        return 0;
    g_directAvailable = largest;
    if (largest < kDirectReserve + kMinHeap)
        return 0;
    size_t want = largest - kDirectReserve;
    if (want > kMaxHeap)
        want = kMaxHeap;
    want &= ~(2 * MB - 1);
    if (sceKernelAllocateDirectMemory(0, (int64_t)total, want, 2 * MB, kMemoryTypeWbOnion, &phys) != 0)
        return 0;
    void* addr = NULL;
    if (sceKernelMapDirectMemory(&addr, want, kProtCpuReadWrite, 0, phys, 2 * MB) != 0) {
        sceKernelReleaseDirectMemory(phys, want);
        return 0;
    }
    *base = addr;
    *size = want;
    return 1;
}

static int tryFlexibleMemory(void** base, size_t* size) {
    size_t available = 0;
    if (sceKernelAvailableFlexibleMemorySize(&available) != 0)
        return 0;
    g_flexibleAvailable = available;
    // Leave half for mmap users (WebKit's JavaScript heap, SDL, ...).
    size_t want = (available / 2) & ~(16 * 1024 - 1);
    if (want < kMinHeap)
        return 0;
    void* addr = NULL;
    if (sceKernelMapNamedFlexibleMemory(&addr, want, kProtCpuReadWrite, 0, "DolphinPS4 heap") != 0)
        return 0;
    *base = addr;
    *size = want;
    return 1;
}

static int tryAnonymousMmap(void** base, size_t* size) {
    for (size_t want = 256 * MB; want >= kMinHeap; want /= 2) {
        void* addr = mmap(NULL, want, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (addr != MAP_FAILED) {
            *base = addr;
            *size = want;
            return 1;
        }
    }
    return 0;
}

static void initHeap(void) {
    void* base = NULL;
    size_t size = 0;
    sceKernelAvailableFlexibleMemorySize(&g_flexibleAvailable);
    if (tryDirectMemory(&base, &size))
        g_heapSource = "direct memory";
    else if (tryFlexibleMemory(&base, &size))
        g_heapSource = "flexible memory";
    else if (tryAnonymousMmap(&base, &size))
        g_heapSource = "anonymous mmap";
    else
        return;
    g_mspace = sceLibcMspaceCreate("DolphinPS4", base, size, 0);
    if (g_mspace)
        g_heapSize = size;
    else
        g_heapSource = "mspace creation failed";
}

static SceLibcMspace heap(void) {
    if (__atomic_load_n(&g_initState, __ATOMIC_ACQUIRE) == 2)
        return g_mspace;
    int expected = 0;
    if (__atomic_compare_exchange_n(&g_initState, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        initHeap();
        __atomic_store_n(&g_initState, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&g_initState, __ATOMIC_ACQUIRE) != 2) {
        }
    }
    return g_mspace;
}

static BlockHeader* headerOf(void* ptr) { return (BlockHeader*)ptr - 1; }

static void* allocateAligned(size_t alignment, size_t size) {
    SceLibcMspace msp = heap();
    if (!msp)
        goto outOfMemory;
    if (size == 0)
        size = 1;
    if (alignment < 16)
        alignment = 16;
    size_t extra = sizeof(BlockHeader) + (alignment > 16 ? alignment : 0);
    if (size > SIZE_MAX - extra)
        goto outOfMemory;
    void* base = sceLibcMspaceMalloc(msp, size + extra);
    if (!base)
        goto outOfMemory;
    uintptr_t user = ((uintptr_t)base + sizeof(BlockHeader) + alignment - 1) & ~(uintptr_t)(alignment - 1);
    BlockHeader* header = headerOf((void*)user);
    header->base = base;
    header->size = size;
    return (void*)user;

outOfMemory:
    errno = ENOMEM;
    return NULL;
}

void* malloc(size_t size) { return allocateAligned(16, size); }

void free(void* ptr) {
    if (!ptr || !g_mspace)
        return;
    sceLibcMspaceFree(g_mspace, headerOf(ptr)->base);
}

void* calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    void* ptr = malloc(count * size);
    if (ptr)
        memset(ptr, 0, count * size);
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr)
        return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    size_t old = headerOf(ptr)->size;
    if (size <= old && size >= old / 2) {
        headerOf(ptr)->size = size;
        return ptr;
    }
    void* grown = malloc(size);
    if (!grown)
        return NULL;
    memcpy(grown, ptr, old < size ? old : size);
    free(ptr);
    return grown;
}

void* __memalign(size_t alignment, size_t size) {
    if (alignment & (alignment - 1)) {
        errno = EINVAL;
        return NULL;
    }
    return allocateAligned(alignment, size);
}

void* memalign(size_t alignment, size_t size) { return __memalign(alignment, size); }
void* aligned_alloc(size_t alignment, size_t size) { return __memalign(alignment, size); }

int posix_memalign(void** out, size_t alignment, size_t size) {
    if (alignment < sizeof(void*) || (alignment & (alignment - 1)))
        return EINVAL;
    void* ptr = allocateAligned(alignment, size);
    if (!ptr)
        return ENOMEM;
    *out = ptr;
    return 0;
}

void* valloc(size_t size) { return allocateAligned(16 * 1024, size); }

size_t malloc_usable_size(void* ptr) { return ptr ? headerOf(ptr)->size : 0; }

// OpenOrbis' malloc.o exports these; define them so it is never linked in.
int malloc_init(void) { return heap() ? 0 : 1; }
int malloc_finalize(void) { return 0; }
int __malloc_replaced;

// Diagnostics for test apps.
const char* ps4rt_heap_source(void) { heap(); return g_heapSource; }
size_t ps4rt_heap_size(void) { heap(); return g_heapSize; }
size_t ps4rt_direct_available(void) { heap(); return g_directAvailable; }
size_t ps4rt_flexible_available(void) { heap(); return g_flexibleAvailable; }
