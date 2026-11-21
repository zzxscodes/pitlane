#pragma once

#include "pitlane/desk/types.hpp"

#include <cstdint>

namespace pitlane {

// Aligns one local order with counter fills. The same venue fill id is applied once.
// Quantity past the order is reported and is not added to the filled quantity.

enum class ReconKind : uint8_t { Ack = 1, Fill = 2, Over = 3, Dup = 4, Cancel = 5 };

struct ReconEvent {
    ReconKind kind{ReconKind::Ack};
    int64_t qty{0};
    int64_t px{0};
    uint64_t venue_id{0};
};

struct BookOrder {
    uint64_t tag{0};
    int64_t qty{0};
    int64_t filled{0};
    int64_t over{0};
    Phase phase{Phase::None};
    uint8_t known{0};
};

template <int Cap>
class SeenIds {
public:
    static_assert(Cap >= 2 && Cap <= 128, "seen");

    int remember(uint64_t id) noexcept {
        if (id == 0) return -1;
        for (int i = 0; i < n_; ++i) {
            if (id_[i] == id) return 0;
        }
        if (n_ >= Cap) return -1;
        id_[n_++] = id;
        return 1;
    }

private:
    uint64_t id_[Cap]{};
    int n_{0};
};

template <int Cap>
inline int recon_fill(BookOrder& order, SeenIds<Cap>& seen, uint64_t venue_id, int64_t qty, int64_t px, int64_t size,
                      ReconEvent* out, int cap) noexcept {
    if (!out || cap <= 0 || qty <= 0 || px <= 0) return -1;
    if (!order.known && size <= 0) return -1;
    if (cap < (order.known ? 2 : 3)) return -1;
    const int mark = seen.remember(venue_id);
    if (mark == 0) {
        out[0] = ReconEvent{ReconKind::Dup, qty, px, venue_id};
        return 1;
    }
    if (mark < 0) return -1;
    int n = 0;
    if (!order.known) {
        order.qty = size;
        order.known = 1;
        order.phase = Phase::Working;
        out[n++] = ReconEvent{ReconKind::Ack, size, 0, venue_id};
    }
    if (order.phase == Phase::Filled || order.phase == Phase::Cancelled || order.phase == Phase::Denied ||
        order.phase == Phase::Expired) {
        if (n >= cap) return -1;
        order.over += qty;
        out[n++] = ReconEvent{ReconKind::Over, qty, px, venue_id};
        return n;
    }
    const int64_t room = order.qty > order.filled ? order.qty - order.filled : 0;
    const int64_t take = qty < room ? qty : room;
    const int64_t extra = qty - take;
    if (take > 0) {
        if (n >= cap) return -1;
        order.filled += take;
        if (order.phase != Phase::PullWait) {
            order.phase = order.filled == order.qty ? Phase::Filled : Phase::Partial;
        } else if (order.filled == order.qty) {
            order.phase = Phase::Filled;
        }
        out[n++] = ReconEvent{ReconKind::Fill, take, px, venue_id};
    }
    if (extra > 0) {
        if (n >= cap) return -1;
        order.over += extra;
        out[n++] = ReconEvent{ReconKind::Over, extra, px, venue_id};
    }
    return n;
}

inline bool recon_cancel(BookOrder& order, ReconEvent& out) noexcept {
    if (!order.known) return false;
    if (order.phase == Phase::Filled || order.phase == Phase::Cancelled || order.phase == Phase::Denied ||
        order.phase == Phase::Expired) {
        return false;
    }
    order.phase = Phase::Cancelled;
    out = ReconEvent{ReconKind::Cancel, order.qty - order.filled, 0, 0};
    return true;
}

// Partial cancel residual. qty is the cancelled size. Fail-closed: bad qty or terminal phase leaves order unchanged.
// Under PullWait a late fill path remains recon_fill (stays PullWait until fully filled).
inline bool recon_cancel_qty(BookOrder& order, int64_t qty, ReconEvent& out) noexcept {
    if (!order.known || qty <= 0) return false;
    if (order.phase == Phase::Filled || order.phase == Phase::Cancelled || order.phase == Phase::Denied ||
        order.phase == Phase::Expired) {
        return false;
    }
    const int64_t left = order.qty > order.filled ? order.qty - order.filled : 0;
    if (qty > left) return false;
    order.qty -= qty;
    if (order.qty == order.filled) {
        order.phase = Phase::Cancelled;
    } else if (order.phase == Phase::PullWait) {
        order.phase = order.filled > 0 ? Phase::Partial : Phase::Working;
    } else if (order.filled > 0) {
        order.phase = Phase::Partial;
    } else {
        order.phase = Phase::Working;
    }
    out = ReconEvent{ReconKind::Cancel, qty, 0, 0};
    return true;
}

}  // namespace pitlane
