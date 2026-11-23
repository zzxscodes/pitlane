#pragma once

// 快照订单簿：用 CTP 的五档快照重建十档视图，并做增量维护。
// 快照本身是“全量覆盖”，这里的增量指：与上一帧逐档比较，只有变化的档位才产生更新事件；
// 同时用交易所时间戳做顺序判定，乱序或跳变时整簿重建（fail-closed，不猜）。
//
// 所有状态定长、无堆、无锁。写者（行情线程）唯一，读者可用 copy() 拿一致快照。

#include "decode.hpp"

#include <cstdint>

namespace pitlane {

enum CtpBookEvent : uint8_t {
    kCtpBookOk = 0,
    kCtpBookCrossed = 1,
    kCtpBookStale = 2,    // 时间戳回退或超过 max_gap_us
    kCtpBookRebuilt = 3,
    kCtpBookNoQuote = 4
};

inline const char* ctp_book_event_name(uint8_t e) noexcept {
    switch (e) {
        case kCtpBookCrossed: return "crossed";
        case kCtpBookStale: return "stale";
        case kCtpBookRebuilt: return "rebuilt";
        case kCtpBookNoQuote: return "no_quote";
        default: return "ok";
    }
}

struct CtpBookStats {
    uint64_t snaps{0};
    uint64_t updates{0};   // 变化的档位总数
    uint64_t inserts{0};
    uint64_t deletes{0};
    uint64_t rebuilds{0};
    uint64_t stale{0};
    uint64_t crossed{0};
    uint64_t dropped{0};
};

struct CtpBookView {
    uint32_t name{0};
    uint8_t status{kCtpNoQuote};
    uint8_t n_bid{0};
    uint8_t n_ask{0};
    uint8_t pad{0};
    int64_t exch_us{0};
    int64_t last_px{0};
    int64_t upper_limit{0};
    int64_t lower_limit{0};
    CtpLevel bid[kCtpLevels]{};
    CtpLevel ask[kCtpLevels]{};
};

class CtpDepthBook {
public:
    CtpDepthBook() noexcept = default;
    // max_gap_us 为 0 时只看时间是否回退；否则相邻快照间隔超过它也算跳变。
    explicit CtpDepthBook(int64_t max_gap_us) noexcept : max_gap_us_(max_gap_us) {}

    void bind(uint32_t name) noexcept { name_ = name; }
    uint32_t name() const noexcept { return name_; }

    void clear() noexcept {
        for (int i = 0; i < kCtpLevels; ++i) {
            bid_[i] = CtpLevel{};
            ask_[i] = CtpLevel{};
        }
        nb_ = 0;
        na_ = 0;
        last_us_ = 0;
        last_px_ = 0;
        upper_ = 0;
        lower_ = 0;
        status_ = kCtpNoQuote;
        stats_ = CtpBookStats{};
    }

    // 应用一帧快照。事件写入 *ev（可为空）。返回 false 表示该帧被丢弃。
    bool apply(const CtpSnap& s, CtpBookEvent* ev = nullptr) noexcept {
        CtpBookEvent e = kCtpBookOk;
        if (s.name == 0 || s.exch_us <= 0) {
            ++stats_.dropped;
            if (ev) *ev = kCtpBookStale;
            return false;
        }

        bool stale = false;
        if (last_us_ != 0 && s.exch_us < last_us_) stale = true;
        if (!stale && last_us_ != 0 && max_gap_us_ > 0 && s.exch_us - last_us_ > max_gap_us_) stale = true;

        ++stats_.snaps;
        if (stale) {
            // 乱序或长时间跳变：整簿重建，并把这一帧之前的档位变化全部记为删除/新增。
            ++stats_.stale;
            rebuild(s);
            e = kCtpBookStale;
            if (ev) *ev = e;
            return true;
        }

        diff(s, true, nb_);
        diff(s, false, na_);
        nb_ = s.n_bid;
        na_ = s.n_ask;
        for (int i = 0; i < s.n_bid; ++i) bid_[i] = s.bid[i];
        for (int i = 0; i < s.n_ask; ++i) ask_[i] = s.ask[i];
        last_us_ = s.exch_us;
        last_px_ = s.last_px > 0 ? s.last_px : last_px_;
        upper_ = s.upper_limit;
        lower_ = s.lower_limit;
        status_ = s.status;

        if (s.crossed) {
            ++stats_.crossed;
            e = kCtpBookCrossed;
        } else if (nb_ == 0 && na_ == 0) {
            e = kCtpBookNoQuote;
        }
        if (ev) *ev = e;
        return true;
    }

    // 强制整簿重建：清空后按当前快照写入。
    void rebuild(const CtpSnap& s) noexcept {
        ++stats_.rebuilds;
        for (int i = 0; i < kCtpLevels; ++i) {
            bid_[i] = CtpLevel{};
            ask_[i] = CtpLevel{};
        }
        nb_ = 0;
        na_ = 0;
        for (int i = 0; i < s.n_bid; ++i) bid_[nb_++] = s.bid[i];
        for (int i = 0; i < s.n_ask; ++i) ask_[na_++] = s.ask[i];
        stats_.inserts += static_cast<uint64_t>(nb_ + na_);
        last_us_ = s.exch_us;
        last_px_ = s.last_px;
        upper_ = s.upper_limit;
        lower_ = s.lower_limit;
        status_ = s.status;
    }

