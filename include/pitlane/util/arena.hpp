#pragma once

#include "pitlane/host/hugepage.hpp"
#include "pitlane/host/numa.hpp"
#include "pitlane/util/hint.hpp"
#include "pitlane/util/page.hpp"
#include "pitlane/util/simd.hpp"
#include "pitlane/util/touch.hpp"

#include <cstddef>
#include <cstdint>

#include <sys/mman.h>
#include <unistd.h>

namespace pitlane {

// Fixed-capacity anonymous bump arena. Default open uses explicit MAP_HUGETLB
// (length rounded to Hugepagesize). No THP / MADV_HUGEPAGE. Falls back to
// ordinary anonymous pages when the kernel has no reserved huge pages.
// Default opt locks with mlock, prefaults every page, and may MPOL_BIND.

struct ArenaOpt {
    uint8_t hugepage{1};   // MAP_HUGETLB first, then ordinary pages
    uint8_t lock{1};       // try mlock when permitted
    uint8_t prefault{1};   // touch every page after map
    int8_t numa_node{-1};  // >=0: MPOL_BIND after map; use numa_local_node() for local
};

class ByteArena {
public:
    ByteArena() = default;
    ByteArena(const ByteArena&) = delete;
    ByteArena& operator=(const ByteArena&) = delete;

    ~ByteArena() { close(); }

    bool open(std::size_t bytes, ArenaOpt opt = {}) noexcept {
        close();
        if (bytes == 0) return false;
        const std::size_t page_u = page_size_bytes();
        if (page_u == 0) return false;
        std::size_t need = round_up_bytes(bytes, page_u);
        if (need < bytes) return false;
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
        bool huge = false;
        if (opt.hugepage) {
            const std::size_t huge_u = hugepage_size_bytes();
            const std::size_t huge_need = round_up_bytes(bytes, huge_u);
            if (huge_need >= bytes) {
                const long want_pages = static_cast<long>((huge_need + huge_u - 1) / huge_u);
                (void)hugepage_count_ensure(want_pages);
                void* p = ::mmap(nullptr, huge_need, PROT_READ | PROT_WRITE, flags | MAP_HUGETLB, -1, 0);
                if (p != MAP_FAILED) {
                    base_ = static_cast<unsigned char*>(p);
                    need = huge_need;
                    huge = true;
                }
            }
        }
        if (!base_) {
            void* p = ::mmap(nullptr, need, PROT_READ | PROT_WRITE, flags, -1, 0);
            if (p == MAP_FAILED) return false;
            base_ = static_cast<unsigned char*>(p);
        }
        cap_ = need;
        used_ = 0;
        locked_ = 0;
        hugepage_ = huge ? 1 : 0;
        fallback_ = (!huge && opt.hugepage) ? 1 : 0;
        numa_node_ = -1;
        if (opt.numa_node >= 0) {
            if (numa_bind_range(base_, cap_, opt.numa_node)) numa_node_ = opt.numa_node;
        }
        if (opt.lock) {
            if (::mlock(base_, cap_) == 0) locked_ = 1;
        }
        if (opt.prefault) touch_lines(base_, cap_);
        return true;
    }

    void close() noexcept {
        if (!base_) return;
        if (locked_) ::munlock(base_, cap_);
        ::munmap(base_, cap_);
        base_ = nullptr;
        cap_ = 0;
        used_ = 0;
        locked_ = 0;
        hugepage_ = 0;
        fallback_ = 0;
        numa_node_ = -1;
    }

    PITLANE_HOT void* take(std::size_t n, std::size_t align = 8) noexcept {
        if (unlikely(!base_ || n == 0 || align == 0 || (align & (align - 1)) != 0)) return nullptr;
        const std::size_t mask = align - 1;
        const std::size_t at = (used_ + mask) & ~mask;
        if (unlikely(at > cap_ || n > cap_ - at)) return nullptr;
        used_ = at + n;
        return base_ + at;
    }

    bool clear() noexcept {
        if (!base_) return false;
        zero_bytes(base_, cap_);
        used_ = 0;
        return true;
    }

    std::size_t capacity() const noexcept { return cap_; }
    std::size_t used() const noexcept { return used_; }
    std::size_t free() const noexcept { return cap_ - used_; }
    bool locked() const noexcept { return locked_ != 0; }
    bool hugepage() const noexcept { return hugepage_ != 0; }
    bool fallback() const noexcept { return fallback_ != 0; }
    int numa_node() const noexcept { return numa_node_; }
    void* data() noexcept { return base_; }
    const void* data() const noexcept { return base_; }

private:
    unsigned char* base_{nullptr};
    std::size_t cap_{0};
    std::size_t used_{0};
    uint8_t locked_{0};
    uint8_t hugepage_{0};
    uint8_t fallback_{0};
    int8_t numa_node_{-1};
};

}  // namespace pitlane
