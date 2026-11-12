#include "pitlane/desk/yard.hpp"

#include <cstring>

namespace pitlane {
namespace {

constexpr char kYardMagic[8] = {'P', 'I', 'T', 'Y', 'A', 'R', 'D', '2'};

bool is_pow2(uint32_t v) noexcept { return v >= 2 && (v & (v - 1u)) == 0; }

std::size_t align_up(std::size_t n, std::size_t a) noexcept { return (n + (a - 1)) & ~(a - 1); }

bool code_copy(char* dst, const char* src) noexcept {
    if (!dst || !src || !src[0]) return false;
    int n = 0;
    for (; src[n] && n < 31; ++n) dst[n] = src[n];
    if (src[n]) return false;
    dst[n] = 0;
    return true;
}

bool code_eq(const char* a, const char* b) noexcept {
    if (!a || !b) return false;
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

struct Layout {
    std::size_t bytes{0};
    std::size_t off_syms{0};
    std::size_t off_quote_pub{0};
    std::size_t off_quote_ack{0};
    std::size_t off_quote_cells{0};
    std::size_t off_quote_free{0};
    std::size_t off_order_pub{0};
    std::size_t off_order_cells{0};
    std::size_t off_order_free{0};
    std::size_t off_trade_pub{0};
    std::size_t off_trade_cells{0};
    std::size_t off_trade_free{0};
    std::size_t off_rebuild_pub{0};
    std::size_t off_rebuild_cells{0};
    std::size_t off_rebuild_free{0};
    std::size_t off_mix_pub{0};
    std::size_t off_mix_cells{0};
    std::size_t off_mix_free{0};
    std::size_t off_facts{0};
    std::size_t off_hours{0};
};

bool build_layout(const YardPlan& plan, Layout& L) noexcept {
    if (!yard_plan_ok(plan)) return false;
    L = Layout{};
    std::size_t at = align_up(sizeof(YardHead), 64);
    L.off_syms = at;
    at = align_up(at + sizeof(YardSym) * plan.sym_slots, 64);

    L.off_quote_pub = at;
    at = align_up(at + sizeof(std::atomic<uint64_t>) * plan.quote_lanes, 64);
    L.off_quote_ack = at;
    at = align_up(at + sizeof(std::atomic<uint64_t>) * plan.quote_lanes, 64);
    L.off_quote_cells = at;
    at = align_up(at + sizeof(YardMark) * static_cast<std::size_t>(plan.quote_lanes) * plan.quote_depth, 64);
    L.off_quote_free = at;
    at = align_up(at + sizeof(int32_t) * plan.quote_lanes, 64);

    L.off_order_pub = at;
    at = align_up(at + sizeof(std::atomic<uint64_t>) * plan.order_lanes, 64);
    L.off_order_cells = at;
    at = align_up(at + sizeof(YardTick) * static_cast<std::size_t>(plan.order_lanes) * plan.order_depth, 64);
    L.off_order_free = at;
    at = align_up(at + sizeof(int32_t) * plan.order_lanes, 64);

    L.off_trade_pub = at;
    at = align_up(at + sizeof(std::atomic<uint64_t>) * plan.trade_lanes, 64);
    L.off_trade_cells = at;
    at = align_up(at + sizeof(YardPrint) * static_cast<std::size_t>(plan.trade_lanes) * plan.trade_depth, 64);
    L.off_trade_free = at;
    at = align_up(at + sizeof(int32_t) * plan.trade_lanes, 64);

    L.off_rebuild_pub = at;
    at = align_up(at + sizeof(std::atomic<uint64_t>) * plan.rebuild_lanes, 64);
    L.off_rebuild_cells = at;
    at = align_up(at + sizeof(YardMark) * static_cast<std::size_t>(plan.rebuild_lanes) * plan.rebuild_depth, 64);
    L.off_rebuild_free = at;
    at = align_up(at + sizeof(int32_t) * plan.rebuild_lanes, 64);

    L.off_mix_pub = at;
    at = align_up(at + sizeof(std::atomic<uint64_t>) * plan.mix_lanes, 64);
    L.off_mix_cells = at;
    at = align_up(at + sizeof(YardMark) * static_cast<std::size_t>(plan.mix_lanes) * plan.mix_depth, 64);
    L.off_mix_free = at;
    at = align_up(at + sizeof(int32_t) * plan.mix_lanes, 64);

    L.off_facts = at;
    at = align_up(at + sizeof(YardFacts) * plan.base_info_cap, 64);
    L.off_hours = at;
    at = align_up(at + sizeof(YardHours) * plan.trade_time_cap, 64);
    L.bytes = at;
    return L.bytes > 0;
}

void wire_view(YardView& y, void* addr, const Layout& L) noexcept {
    auto* base = static_cast<unsigned char*>(addr);
    y.head = reinterpret_cast<YardHead*>(base);
    y.syms = reinterpret_cast<YardSym*>(base + L.off_syms);
    y.quote_pub = reinterpret_cast<std::atomic<uint64_t>*>(base + L.off_quote_pub);
    y.quote_ack = reinterpret_cast<std::atomic<uint64_t>*>(base + L.off_quote_ack);
    y.quote_cells = reinterpret_cast<YardMark*>(base + L.off_quote_cells);
    y.quote_free = reinterpret_cast<int32_t*>(base + L.off_quote_free);
    y.order_pub = reinterpret_cast<std::atomic<uint64_t>*>(base + L.off_order_pub);
    y.order_cells = reinterpret_cast<YardTick*>(base + L.off_order_cells);
    y.order_free = reinterpret_cast<int32_t*>(base + L.off_order_free);
    y.trade_pub = reinterpret_cast<std::atomic<uint64_t>*>(base + L.off_trade_pub);
    y.trade_cells = reinterpret_cast<YardPrint*>(base + L.off_trade_cells);
    y.trade_free = reinterpret_cast<int32_t*>(base + L.off_trade_free);
    y.rebuild_pub = reinterpret_cast<std::atomic<uint64_t>*>(base + L.off_rebuild_pub);
    y.rebuild_cells = reinterpret_cast<YardMark*>(base + L.off_rebuild_cells);
    y.rebuild_free = reinterpret_cast<int32_t*>(base + L.off_rebuild_free);
    y.mix_pub = reinterpret_cast<std::atomic<uint64_t>*>(base + L.off_mix_pub);
    y.mix_cells = reinterpret_cast<YardMark*>(base + L.off_mix_cells);
    y.mix_free = reinterpret_cast<int32_t*>(base + L.off_mix_free);
    y.facts = reinterpret_cast<YardFacts*>(base + L.off_facts);
    y.hours = reinterpret_cast<YardHours*>(base + L.off_hours);
}

void fill_free(int32_t* free, uint32_t n, std::atomic<uint32_t>& top) noexcept {
    for (uint32_t i = 0; i < n; ++i) free[i] = static_cast<int32_t>(n - 1u - i);
    top.store(n, std::memory_order_release);
}

void format_yard(YardView& y, const YardPlan& plan) noexcept {
    YardHead* h = y.head;
    std::memcpy(h->magic, kYardMagic, 8);
    h->version = kYardVersion;
    h->plan = plan;
    h->sym_used.store(0, std::memory_order_relaxed);
    h->facts_used.store(0, std::memory_order_relaxed);
    h->hours_used.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < plan.sym_slots; ++i) y.syms[i] = YardSym{};
    for (uint32_t i = 0; i < plan.quote_lanes; ++i) {
        y.quote_pub[i].store(0, std::memory_order_relaxed);
        y.quote_ack[i].store(0, std::memory_order_relaxed);
    }
    fill_free(y.quote_free, plan.quote_lanes, h->quote_free_top);
    for (uint32_t i = 0; i < plan.order_lanes; ++i) y.order_pub[i].store(0, std::memory_order_relaxed);
    fill_free(y.order_free, plan.order_lanes, h->order_free_top);
    for (uint32_t i = 0; i < plan.trade_lanes; ++i) y.trade_pub[i].store(0, std::memory_order_relaxed);
    fill_free(y.trade_free, plan.trade_lanes, h->trade_free_top);
    for (uint32_t i = 0; i < plan.rebuild_lanes; ++i) y.rebuild_pub[i].store(0, std::memory_order_relaxed);
    fill_free(y.rebuild_free, plan.rebuild_lanes, h->rebuild_free_top);
    for (uint32_t i = 0; i < plan.mix_lanes; ++i) y.mix_pub[i].store(0, std::memory_order_relaxed);
    fill_free(y.mix_free, plan.mix_lanes, h->mix_free_top);
    for (uint32_t i = 0; i < plan.base_info_cap; ++i) y.facts[i] = YardFacts{};
    for (uint32_t i = 0; i < plan.trade_time_cap; ++i) y.hours[i] = YardHours{};
}

bool bind_lane(int32_t& lane_field, std::atomic<uint32_t>& top, int32_t* free, uint32_t lanes) noexcept {
    if (lane_field >= 0) return true;
    if (!free || lanes == 0) return false;
    uint32_t cur = top.load(std::memory_order_acquire);
    for (;;) {
        if (cur == 0) return false;
        if (!top.compare_exchange_weak(cur, cur - 1, std::memory_order_acq_rel, std::memory_order_acquire)) continue;
        const int32_t lane = free[cur - 1];
        if (lane < 0 || static_cast<uint32_t>(lane) >= lanes) return false;
        lane_field = lane;
        return true;
    }
}

template <class Cell>
bool publish_cell(std::atomic<uint64_t>* pub, Cell* cells, uint32_t lane, uint32_t depth, Cell item) noexcept {
    if (!pub || !cells || depth < 2) return false;
    const uint32_t mask = depth - 1u;
    const uint64_t seq = pub[lane].load(std::memory_order_relaxed) + 1ull;
    item.seq = seq;
    cells[static_cast<std::size_t>(lane) * depth + (seq & mask)] = item;
    pub[lane].store(seq, std::memory_order_release);
    return true;
}

template <class Cell>
bool latest_cell(const std::atomic<uint64_t>* pub, const Cell* cells, uint32_t lane, uint32_t depth, Cell& out) noexcept {
    if (!pub || !cells || depth < 2) return false;
    const uint32_t mask = depth - 1u;
    const uint64_t seq = pub[lane].load(std::memory_order_acquire);
    if (seq == 0) return false;
    out = cells[static_cast<std::size_t>(lane) * depth + (seq & mask)];
    return out.seq == seq;
}

} // namespace

bool yard_plan_ok(const YardPlan& plan) noexcept {
    if (plan.sym_slots < 1 || plan.sym_slots > 2000000u) return false;
    if (plan.quote_lanes < 1 || plan.quote_lanes > 2000000u) return false;
    if (!is_pow2(plan.quote_depth) || plan.quote_depth > 65536u) return false;
    if (plan.order_lanes > 1024u || plan.trade_lanes > 1024u || plan.rebuild_lanes > 1024u || plan.mix_lanes > 1024u)
        return false;
    if (!is_pow2(plan.order_depth) || plan.order_depth > (1u << 20)) return false;
    if (!is_pow2(plan.trade_depth) || plan.trade_depth > (1u << 20)) return false;
    if (!is_pow2(plan.rebuild_depth) || plan.rebuild_depth > (1u << 20)) return false;
    if (!is_pow2(plan.mix_depth) || plan.mix_depth > (1u << 20)) return false;
    if (plan.base_info_cap < 1 || plan.base_info_cap > 2000000u) return false;
    if (plan.trade_time_cap < 1 || plan.trade_time_cap > 100000u) return false;
    return true;
}

std::size_t yard_bytes(const YardPlan& plan) noexcept {
    Layout L;
    if (!build_layout(plan, L)) return 0;
    return L.bytes;
}

int yard_open(const char* path, const YardPlan& plan, YardView* out, MapOpt opt, int create) noexcept {
    if (!path || !out || !yard_plan_ok(plan)) return -1;
    Layout L;
    if (!build_layout(plan, L)) return -1;
    *out = YardView{};
    const int rc = create ? map_create(path, L.bytes, &out->map, opt) : map_attach(path, L.bytes, &out->map, opt);
    if (rc != 0 || !out->map.addr) {
        yard_close(out);
        return -1;
    }
    wire_view(*out, out->map.addr, L);
    const bool blank = out->head->magic[0] == 0;
    if (blank) {
        if (!create) {
            yard_close(out);
            return -1;
        }
        format_yard(*out, plan);
    } else {
        if (std::memcmp(out->head->magic, kYardMagic, 8) != 0 || out->head->version != kYardVersion) {
            yard_close(out);
            return -1;
        }
        const YardPlan& got = out->head->plan;
        if (got.sym_slots != plan.sym_slots || got.quote_lanes != plan.quote_lanes || got.quote_depth != plan.quote_depth ||
            got.order_lanes != plan.order_lanes || got.trade_lanes != plan.trade_lanes ||
            got.rebuild_lanes != plan.rebuild_lanes || got.mix_lanes != plan.mix_lanes ||
            got.order_depth != plan.order_depth || got.trade_depth != plan.trade_depth ||
            got.rebuild_depth != plan.rebuild_depth || got.mix_depth != plan.mix_depth ||
            got.base_info_cap != plan.base_info_cap || got.trade_time_cap != plan.trade_time_cap) {
            yard_close(out);
            return -1;
        }
    }
    return 0;
}

void yard_close(YardView* io) noexcept {
    if (!io) return;
    map_release(&io->map);
    *io = YardView{};
}

uint32_t yard_claim_sym(YardView& y, const char* code) noexcept {
    if (!y.head || !y.syms || !code || !code[0]) return 0;
    const uint32_t n = y.head->plan.sym_slots;
    for (uint32_t i = 0; i < n; ++i) {
        if (y.syms[i].live && code_eq(y.syms[i].code, code)) return y.syms[i].id;
    }
    uint32_t used = y.head->sym_used.load(std::memory_order_relaxed);
    for (;;) {
        if (used >= n) return 0;
        if (y.head->sym_used.compare_exchange_weak(used, used + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            YardSym& s = y.syms[used];
            if (!code_copy(s.code, code)) {
                y.head->sym_used.fetch_sub(1, std::memory_order_acq_rel);
                return 0;
            }
            s.id = used + 1;
            s.quote_lane = s.order_lane = s.trade_lane = s.rebuild_lane = s.mix_lane = -1;
            s.facts_id = s.hours_id = 0;
            s.live = 1;
            return s.id;
        }
    }
}

YardSym* yard_sym(YardView& y, uint32_t id) noexcept {
    if (!y.syms || !y.head || id == 0 || id > y.head->plan.sym_slots) return nullptr;
    YardSym& s = y.syms[id - 1];
    return s.live ? &s : nullptr;
}

const YardSym* yard_sym(const YardView& y, uint32_t id) noexcept {
    if (!y.syms || !y.head || id == 0 || id > y.head->plan.sym_slots) return nullptr;
    const YardSym& s = y.syms[id - 1];
    return s.live ? &s : nullptr;
}

bool yard_bind_quote(YardView& y, uint32_t id) noexcept {
    YardSym* s = yard_sym(y, id);
    if (!s) return false;
    return bind_lane(s->quote_lane, y.head->quote_free_top, y.quote_free, y.head->plan.quote_lanes);
}

bool yard_bind_order(YardView& y, uint32_t id) noexcept {
    YardSym* s = yard_sym(y, id);
    if (!s) return false;
    return bind_lane(s->order_lane, y.head->order_free_top, y.order_free, y.head->plan.order_lanes);
}

bool yard_bind_trade(YardView& y, uint32_t id) noexcept {
    YardSym* s = yard_sym(y, id);
    if (!s) return false;
    return bind_lane(s->trade_lane, y.head->trade_free_top, y.trade_free, y.head->plan.trade_lanes);
}

bool yard_bind_rebuild(YardView& y, uint32_t id) noexcept {
    YardSym* s = yard_sym(y, id);
    if (!s) return false;
    return bind_lane(s->rebuild_lane, y.head->rebuild_free_top, y.rebuild_free, y.head->plan.rebuild_lanes);
}

bool yard_bind_mix(YardView& y, uint32_t id) noexcept {
    YardSym* s = yard_sym(y, id);
    if (!s) return false;
    return bind_lane(s->mix_lane, y.head->mix_free_top, y.mix_free, y.head->plan.mix_lanes);
}

bool yard_publish(YardView& y, uint32_t id, const YardMark& mark) noexcept {
    const YardSym* s = yard_sym(y, id);
    if (!s || s->quote_lane < 0) return false;
    return publish_cell(y.quote_pub, y.quote_cells, static_cast<uint32_t>(s->quote_lane), y.head->plan.quote_depth, mark);
}

bool yard_latest(const YardView& y, uint32_t id, YardMark& out) noexcept {
    const YardSym* s = yard_sym(y, id);
    if (!s || s->quote_lane < 0) return false;
    return latest_cell(y.quote_pub, y.quote_cells, static_cast<uint32_t>(s->quote_lane), y.head->plan.quote_depth, out);
}

bool yard_publish_tick(YardView& y, uint32_t id, const YardTick& tick) noexcept {
    const YardSym* s = yard_sym(y, id);
    if (!s || s->order_lane < 0) return false;
    return publish_cell(y.order_pub, y.order_cells, static_cast<uint32_t>(s->order_lane), y.head->plan.order_depth, tick);
}

bool yard_latest_tick(const YardView& y, uint32_t id, YardTick& out) noexcept {
    const YardSym* s = yard_sym(y, id);
    if (!s || s->order_lane < 0) return false;
    return latest_cell(y.order_pub, y.order_cells, static_cast<uint32_t>(s->order_lane), y.head->plan.order_depth, out);
}

bool yard_publish_print(YardView& y, uint32_t id, const YardPrint& print) noexcept {
    const YardSym* s = yard_sym(y, id);
    if (!s || s->trade_lane < 0) return false;
    return publish_cell(y.trade_pub, y.trade_cells, static_cast<uint32_t>(s->trade_lane), y.head->plan.trade_depth, print);
}

bool yard_latest_print(const YardView& y, uint32_t id, YardPrint& out) noexcept {
    const YardSym* s = yard_sym(y, id);
    if (!s || s->trade_lane < 0) return false;
    return latest_cell(y.trade_pub, y.trade_cells, static_cast<uint32_t>(s->trade_lane), y.head->plan.trade_depth, out);
}

bool yard_publish_rebuild(YardView& y, uint32_t id, const YardMark& mark) noexcept {
    const YardSym* s = yard_sym(y, id);
    if (!s || s->rebuild_lane < 0) return false;
    return publish_cell(y.rebuild_pub, y.rebuild_cells, static_cast<uint32_t>(s->rebuild_lane), y.head->plan.rebuild_depth,
                        mark);
}

bool yard_latest_rebuild(const YardView& y, uint32_t id, YardMark& out) noexcept {
    const YardSym* s = yard_sym(y, id);
    if (!s || s->rebuild_lane < 0) return false;
    return latest_cell(y.rebuild_pub, y.rebuild_cells, static_cast<uint32_t>(s->rebuild_lane), y.head->plan.rebuild_depth,
                       out);
}

bool yard_publish_mix(YardView& y, uint32_t id, const YardMark& mark) noexcept {
    const YardSym* s = yard_sym(y, id);
    if (!s || s->mix_lane < 0) return false;
    return publish_cell(y.mix_pub, y.mix_cells, static_cast<uint32_t>(s->mix_lane), y.head->plan.mix_depth, mark);
}

bool yard_latest_mix(const YardView& y, uint32_t id, YardMark& out) noexcept {
    const YardSym* s = yard_sym(y, id);
    if (!s || s->mix_lane < 0) return false;
    return latest_cell(y.mix_pub, y.mix_cells, static_cast<uint32_t>(s->mix_lane), y.head->plan.mix_depth, out);
}

uint32_t yard_claim_facts(YardView& y, const char* code) noexcept {
    if (!y.head || !y.facts || !code || !code[0]) return 0;
    const uint32_t n = y.head->plan.base_info_cap;
    for (uint32_t i = 0; i < n; ++i) {
        if (y.facts[i].live && code_eq(y.facts[i].code, code)) return i + 1;
    }
    uint32_t used = y.head->facts_used.load(std::memory_order_relaxed);
    for (;;) {
        if (used >= n) return 0;
        if (y.head->facts_used.compare_exchange_weak(used, used + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            YardFacts& f = y.facts[used];
            if (!code_copy(f.code, code)) {
                y.head->facts_used.fetch_sub(1, std::memory_order_acq_rel);
                return 0;
            }
            f.tick_sz = f.lot_sz = f.mult = 0;
            f.hours_id = 0;
            f.live = 1;
            return used + 1;
        }
    }
}

YardFacts* yard_facts(YardView& y, uint32_t facts_id) noexcept {
    if (!y.facts || !y.head || facts_id == 0 || facts_id > y.head->plan.base_info_cap) return nullptr;
    YardFacts& f = y.facts[facts_id - 1];
    return f.live ? &f : nullptr;
}

const YardFacts* yard_facts(const YardView& y, uint32_t facts_id) noexcept {
    if (!y.facts || !y.head || facts_id == 0 || facts_id > y.head->plan.base_info_cap) return nullptr;
    const YardFacts& f = y.facts[facts_id - 1];
    return f.live ? &f : nullptr;
}

bool yard_link_facts(YardView& y, uint32_t sym_id, uint32_t facts_id) noexcept {
    YardSym* s = yard_sym(y, sym_id);
    if (!s || !yard_facts(y, facts_id)) return false;
    s->facts_id = static_cast<int32_t>(facts_id);
    return true;
}

uint32_t yard_claim_hours(YardView& y, const char* key) noexcept {
    if (!y.head || !y.hours || !key || !key[0]) return 0;
    const uint32_t n = y.head->plan.trade_time_cap;
    for (uint32_t i = 0; i < n; ++i) {
        if (y.hours[i].live && code_eq(y.hours[i].key, key)) return i + 1;
    }
    uint32_t used = y.head->hours_used.load(std::memory_order_relaxed);
    for (;;) {
        if (used >= n) return 0;
        if (y.head->hours_used.compare_exchange_weak(used, used + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            YardHours& h = y.hours[used];
            if (!code_copy(h.key, key)) {
                y.head->hours_used.fetch_sub(1, std::memory_order_acq_rel);
                return 0;
            }
            h.open_sec = h.close_sec = h.open2_sec = h.close2_sec = 0;
            h.live = 1;
            return used + 1;
        }
    }
}

YardHours* yard_hours(YardView& y, uint32_t hours_id) noexcept {
    if (!y.hours || !y.head || hours_id == 0 || hours_id > y.head->plan.trade_time_cap) return nullptr;
    YardHours& h = y.hours[hours_id - 1];
    return h.live ? &h : nullptr;
}

const YardHours* yard_hours(const YardView& y, uint32_t hours_id) noexcept {
    if (!y.hours || !y.head || hours_id == 0 || hours_id > y.head->plan.trade_time_cap) return nullptr;
    const YardHours& h = y.hours[hours_id - 1];
    return h.live ? &h : nullptr;
}

bool yard_link_hours(YardView& y, uint32_t sym_id, uint32_t hours_id) noexcept {
    YardSym* s = yard_sym(y, sym_id);
    if (!s || !yard_hours(y, hours_id)) return false;
    s->hours_id = static_cast<int32_t>(hours_id);
    if (s->facts_id > 0) {
        if (YardFacts* f = yard_facts(y, static_cast<uint32_t>(s->facts_id))) f->hours_id = static_cast<int32_t>(hours_id);
    }
    return true;
}

} // namespace pitlane
