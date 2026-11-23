#pragma once

// 行情分发：一条解码后的快照/逐笔按固定顺序送给多个订阅者。
// 订阅者各自返回 false 表示这一帧对它无效（背压或未就绪），只记该订阅者的
// 丢弃计数，不影响其它订阅者，也不会在热路径上阻塞。定长、无堆。

#include "book.hpp"

#include <cstdint>

namespace pitlane {

struct CtpFanStats {
    uint64_t snaps{0};
    uint64_t prints{0};
    uint64_t dropped{0};
    uint64_t errors{0};
};

class CtpFanout {
public:
    static constexpr int kSinks = 6;

    using SnapFn = bool (*)(void* user, const CtpSnap& s);
    using PrintFn = bool (*)(void* user, const CtpPrint& p);

    // 返回订阅者下标，满员返回 -1。
    int add(void* user, SnapFn on_snap, PrintFn on_print) noexcept {
        if (n_ >= kSinks || !on_snap) return -1;
        sinks_[n_].user = user;
        sinks_[n_].on_snap = on_snap;
        sinks_[n_].on_print = on_print;
        stats_[n_] = CtpFanStats{};
        return n_++;
    }

    // 分发一条快照与该快照重建出的逐笔（逐笔可为空）。返回成功送达的订阅者数。
    int publish(const CtpSnap& s, const CtpPrint* p, int nprints = 0) noexcept {
        int ok = 0;
        for (int i = 0; i < n_; ++i) {
            Sink& k = sinks_[i];
            ++stats_[i].snaps;
            if (!k.on_snap(k.user, s)) {
                ++stats_[i].dropped;
                continue;
            }
            bool good = true;
            for (int j = 0; j < nprints; ++j) {
                ++stats_[i].prints;
                if (k.on_print && !k.on_print(k.user, p[j])) {
                    ++stats_[i].dropped;
                    good = false;
                    break;
                }
            }
            if (good) ++ok;
        }
        return ok;
    }

    // 只分发逐笔，用于没有新快照但有成交补充的场景。
    int publish_print(const CtpPrint& p) noexcept {
        int ok = 0;
        for (int i = 0; i < n_; ++i) {
            ++stats_[i].prints;
            if (sinks_[i].on_print && sinks_[i].on_print(sinks_[i].user, p)) {
                ++ok;
            } else {
                ++stats_[i].dropped;
            }
        }
        return ok;
    }

    int count() const noexcept { return n_; }
    const CtpFanStats& stats(int i) const noexcept { return stats_[i]; }

private:
    struct Sink {
        void* user{nullptr};
        SnapFn on_snap{nullptr};
        PrintFn on_print{nullptr};
    };

    Sink sinks_[kSinks]{};
    CtpFanStats stats_[kSinks]{};
    int n_{0};
};

} // namespace pitlane
