// Host name lookups on the PS4. The OpenOrbis C library's getaddrinfo and gethostbyname have no
// DNS configuration on the PS4, so names never resolve: online services (Pretendo, through curl
// and the games' own sockets) couldn't find their servers. With the linker's --wrap
// (toolchain/ps4-love-style.cmake) a name goes through libSceNet's resolver, and the address it
// finds goes back to the C library as text, so its results (and freeaddrinfo) stay its own.
// Numeric addresses and empty names go straight to the C library.

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include <mutex>

extern "C" {
int __real_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res);
void ps4_boot_trace(const char* stage);  // ps4_crashlog.cpp

// libSceNet (declared by hand: the OpenOrbis headers pull in their own socket types)
int sceSysmoduleLoadModuleInternal(uint32_t id);
int sceNetInit(void);
int sceNetPoolCreate(const char* name, int size, int flags);
int sceNetResolverCreate(const char* name, int pool, int flags);
int sceNetResolverStartNtoa(int resolver, const char* hostname, uint32_t* address, int timeout_us, int retries,
                            int flags);
int sceNetResolverDestroy(int resolver);
}

namespace {

constexpr uint32_t kSysmoduleNet = 0x8000001C;  // ORBIS_SYSMODULE_INTERNAL_NET

// The name's IPv4 address as text ("a.b.c.d"), from libSceNet's resolver; false if it has none.
bool resolve(const char* name, char* text, size_t size) {
    static std::once_flag s_init;
    static int s_pool = -1;
    std::call_once(s_init, [] {
        sceSysmoduleLoadModuleInternal(kSysmoduleNet);
        sceNetInit();
        s_pool = sceNetPoolCreate("m33-dns", 64 * 1024, 0);
    });
    if (s_pool < 0)
        return false;
    const int resolver = sceNetResolverCreate("m33-dns", s_pool, 0);
    if (resolver < 0)
        return false;
    uint32_t address = 0;  // network byte order
    const int result = sceNetResolverStartNtoa(resolver, name, &address, 5 * 1000 * 1000, 2, 0);
    sceNetResolverDestroy(resolver);
    char line[200];
    if (result < 0) {
        snprintf(line, sizeof(line), "dns: %s not found (%#x)", name, static_cast<unsigned>(result));
        ps4_boot_trace(line);
        return false;
    }
    snprintf(text, size, "%u.%u.%u.%u", address & 0xFF, (address >> 8) & 0xFF, (address >> 16) & 0xFF,
             (address >> 24) & 0xFF);
    snprintf(line, sizeof(line), "dns: %s is %s", name, text);
    ps4_boot_trace(line);
    return true;
}

bool isNumeric(const char* name) {
    in_addr v4;
    in6_addr v6;
    return inet_pton(AF_INET, name, &v4) == 1 || inet_pton(AF_INET6, name, &v6) == 1;
}

}  // namespace

extern "C" int __wrap_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                                  struct addrinfo** res) {
    if (!node || !*node || isNumeric(node) || (hints && (hints->ai_flags & AI_NUMERICHOST)))
        return __real_getaddrinfo(node, service, hints, res);
    char address[64];
    if (!resolve(node, address, sizeof(address)))
        return EAI_NONAME;
    struct addrinfo numeric = {};
    if (hints)
        numeric = *hints;
    numeric.ai_flags |= AI_NUMERICHOST;
    if (numeric.ai_family == AF_UNSPEC)
        numeric.ai_family = AF_INET;
    return __real_getaddrinfo(address, service, &numeric, res);
}

// M33: gethostbyname itself (not wrapped), with a static IPv4 result of its own as the C library's
// is. The OpenOrbis libc.a's gethostbyname imports a function no console module has (create-fself:
// "missing module for symbol"), and with --wrap the linker kept a reference to it even unused.
extern "C" struct hostent* gethostbyname(const char* name) {
    static char s_name[256];
    static in_addr s_address;
    static char* s_addresses[2];
    static char* s_aliases[1];
    static hostent s_host;
    if (!name || !*name)
        return nullptr;
    char address[64];
    if (inet_pton(AF_INET, name, &s_address) != 1) {
        if (!resolve(name, address, sizeof(address)) || inet_pton(AF_INET, address, &s_address) != 1)
            return nullptr;
    }
    snprintf(s_name, sizeof(s_name), "%s", name);
    s_addresses[0] = reinterpret_cast<char*>(&s_address);
    s_addresses[1] = nullptr;
    s_aliases[0] = nullptr;
    s_host.h_name = s_name;
    s_host.h_aliases = s_aliases;
    s_host.h_addrtype = AF_INET;
    s_host.h_length = sizeof(s_address);
    s_host.h_addr_list = s_addresses;
    return &s_host;
}
