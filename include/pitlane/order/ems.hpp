#pragma once

#include "pitlane/desk/types.hpp"
#include "pitlane/order/parent.hpp"
#include "pitlane/order/sor.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Execution management: a parent quantity on a time grid, sent as child slices through the router.
// The caller owns clocks and the send decision. A slice that cannot be fully allocated returns the
// unallocated part to the parent pool. Terminal children are acknowledged with on_child(done=true).

template <int Parents = 8, int Children = 64>
class Ems {
public:
    static_assert(Parents >= 1 && Parents <= 64, "ems parents");
    static_assert(Children >= 1 && Children <= 256, "ems children");

    bool open(uint64_t parent_id, int64_t total, int64_t horizon_us, int64_t interval_us, int64_t lot, int64_t t0_us,
              uint8_t side = 1) noexcept {
        if (unlikely(parent_id == 0 || !known_side(side))) return false;
        if (find(parent_id) != nullptr) return false;
        int slot = -1;
        for (int i = 0; i < Parents; ++i) {
            if (!row_[i].used) {
                slot = i;
                break;
            }
        }
        if (slot < 0) return false;
        SlicePlan plan{};
        if (!slice_open(plan, total, horizon_us, interval_us, lot, t0_us)) return false;
        row_[slot].id = parent_id;
        row_[slot].plan = plan;
        row_[slot].working = 0;
        row_[slot].side = side;
        row_[slot].used = 1;
        return true;
    }

    // Emits the slices routed for one due cut. Returns the slice count; the sum of the slice
    // quantities is recorded as working. A short allocation is returned to the pool.
    PITLANE_HOT int poll(uint64_t parent_id, int64_t now_us, const VenueTop* venues, int n, SorSlice* out,
                         int cap) noexcept {
        EmsRow* row = find(parent_id);
        if (unlikely(!row)) return 0;
        SliceCut cut{};
        if (!slice_poll(row->plan, now_us, cut)) return 0;
        const int got = row->side == static_cast<uint8_t>(Side::Buy)
                            ? sor_aggressive_buy(venues, n, cut.qty, out, cap)
                            : sor_aggressive_sell(venues, n, cut.qty, out, cap);
        int64_t allocated = 0;
        for (int i = 0; i < got; ++i) allocated += out[i].qty;
        if (allocated < cut.qty) {
            const int64_t back = cut.qty - allocated;
            if (!slice_restore(row->plan, back)) return 0;
        }
        row->working += allocated;
        return got;
    }

    // A finished child reduces scheduled quantity and working. Unfinished children are only noted.
    PITLANE_HOT bool on_child(uint64_t parent_id, uint64_t child_id, int64_t qty, bool done) noexcept {
        (void)child_id;
        EmsRow* row = find(parent_id);
        if (unlikely(!row || qty <= 0)) return false;
        if (!done) return true;
        if (qty > row->plan.scheduled) return false;
        if (!slice_fill(row->plan, qty)) return false;
        row->working = row->working > qty ? row->working - qty : 0;
        return true;
    }

    // A parent can close only after every slice was either filled or returned.
    bool close(uint64_t parent_id) noexcept {
        EmsRow* row = find(parent_id);
        if (unlikely(!row)) return false;
        if (row->plan.pool != 0 || row->plan.scheduled != 0) return false;
        *row = EmsRow{};
        return true;
    }

    bool progress(uint64_t parent_id, int64_t& done, int64_t& working) const noexcept {
        const EmsRow* row = find(parent_id);
        if (unlikely(!row)) return false;
        done = row->plan.filled;
        working = row->working;
        return true;
    }

    int count() const noexcept {
        int n = 0;
        for (int i = 0; i < Parents; ++i) {
            if (row_[i].used) ++n;
        }
        return n;
    }

private:
    struct EmsRow {
        uint64_t id{0};
        SlicePlan plan{};
        int64_t working{0};
        uint8_t side{1};
        uint8_t used{0};
    };

    EmsRow* find(uint64_t id) noexcept {
        for (int i = 0; i < Parents; ++i) {
            if (row_[i].used && row_[i].id == id) return &row_[i];
        }
        return nullptr;
    }

    const EmsRow* find(uint64_t id) const noexcept {
        for (int i = 0; i < Parents; ++i) {
            if (row_[i].used && row_[i].id == id) return &row_[i];
        }
        return nullptr;
    }

    EmsRow row_[Parents]{};
};

}  // namespace pitlane
