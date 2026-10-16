// Network game library: SMB and NFS shares, read-only, under /net/<name>/... Below the C library:
// open, read, pread, lseek, fstat, stat, getdents and close of those paths are answered here
// (open and close from ps4_crashlog.cpp's wraps, the rest through the linker's --wrap,
// toolchain/ps4-love-style.cmake), so whatever reads files - std::ifstream (ZArchive's .wua),
// FileStream (.wud/.wux, unpacked games), std::filesystem (the launcher's scan) - reads the share
// as it reads /data. SMB through libsmb2, NFS through libnfs (Ronnie Sahlberg, LGPL-2.1).
//
// A network file's descriptor is a dup() of one socket: the number is really taken, and a call not
// answered here (an ioctl) fails on the socket instead of touching another file.
//
// Each share has one connection, one request at a time. What a folder holds and a file's size are
// kept for a minute (a game scan asks again and again); file data goes through 256 KiB blocks
// shared by all files (12 MiB), except large reads, which go straight to the caller.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <nfsc/libnfs.h>
}

extern "C" {
void ps4_boot_trace(const char* stage);  // ps4_crashlog.cpp
int __real_close(int fd);
ssize_t __real_read(int fd, void* buffer, size_t size);
ssize_t __real__read(int fd, void* buffer, size_t size);
ssize_t __real_readv(int fd, const struct iovec* iov, int count);
ssize_t __real__readv(int fd, const struct iovec* iov, int count);
ssize_t __real_pread(int fd, void* buffer, size_t size, off_t offset);
off_t __real_lseek(int fd, off_t offset, int whence);
int __real_fstat(int fd, struct stat* st);
int __real_stat(const char* path, struct stat* st);
int __real_getdents(int fd, char* buffer, size_t size);
}

namespace {

constexpr uint32_t kBlockSize = 256 * 1024;
constexpr size_t kCacheBlocks = 48;
constexpr size_t kDirectRead = 1024 * 1024;  // and up: not through the blocks
constexpr uint64_t kInfoSeconds = 60;
constexpr uint64_t kRetrySeconds = 10;       // after a failed connection, answer "not there" at once
constexpr int kMaxFds = 8192;

uint64_t nowSeconds() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec);
}

void trace(const std::string& line) {
    ps4_boot_trace(("netfs: " + line).c_str());
}

struct Info {
    bool exists = false;
    bool dir = false;
    uint64_t size = 0;
    uint64_t mtime = 0;
};

struct Entry {
    std::string name;
    Info info;
};

// Whether host answers on port within 3 s: an unreachable server fails at once instead of after
// the system's connection timeout (the launcher's scan waits for it).
bool answers(const std::string& host, int port, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
        error = "can't find " + host;
        return false;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    bool ok = false;
    if (fd >= 0) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        if (::connect(fd, found->ai_addr, found->ai_addrlen) == 0) {
            ok = true;
        } else if (errno == EINPROGRESS) {
            pollfd wait{fd, POLLOUT, 0};
            int failure = 0;
            socklen_t length = sizeof(failure);
            ok = poll(&wait, 1, 3000) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &failure, &length) == 0 && failure == 0;
        }
        __real_close(fd);
    }
    freeaddrinfo(found);
    if (!ok)
        error = host + " doesn't answer (port " + std::to_string(port) + ")";
    return ok;
}

// One share's connection. Paths are inside the share's folder, "/" first ("/" itself: the folder).
// Calls return 0 or -errno; the caller holds the share's lock.
class Share {
public:
    virtual ~Share() = default;
    virtual int connect(std::string& error) = 0;
    virtual void disconnect() = 0;
    virtual int stat(const std::string& path, Info& info) = 0;
    virtual int list(const std::string& path, std::vector<Entry>& entries) = 0;
    virtual int open(const std::string& path, void*& file) = 0;
    virtual int64_t pread(void* file, void* buffer, uint32_t size, uint64_t offset) = 0;
    virtual void close(void* file) = 0;
};

