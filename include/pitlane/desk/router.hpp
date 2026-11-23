#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pitlane {

// Routing helpers for a request/response hub. No heap; every table is fixed.
//
// RouteTable maps a counter to the routing key used for its outbound requests
// and keeps one fallback key for counters that are not listed. route_binding
// derives the consume names a router registers from an exchange name and a
// counter. RequestBook remembers the outbound request behind each request id so
// a reply can be completed with fields the response does not repeat.

enum BindMode : uint8_t {
    kBindDirect = 0,    // routing key is the counter name
    kBindSuffixed = 1,  // routing key is <counter>_<exchange with exchange->routing>
};

namespace detail {

inline bool router_copy(char* dst, int cap, const char* src) noexcept {
    if (!dst || cap <= 0 || !src) return false;
    const std::size_t n = std::strlen(src);
    if (n >= static_cast<std::size_t>(cap)) return false;
    std::memcpy(dst, src, n + 1);
    return true;
}

// Replaces every occurrence of `from`. Returns false without a partial result
// when the output does not fit.
inline bool router_replace(char* dst, int cap, const char* src, const char* from, const char* to) noexcept {
    if (!dst || cap <= 0 || !src || !from || !*from || !to) return false;
    const std::size_t fl = std::strlen(from);
    const std::size_t tl = std::strlen(to);
    int out = 0;
    while (*src) {
        if (std::strncmp(src, from, fl) == 0) {
            if (out + static_cast<int>(tl) >= cap) return false;
            std::memcpy(dst + out, to, tl);
            out += static_cast<int>(tl);
            src += fl;
        } else {
            if (out + 1 >= cap) return false;
            dst[out++] = *src++;
        }
    }
    dst[out] = 0;
    return true;
}

inline bool router_join(char* dst, int cap, const char* a, const char* b) noexcept {
    if (!dst || cap <= 0 || !a || !b) return false;
    const std::size_t al = std::strlen(a);
    const std::size_t bl = std::strlen(b);
    if (al + 1 + bl >= static_cast<std::size_t>(cap)) return false;
    std::memcpy(dst, a, al);
    dst[al] = '_';
    std::memcpy(dst + al + 1, b, bl + 1);
    return true;
}

}  // namespace detail

// Queue and routing key for one consume binding.
//
// queue   = <counter>_<exchange with "exchange" replaced by "queue">
// routing = <counter>                        when mode is kBindDirect
//           <counter>_<exchange with "exchange" replaced by "routing"> otherwise
//
// An exchange without the word "exchange" is used unchanged after the counter.
inline bool route_binding(const char* exchange, const char* counter, BindMode mode,
                          char* queue, int queue_cap, char* routing, int routing_cap) noexcept {
    if (!exchange || !exchange[0] || !counter || !counter[0]) return false;
    if (!queue || queue_cap <= 0 || !routing || routing_cap <= 0) return false;
    char queue_exchange[192]{};
    char routing_exchange[192]{};
    if (!detail::router_replace(queue_exchange, sizeof queue_exchange, exchange, "exchange", "queue")) return false;
    if (!detail::router_replace(routing_exchange, sizeof routing_exchange, exchange, "exchange", "routing")) return false;
    if (!detail::router_join(queue, queue_cap, counter, queue_exchange)) return false;
    if (mode == kBindSuffixed) return detail::router_join(routing, routing_cap, counter, routing_exchange);
    return detail::router_copy(routing, routing_cap, counter);
}

class RouteTable {
public:
    static constexpr int kRows = 256;
    static constexpr int kName = 32;
    static constexpr int kKey = 64;

    void clear() noexcept {
        n_ = 0;
        def_[0] = 0;
    }

    // Inserts or updates one counter. Fails on empty names, an oversized name,
    // or a full table when the counter is new.
    bool set(const char* counter, const char* key) noexcept {
        if (!counter || !counter[0] || !key || !key[0]) return false;
        if (std::strlen(counter) >= static_cast<std::size_t>(kName)) return false;
        if (std::strlen(key) >= static_cast<std::size_t>(kKey)) return false;
        int i = row_of(counter);
        if (i < 0) {
            if (n_ >= kRows) return false;
            i = n_++;
            detail::router_copy(counter_[i], kName, counter);
        }
        detail::router_copy(key_[i], kKey, key);
        return true;
    }

