#include "pitlane/desk/broker.hpp"

#include <cstring>

namespace pitlane {

Why BrokerWatch::screen(const Intent& in, const RiskCaps& caps) const noexcept {
    if (in.qty <= 0) return Why::Qty;
    if (!known_side(in.side)) return Why::Side;
    if (caps.max_qty > 0 && in.qty > caps.max_qty) return Why::Qty;
    if (caps.max_notional > 0 && in.px > 0 && in.qty > caps.max_notional / in.px) return Why::Notional;
    if (caps.max_live > 0 && live_ >= caps.max_live) return Why::Rate;
    return Why::None;
}

bool BrokerWatch::remember(const Intent& in, int64_t now_us) noexcept {
    if (in.tag == 0) return false;
    for (int i = 0; i < kCap; ++i) {
        if (row_[i].used && row_[i].tag == in.tag) return false;
    }
    for (int i = 0; i < kCap; ++i) {
        if (!row_[i].used) {
            row_[i] = Sent{};
            row_[i].tag = in.tag;
            row_[i].sent_us = now_us;
            row_[i].used = 1;
            ++live_;
            return true;
        }
    }
    return false;
}

bool BrokerWatch::ack(uint64_t tag) noexcept {
    for (int i = 0; i < kCap; ++i) {
        if (row_[i].used && row_[i].tag == tag) {
            row_[i].acked = 1;
            return true;
        }
    }
    return false;
}

bool BrokerWatch::mark_pull(uint64_t tag, int64_t now_us) noexcept {
    for (int i = 0; i < kCap; ++i) {
        if (row_[i].used && row_[i].tag == tag) {
            row_[i].pull_us = now_us;
            return true;
        }
    }
    return false;
}

int BrokerWatch::expire(int64_t now_us, int64_t ack_limit_us, int64_t life_limit_us, int64_t pull_limit_us,
                        uint64_t* tags, int cap) noexcept {
    int n = 0;
    for (int i = 0; i < kCap && n < cap; ++i) {
        Sent& s = row_[i];
        if (!s.used) continue;
        bool due = false;
        if (!s.acked && ack_limit_us > 0 && now_us - s.sent_us >= ack_limit_us) due = true;
        if (s.acked && life_limit_us > 0 && now_us - s.sent_us >= life_limit_us) due = true;
        if (s.pull_us > 0 && pull_limit_us > 0 && now_us - s.pull_us >= pull_limit_us) due = true;
        if (!due) continue;
        if (tags) tags[n] = s.tag;
        ++n;
        s.used = 0;
        if (live_ > 0) --live_;
    }
    return n;
}

bool QueryFold::push(uint32_t engine, uint32_t acct) noexcept {
    if (engine >= kEngines || acct >= kAccounts) return false;
    Lane& lane = lane_[engine];
    if (lane.tail - lane.head >= kQueue) return false;
    lane.q[lane.tail % kQueue] = acct;
    ++lane.tail;
    return true;
}

void QueryFold::fold() noexcept {
    for (uint32_t e = 0; e < kEngines; ++e) {
        Lane& lane = lane_[e];
        while (lane.head != lane.tail) {
            const uint32_t acct = lane.q[lane.head % kQueue];
            ++lane.head;
            if (acct < kAccounts) wait_[e][acct] = 1;
        }
    }
}

int QueryFold::pending(uint32_t* out, int cap) const noexcept {
    int n = 0;
    for (uint32_t a = 0; a < kAccounts && n < cap; ++a) {
        bool any = false;
        for (uint32_t e = 0; e < kEngines; ++e) any = any || wait_[e][a] != 0;
        if (!any) continue;
        if (out) out[n] = a;
        ++n;
    }
    return n;
}

void QueryFold::clear_acct(uint32_t acct) noexcept {
    if (acct >= kAccounts) return;
    for (uint32_t e = 0; e < kEngines; ++e) wait_[e][acct] = 0;
}

bool acct_visible(const AcctLink* rows, int n, const char* trader, const char* counter, const char* account) noexcept {
    if (!rows || n <= 0 || !trader || !counter || !account) return false;
    for (int i = 0; i < n; ++i) {
        if (!rows[i].enabled) continue;
        if (std::strcmp(rows[i].trader, trader) == 0 && std::strcmp(rows[i].counter, counter) == 0 &&
            std::strcmp(rows[i].account, account) == 0)
            return true;
    }
    return false;
}

bool reply_route(const char* counter, char* out, int cap) noexcept {
    if (!counter || !out || cap < 8) return false;
    const int n = static_cast<int>(std::strlen(counter));
    if (n <= 0 || n + 7 >= cap) return false;
    for (int i = 0; i < n; ++i) out[i] = counter[i];
    const char* suf = ".reply";
    for (int i = 0; suf[i]; ++i) out[n + i] = suf[i];
    out[n + 6] = 0;
    return true;
}

}  // namespace pitlane