// smb://host/share[/folder], with a user name and password (none: guest)
class SmbShare : public Share {
public:
    SmbShare(std::string url, std::string user, std::string password)
        : m_url(std::move(url)), m_user(std::move(user)), m_password(std::move(password)) {}
    ~SmbShare() override { disconnect(); }

    int connect(std::string& error) override {
        disconnect();
        m_smb2 = smb2_init_context();
        if (!m_smb2) {
            error = "out of memory";
            return -ENOMEM;
        }
        smb2_url* url = smb2_parse_url(m_smb2, m_url.c_str());
        if (!url) {
            error = std::string("bad address: ") + smb2_get_error(m_smb2);
            disconnect();
            return -EINVAL;
        }
        if (!url->share || !*url->share) {
            error = "no shared folder in the address (host/share)";
            smb2_destroy_url(url);
            disconnect();
            return -EINVAL;
        }
        if (!answers(url->server, 445, error)) {
            smb2_destroy_url(url);
            disconnect();
            return -EHOSTUNREACH;
        }
        const std::string user = !m_user.empty() ? m_user : url->user ? url->user : "guest";
        smb2_set_user(m_smb2, user.c_str());
        smb2_set_password(m_smb2, m_password.c_str());
        if (url->domain)
            smb2_set_domain(m_smb2, url->domain);
        smb2_set_security_mode(m_smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
        smb2_set_timeout(m_smb2, 10);
        m_folder = url->path ? url->path : "";
        while (!m_folder.empty() && m_folder.back() == '/')
            m_folder.pop_back();
        const int result = smb2_connect_share(m_smb2, url->server, url->share, user.c_str());
        smb2_destroy_url(url);
        if (result < 0) {
            error = smb2_get_error(m_smb2);
            disconnect();
            return result;
        }
        return 0;
    }

    void disconnect() override {
        if (!m_smb2)
            return;
        smb2_disconnect_share(m_smb2);
        smb2_destroy_context(m_smb2);
        m_smb2 = nullptr;
    }

    int stat(const std::string& path, Info& info) override {
        smb2_stat_64 st{};
        const int result = smb2_stat(m_smb2, inShare(path).c_str(), &st);
        if (result < 0)
            return result;
        info = toInfo(st);
        return 0;
    }

    int list(const std::string& path, std::vector<Entry>& entries) override {
        smb2dir* dir = smb2_opendir(m_smb2, inShare(path).c_str());
        if (!dir)
            return lastErrno();
        while (smb2dirent* entry = smb2_readdir(m_smb2, dir)) {
            if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0)
                continue;
            entries.push_back({entry->name, toInfo(entry->st)});
        }
        smb2_closedir(m_smb2, dir);
        return 0;
    }

    int open(const std::string& path, void*& file) override {
        file = smb2_open(m_smb2, inShare(path).c_str(), O_RDONLY);
        return file ? 0 : lastErrno();
    }

    int64_t pread(void* file, void* buffer, uint32_t size, uint64_t offset) override {
        const uint32_t most = smb2_get_max_read_size(m_smb2);
        return smb2_pread(m_smb2, static_cast<smb2fh*>(file), static_cast<uint8_t*>(buffer),
                          most ? std::min(size, most) : size, offset);
    }

    void close(void* file) override {
        if (m_smb2)
            smb2_close(m_smb2, static_cast<smb2fh*>(file));
    }

private:
    // libsmb2 takes paths without the leading slash
    std::string inShare(const std::string& path) const {
        std::string full = m_folder + path;
        size_t start = 0;
        while (start < full.size() && full[start] == '/')
            start++;
        full.erase(0, start);
        while (!full.empty() && full.back() == '/')
            full.pop_back();
        return full;
    }

    int lastErrno() const {
        const int status = smb2_get_nterror(m_smb2);
        const int error = status ? nterror_to_errno(status) : EIO;
        return -(error ? error : EIO);
    }

    static Info toInfo(const smb2_stat_64& st) {
        Info info;
        info.exists = true;
        info.dir = st.smb2_type == SMB2_TYPE_DIRECTORY;
        info.size = info.dir ? 0 : st.smb2_size;
        info.mtime = st.smb2_mtime;
        return info;
    }

