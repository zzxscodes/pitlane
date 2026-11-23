#pragma once

#include <cstdint>
#include <cstring>

namespace pitlane {

// Place and cancel acknowledgements age on separate limits.
// A sweep only lists due local ids. It does not change the row.

struct VenueKey {
    uint64_t num{0};
    char text[24]{};
    char api[16]{};
    uint8_t kind{0};
};

struct AckClock {
    int64_t place_lim{0};
    int64_t cancel_lim{0};
    int64_t gap{0};
    int64_t last{0};
    uint8_t started{0};
};

inline bool copy_field(char* dst, int cap, const char* src) noexcept {
    if (!dst || cap <= 1) return false;
    if (!src) src = "";
    int n = 0;
    for (; src[n] != 0; ++n) {
        if (n + 1 >= cap) return false;
    }
    for (int i = 0; i < n; ++i) dst[i] = src[i];
    for (int i = n; i < cap; ++i) dst[i] = 0;
    return true;
}

inline bool key_num(VenueKey& key, uint64_t num, const char* api) noexcept {
    VenueKey next{};
    if (!copy_field(next.api, 16, api)) return false;
    next.num = num;
    next.kind = 0;
    key = next;
    return true;
}

inline bool key_text(VenueKey& key, const char* text, const char* api) noexcept {
    if (!text || text[0] == 0) return false;
    VenueKey next{};
    if (!copy_field(next.text, 24, text) || !copy_field(next.api, 16, api)) return false;
    next.kind = 1;
    key = next;
    return true;
}

inline bool key_same(const VenueKey& a, const VenueKey& b) noexcept {
    if (a.kind != b.kind) return false;
    if (std::memcmp(a.api, b.api, 16) != 0) return false;
    if (a.kind == 1) return std::memcmp(a.text, b.text, 24) == 0;
    return a.num == b.num;
}

template <int Cap>
class AckBook {
public:
    static_assert(Cap >= 2 && Cap <= 64, "acks");

    bool put(const VenueKey& key, uint64_t local, int64_t now) noexcept {
        if (local == 0 || now < 0) return false;
        if (find(key) >= 0) return false;
        int at = -1;
        for (int i = 0; i < Cap; ++i) {
            if (row_[i].state == 0) {
                at = i;
                break;
            }
        }
        if (at < 0) return false;
        row_[at] = Row{};
        row_[at].key = key;
        row_[at].local = local;
        row_[at].mark = now;
        row_[at].state = 1;
        return true;
    }

    bool link(const VenueKey& key) noexcept {
        const int at = find(key);
        if (at < 0 || row_[at].state != 1) return false;
        row_[at].state = 2;
        return true;
    }

    bool arm_cancel(const VenueKey& key, int64_t now) noexcept {
        if (now < 0) return false;
        const int at = find(key);
        if (at < 0 || row_[at].state != 2) return false;
        row_[at].state = 3;
        row_[at].mark = now;
        return true;
    }

    int sweep(AckClock& clock, int64_t now, uint64_t* out, int cap) noexcept {
        if (now < 0 || clock.place_lim < 0 || clock.cancel_lim < 0 || clock.gap < 0) return -1;
        if (clock.started && (now < clock.last || now - clock.last < clock.gap)) return 0;
        uint64_t due[Cap];
        int n = 0;
        for (int i = 0; i < Cap; ++i) {
            const Row& row = row_[i];
            if (row.state != 1 && row.state != 3) continue;
            if (now < row.mark) continue;
            const int64_t lim = row.state == 1 ? clock.place_lim : clock.cancel_lim;
            if (now - row.mark <= lim) continue;
            due[n++] = row.local;
        }
        if (n > cap || (n > 0 && !out)) return -1;
        for (int i = 0; i < n; ++i) out[i] = due[i];
        clock.last = now;
        clock.started = 1;
        return n;
    }

private:
    struct Row {
        VenueKey key{};
        uint64_t local{0};
        int64_t mark{0};
        uint8_t state{0};
    };

    int find(const VenueKey& key) const noexcept {
        for (int i = 0; i < Cap; ++i) {
            if (row_[i].state != 0 && key_same(row_[i].key, key)) return i;
        }
        return -1;
    }

    Row row_[Cap]{};
};

}  // namespace pitlane