    bool best(CtpLevel& bid, CtpLevel& ask) const noexcept {
        bid = nb_ > 0 ? bid_[0] : CtpLevel{};
        ask = na_ > 0 ? ask_[0] : CtpLevel{};
        return nb_ > 0 && na_ > 0;
    }

    bool level(uint8_t side, int i, CtpLevel& out) const noexcept {
        const CtpLevel* row = side == 1 ? bid_ : (side == 2 ? ask_ : nullptr);
        const int n = side == 1 ? nb_ : (side == 2 ? na_ : 0);
        if (!row || i < 0 || i >= n) return false;
        out = row[i];
        return true;
    }

    int count(uint8_t side) const noexcept { return side == 1 ? nb_ : (side == 2 ? na_ : 0); }

    int64_t price_at(uint8_t side, int i) const noexcept {
        CtpLevel l{};
        return level(side, i, l) ? l.px : 0;
    }

    // 盘口不平衡（买一量 - 卖一量）/（买一量 + 卖一量），单位 ppm。
    int64_t imbalance_ppm() const noexcept {
        const int64_t bq = nb_ > 0 ? bid_[0].qty : 0;
        const int64_t aq = na_ > 0 ? ask_[0].qty : 0;
        const int64_t sum = bq + aq;
        return sum > 0 ? (bq - aq) * 1000000 / sum : 0;
    }

    void copy(CtpBookView& out) const noexcept {
        out = CtpBookView{};
        out.name = name_;
        out.n_bid = static_cast<uint8_t>(nb_);
        out.n_ask = static_cast<uint8_t>(na_);
        out.exch_us = last_us_;
        out.last_px = last_px_;
        out.upper_limit = upper_;
        out.lower_limit = lower_;
        out.status = status_;
        for (int i = 0; i < nb_; ++i) out.bid[i] = bid_[i];
        for (int i = 0; i < na_; ++i) out.ask[i] = ask_[i];
    }

    const CtpBookStats& stats() const noexcept { return stats_; }
    int64_t last_us() const noexcept { return last_us_; }
    uint8_t status() const noexcept { return status_; }

private:
    // 与上一帧同侧比较：新增档位记 insert，消失档位记 delete，价格相同但量变记 update。
    void diff(const CtpSnap& s, bool is_bid, int old_n) noexcept {
        const CtpLevel* old_row = is_bid ? bid_ : ask_;
        const CtpLevel* new_row = is_bid ? s.bid : s.ask;
        const int new_n = is_bid ? s.n_bid : s.n_ask;
        int i = 0;
        int j = 0;
        while (i < old_n || j < new_n) {
            if (i >= old_n) {
                ++stats_.inserts;
                ++stats_.updates;
                ++j;
            } else if (j >= new_n) {
                ++stats_.deletes;
                ++stats_.updates;
                ++i;
            } else if (old_row[i].px == new_row[j].px) {
                if (old_row[i].qty != new_row[j].qty) ++stats_.updates;
                ++i;
                ++j;
            } else if (old_row[i].px > new_row[j].px) {
                ++stats_.deletes;
                ++stats_.updates;
                ++i;
            } else {
                ++stats_.inserts;
                ++stats_.updates;
                ++j;
            }
        }
    }

    CtpLevel bid_[kCtpLevels]{};
    CtpLevel ask_[kCtpLevels]{};
    uint32_t name_{0};
    int nb_{0};
    int na_{0};
    int64_t last_us_{0};
    int64_t last_px_{0};
    int64_t upper_{0};
    int64_t lower_{0};
    int64_t max_gap_us_{5000000};
    uint8_t status_{kCtpNoQuote};
    CtpBookStats stats_{};
};

// 多品种订单簿。品种以 name（InstMap 分配的编号）绑定，最多 1024 个，
// 与 1024 个合约的目录容量一致。编号直接索引槽位，查找为 O(1)。
class CtpBookSet {
public:
    static constexpr int kNames = 1024;
    // InstMap 的编号从 1 开始且不超过 kNames，槽位数组按编号直接寻址。
    static constexpr int kSlots = kNames + 1;

    CtpDepthBook* find(uint32_t name) noexcept {
        if (name >= static_cast<uint32_t>(kSlots)) return nullptr;
        const uint32_t s = slot_[name];
        return s ? &books_[s - 1] : nullptr;
    }

    const CtpDepthBook* find(uint32_t name) const noexcept {
        if (name >= static_cast<uint32_t>(kSlots)) return nullptr;
        const uint32_t s = slot_[name];
        return s ? &books_[s - 1] : nullptr;
    }

    CtpDepthBook* ensure(uint32_t name) noexcept {
        if (CtpDepthBook* b = find(name)) return b;
        if (name == 0 || name >= static_cast<uint32_t>(kSlots) || n_ >= kNames) return nullptr;
        books_[n_].bind(name);
        names_[n_] = name;
        slot_[name] = static_cast<uint32_t>(n_ + 1);
        return &books_[n_++];
    }

    int count() const noexcept { return n_; }
    uint32_t name_at(int i) const noexcept { return i >= 0 && i < n_ ? names_[i] : 0; }
    CtpDepthBook& at(int i) noexcept { return books_[i]; }
    const CtpDepthBook& at(int i) const noexcept { return books_[i]; }

private:
    uint32_t names_[kNames]{};
    uint32_t slot_[kSlots]{}; // name -> 1-based book index, 0 means absent
    CtpDepthBook books_[kNames]{};
    int n_{0};
};

} // namespace pitlane