    std::string m_url, m_user, m_password, m_folder;
    smb2_context* m_smb2 = nullptr;
};

// nfs://host/export[/folder]: the whole path is mounted (NFSv3)
class NfsShare : public Share {
public:
    explicit NfsShare(std::string url) : m_url(std::move(url)) {}
    ~NfsShare() override { disconnect(); }

    int connect(std::string& error) override {
        disconnect();
        std::string rest = m_url.substr(strlen("nfs://"));
        const size_t slash = rest.find('/');
        const std::string server = rest.substr(0, slash);
        const std::string exportPath = slash == std::string::npos ? "/" : rest.substr(slash);
        if (server.empty()) {
            error = "no server in the address";
            return -EINVAL;
        }
        if (!answers(server, 2049, error))
            return -EHOSTUNREACH;
        m_nfs = nfs_init_context();
        if (!m_nfs) {
            error = "out of memory";
            return -ENOMEM;
        }
        nfs_set_timeout(m_nfs, 10000);
        const int result = nfs_mount(m_nfs, server.c_str(), exportPath.c_str());
        if (result < 0) {
            error = nfs_get_error(m_nfs);
            disconnect();
            return result;
        }
        return 0;
    }

    void disconnect() override {
        if (!m_nfs)
            return;
        nfs_destroy_context(m_nfs);
        m_nfs = nullptr;
    }

    int stat(const std::string& path, Info& info) override {
        nfs_stat_64 st{};
        const int result = nfs_stat64(m_nfs, path.c_str(), &st);
        if (result < 0)
            return result;
        info.exists = true;
        info.dir = S_ISDIR(st.nfs_mode);
        info.size = info.dir ? 0 : st.nfs_size;
        info.mtime = st.nfs_mtime;
        return 0;
    }

    int list(const std::string& path, std::vector<Entry>& entries) override {
        nfsdir* dir = nullptr;
        const int result = nfs_opendir(m_nfs, path.c_str(), &dir);
        if (result < 0)
            return result;
        while (nfsdirent* entry = nfs_readdir(m_nfs, dir)) {
            if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0)
                continue;
            Info info;
            info.exists = true;
            info.dir = entry->type == 2 /* NF3DIR */ || S_ISDIR(entry->mode);
            info.size = info.dir ? 0 : entry->size;
            info.mtime = static_cast<uint64_t>(entry->mtime.tv_sec);
            entries.push_back({entry->name, info});
        }
        nfs_closedir(m_nfs, dir);
        return 0;
    }

    int open(const std::string& path, void*& file) override {
        nfsfh* handle = nullptr;
        const int result = nfs_open(m_nfs, path.c_str(), O_RDONLY, &handle);
        file = handle;
        return result < 0 ? result : 0;
    }

    int64_t pread(void* file, void* buffer, uint32_t size, uint64_t offset) override {
        return nfs_pread(m_nfs, static_cast<nfsfh*>(file), buffer, size, offset);
    }

    void close(void* file) override {
        if (m_nfs)
            nfs_close(m_nfs, static_cast<nfsfh*>(file));
    }

private:
    std::string m_url;
    nfs_context* m_nfs = nullptr;
};

struct Mount {
    std::string name, url;
    std::string identity;  // address, user and password
    std::unique_ptr<Share> share;
    std::mutex lock;  // one request at a time on the connection
    bool connected = false;
    uint64_t failedAt = 0;
    uint32_t generation = 0;  // each connection: open files open again
    std::string status;
};

std::mutex s_mountsLock;
std::map<std::string, std::shared_ptr<Mount>> s_mounts;

std::shared_ptr<Mount> findMount(const std::string& name) {
    std::lock_guard lock(s_mountsLock);
    auto it = s_mounts.find(name);
    return it == s_mounts.end() ? nullptr : it->second;
}

// with the mount's lock held
bool ensureConnected(Mount& mount, bool now = false) {
    if (mount.connected)
        return true;
    if (!now && mount.failedAt && nowSeconds() - mount.failedAt < kRetrySeconds)
        return false;
    std::string error;
    const int result = mount.share->connect(error);
    if (result == 0) {
        mount.connected = true;
        mount.failedAt = 0;
        mount.generation++;
        mount.status = "Connected";
        trace("connected to " + mount.name);
        return true;
    }
    mount.failedAt = std::max<uint64_t>(nowSeconds(), 1);
    mount.status = error.empty() ? std::string(strerror(-result)) : error;
    trace("can't connect to " + mount.name + ": " + mount.status);
    return false;
}

