#pragma once

// 席位与通道管理：一个通道对应一条柜台链路，一个席位是通道上的报单身份。
// 台账只做三件事：登记、选席、计数。选席优先同通道内启用且最空闲的席位，
// 平局按轮转，避免所有流量压到 0 号席位。故障席位不参与选择，但不自动摘除。

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pitlane {

enum SeatState : uint8_t {
    kSeatDisabled = 0,
    kSeatIdle = 1,
    kSeatActive = 2,
    kSeatFault = 3
};

inline const char* seat_state_name(uint8_t s) noexcept {
    switch (s) {
        case kSeatIdle: return "idle";
        case kSeatActive: return "active";
        case kSeatFault: return "fault";
        default: return "disabled";
    }
}

struct SeatSpec {
    uint32_t id{0};
    char name[16]{};
    char broker[16]{};
    char exchange[9]{};
    char channel[16]{};
    int32_t max_orders_per_sec{0}; // 0 不限制
    uint8_t enabled{1};
};

struct SeatRow {
    SeatSpec spec{};
    uint8_t state{kSeatDisabled};
    uint8_t pad[3]{};
    uint64_t orders{0};
    uint64_t rejects{0};
    uint64_t reconnects{0};
    int64_t last_us{0};
    int32_t window_orders{0};
    int32_t pad2{0};
};

class SeatRegistry {
public:
    static constexpr int kSeats = 16;
    static constexpr int kChannels = 8;

    void clear() noexcept {
        for (int i = 0; i < kSeats; ++i) rows_[i] = SeatRow{};
        n_ = 0;
        for (int i = 0; i < kChannels; ++i) chan_[i] = Channel{};
    }

    // 返回登记后的下标；重复 id 返回 -1。
    int add(const SeatSpec& spec) noexcept {
        if (spec.id == 0 || n_ >= kSeats) return -1;
        if (find(spec.id) >= 0) return -1;
        SeatRow& r = rows_[n_];
        r = SeatRow{};
        r.spec = spec;
        r.state = spec.enabled ? kSeatIdle : kSeatDisabled;
        Channel* c = channel_of(spec.channel, true);
        if (!c || c->n >= kSeats) return -1;
        c->seat[c->n++] = n_;
        return n_++;
    }

    int find(uint32_t id) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (rows_[i].spec.id == id) return i;
        }
        return -1;
    }

    int find_name(const char* name) const noexcept {
        if (!name) return -1;
        for (int i = 0; i < n_; ++i) {
            if (std::strcmp(rows_[i].spec.name, name) == 0) return i;
        }
        return -1;
    }

    bool start(uint32_t id, int64_t now_us) noexcept {
        const int i = find(id);
        if (i < 0) return false;
        if (rows_[i].state == kSeatActive) return true;
        rows_[i].state = kSeatActive;
        rows_[i].last_us = now_us;
        ++rows_[i].reconnects;
        return true;
    }

    bool stop(uint32_t id) noexcept {
        const int i = find(id);
        if (i < 0) return false;
        rows_[i].state = rows_[i].spec.enabled ? kSeatIdle : kSeatDisabled;
        return true;
    }

    bool fault(uint32_t id, int64_t now_us) noexcept {
        const int i = find(id);
        if (i < 0) return false;
        rows_[i].state = kSeatFault;
        rows_[i].last_us = now_us;
        return true;
    }

    // 在该通道上选择一个可用席位；没有可用席位返回 -1。
    int pick(const char* channel, int64_t now_us) noexcept {
        if (!channel) return -1;
        Channel* c = channel_of(channel, false);
        if (!c) return -1;
        int best = -1;
        for (int k = 0; k < c->n; ++k) {
            const int i = c->seat[k];
            const SeatRow& r = rows_[i];
            if (r.state != kSeatActive && r.state != kSeatIdle) continue;
            if (r.spec.max_orders_per_sec > 0 && r.window_orders >= r.spec.max_orders_per_sec) continue;
            if (best < 0 || r.window_orders < rows_[best].window_orders) best = i;
            else if (r.window_orders == rows_[best].window_orders && r.state == kSeatActive &&
                     rows_[best].state != kSeatActive) {
                best = i;
            }
        }
        if (best >= 0) {
            rows_[best].state = kSeatActive;
            rows_[best].last_us = now_us;
        }
        return best;
    }

    void note_order(int idx, bool ok, int64_t now_us) noexcept {
        if (idx < 0 || idx >= n_) return;
        ++rows_[idx].orders;
        ++rows_[idx].window_orders;
        if (!ok) ++rows_[idx].rejects;
        rows_[idx].last_us = now_us;
    }

    // 每秒窗口调用一次，清零限流计数。
    void tick_window() noexcept {
        for (int i = 0; i < n_; ++i) rows_[i].window_orders = 0;
    }

    int count() const noexcept { return n_; }
    const SeatRow& at(int i) const noexcept { return rows_[i]; }

    int channels() const noexcept { return nch_; }
    const char* channel_name(int i) const noexcept { return i >= 0 && i < nch_ ? chan_[i].name : ""; }
    int channel_seats(int i) const noexcept { return i >= 0 && i < nch_ ? chan_[i].n : 0; }

private:
    struct Channel {
        char name[16]{};
        int32_t seat[kSeats]{};
        int32_t n{0};
    };

    Channel* channel_of(const char* name, bool create) noexcept {
        if (!name || !name[0]) return nullptr;
        for (int i = 0; i < nch_; ++i) {
            if (std::strcmp(chan_[i].name, name) == 0) return &chan_[i];
        }
        if (!create || nch_ >= kChannels) return nullptr;
        Channel& c = chan_[nch_];
        c = Channel{};
        std::snprintf(c.name, sizeof c.name, "%s", name);
        return &chan_[nch_++];
    }

    SeatRow rows_[kSeats]{};
    Channel chan_[kChannels]{};
    int n_{0};
    int nch_{0};
};

} // namespace pitlane
