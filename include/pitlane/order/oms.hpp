#pragma once

#include "pitlane/desk/types.hpp"
#include "pitlane/order/pool.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>
#include <type_traits>

namespace pitlane {

// One order in the order management system. Terminal orders stay in the pool so callers can
// still read their outcome. Ids are owned by the caller; client and venue ids are looked up by scan.

struct OmsOrder {
    uint64_t local;
    uint64_t client;
    uint64_t parent;
    uint64_t venue_id;
    uint32_t name;
    uint32_t acct;
    uint32_t venue;
    int64_t px;
    int64_t qty;
    int64_t filled;
    int64_t leaves;
    int64_t avg_px;
    int64_t created_us;
    int64_t updated_us;
    uint16_t version;
    uint8_t side;
    uint8_t phase;
    uint8_t type;
    uint8_t tif;
    uint64_t seen[8];
    uint8_t seen_n;
    uint8_t seen_head;
};

static_assert(std::is_trivially_copyable<OmsOrder>::value, "oms order");

struct OmsFill {
    int64_t qty;
    int64_t px;
    int64_t leaves;
    int64_t avg_px;
    uint8_t phase;
    uint8_t dup;
};

template <int Cap = 1024>
class Oms {
public:
    static_assert(Cap >= 2 && Cap <= 4096 && (Cap & (Cap - 1)) == 0, "oms capacity");

    Oms() noexcept = default;

    // Staged from a fresh or already staged draft. Duplicate id or a full pool leaves the pool unchanged.
    PITLANE_HOT bool submit(const OmsOrder& draft) noexcept {
        if (unlikely(draft.local == 0 || !known_side(draft.side) || draft.qty <= 0 || draft.px <= 0)) return false;
        const uint8_t none = static_cast<uint8_t>(Phase::None);
        const uint8_t staged = static_cast<uint8_t>(Phase::Staged);
        if (draft.phase != none && draft.phase != staged) return false;
        OmsOrder order = draft;
        order.phase = staged;
        order.filled = 0;
        order.leaves = draft.qty;
        order.avg_px = 0;
        order.version = 1;
        order.created_us = draft.created_us;
        order.updated_us = draft.updated_us;
        order.seen_n = 0;
        order.seen_head = 0;
        for (int i = 0; i < 8; ++i) order.seen[i] = 0;
        if (unlikely(!pool_.take(order.local, order))) return false;
        ids_[n_++] = order.local;
        return true;
    }

    // Staged -> Working. Only the first ack is accepted.
    PITLANE_HOT bool ack(uint64_t local, uint64_t venue_id, uint32_t venue, int64_t now_us) noexcept {
        OmsOrder* o = pool_.find(local);
        if (unlikely(!o || o->phase != static_cast<uint8_t>(Phase::Staged))) return false;
        o->venue_id = venue_id;
        o->venue = venue;
        o->updated_us = now_us;
        ++o->version;
        o->phase = static_cast<uint8_t>(Phase::Working);
        return true;
    }

    // Applies a venue fill once. Duplicates report dup=1 and change nothing. Overfill fails closed.
    PITLANE_HOT bool fill(uint64_t local, uint64_t venue_fill_id, int64_t qty, int64_t px, int64_t now_us,
                          OmsFill& out) noexcept {
        if (unlikely(qty <= 0 || px <= 0)) return false;
        OmsOrder* o = pool_.find(local);
        if (unlikely(!o)) return false;
        const uint8_t phase = o->phase;
        const uint8_t working = static_cast<uint8_t>(Phase::Working);
        const uint8_t partial = static_cast<uint8_t>(Phase::Partial);
        const uint8_t pull = static_cast<uint8_t>(Phase::PullWait);
        if (phase != working && phase != partial && phase != pull) return false;
        if (unlikely(qty > o->leaves)) return false;
        if (venue_fill_id != 0) {
            if (seen_has(*o, venue_fill_id)) {
                out = OmsFill{};
                out.dup = 1;
                return false;
            }
            seen_put(*o, venue_fill_id);
        }
        const int64_t new_filled = o->filled + qty;
        int64_t avg = px;
        if (o->filled > 0) {
            if (o->avg_px <= INT64_MAX / o->filled && px <= INT64_MAX / qty) {
                const int64_t prior = o->avg_px * o->filled;
                const int64_t add = px * qty;
                if (prior <= INT64_MAX - add) avg = (prior + add) / new_filled;
            }
        }
        o->filled = new_filled;
        o->leaves -= qty;
        o->avg_px = avg;
        o->updated_us = now_us;
        ++o->version;
        if (o->leaves == 0) {
            o->phase = static_cast<uint8_t>(Phase::Filled);
        } else if (phase == pull) {
            o->phase = pull;
        } else {
            o->phase = partial;
        }
        out.qty = qty;
        out.px = px;
        out.leaves = o->leaves;
        out.avg_px = avg;
        out.phase = o->phase;
        out.dup = 0;
        return true;
    }

