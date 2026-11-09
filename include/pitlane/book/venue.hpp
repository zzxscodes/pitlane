#pragma once

#include "pitlane/book/arrive.hpp"
#include "pitlane/book/gap.hpp"
#include "pitlane/book/lags.hpp"
#include "pitlane/desk/types.hpp"

#include <cstdint>

namespace pitlane {

struct VenueCfg {
    LagTable lag{};
    int64_t rigid_extra{0};   // buy fills a print only at or below limit - extra
    int64_t lot{1};
    double cancel_pow{1.0};
    int rate_cap{0};          // admits per name per second, 0 disables
    int64_t notional_cap{0};  // px * qty ceiling, 0 disables
};

struct VenueOut {
    Why why{Why::None};
    int reports{0};
};

// Paper venue. An order is judged only once its go-lag has elapsed, against the
// book as of that moment. Pulls wait for the pull-lag. Resting orders then lose
// queue position only through prints and displayed-size changes.
class PaperVenue {
public:
    static constexpr uint32_t kNames = 48;
    static constexpr uint32_t kTickets = 192;
    static constexpr int kDepth = 8;

    explicit PaperVenue(const VenueCfg& cfg) noexcept;

    void set_band(uint32_t name, int64_t down, int64_t up) noexcept;
    void set_top(uint32_t name, int64_t bid_px, int64_t bid_qty, int64_t ask_px, int64_t ask_qty, int64_t last_px) noexcept;
    void set_levels(uint32_t name, const Rung* bids, int nbid, const Rung* asks, int nask) noexcept;

    VenueOut admit(const Intent& in, int64_t now_us, Report* buf, int cap) noexcept;
    int on_clock(int64_t now_us, Report* buf, int cap) noexcept;
    int on_print(uint32_t name, int64_t px, int64_t qty, int64_t now_us, Report* buf, int cap) noexcept;
    // aggressor 0 keeps every print. A buy aggressor does not fill a resting buy, and the reverse.
    int on_tape(uint32_t name, int64_t px, int64_t qty, uint8_t aggressor, int64_t now_us, Report* buf, int cap) noexcept;
    int on_shown(uint32_t name, uint8_t side, int64_t px, int64_t prev_qty, int64_t now_qty, int64_t now_us, Report* buf,
                 int cap) noexcept;

private:
    struct Ticket {
        uint64_t tag;
        uint32_t name;
        uint32_t acct;
        int64_t px;
        int64_t qty;
        int64_t done;
        int64_t born_us;
        int64_t ready_us;
        int64_t pull_ready_us;
        Gap gap;
        int64_t lot;
        uint8_t side;
        uint8_t style;
        uint8_t phase;
        uint8_t used;
    };
    struct NameState {
        int64_t down;
        int64_t up;
        int64_t bid_px;
        int64_t ask_px;
        int64_t last_px;
        int64_t bid_qty;
        int64_t ask_qty;
        Rung bids[kDepth];
        Rung asks[kDepth];
        int nbid;
        int nask;
        int64_t window_sec;
        int admits;
    };

    int find_tag(uint64_t tag) const noexcept;
    int alloc_ticket() noexcept;
    void free_ticket(int i) noexcept;
    int64_t shown_at(const NameState& ns, uint8_t side, int64_t px) const noexcept;
    bool washes(const Intent& in) const noexcept;
    int activate(int ti, int64_t now_us, Report* buf, int cap, int filled_base) noexcept;
    int emit(Report* buf, int cap, int n, const Report& r) const noexcept;
    int fill_ticket(int ti, int64_t px, int64_t qty, int64_t now_us, Report* buf, int cap, int n) noexcept;

    VenueCfg cfg_;
    Ticket tickets_[kTickets]{};
    NameState names_[kNames]{};
    int free_head_{0};
    int free_next_[kTickets]{};
};

}  // namespace pitlane