bool notThere(int result) {
    return result == -ENOENT || result == -ENOTDIR || result == -EACCES || result == -EPERM || result == -EINVAL;
}

// Runs a request; a failure that isn't "not there" may be the connection: once more on a new one.
template <typename Request>
int run(Mount& mount, Request request) {
    if (!ensureConnected(mount))
        return -EIO;
    int result = request();
    if (result >= 0 || notThere(result))
        return result;
    mount.share->disconnect();
    mount.connected = false;
    if (!ensureConnected(mount, true))
        return -EIO;
    return request();
}

// --- paths ---------------------------------------------------------------

bool isNetPath(const char* path) {
    return path && strncmp(path, "/net", 4) == 0 && (path[4] == '\0' || path[4] == '/');
}

// "/net//library/./a/" -> parts {"library", "a"}
std::vector<std::string> splitPath(const char* path) {
    std::vector<std::string> parts;
    const char* p = path + 4;
    while (*p) {
        while (*p == '/')
            p++;
        const char* end = p;
        while (*end && *end != '/')
            end++;
        std::string part(p, end);
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(std::move(part));
        }
        p = end;
    }
    return parts;
}

struct Where {
    std::shared_ptr<Mount> mount;  // none: /net itself, or no such share
    bool root = false;             // /net
    std::string path;              // in the share: "/", "/a/b"
    std::string key;               // "library/a/b"
};

Where locate(const char* full) {
    Where where;
    const std::vector<std::string> parts = splitPath(full);
    if (parts.empty()) {
        where.root = true;
        return where;
    }
    where.mount = findMount(parts[0]);
    where.key = parts[0];
    for (size_t i = 1; i < parts.size(); i++) {
        where.path += "/" + parts[i];
        where.key += "/" + parts[i];
    }
    if (where.path.empty())
        where.path = "/";
    return where;
}

// --- what's there (kept a minute) ------------------------------------------

struct KeptInfo {
    Info info;
    uint64_t at;
};
struct KeptList {
    std::vector<Entry> entries;
    uint64_t at;
};
std::mutex s_keptLock;
std::unordered_map<std::string, KeptInfo> s_infos;  // by key
std::unordered_map<std::string, KeptList> s_lists;

bool fresh(uint64_t at) {
    return nowSeconds() - at < kInfoSeconds;
}

int listFolder(const Where& where, std::vector<Entry>& entries) {
    {
        std::lock_guard lock(s_keptLock);
        auto it = s_lists.find(where.key);
        if (it != s_lists.end() && fresh(it->second.at)) {
            entries = it->second.entries;
            return 0;
        }
    }
    Mount& mount = *where.mount;
    int result;
    {
        std::lock_guard lock(mount.lock);
        result = run(mount, [&] {
            entries.clear();
            return mount.share->list(where.path, entries);
        });
    }
    if (result < 0)
        return result;
    std::lock_guard lock(s_keptLock);
    const uint64_t now = nowSeconds();
    s_lists[where.key] = {entries, now};
    for (const Entry& entry : entries)
        s_infos[where.key + "/" + entry.name] = {entry.info, now};
    return 0;
}

