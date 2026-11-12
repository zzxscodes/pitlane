#include "pitlane/desk/region.hpp"

#include "pitlane/host/hugepage.hpp"
#include "pitlane/host/numa.hpp"
#include "pitlane/util/page.hpp"
#include "pitlane/util/touch.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#ifndef HUGETLBFS_MAGIC
#define HUGETLBFS_MAGIC 0x958458f6
#endif

namespace pitlane {
namespace {

constexpr char kMagic[8] = {'P', 'I', 'T', 'L', 'A', 'N', 'E', '1'};

bool path_on_hugetlbfs(const char* path) noexcept {
    if (!path || !path[0]) return false;
    struct statfs s {};
    if (::statfs(path, &s) == 0) {
        return static_cast<unsigned long>(s.f_type) == HUGETLBFS_MAGIC;
    }
    char parent[512];
    const char* slash = std::strrchr(path, '/');
    if (!slash || slash == path) return false;
    const std::size_t n = static_cast<std::size_t>(slash - path);
    if (n >= sizeof parent) return false;
    std::memcpy(parent, path, n);
    parent[n] = 0;
    if (::statfs(parent, &s) != 0) return false;
    return static_cast<unsigned long>(s.f_type) == HUGETLBFS_MAGIC;
}

bool dir_is_hugetlbfs(const char* dir) noexcept {
    if (!dir || !dir[0]) return false;
    struct statfs s {};
    if (::statfs(dir, &s) != 0) return false;
    return static_cast<unsigned long>(s.f_type) == HUGETLBFS_MAGIC;
}

// Maps a logical path to a stable name under hugepage_dir.
bool huge_path_for(char* out, std::size_t cap, const char* dir, const char* path) noexcept {
    if (!out || cap < 16 || !dir || !path) return false;
    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!base[0]) base = "desk";
    char safe[128];
    std::size_t j = 0;
    for (std::size_t i = 0; base[i] && j + 1 < sizeof safe; ++i) {
        const char c = base[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        safe[j++] = ok ? c : '_';
    }
    safe[j] = 0;
    if (j == 0) return false;
    const int n = std::snprintf(out, cap, "%s/pitlane_%s", dir, safe);
    return n > 0 && static_cast<std::size_t>(n) < cap;
}

void map_finish(MapInfo* out, MapOpt opt, bool explicit_huge) noexcept {
    if (!out || !out->addr || out->bytes == 0) return;
    out->hugepage = explicit_huge ? 1 : 0;
    out->fallback = (!explicit_huge && opt.hugepage) ? 1 : 0;
    out->locked = 0;
    out->numa_node = -1;
    if (opt.numa_node >= 0) {
        if (numa_bind_range(out->addr, out->bytes, opt.numa_node)) out->numa_node = opt.numa_node;
    }
    if (opt.lock) {
        if (::mlock(out->addr, out->bytes) == 0) out->locked = 1;
    }
    if (opt.prefault) touch_lines(out->addr, out->bytes);
}

int map_fd_at(const char* path, std::size_t need, MapInfo* out, bool create, bool explicit_huge,
              MapOpt opt) noexcept {
    const int fd = ::open(path, create ? (O_RDWR | O_CREAT) : O_RDWR, 0644);
    if (fd < 0) return -1;
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return -1;
    }
    const bool grew = static_cast<std::size_t>(st.st_size) < need;
    std::size_t map_bytes = need;
    if (grew) {
        if (!create || ::ftruncate(fd, static_cast<off_t>(need)) != 0) {
            ::close(fd);
            if (create) ::unlink(path);
            return -1;
        }
    } else {
        map_bytes = static_cast<std::size_t>(st.st_size);
    }
    void* addr = ::mmap(nullptr, map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        ::close(fd);
        if (create && grew) ::unlink(path);
        return -1;
    }
    out->addr = addr;
    out->bytes = map_bytes;
    out->fd = fd;
    map_finish(out, opt, explicit_huge);
    return 0;
}

int map_fd(const char* path, std::size_t bytes, MapInfo* out, bool create, MapOpt opt) noexcept {
    if (!path || !out || bytes == 0) return -1;
    out->addr = nullptr;
    out->bytes = 0;
    out->fd = -1;
    out->hugepage = 0;
    out->locked = 0;
    out->fallback = 0;
    out->numa_node = -1;

    if (opt.hugepage) {
        const std::size_t huge = hugepage_size_bytes();
        const std::size_t need = round_up_bytes(bytes, huge);
        if (need < bytes) return -1;
        const long want_pages = static_cast<long>((need + huge - 1) / huge);
        (void)hugepage_count_ensure(want_pages);  // best-effort; may lack privilege
        long free_pages = 0;
        const bool have_free = hugepage_free_get(free_pages) && free_pages >= want_pages;

        if (have_free && path_on_hugetlbfs(path)) {
            if (map_fd_at(path, need, out, create, true, opt) == 0) return 0;
        } else if (have_free) {
            const char* dir = opt.hugepage_dir ? opt.hugepage_dir : "/dev/hugepages";
            char huge_path[512];
            if (dir_is_hugetlbfs(dir) && huge_path_for(huge_path, sizeof huge_path, dir, path)) {
                if (map_fd_at(huge_path, need, out, create, true, opt) == 0) return 0;
            }
        }
        // Fall through to ordinary pages at the caller path.
    }

    const std::size_t need = round_up_bytes(bytes, page_size_bytes());
    if (need < bytes) return -1;
    return map_fd_at(path, need, out, create, false, opt);
}

}  // namespace

std::size_t desk_bytes() noexcept { return sizeof(Desk); }

int map_create(const char* path, std::size_t bytes, MapInfo* out, MapOpt opt) noexcept {
    return map_fd(path, bytes, out, true, opt);
}

int map_attach(const char* path, std::size_t bytes, MapInfo* out, MapOpt opt) noexcept {
    return map_fd(path, bytes, out, false, opt);
}

void map_release(MapInfo* io) noexcept {
    if (!io) return;
    if (io->addr && io->bytes) {
        if (io->locked) ::munlock(io->addr, io->bytes);
        ::munmap(io->addr, io->bytes);
    }
    if (io->fd >= 0) ::close(io->fd);
    io->addr = nullptr;
    io->bytes = 0;
    io->fd = -1;
    io->hugepage = 0;
    io->locked = 0;
    io->fallback = 0;
    io->numa_node = -1;
}

void desk_format(Desk* desk) noexcept {
    if (!desk) return;
    new (desk) Desk();
    std::memcpy(desk->magic, kMagic, 8);
    desk->version = kDeskVersion;
}

Desk* desk_view(MapInfo* io, int create_if_empty) noexcept {
    if (!io || !io->addr || io->bytes < sizeof(Desk)) return nullptr;
    auto* desk = static_cast<Desk*>(io->addr);
    if (std::memcmp(desk->magic, kMagic, 8) == 0 && desk->version == kDeskVersion) return desk;
    bool blank = true;
    for (char c : desk->magic) {
        if (c != 0) blank = false;
    }
    if (blank && create_if_empty) {
        desk_format(desk);
        return desk;
    }
    return nullptr;
}

}  // namespace pitlane
