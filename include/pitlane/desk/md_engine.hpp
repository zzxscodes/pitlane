#pragma once

#include "pitlane/desk/engine.hpp"
#include "pitlane/desk/feed.hpp"
#include "pitlane/desk/runtime.hpp"
#include "pitlane/desk/types.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Copied subscription ids. The copy is what a later send walks (notes §40).
struct SubSnap {
    static constexpr int kCap = 128;
    uint32_t id[kCap]{};
    int n{0};

    bool add(uint32_t name) noexcept {
        if (name == 0) return false;
        for (int i = 0; i < n; ++i)
            if (id[i] == name) return true;
        if (n >= kCap) return false;
        id[n++] = name;
        return true;
    }

    int copy_to(uint32_t* dst, int cap) const noexcept {
        if (!dst || cap <= 0 || n <= 0) return 0;
        const int w = n < cap ? n : cap;
        for (int i = 0; i < w; ++i) dst[i] = id[i];
        return w;
    }
};

// Market-data gateway.
// The subclass talks to a vendor. This class only moves notes on the desk.
// Five subscribe failures in a row stop further sends until one succeeds (notes §3).

class MdEngine : public Engine {
public:
    static constexpr int kSubFailMax = 5;

    unsigned quote_mask{0};
    unsigned print_mask{0};
    unsigned signal_mask{1ull << kStratReader};
    unsigned mkt_mask{1ull << kStratReader};
    unsigned rebuild_mask{1ull << kStratReader};
    unsigned sub_reader{kMdReader};
    WantMask* want{nullptr};

    bool set_up() override { return Engine::set_up(); }

    void loop_task() override { process_subs(); housekeep(); }

    virtual bool subscribe(const char* const* codes, int n) = 0;
    virtual bool unsubscribe(const char* const* codes, int n) {
        (void)codes;
        (void)n;
        return true;
    }
    virtual bool subscribe_signal(const SubNote& note) {
        (void)note;
        return true;
    }
    virtual bool on_sub(const SubNote& note) {
        (void)note;
        return true;
    }

    int sub_fails() const noexcept { return sub_fails_; }
    const SubSnap& subs() const noexcept { return snap_; }
    int copy_subs(uint32_t* dst, int cap) const noexcept { return snap_.copy_to(dst, cap); }

protected:
    PITLANE_HOT bool write_quote(const Quote& q) noexcept {
        if (!desk_) return false;
        if (want && !want->accept(q.name)) return false;
        return desk_->quotes.publish(q, quote_mask);
    }

    PITLANE_HOT bool write_print(const TapePrint& p) noexcept {
        if (!desk_) return false;
        if (want && !want->accept(p.name)) return false;
        return desk_->prints.publish(p, print_mask);
    }

    PITLANE_HOT bool write_signal(const SigNote& s) noexcept {
        if (!desk_) return false;
        return desk_->signals.publish(s, signal_mask);
    }

    PITLANE_HOT bool write_factor(uint32_t name, const char* topic, int64_t value, int64_t stamp_us) noexcept {
        SigNote s{};
        s.name = name;
        s.value = value;
        s.stamp_us = stamp_us;
        s.last = 1;
        slot_text(s.topic, static_cast<int>(sizeof s.topic), topic);
        return write_signal(s);
    }

    PITLANE_HOT bool write_mkt(const MktNote& m) noexcept {
        if (!desk_) return false;
        return desk_->mkt_ords.publish(m, mkt_mask);
    }

    PITLANE_HOT bool write_rebuild(const Quote& q) noexcept {
        if (!desk_) return false;
        return desk_->rebuilds.publish(q, rebuild_mask);
    }

    void process_subs() noexcept {
        if (!desk_) return;
        ring_take(desk_->subs, sub_cur_, sub_reader, true, [&](const SubNote& n) {
            if (sub_fails_ >= kSubFailMax && n.kind != static_cast<uint8_t>(SubKind::Unquote)) return;
            const char* code = n.code;
            bool ok = true;
            const auto kind = static_cast<SubKind>(n.kind);
            if (kind == SubKind::Unquote) {
                ok = unsubscribe(&code, 1);
            } else if (kind == SubKind::Signal) {
                ok = subscribe_signal(n);
            } else if (kind == SubKind::Quote || n.kind == 0) {
                ok = subscribe(&code, 1);
                if (ok) snap_.add(n.name);
            } else if (kind == SubKind::DropSignals) {
                ok = true;
            } else {
                ok = on_sub(n);
            }
            if (ok) sub_fails_ = 0;
            else ++sub_fails_;
        });
    }

private:
    uint64_t sub_cur_{0};
    int sub_fails_{0};
    SubSnap snap_{};
};

} // namespace pitlane
