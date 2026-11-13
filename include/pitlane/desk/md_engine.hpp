#pragma once

#include "pitlane/desk/engine.hpp"
#include "pitlane/desk/types.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Market-data engine role (cts::broker::MarketData).
// Subclasses pull vendor ticks and publish Quote / TapePrint onto Desk.

class MdEngine : public Engine {
public:
    unsigned quote_mask{0};
    unsigned print_mask{0};

    bool set_up() override { return Engine::set_up(); }

    // Vendor subscribe. Return false on hard failure.
    virtual bool subscribe(const char* const* codes, int n) = 0;
    virtual bool unsubscribe(const char* const* codes, int n) {
        (void)codes;
        (void)n;
        return true;
    }

protected:
    PITLANE_HOT bool write_quote(const Quote& q) noexcept {
        if (!desk_) return false;
        return desk_->quotes.publish(q, quote_mask);
    }

    PITLANE_HOT bool write_print(const TapePrint& p) noexcept {
        if (!desk_) return false;
        return desk_->prints.publish(p, print_mask);
    }
};

} // namespace pitlane
