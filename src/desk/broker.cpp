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

bool BrokerWatch::row_due(const Sent& s, int64_t now_us, int64_t ack_limit_us, int64_t life_limit_us,
                          int64_t pull_limit_us) noexcept {
    if (!s.used) return false;
    if (!s.acked && ack_limit_us > 0 && now_us - s.sent_us >= ack_limit_us) return true;
    if (s.acked && life_limit_us > 0 && now_us - s.sent_us >= life_limit_us) return true;
    if (s.pull_us > 0 && pull_limit_us > 0 && now_us - s.pull_us >= pull_limit_us) return true;
    return false;
}

int BrokerWatch::due(int64_t now_us, int64_t ack_limit_us, int64_t life_limit_us, int64_t pull_limit_us, uint64_t* tags,
                     int cap) const noexcept {
    if (!tags || cap <= 0) return -1;
    int need = 0;
    for (int i = 0; i < kCap; ++i) {
        if (row_due(row_[i], now_us, ack_limit_us, life_limit_us, pull_limit_us)) ++need;
    }
    if (need > cap) return -1;
    int n = 0;
    for (int i = 0; i < kCap; ++i) {
        if (!row_due(row_[i], now_us, ack_limit_us, life_limit_us, pull_limit_us)) continue;
        tags[n++] = row_[i].tag;
    }
    return n;
}

int BrokerWatch::expire(int64_t now_us, int64_t ack_limit_us, int64_t life_limit_us, int64_t pull_limit_us,
                        uint64_t* tags, int cap) noexcept {
    int n = 0;
    for (int i = 0; i < kCap && n < cap; ++i) {
        Sent& s = row_[i];
        if (!row_due(s, now_us, ack_limit_us, life_limit_us, pull_limit_us)) continue;
        if (tags) tags[n] = s.tag;
        ++n;
        s.used = 0;
        if (live_ > 0) --live_;
    }
    return n;
}

bool QueryFold::push(uint32_t engine, uint32_t acct) noexcept { return push(engine, acct, 0, 0); }

bool QueryFold::push(uint32_t engine, uint32_t acct, uint8_t kind, uint64_t page) noexcept {
    if (engine >= kEngines || acct >= kAccounts || kind >= kKinds) return false;
    Lane& lane = lane_[engine];
    if (lane.tail - lane.head >= kQueue) return false;
    const uint32_t slot = lane.tail % kQueue;
    lane.q[slot] = acct;
    lane.kind[slot] = kind;
    lane.page[slot] = page;
    ++lane.tail;
    return true;
}

void QueryFold::fold() noexcept {
    for (uint32_t e = 0; e < kEngines; ++e) {
        Lane& lane = lane_[e];
        while (lane.head != lane.tail) {
            const uint32_t slot = lane.head % kQueue;
            const uint32_t acct = lane.q[slot];
            const uint8_t kind = lane.kind[slot];
            const uint64_t page = lane.page[slot];
            ++lane.head;
            if (acct >= kAccounts || kind >= kKinds) continue;
            wait_[e][acct][kind] = 1;
            page_[e][acct][kind] = page;
        }
    }
}

int QueryFold::pending(uint32_t* out, int cap) const noexcept {
    int n = 0;
    for (uint32_t a = 0; a < kAccounts && n < cap; ++a) {
        bool any = false;
        for (uint32_t e = 0; e < kEngines && !any; ++e) {
            for (uint32_t k = 0; k < kKinds; ++k) {
                if (wait_[e][a][k] != 0) {
                    any = true;
                    break;
                }
            }
        }
        if (!any) continue;
        if (out) out[n] = a;
        ++n;
    }
    return n;
}

int QueryFold::pending_kind(QueryWant* out, int cap) const noexcept {
    if (!out || cap <= 0) return -1;
    int n = 0;
    for (uint32_t a = 0; a < kAccounts; ++a) {
        for (uint32_t k = 0; k < kKinds; ++k) {
            bool any = false;
            uint64_t page = 0;
            for (uint32_t e = 0; e < kEngines; ++e) {
                if (wait_[e][a][k] == 0) continue;
                any = true;
                page = page_[e][a][k];
            }
            if (!any) continue;
            if (n >= cap) return -1;
            out[n].acct = a;
            out[n].kind = static_cast<uint8_t>(k);
            out[n].page = page;
            ++n;
        }
    }
    return n;
}

void QueryFold::clear_acct(uint32_t acct) noexcept {
    if (acct >= kAccounts) return;
    for (uint32_t e = 0; e < kEngines; ++e) {
        for (uint32_t k = 0; k < kKinds; ++k) {
            wait_[e][acct][k] = 0;
            page_[e][acct][k] = 0;
        }
    }
}

void QueryFold::clear_want(uint32_t acct, uint8_t kind) noexcept {
    if (acct >= kAccounts || kind >= kKinds) return;
    for (uint32_t e = 0; e < kEngines; ++e) {
        wait_[e][acct][kind] = 0;
        page_[e][acct][kind] = 0;
    }
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
