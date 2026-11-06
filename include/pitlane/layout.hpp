#pragma once

#include "pitlane/names.hpp"
#include "pitlane/region.hpp"
#include "pitlane/signal.hpp"

#include <cstddef>

namespace pitlane {

struct Footprint {
    std::size_t desk{0};
    std::size_t names{0};
    std::size_t pulses{0};
    std::size_t total() const noexcept { return desk + names + pulses; }
};

inline Footprint measure() noexcept {
    Footprint f;
    f.desk = sizeof(Desk);
    f.names = sizeof(NameBook);
    f.pulses = sizeof(PulseBoard);
    return f;
}

}  // namespace pitlane
