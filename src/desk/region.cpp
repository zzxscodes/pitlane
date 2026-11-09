#include "pitlane/desk/region.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pitlane {
namespace {

constexpr char kMagic[8] = {'P', 'I', 'T', 'L', 'A', 'N', 'E', '1'};

int map_fd(const char* path, std::size_t bytes, MapInfo* out, bool create) noexcept {
    if (!path || !out || bytes == 0) return -1;
    const int fd = ::open(path, create ? (O_RDWR | O_CREAT) : O_RDWR, 0644);
    if (fd < 0) return -1;
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return -1;
    }
    if (static_cast<std::size_t>(st.st_size) < bytes) {
        if (!create || ::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
            ::close(fd);
            return -1;
        }
    }
    void* addr = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        ::close(fd);
        return -1;
    }
    out->addr = addr;
    out->bytes = bytes;
    out->fd = fd;
    return 0;
}

}  // namespace

std::size_t desk_bytes() noexcept { return sizeof(Desk); }

int map_create(const char* path, std::size_t bytes, MapInfo* out) noexcept {
    return map_fd(path, bytes, out, true);
}

int map_attach(const char* path, std::size_t bytes, MapInfo* out) noexcept {
    return map_fd(path, bytes, out, false);
}

void map_release(MapInfo* io) noexcept {
    if (!io) return;
    if (io->addr && io->bytes) ::munmap(io->addr, io->bytes);
    if (io->fd >= 0) ::close(io->fd);
    io->addr = nullptr;
    io->bytes = 0;
    io->fd = -1;
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
