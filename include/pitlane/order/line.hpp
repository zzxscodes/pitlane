#pragma once

#include <cstdint>

namespace pitlane {

// Ids at one price, oldest at the front. A later id waits behind the earlier ones.
// The line refuses id 0, a duplicate, and a push when it is full. The previous ids stay.
// The library does not decide which id trades.

template <int Cap>
class TimeLine {
public:
    static_assert(Cap >= 4 && Cap <= 256 && (Cap & (Cap - 1)) == 0, "time line");

    int size() const noexcept { return n_; }

    bool push(uint64_t id) noexcept {
        if (id == 0 || n_ >= Cap || has(id)) return false;
        id_[(head_ + n_) & (Cap - 1)] = id;
        ++n_;
        return true;
    }

    bool front(uint64_t& id) const noexcept {
        if (n_ == 0) return false;
        id = id_[head_];
        return true;
    }

    bool pop(uint64_t& id) noexcept {
        if (!front(id)) return false;
        head_ = (head_ + 1) & (Cap - 1);
        --n_;
        return true;
    }

    bool erase(uint64_t id) noexcept {
        if (id == 0 || n_ == 0) return false;
        for (int i = 0; i < n_; ++i) {
            const int at = (head_ + i) & (Cap - 1);
            if (id_[at] != id) continue;
            for (int j = i; j < n_ - 1; ++j) {
                const int dst = (head_ + j) & (Cap - 1);
                const int src = (head_ + j + 1) & (Cap - 1);
                id_[dst] = id_[src];
            }
            --n_;
            return true;
        }
        return false;
    }

private:
    bool has(uint64_t id) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (id_[(head_ + i) & (Cap - 1)] == id) return true;
        }
        return false;
    }

    uint64_t id_[Cap]{};
    int head_{0};
    int n_{0};
};

}  // namespace pitlane
