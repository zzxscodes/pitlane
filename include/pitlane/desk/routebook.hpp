#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pitlane {

// Local algo route table. No heap.
//
// Entries can be changed while the process runs. generation() advances on every
// real change, so a strategy loop can cache its own view and reconcile only
// after a control note. The table is not shared memory; each engine process owns
// one and receives route control notes on its control lane.

class RouteBook {
public:
    static constexpr int kKeys = 256;
    static constexpr int kAlgos = 16;
    static constexpr int kName = 32;
    static constexpr uint8_t kControlBind = 10;
    static constexpr uint8_t kControlDrop = 11;

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
        ++generation_;
        return true;
    }

    // Removes one strategy/algo pair. The empty strategy key is dropped so a
    // long-lived process does not leak key slots across reloads.
    bool unbind(const char* strategy, const char* algo) noexcept {
        if (!strategy || !algo) return false;
        const int ki = find_key(strategy);
        if (ki < 0) return false;
        int ai = -1;
        for (int i = 0; i < n_algo_[ki]; ++i) {
            if (std::strcmp(algo_[ki][i], algo) == 0) {
                ai = i;
                break;
            }
        }
        if (ai < 0) return false;
        for (int i = ai; i + 1 < n_algo_[ki]; ++i) {
            std::memcpy(algo_[ki][i], algo_[ki][i + 1], kName);
        }
        --n_algo_[ki];
        if (n_algo_[ki] == 0) {
            const int last = n_ - 1;
            if (ki != last) {
                std::memcpy(key_[ki], key_[last], kName);
                std::memcpy(algo_[ki], algo_[last], sizeof algo_[ki]);
                n_algo_[ki] = n_algo_[last];
            }
            --n_;
        }
        ++generation_;
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
    uint32_t generation() const noexcept { return generation_; }

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
    uint32_t generation_{0};
};

// Control text is "strategy|algo", "strategy:algo", or "strategy algo".
// len is the bounded field size; a zero length means the caller passes a
// ControlNote.text-style buffer and the parser stops at the first NUL or at 48.
inline bool route_split(const char* text, std::size_t len, char* strategy, char* algo,
                        std::size_t cap) noexcept {
    if (!text || !strategy || !algo || cap == 0) return false;
    if (len == 0) len = 48;
    std::size_t n = 0;
    while (n < len && text[n]) ++n;
    std::size_t sep = n;
    for (std::size_t i = 0; i < n; ++i) {
        if (text[i] == '|' || text[i] == ':' || text[i] == ' ') {
            sep = i;
            break;
        }
    }
    if (sep == 0 || sep + 1u >= n) return false;
    const std::size_t la = sep;
    const std::size_t lb = n - sep - 1u;
    if (la >= cap || lb >= cap) return false;
    std::memcpy(strategy, text, la);
    strategy[la] = 0;
    std::memcpy(algo, text + sep + 1u, lb);
    algo[lb] = 0;
    return true;
}

inline bool route_control(RouteBook& book, uint8_t kind, const char* text, std::size_t len = 0) noexcept {
    if (kind != RouteBook::kControlBind && kind != RouteBook::kControlDrop) return false;
    char strategy[RouteBook::kName]{};
    char algo[RouteBook::kName]{};
    if (!route_split(text, len, strategy, algo, RouteBook::kName)) return false;
    return kind == RouteBook::kControlBind ? book.bind(strategy, algo) : book.unbind(strategy, algo);
}

} // namespace pitlane
