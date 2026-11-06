#include "pitlane/store.hpp"

#include <cstring>

namespace pitlane {

int Dict::intern(const char* name) noexcept {
    if (!name || !name[0]) return 0;
    const int found = find(name);
    if (found > 0) return found;
    if (n_ >= kCap) return 0;
    std::strncpy(text_[n_], name, 31);
    text_[n_][31] = 0;
    ++n_;
    return n_;
}

int Dict::find(const char* name) const noexcept {
    if (!name) return 0;
    for (int i = 0; i < n_; ++i) {
        if (std::strcmp(text_[i], name) == 0) return i + 1;
    }
    return 0;
}

const char* Dict::text(int id) const noexcept {
    if (id < 1 || id > n_) return "";
    return text_[id - 1];
}

bool TimeIndex::put(int64_t ts, uint32_t row) noexcept {
    uint32_t slot = static_cast<uint32_t>(static_cast<uint64_t>(ts) * 11400714819323198485ull >> 32) & (kCap - 1u);
    for (int n = 0; n < kCap; ++n) {
        const uint32_t i = (slot + static_cast<uint32_t>(n)) & (kCap - 1u);
        if (!used_[i]) {
            used_[i] = 1;
            key_[i] = ts;
            row_[i] = row;
            return true;
        }
        if (key_[i] == ts) return false;
    }
    return false;
}

bool TimeIndex::get(int64_t ts, uint32_t* row) const noexcept {
    uint32_t slot = static_cast<uint32_t>(static_cast<uint64_t>(ts) * 11400714819323198485ull >> 32) & (kCap - 1u);
    for (int n = 0; n < kCap; ++n) {
        const uint32_t i = (slot + static_cast<uint32_t>(n)) & (kCap - 1u);
        if (!used_[i]) return false;
        if (key_[i] == ts) {
            if (row) *row = row_[i];
            return true;
        }
    }
    return false;
}

}  // namespace pitlane
