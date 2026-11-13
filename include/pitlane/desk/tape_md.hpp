#pragma once

#include "pitlane/desk/md_engine.hpp"

namespace pitlane {

// Replay market-data engine (tape engine role). Caller owns the quote array.
// loop_task publishes up to burst quotes. A full ring stops the burst without skipping.

class TapeMd : public MdEngine {
public:
    const Quote* src{nullptr};
    int n{0};
    int at{0};
    int burst{1};

    bool done() const noexcept { return at >= n; }

    bool subscribe(const char* const*, int) override { return true; }

    void loop_task() override {
        if (!src || burst < 1) return;
        int k = 0;
        while (at < n && k < burst) {
            if (!write_quote(src[at])) return;
            ++at;
            ++k;
        }
    }
};

} // namespace pitlane
