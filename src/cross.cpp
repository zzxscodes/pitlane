#include "pitlane/cross.hpp"

#include <cstring>
#include <vector>

namespace pitlane {
namespace {

uint32_t mix_px(uint8_t side, int64_t px) noexcept {
    uint64_t x = static_cast<uint64_t>(px) * 0x9E3779B185EBCA87ull;
    x ^= static_cast<uint64_t>(side) << 17;
    return static_cast<uint32_t>(x);
}

uint32_t mix_tag(uint64_t tag) noexcept {
    uint64_t x = tag * 0xBF58476D1CE4E5B9ull;
    x ^= x >> 30;
    return static_cast<uint32_t>(x);
}

}  // namespace

CrossBook::CrossBook() { reset(); }

void CrossBook::reset() noexcept {
    std::memset(px_state_, 0, sizeof px_state_);
    std::memset(tag_state_, 0, sizeof tag_state_);
    best_bid_ = 0;
    best_ask_ = 0;
    live_ = 0;
    free_node_ = 1;
    for (uint32_t i = 1; i < kNodes - 1; ++i) nodes_[i].next = i + 1;
    nodes_[kNodes - 1].next = 0;
    free_level_ = 1;
    for (uint32_t i = 1; i < kLevels - 1; ++i) levels_[i].worse = i + 1;
    levels_[kLevels - 1].worse = 0;
}

uint32_t CrossBook::alloc_node() noexcept {
    const uint32_t id = free_node_;
    if (!id) return 0;
    free_node_ = nodes_[id].next;
    nodes_[id] = Node{};
    return id;
}

void CrossBook::recycle_node(uint32_t id) noexcept {
    nodes_[id].next = free_node_;
    free_node_ = id;
}

uint32_t CrossBook::alloc_level() noexcept {
    const uint32_t id = free_level_;
    if (!id) return 0;
    free_level_ = levels_[id].worse;
    levels_[id] = Level{};
    return id;
}

void CrossBook::recycle_level(uint32_t id) noexcept {
    levels_[id].worse = free_level_;
    free_level_ = id;
}

uint32_t* CrossBook::best_of(uint8_t side) noexcept { return buy_side(side) ? &best_bid_ : &best_ask_; }

const uint32_t* CrossBook::best_of(uint8_t side) const noexcept {
    return buy_side(side) ? &best_bid_ : &best_ask_;
}

uint32_t CrossBook::find_level(uint8_t side, int64_t px) const noexcept {
    uint32_t i = mix_px(side, px) & (kHash - 1u);
    for (uint32_t n = 0; n < kHash; ++n) {
        const uint32_t s = (i + n) & (kHash - 1u);
        if (px_state_[s] == 0) return 0;
        if (px_state_[s] != 1) continue;
        const uint32_t id = px_slot_[s];
        if (id && levels_[id].side == side && levels_[id].px == px) return id;
    }
    return 0;
}

uint32_t CrossBook::find_tag(uint64_t tag) const noexcept {
    if (tag == 0) return 0;
    constexpr uint32_t kTags = kHash * 2;
    uint32_t i = mix_tag(tag) & (kTags - 1u);
    for (uint32_t n = 0; n < kTags; ++n) {
        const uint32_t s = (i + n) & (kTags - 1u);
        if (tag_state_[s] == 0) return 0;
        if (tag_state_[s] == 1 && tag_key_[s] == tag) return tag_node_[s];
    }
    return 0;
}

void CrossBook::index_level(uint32_t id) noexcept {
    uint32_t i = mix_px(levels_[id].side, levels_[id].px) & (kHash - 1u);
    int tomb = -1;
    for (uint32_t n = 0; n < kHash; ++n) {
        const uint32_t s = (i + n) & (kHash - 1u);
        if (px_state_[s] == 1 && px_slot_[s] == id) return;
        if (px_state_[s] == 2 && tomb < 0) tomb = static_cast<int>(s);
        if (px_state_[s] == 0) {
            const uint32_t at = tomb >= 0 ? static_cast<uint32_t>(tomb) : s;
            px_state_[at] = 1;
            px_slot_[at] = id;
            return;
        }
    }
    if (tomb >= 0) {
        px_state_[tomb] = 1;
        px_slot_[tomb] = id;
    }
}

void CrossBook::drop_level_index(uint8_t side, int64_t px) noexcept {
    uint32_t i = mix_px(side, px) & (kHash - 1u);
    for (uint32_t n = 0; n < kHash; ++n) {
        const uint32_t s = (i + n) & (kHash - 1u);
        if (px_state_[s] == 0) return;
        if (px_state_[s] == 1 && levels_[px_slot_[s]].side == side && levels_[px_slot_[s]].px == px) {
            px_state_[s] = 2;
            return;
        }
    }
}

void CrossBook::index_tag(uint64_t tag, uint32_t node) noexcept {
    constexpr uint32_t kTags = kHash * 2;
    uint32_t i = mix_tag(tag) & (kTags - 1u);
    int tomb = -1;
    for (uint32_t n = 0; n < kTags; ++n) {
        const uint32_t s = (i + n) & (kTags - 1u);
        if (tag_state_[s] == 2 && tomb < 0) tomb = static_cast<int>(s);
        if (tag_state_[s] == 0) {
            const uint32_t at = tomb >= 0 ? static_cast<uint32_t>(tomb) : s;
            tag_state_[at] = 1;
            tag_key_[at] = tag;
            tag_node_[at] = node;
            return;
        }
    }
    if (tomb >= 0) {
        tag_state_[tomb] = 1;
        tag_key_[tomb] = tag;
        tag_node_[tomb] = node;
    }
}

void CrossBook::drop_tag(uint64_t tag) noexcept {
    constexpr uint32_t kTags = kHash * 2;
    uint32_t i = mix_tag(tag) & (kTags - 1u);
    for (uint32_t n = 0; n < kTags; ++n) {
        const uint32_t s = (i + n) & (kTags - 1u);
        if (tag_state_[s] == 0) return;
        if (tag_state_[s] == 1 && tag_key_[s] == tag) {
            tag_state_[s] = 2;
            return;
        }
    }
}

void CrossBook::link_level(uint32_t id) noexcept {
    Level& lv = levels_[id];
    uint32_t& best = *best_of(lv.side);
    lv.better = 0;
    lv.worse = 0;
    const bool bid = buy_side(lv.side);
    auto tighter = [&](int64_t a, int64_t b) { return bid ? a > b : a < b; };
    if (!best) {
        best = id;
        return;
    }
    if (tighter(lv.px, levels_[best].px)) {
        lv.worse = best;
        levels_[best].better = id;
        best = id;
        return;
    }
    uint32_t prev = best;
    uint32_t cur = levels_[best].worse;
    while (cur && tighter(levels_[cur].px, lv.px)) {
        prev = cur;
        cur = levels_[cur].worse;
    }
    lv.better = prev;
    lv.worse = cur;
    levels_[prev].worse = id;
    if (cur) levels_[cur].better = id;
}

void CrossBook::unlink_level(uint32_t id) noexcept {
    Level& lv = levels_[id];
    const uint32_t better = lv.better;
    const uint32_t worse = lv.worse;
    const uint8_t side = lv.side;
    const int64_t px = lv.px;
    if (better) levels_[better].worse = worse;
    else *best_of(side) = worse;
    if (worse) levels_[worse].better = better;
    drop_level_index(side, px);
    recycle_level(id);
}

void CrossBook::push_tail(uint32_t level, uint32_t node) noexcept {
    Node& nd = nodes_[node];
    Level& lv = levels_[level];
    nd.level = level;
    nd.prev = lv.tail;
    nd.next = 0;
    if (lv.tail) nodes_[lv.tail].next = node;
    else lv.head = node;
    lv.tail = node;
}

void CrossBook::unlink_node(uint32_t node) noexcept {
    Node& nd = nodes_[node];
    Level& lv = levels_[nd.level];
    if (nd.prev) nodes_[nd.prev].next = nd.next;
    else lv.head = nd.next;
    if (nd.next) nodes_[nd.next].prev = nd.prev;
    else lv.tail = nd.prev;
}

int64_t CrossBook::clean_qty(uint8_t side, int64_t px, uint32_t acct) const noexcept {
    int64_t sum = 0;
    uint32_t lv = buy_side(side) ? best_ask_ : best_bid_;
    while (lv) {
        const Level& level = levels_[lv];
        const bool cross = buy_side(side) ? level.px <= px : level.px >= px;
        if (!cross) break;
        for (uint32_t n = level.head; n; n = nodes_[n].next) {
            if (acct != 0 && nodes_[n].acct == acct) return sum;
            sum += nodes_[n].leaves;
        }
        lv = level.worse;
    }
    return sum;
}

bool CrossBook::next_is_self(uint8_t side, int64_t px, uint32_t acct) const noexcept {
    if (acct == 0) return false;
    const uint32_t lv = buy_side(side) ? best_ask_ : best_bid_;
    if (!lv) return false;
    const Level& level = levels_[lv];
    const bool cross = buy_side(side) ? level.px <= px : level.px >= px;
    if (!cross || !level.head) return false;
    return nodes_[level.head].acct == acct;
}

bool CrossBook::top_bid(int64_t* px, int64_t* qty) const noexcept {
    if (!best_bid_) return false;
    if (px) *px = levels_[best_bid_].px;
    if (qty) *qty = levels_[best_bid_].sum;
    return true;
}

bool CrossBook::top_ask(int64_t* px, int64_t* qty) const noexcept {
    if (!best_ask_) return false;
    if (px) *px = levels_[best_ask_].px;
    if (qty) *qty = levels_[best_ask_].sum;
    return true;
}

bool CrossBook::withdraw(uint64_t tag, int64_t* leaves_out) noexcept {
    const uint32_t node = find_tag(tag);
    if (!node) return false;
    const int64_t leaves = nodes_[node].leaves;
    const uint32_t level = nodes_[node].level;
    unlink_node(node);
    levels_[level].sum -= leaves;
    drop_tag(tag);
    recycle_node(node);
    if (live_ > 0) --live_;
    if (levels_[level].head == 0 || levels_[level].sum <= 0) unlink_level(level);
    if (leaves_out) *leaves_out = leaves;
    return true;
}

int CrossBook::submit(const CrossIn& in, Deal* out, int cap, int* why) noexcept {
    auto set = [&](Why w) {
        if (why) *why = static_cast<int>(w);
    };
    set(Why::None);
    if (in.tag == 0 || in.qty <= 0 || in.px <= 0) {
        set(Why::Qty);
        return 0;
    }
    if (!known_side(in.side)) {
        set(Why::Side);
        return 0;
    }
    if (find_tag(in.tag)) {
        set(Why::Dup);
        return 0;
    }
    const auto style = static_cast<Style>(in.style);
    if (style != Style::Day && style != Style::Passive && style != Style::Immediate && style != Style::All) {
        set(Why::Mode);
        return 0;
    }

    const uint32_t opp = buy_side(in.side) ? best_ask_ : best_bid_;
    const bool crosses = opp && (buy_side(in.side) ? levels_[opp].px <= in.px : levels_[opp].px >= in.px);
    if (style == Style::Passive && crosses) {
        set(Why::Mode);
        return 0;
    }
    const int64_t clean = clean_qty(in.side, in.px, in.acct);
    if (style == Style::All && clean < in.qty) {
        set(Why::Qty);
        return 0;
    }
    if (crosses && next_is_self(in.side, in.px, in.acct) && clean == 0) {
        set(Why::Wash);
        return 0;
    }

    int stored = 0;
    int64_t left = in.qty;
    bool stopped_self = false;
    while (left > 0 && !stopped_self) {
        const uint32_t level_id = buy_side(in.side) ? best_ask_ : best_bid_;
        if (!level_id) break;
        Level& level = levels_[level_id];
        const bool ok = buy_side(in.side) ? level.px <= in.px : level.px >= in.px;
        if (!ok) break;
        uint32_t node = level.head;
        while (node && left > 0) {
            Node& maker = nodes_[node];
            const uint32_t nxt = maker.next;
            if (in.acct != 0 && maker.acct == in.acct) {
                stopped_self = true;
                break;
            }
            const int64_t take = maker.leaves < left ? maker.leaves : left;
            if (out && stored < cap) {
                out[stored] = Deal{in.tag, maker.tag, level.px, take, in.side};
                ++stored;
            } else if (!out) {
                ++stored;
            }
            left -= take;
            maker.leaves -= take;
            level.sum -= take;
            if (maker.leaves == 0) {
                const uint64_t gone = maker.tag;
                unlink_node(node);
                drop_tag(gone);
                recycle_node(node);
                if (live_ > 0) --live_;
            }
            node = nxt;
        }
        if (level.head == 0 || level.sum <= 0) unlink_level(level_id);
    }

    if (left == 0 || stopped_self) return stored;
    if (style == Style::Immediate || style == Style::All) return stored;

    uint32_t level_id = find_level(in.side, in.px);
    bool created = false;
    if (!level_id) {
        level_id = alloc_level();
        if (!level_id) {
            set(Why::Book);
            return stored;
        }
        levels_[level_id].px = in.px;
        levels_[level_id].side = in.side;
        link_level(level_id);
        index_level(level_id);
        created = true;
    }
    const uint32_t node = alloc_node();
    if (!node) {
        if (created) unlink_level(level_id);
        set(Why::Book);
        return stored;
    }
    Node& nd = nodes_[node];
    nd.tag = in.tag;
    nd.acct = in.acct;
    nd.px = in.px;
    nd.leaves = left;
    nd.side = in.side;
    push_tail(level_id, node);
    levels_[level_id].sum += left;
    index_tag(in.tag, node);
    ++live_;
    return stored;
}

int64_t auction_clear(const CallIn* in, int n, int64_t ref_px, int tie_higher, CallOut* out, int cap,
                      int64_t* matched) {
    if (matched) *matched = 0;
    if (!in || n <= 0) return 0;

    int64_t prices[128];
    int np = 0;
    for (int i = 0; i < n; ++i) {
        if (in[i].qty <= 0 || in[i].px <= 0 || !known_side(in[i].side)) continue;
        bool seen = false;
        for (int j = 0; j < np; ++j) {
            if (prices[j] == in[i].px) seen = true;
        }
        if (seen || np >= 128) continue;
        int at = np;
        while (at > 0 && prices[at - 1] > in[i].px) {
            prices[at] = prices[at - 1];
            --at;
        }
        prices[at] = in[i].px;
        ++np;
    }
    if (np == 0) return 0;

    auto nearer = [](int64_t a, int64_t b, int64_t ref) {
        const int64_t da = a > ref ? a - ref : ref - a;
        const int64_t db = b > ref ? b - ref : ref - b;
        return da < db;
    };

    int64_t best_vol = -1;
    int64_t best_px = prices[0];
    for (int p = 0; p < np; ++p) {
        const int64_t px = prices[p];
        int64_t buy = 0;
        int64_t sell = 0;
        for (int i = 0; i < n; ++i) {
            if (in[i].qty <= 0) continue;
            if (buy_side(in[i].side) && in[i].px >= px) buy += in[i].qty;
            else if (sell_side(in[i].side) && in[i].px <= px) sell += in[i].qty;
        }
        const int64_t cross = buy < sell ? buy : sell;
        if (cross > best_vol) {
            best_vol = cross;
            best_px = px;
        } else if (cross == best_vol) {
            if (tie_higher) {
                if (px > best_px) best_px = px;
            } else if (nearer(px, best_px, ref_px)) {
                best_px = px;
            }
        }
    }
    if (best_vol < 0) best_vol = 0;
    if (matched) *matched = best_vol;
    if (best_vol == 0 || !out || cap <= 0) return best_px;

    std::vector<int64_t> left(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) left[static_cast<std::size_t>(i)] = in[i].qty;
    int64_t remain = best_vol;
    int stored = 0;
    int bi = 0;
    int si = 0;
    while (remain > 0 && stored < cap) {
        while (bi < n && !(buy_side(in[bi].side) && in[bi].px >= best_px && left[static_cast<std::size_t>(bi)] > 0)) ++bi;
        while (si < n && !(sell_side(in[si].side) && in[si].px <= best_px && left[static_cast<std::size_t>(si)] > 0)) ++si;
        if (bi >= n || si >= n) break;
        int64_t q = left[static_cast<std::size_t>(bi)];
        if (left[static_cast<std::size_t>(si)] < q) q = left[static_cast<std::size_t>(si)];
        if (remain < q) q = remain;
        out[stored++] = CallOut{in[bi].tag, in[si].tag, best_px, q};
        left[static_cast<std::size_t>(bi)] -= q;
        left[static_cast<std::size_t>(si)] -= q;
        remain -= q;
    }
    return best_px;
}

}  // namespace pitlane
