#pragma once

#include <cstdint>
#include <cstring>

namespace pitlane {

// Local algo route table (trading center algo route table role). Startup only; no heap.

class RouteBook {
public:
    static constexpr int kKeys = 64;
    static constexpr int kAlgos = 8;
    static constexpr int kName = 32;

    bool bind(const char* strategy, const char* algo) noexcept {
        if (!strategy || !strategy[0] || !algo || !algo[0]) return false;
        if (std::strlen(strategy) >= static_cast<std::size_t>(kName)) return false;
        if (std::strlen(algo) >= static_cast<std::size_t>(kName)) return false;
        int ki = find_key(strategy);
        if (ki < 0) {
            if (n_ >= kKeys) return false;
            ki = n_++;
            std::memcpy(key_[ki], strategy, std::strlen(strategy) + 1);
            n_algo_[ki] = 0;
        }
        for (int i = 0; i < n_algo_[ki]; ++i) {
            if (std::strcmp(algo_[ki][i], algo) == 0) return true;
        }
        if (n_algo_[ki] >= kAlgos) return false;
        std::memcpy(algo_[ki][n_algo_[ki]], algo, std::strlen(algo) + 1);
        ++n_algo_[ki];
        return true;
    }

    int lookup(const char* strategy, char out[][kName], int cap) const noexcept {
        if (!strategy || !out || cap <= 0) return -1;
        const int ki = find_key(strategy);
        if (ki < 0) return 0;
        const int n = n_algo_[ki] < cap ? n_algo_[ki] : cap;
        for (int i = 0; i < n; ++i) std::memcpy(out[i], algo_[ki][i], kName);
        return n;
    }

    bool has(const char* strategy, const char* algo) const noexcept {
        if (!strategy || !algo) return false;
        const int ki = find_key(strategy);
        if (ki < 0) return false;
        for (int i = 0; i < n_algo_[ki]; ++i) {
            if (std::strcmp(algo_[ki][i], algo) == 0) return true;
        }
        return false;
    }

    int size() const noexcept { return n_; }

private:
    int find_key(const char* strategy) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (std::strcmp(key_[i], strategy) == 0) return i;
        }
        return -1;
    }

    char key_[kKeys][kName]{};
    char algo_[kKeys][kAlgos][kName]{};
    int n_algo_[kKeys]{};
    int n_{0};
};

} // namespace pitlane