    // Working or partial -> pull wait.
    PITLANE_HOT bool cancel(uint64_t local, int64_t now_us) noexcept {
        OmsOrder* o = pool_.find(local);
        if (unlikely(!o)) return false;
        const uint8_t phase = o->phase;
        if (phase != static_cast<uint8_t>(Phase::Working) && phase != static_cast<uint8_t>(Phase::Partial)) {
            return false;
        }
        o->phase = static_cast<uint8_t>(Phase::PullWait);
        o->updated_us = now_us;
        ++o->version;
        return true;
    }

    // Pull wait -> cancelled.
    PITLANE_HOT bool confirm_cancel(uint64_t local, int64_t now_us) noexcept {
        OmsOrder* o = pool_.find(local);
        if (unlikely(!o || o->phase != static_cast<uint8_t>(Phase::PullWait))) return false;
        o->phase = static_cast<uint8_t>(Phase::Cancelled);
        o->updated_us = now_us;
        ++o->version;
        return true;
    }

    // Staged -> denied.
    PITLANE_HOT bool reject(uint64_t local, int64_t now_us) noexcept {
        OmsOrder* o = pool_.find(local);
        if (unlikely(!o || o->phase != static_cast<uint8_t>(Phase::Staged))) return false;
        o->phase = static_cast<uint8_t>(Phase::Denied);
        o->updated_us = now_us;
        ++o->version;
        return true;
    }

    // Working or partial amend with an optimistic version check. The old order is copied out.
    bool amend(uint64_t local, int64_t new_px, int64_t new_qty, uint16_t expect_version, OmsOrder& before) noexcept {
        OmsOrder* o = pool_.find(local);
        if (unlikely(!o)) return false;
        const uint8_t phase = o->phase;
        if (phase != static_cast<uint8_t>(Phase::Working) && phase != static_cast<uint8_t>(Phase::Partial)) {
            return false;
        }
        if (new_px <= 0 || new_qty < o->filled || expect_version != o->version) return false;
        before = *o;
        o->px = new_px;
        o->qty = new_qty;
        o->leaves = new_qty - o->filled;
        o->phase = o->filled == 0 ? static_cast<uint8_t>(Phase::Working) : static_cast<uint8_t>(Phase::Partial);
        ++o->version;
        return true;
    }

    PITLANE_HOT OmsOrder* find_local(uint64_t local) noexcept { return pool_.find(local); }

    PITLANE_HOT const OmsOrder* find_local(uint64_t local) const noexcept { return pool_.find(local); }

    const OmsOrder* find_client(uint64_t client) const noexcept {
        for (int i = 0; i < n_; ++i) {
            const OmsOrder* o = pool_.find(ids_[i]);
            if (o && o->client == client) return o;
        }
        return nullptr;
    }

    const OmsOrder* find_venue(uint64_t venue_id) const noexcept {
        for (int i = 0; i < n_; ++i) {
            const OmsOrder* o = pool_.find(ids_[i]);
            if (o && o->venue_id == venue_id) return o;
        }
        return nullptr;
    }

    // Done is filled quantity, working is open leaves, children counts every matching order.
    bool parent_progress(uint64_t parent, int64_t& done, int64_t& working, int& children) const noexcept {
        done = 0;
        working = 0;
        children = 0;
        for (int i = 0; i < n_; ++i) {
            const OmsOrder* o = pool_.find(ids_[i]);
            if (!o || o->parent != parent) continue;
            done += o->filled;
            if (live_phase(o->phase)) working += o->leaves;
            ++children;
        }
        return children > 0;
    }

    int active() const noexcept {
        int n = 0;
        for (int i = 0; i < n_; ++i) {
            const OmsOrder* o = pool_.find(ids_[i]);
            if (o && live_phase(o->phase)) ++n;
        }
        return n;
    }

    int size() const noexcept { return pool_.size(); }

private:
    static bool live_phase(uint8_t phase) noexcept {
        return phase == static_cast<uint8_t>(Phase::Staged) || phase == static_cast<uint8_t>(Phase::Working) ||
               phase == static_cast<uint8_t>(Phase::Partial) || phase == static_cast<uint8_t>(Phase::PullWait);
    }

    static bool seen_has(const OmsOrder& o, uint64_t id) noexcept {
        for (uint8_t i = 0; i < o.seen_n; ++i) {
            if (o.seen[i] == id) return true;
        }
        return false;
    }

    static void seen_put(OmsOrder& o, uint64_t id) noexcept {
        o.seen[o.seen_head] = id;
        o.seen_head = static_cast<uint8_t>((o.seen_head + 1) & 7u);
        if (o.seen_n < 8) ++o.seen_n;
    }

    SlotPool<OmsOrder, Cap> pool_{};
    uint64_t ids_[Cap]{};
    int n_{0};
};

}  // namespace pitlane