int lookup(const Where& where, Info& info) {
    if (where.root) {
        info = {true, true, 0, 0};
        return 0;
    }
    if (!where.mount)
        return -ENOENT;
    {
        std::lock_guard lock(s_keptLock);
        auto it = s_infos.find(where.key);
        if (it != s_infos.end() && fresh(it->second.at)) {
            info = it->second.info;
            return info.exists ? 0 : -ENOENT;
        }
        // the folder it's in was listed: it's there or it isn't
        const size_t slash = where.key.rfind('/');
        if (slash != std::string::npos) {
            auto parent = s_lists.find(where.key.substr(0, slash));
            if (parent != s_lists.end() && fresh(parent->second.at))
                return -ENOENT;
        }
    }
    Mount& mount = *where.mount;
    if (where.path == "/") {
        // the share itself: there when it answers (not kept: it may answer later)
        std::lock_guard lock(mount.lock);
        if (!ensureConnected(mount))
            return -ENOENT;
        info = {true, true, 0, 0};
        return 0;
    }
    int result;
    {
        std::lock_guard lock(mount.lock);
        result = run(mount, [&] { return mount.share->stat(where.path, info); });
    }
    if (result < 0) {
        if (!notThere(result))
            return -ENOENT;  // no connection: not there (std::filesystem throws on other errors)
        info = {};
    }
    std::lock_guard lock(s_keptLock);
    s_infos[where.key] = {info, nowSeconds()};
    return info.exists ? 0 : -ENOENT;
}

// --- file data -----------------------------------------------------------

struct NetFile {
    std::shared_ptr<Mount> mount;
    std::string path, key;
    Info info;
    uint64_t id = 0;  // the blocks' key (path, size and time)
    void* native = nullptr;
    uint32_t generation = 0;
    std::mutex lock;  // position and folder reading
    uint64_t position = 0;
    std::vector<Entry> entries;  // a folder's
    size_t nextEntry = 0;
};

struct Block {
    uint64_t file = 0, index = 0, used = 0;
    std::vector<uint8_t> data;
};
std::mutex s_blocksLock;
Block s_blocks[kCacheBlocks];
uint64_t s_blockTick = 0;

// with the mount's lock held: reads exactly size bytes (or to the end of the file)
int64_t readFromShare(NetFile& file, void* buffer, uint64_t size, uint64_t offset) {
    Mount& mount = *file.mount;
    uint64_t done = 0;
    const int result = run(mount, [&]() -> int {
        if (!file.native || file.generation != mount.generation) {
            file.native = nullptr;
            void* native = nullptr;
            const int opened = mount.share->open(file.path, native);
            if (opened < 0)
                return opened;
            file.native = native;
            file.generation = mount.generation;
        }
        while (done < size) {
            const uint64_t want = std::min<uint64_t>(size - done, 8u * 1024 * 1024);
            const int64_t got = mount.share->pread(file.native, static_cast<uint8_t*>(buffer) + done,
                                                   static_cast<uint32_t>(want), offset + done);
            if (got < 0)
                return static_cast<int>(got);
            if (got == 0)
                break;
            done += static_cast<uint64_t>(got);
        }
        return 0;
    });
    return result < 0 ? result : static_cast<int64_t>(done);
}

