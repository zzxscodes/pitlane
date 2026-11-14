#pragma once

#include "pitlane/desk/wire.hpp"
#include "pitlane/study/factor.hpp"
#include "pitlane/study/factor_eval.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Research→backtest→paper→live staging around one factor engine (study §22).
// Research and Backtest feed forward returns into FactorEval; paper/live skip it.

enum class PipeStage : uint8_t { Research = 1, Backtest = 2, Paper = 3, Live = 4 };

template <int Names = 8, int Depth = 256, int EvalCap = 1024>
class FactorPipeline {
public:
    bool set_stage(PipeStage s) noexcept {
        const uint8_t v = static_cast<uint8_t>(s);
        if (v < 1 || v > 4) return false;
        stage_ = s;
        return true;
    }

    PipeStage stage() const noexcept { return stage_; }

    bool add(const FactorSpec& s) noexcept { return engine_.add(s); }

    PITLANE_HOT PITLANE_INLINE bool on_sample(int64_t px, int64_t stamp_us, int64_t fwd_return) noexcept {
        if (!engine_.on_sample(px, stamp_us)) return false;
        if (stage_ == PipeStage::Research || stage_ == PipeStage::Backtest) {
            FactorValue v{};
            if (engine_.value(0, v) && v.ready) (void)eval_.observe(v.value, fwd_return);
        }
        return true;
    }

    PITLANE_HOT PITLANE_INLINE bool on_quote(int64_t bid_px, int64_t ask_px, int64_t bid_qty, int64_t ask_qty,
                                             int64_t stamp_us) noexcept {
        return engine_.on_quote(bid_px, ask_px, bid_qty, ask_qty, stamp_us);
    }

    int eval_size() const noexcept { return eval_.size(); }

    bool eval(typename FactorEval<EvalCap>::Eval& out) const noexcept { return eval_.result(out); }

    bool signal(int slot, int64_t threshold, SigNote& out, uint32_t name, const char* topic,
                int64_t now_us) noexcept {
        FactorValue v{};
        if (!engine_.value(slot, v) || !v.ready) return false;
        if (abs64(v.value) < threshold) return false;
        out = SigNote{};
        out.name = name;
        out.stamp_us = now_us;
        out.value = v.value;
        out.seq = ++seq_;
        out.last = 1;
        slot_text(out.topic, static_cast<int>(sizeof(out.topic)), topic);
        return true;
    }

private:
    static int64_t abs64(int64_t v) noexcept { return v == INT64_MIN ? INT64_MAX : (v < 0 ? -v : v); }

    FactorEngine<Names, Depth> engine_{};
    FactorEval<EvalCap> eval_{};
    PipeStage stage_{PipeStage::Research};
    int32_t seq_{0};
};

}  // namespace pitlane