    bool set_default(const char* key) noexcept {
        if (!key || !key[0] || std::strlen(key) >= static_cast<std::size_t>(kKey)) return false;
        detail::router_copy(def_, kKey, key);
        return true;
    }

    void remove(const char* counter) noexcept {
        const int i = row_of(counter);
        if (i < 0) return;
        for (int j = i + 1; j < n_; ++j) {
            std::memcpy(counter_[j - 1], counter_[j], sizeof counter_[0]);
            std::memcpy(key_[j - 1], key_[j], sizeof key_[0]);
        }
        --n_;
    }

    // Direct key only; nullptr when the counter is not listed.
    const char* find(const char* counter) const noexcept {
        const int i = row_of(counter);
        return i < 0 ? nullptr : key_[i];
    }

    // Direct key, then the default. nullptr when neither is set.
    const char* route(const char* counter) const noexcept {
        const char* key = find(counter);
        if (key) return key;
        return def_[0] ? def_ : nullptr;
    }

    const char* default_key() const noexcept { return def_[0] ? def_ : nullptr; }
    int count() const noexcept { return n_; }

private:
    int row_of(const char* counter) const noexcept {
        if (!counter) return -1;
        for (int i = 0; i < n_; ++i) {
            if (std::strcmp(counter_[i], counter) == 0) return i;
        }
        return -1;
    }

    char counter_[kRows][kName]{};
    char key_[kRows][kKey]{};
    char def_[kKey]{};
    int n_{0};
};

// One outbound request kept for reply completion.
struct RoutedRequest {
    uint64_t request_id{0};
    uint64_t parent_id{0};
    uint64_t client_id{0};
    int64_t saved_us{0};
    int64_t frozen_volume{0};
    int64_t frozen_price{0};
    char routing_id[64]{};
    char counter[32]{};
    char account[24]{};
    char symbol[32]{};
    int32_t engine_id{0};
    int32_t basket_id{0};
    int32_t relate_count{0};
    int8_t direction{0};
    uint8_t flags{0};
};

class RequestBook {
public:
    static constexpr int kRows = 1024;

    void clear() noexcept { n_ = 0; }
    int count() const noexcept { return n_; }

    // Inserts, or replaces the row with the same request id. Fails only when
    // the table is full and the id is new.
    bool put(const RoutedRequest& req) noexcept {
        int i = row_of(req.request_id);
        if (i < 0) {
            if (n_ >= kRows) return false;
            i = n_++;
        }
        rows_[i] = req;
        return true;
    }

    const RoutedRequest* find(uint64_t request_id) const noexcept {
        const int i = row_of(request_id);
        return i < 0 ? nullptr : &rows_[i];
    }

    bool contains(uint64_t request_id) const noexcept { return row_of(request_id) >= 0; }

    // Removes and copies one row. A reply without a row is unsolicited; the
    // caller decides whether it becomes an external order.
    bool take(uint64_t request_id, RoutedRequest& out) noexcept {
        const int i = row_of(request_id);
        if (i < 0) return false;
        out = rows_[i];
        erase_at(i);
        return true;
    }

    bool erase(uint64_t request_id) noexcept {
        const int i = row_of(request_id);
        if (i < 0) return false;
        erase_at(i);
        return true;
    }

    // Drops rows whose age is strictly greater than `ttl_us`. A non-positive
    // ttl disables expiry. Rows stamped in the future are left alone.
    int expire(int64_t now_us, int64_t ttl_us) noexcept {
        if (ttl_us <= 0) return 0;
        int out = 0;
        int removed = 0;
        for (int i = 0; i < n_; ++i) {
            const RoutedRequest& r = rows_[i];
            if (now_us >= r.saved_us && now_us - r.saved_us > ttl_us) {
                ++removed;
                continue;
            }
            if (out != i) rows_[out] = r;
            ++out;
        }
        n_ = out;
        return removed;
    }

private:
    int row_of(uint64_t request_id) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (rows_[i].request_id == request_id) return i;
        }
        return -1;
    }

    void erase_at(int i) noexcept {
        for (int j = i + 1; j < n_; ++j) rows_[j - 1] = rows_[j];
        --n_;
    }

    RoutedRequest rows_[kRows]{};
    int n_{0};
};

}  // namespace pitlane