int64_t readAt(NetFile& file, void* buffer, uint64_t size, uint64_t offset) {
    if (offset >= file.info.size)
        return 0;
    size = std::min(size, file.info.size - offset);
    if (size >= kDirectRead) {
        std::lock_guard lock(file.mount->lock);
        return readFromShare(file, buffer, size, offset);
    }
    uint64_t done = 0;
    while (done < size) {
        const uint64_t at = offset + done;
        const uint64_t index = at / kBlockSize;
        const uint64_t inBlock = at % kBlockSize;
        const uint64_t take = std::min<uint64_t>(size - done, kBlockSize - inBlock);
        bool found = false;
        {
            std::lock_guard lock(s_blocksLock);
            for (Block& block : s_blocks) {
                if (block.used && block.file == file.id && block.index == index && block.data.size() >= inBlock + take) {
                    memcpy(static_cast<uint8_t*>(buffer) + done, block.data.data() + inBlock, take);
                    block.used = ++s_blockTick;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            const uint64_t start = index * kBlockSize;
            std::vector<uint8_t> data(std::min<uint64_t>(kBlockSize, file.info.size - start));
            int64_t got;
            {
                std::lock_guard lock(file.mount->lock);
                got = readFromShare(file, data.data(), data.size(), start);
            }
            if (got < 0)
                return done ? static_cast<int64_t>(done) : got;
            data.resize(static_cast<size_t>(got));
            if (data.size() < inBlock + take) {
                const uint64_t have = data.size() > inBlock ? data.size() - inBlock : 0;
                memcpy(static_cast<uint8_t*>(buffer) + done, data.data() + inBlock, have);
                return static_cast<int64_t>(done + have);
            }
            memcpy(static_cast<uint8_t*>(buffer) + done, data.data() + inBlock, take);
            std::lock_guard lock(s_blocksLock);
            Block* oldest = &s_blocks[0];
            for (Block& block : s_blocks)
                if (block.used < oldest->used)
                    oldest = &block;
            oldest->file = file.id;
            oldest->index = index;
            oldest->data = std::move(data);
            oldest->used = ++s_blockTick;
        }
        done += take;
    }
    return static_cast<int64_t>(done);
}

// --- descriptors ---------------------------------------------------------

std::atomic<uint8_t> s_isNet[kMaxFds];
std::mutex s_filesLock;
std::unordered_map<int, std::shared_ptr<NetFile>> s_files;
int s_placeholder = -1;

std::shared_ptr<NetFile> netFile(int fd) {
    if (fd < 0 || fd >= kMaxFds || !s_isNet[fd].load(std::memory_order_relaxed))
        return nullptr;
    std::lock_guard lock(s_filesLock);
    auto it = s_files.find(fd);
    return it == s_files.end() ? nullptr : it->second;
}

void fillStat(const Info& info, const std::string& key, struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_dev = 0x4E4554;  // "NET"
    st->st_ino = std::hash<std::string>()(key) & 0x7FFFFFFF;
    st->st_mode = info.dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    st->st_nlink = 1;
    st->st_size = static_cast<off_t>(info.size);
    st->st_blksize = 64 * 1024;
    st->st_blocks = static_cast<blkcnt_t>((info.size + 511) / 512);
    st->st_mtim.tv_sec = static_cast<time_t>(info.mtime);
    st->st_atim = st->st_mtim;
    st->st_ctim = st->st_mtim;
    st->st_birthtim = st->st_mtim;
}

int statPath(const char* path, struct stat* st) {
    const Where where = locate(path);
    Info info;
    const int result = lookup(where, info);
    if (result < 0) {
        errno = -result;
        return -1;
    }
    fillStat(info, where.key, st);
    return 0;
}

int64_t readFd(NetFile& file, void* buffer, size_t size) {
    if (file.info.dir) {
        errno = EISDIR;
        return -1;
    }
    std::lock_guard lock(file.lock);
    const int64_t got = readAt(file, buffer, size, file.position);
    if (got < 0) {
        errno = static_cast<int>(-got);
        return -1;
    }
    file.position += static_cast<uint64_t>(got);
    return got;
}

int64_t readvFd(NetFile& file, const struct iovec* iov, int count) {
    int64_t total = 0;
    for (int i = 0; i < count; i++) {
        if (iov[i].iov_len == 0)
            continue;
        const int64_t got = readFd(file, iov[i].iov_base, iov[i].iov_len);
        if (got < 0)
            return total ? total : -1;
        total += got;
        if (static_cast<size_t>(got) < iov[i].iov_len)
            break;
    }
    return total;
}

}  // namespace

// --- what the menus call -------------------------------------------------

// Mounts url (smb://host/share[/folder] or nfs://host/export[/folder]) at /net/<name>; an empty url
// takes it away. Connects on first use.
extern "C" void ps4_netfs_mount(const char* name, const char* url, const char* user, const char* password) {
    // a server that goes away mid-read raises SIGPIPE on the next write, which would end the app
    // (DolphinPS4 Lab's network shares hit it): a failed write is an error instead
    static std::once_flag s_sigpipe;
    std::call_once(s_sigpipe, [] { signal(SIGPIPE, SIG_IGN); });
    std::shared_ptr<Mount> mount;
    const std::string address = url ? url : "";
    if (!address.empty()) {
        mount = std::make_shared<Mount>();
        mount->name = name;
        mount->url = address;
        mount->identity = address + '\n' + (user ? user : "") + '\n' + (password ? password : "");
        if (address.rfind("smb://", 0) == 0)
            mount->share = std::make_unique<SmbShare>(address, user ? user : "", password ? password : "");
        else if (address.rfind("nfs://", 0) == 0)
            mount->share = std::make_unique<NfsShare>(address);
        else
            mount = nullptr;
    }
    {
        std::lock_guard lock(s_mountsLock);
        auto it = s_mounts.find(name);
        if (it != s_mounts.end() && mount && it->second->identity == mount->identity)
            return;  // the same: keep its connection
        if (mount)
            s_mounts[name] = mount;
        else
            s_mounts.erase(name);
    }
    std::lock_guard lock(s_keptLock);
    s_infos.clear();
    s_lists.clear();
    trace(std::string(name) + (mount ? " is " + address : " removed"));
}

// Connects now (if it isn't) and lists the share: 1 and "Connected: 12 items", or 0 and why not.
extern "C" int ps4_netfs_check(const char* name, char* message, size_t size) {
    std::shared_ptr<Mount> mount = findMount(name);
    if (!mount) {
        snprintf(message, size, "Not set");
        return 0;
    }
    {
        std::lock_guard lock(mount->lock);
        if (!ensureConnected(*mount, true)) {
            snprintf(message, size, "%s", mount->status.c_str());
            return 0;
        }
    }
    {
        std::lock_guard lock(s_keptLock);
        s_lists.erase(name);
    }
    std::vector<Entry> entries;
    const Where where = locate(("/net/" + std::string(name)).c_str());
    const int result = listFolder(where, entries);
    if (result < 0) {
        snprintf(message, size, "Connected, but the folder can't be read (%s)", strerror(-result));
        return 0;
    }
    snprintf(message, size, "Connected: %zu items", entries.size());
    return 1;
}

// What was learned about the shares is asked again (the launcher's refresh).
extern "C" void ps4_netfs_forget() {
    std::lock_guard lock(s_keptLock);
    s_infos.clear();
    s_lists.clear();
}

// --- the C library's calls -------------------------------------------------

extern "C" int ps4_netfs_is_path(const char* path) {
    return isNetPath(path);
}

// From __wrap_open (ps4_crashlog.cpp) for /net paths.
extern "C" int ps4_netfs_open(const char* path, int flags) {
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND))) {
        errno = EROFS;
        return -1;
    }
    const Where where = locate(path);
    Info info;
    const int result = lookup(where, info);
    if (result < 0) {
        errno = -result;
        return -1;
    }
    if ((flags & O_DIRECTORY) && !info.dir) {
        errno = ENOTDIR;
        return -1;
    }
    auto file = std::make_shared<NetFile>();
    file->mount = where.mount;
    file->path = where.path;
    file->key = where.key;
    file->info = info;
    file->id = std::hash<std::string>()(where.key) ^ (info.size * 0x9E3779B97F4A7C15ull) ^ info.mtime;
    if (info.dir) {
        if (where.root) {
            std::lock_guard lock(s_mountsLock);
            for (auto& [name, mount] : s_mounts)
                file->entries.push_back({name, {true, true, 0, 0}});
        } else {
            const int listed = listFolder(where, file->entries);
            if (listed < 0) {
                errno = -listed;
                return -1;
            }
        }
    }
    std::lock_guard lock(s_filesLock);
    if (s_placeholder < 0)
        s_placeholder = socket(AF_INET, SOCK_DGRAM, 0);
    const int fd = s_placeholder >= 0 ? dup(s_placeholder) : -1;
    if (fd < 0 || fd >= kMaxFds) {
        if (fd >= 0)
            __real_close(fd);
        errno = EMFILE;
        return -1;
    }
    s_files[fd] = file;
    s_isNet[fd].store(1);
    return fd;
}

