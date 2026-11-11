#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Grey-black DAG order for a fixed set of factor nodes.
// A cycle makes build fail. The hot path then walks the order array with no graph search.
// Nodes store integers the caller owns. The plan does not choose a trade.

template <int N>
class DagPlan {
public:
    static_assert(N >= 2 && N <= 64, "dag nodes");

    void clear() noexcept {
        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < N; ++j) need_[i][j] = 0;
            mark_[i] = 0;
            order_[i] = -1;
        }
        n_order_ = 0;
        cycle_ = 0;
    }

    bool add_edge(int reader, int dep) noexcept {
        if (unlikely(reader < 0 || reader >= N || dep < 0 || dep >= N || reader == dep)) return false;
        need_[reader][dep] = 1;
        return true;
    }

    bool build(int start) noexcept {
        if (unlikely(start < 0 || start >= N)) return false;
        for (int i = 0; i < N; ++i) mark_[i] = 0;
        n_order_ = 0;
        cycle_ = 0;
        dfs(start);
        return cycle_ == 0 && n_order_ > 0;
    }

    int size() const noexcept { return n_order_; }
    bool cycle() const noexcept { return cycle_ != 0; }

    int at(int i) const noexcept {
        if (unlikely(i < 0 || i >= n_order_)) return -1;
        return order_[i];
    }

    // Runs fn(node) from dependencies to the start node. Stops if fn returns false.
    template <class Fn>
    PITLANE_HOT bool run(Fn&& fn) const noexcept {
        for (int i = n_order_ - 1; i >= 0; --i) {
            if (!fn(order_[i])) return false;
        }
        return true;
    }

private:
    void dfs(int u) noexcept {
        if (mark_[u] == 1) {
            cycle_ = 1;
            return;
        }
        if (mark_[u] == 2) return;
        mark_[u] = 1;
        for (int v = 0; v < N; ++v) {
            if (need_[u][v]) dfs(v);
            if (cycle_) return;
        }
        mark_[u] = 2;
        order_[n_order_++] = u;
    }

    uint8_t need_[N][N]{};
    uint8_t mark_[N]{};
    int order_[N]{};
    int n_order_{0};
    uint8_t cycle_{0};
};

// Compile-time step list. The loop is a straight walk of a fixed array.
template <class Act, int Cap>
struct ActPlan {
    Act step[Cap]{};
    int n{0};

    template <class Fn>
    PITLANE_HOT bool run(Fn&& fn) const noexcept {
        for (int i = 0; i < n; ++i) {
            if (!fn(step[i])) return false;
        }
        return true;
    }
};

}  // namespace pitlane