// From __wrap_close: 1 when fd was a network file (then closed).
extern "C" int ps4_netfs_close(int fd) {
    std::shared_ptr<NetFile> file;
    {
        std::lock_guard lock(s_filesLock);
        if (fd < 0 || fd >= kMaxFds || !s_isNet[fd].load())
            return 0;
        auto it = s_files.find(fd);
        if (it != s_files.end()) {
            file = it->second;
            s_files.erase(it);
        }
        s_isNet[fd].store(0);
        __real_close(fd);
    }
    if (file && file->native) {
        std::lock_guard lock(file->mount->lock);
        if (file->generation == file->mount->generation && file->mount->connected)
            file->mount->share->close(file->native);
        file->native = nullptr;
    }
    return 1;
}

extern "C" ssize_t __wrap_read(int fd, void* buffer, size_t size) {
    if (auto file = netFile(fd))
        return readFd(*file, buffer, size);
    return __real_read(fd, buffer, size);
}

extern "C" ssize_t __wrap__read(int fd, void* buffer, size_t size) {
    if (auto file = netFile(fd))
        return readFd(*file, buffer, size);
    return __real__read(fd, buffer, size);
}

extern "C" ssize_t __wrap_readv(int fd, const struct iovec* iov, int count) {
    if (auto file = netFile(fd))
        return readvFd(*file, iov, count);
    return __real_readv(fd, iov, count);
}

extern "C" ssize_t __wrap__readv(int fd, const struct iovec* iov, int count) {
    if (auto file = netFile(fd))
        return readvFd(*file, iov, count);
    return __real__readv(fd, iov, count);
}

extern "C" ssize_t __wrap_pread(int fd, void* buffer, size_t size, off_t offset) {
    if (auto file = netFile(fd)) {
        if (file->info.dir) {
            errno = EISDIR;
            return -1;
        }
        const int64_t got = readAt(*file, buffer, size, static_cast<uint64_t>(offset));
        if (got < 0) {
            errno = static_cast<int>(-got);
            return -1;
        }
        return got;
    }
    return __real_pread(fd, buffer, size, offset);
}

extern "C" off_t __wrap_lseek(int fd, off_t offset, int whence) {
    if (auto file = netFile(fd)) {
        std::lock_guard lock(file->lock);
        if (file->info.dir) {
            file->nextEntry = 0;  // rewinddir
            return 0;
        }
        int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? static_cast<int64_t>(file->position) :
                       whence == SEEK_END ? static_cast<int64_t>(file->info.size) : -1;
        if (base < 0 || base + offset < 0) {
            errno = EINVAL;
            return -1;
        }
        file->position = static_cast<uint64_t>(base + offset);
        return static_cast<off_t>(file->position);
    }
    return __real_lseek(fd, offset, whence);
}

extern "C" int __wrap_fstat(int fd, struct stat* st) {
    if (auto file = netFile(fd)) {
        fillStat(file->info, file->key, st);
        return 0;
    }
    return __real_fstat(fd, st);
}

extern "C" int __wrap_stat(const char* path, struct stat* st) {
    if (isNetPath(path))
        return statPath(path, st);
    return __real_stat(path, st);
}

// readdir's: as many entries as fit (the PS4's struct dirent: fileno, reclen, type, namlen, name)
extern "C" int __wrap_getdents(int fd, char* buffer, size_t size) {
    auto file = netFile(fd);
    if (!file)
        return __real_getdents(fd, buffer, size);
    if (!file->info.dir) {
        errno = ENOTDIR;
        return -1;
    }
    std::lock_guard lock(file->lock);
    size_t used = 0;
    while (file->nextEntry < file->entries.size()) {
        const Entry& entry = file->entries[file->nextEntry];
        const size_t nameLength = std::min<size_t>(entry.name.size(), 255);
        const size_t length = (offsetof(struct dirent, d_name) + nameLength + 1 + 7) & ~size_t(7);
        if (used + length > size)
            break;
        auto* out = reinterpret_cast<struct dirent*>(buffer + used);
        memset(out, 0, length);
        // d_fileno (the C library's headers rename it d_ino): left 0, nothing here reads it
        out->d_reclen = static_cast<unsigned short>(length);
        out->d_type = entry.info.dir ? DT_DIR : DT_REG;
        out->d_namlen = static_cast<unsigned char>(nameLength);
        memcpy(out->d_name, entry.name.data(), nameLength);
        used += length;
        file->nextEntry++;
    }
    if (used == 0 && file->nextEntry < file->entries.size()) {
        errno = EINVAL;
        return -1;
    }
    return static_cast<int>(used);
}
