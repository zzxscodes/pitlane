#pragma once

// Wall-clock pacing for a recorded event stream. Media time is the event
// timestamp; wall time is what a replay loop sleeps on. Speed is an integer
// multiple of real time in parts per million, so 1000000 is 1x and 40000000 is
// 40x. Up to 16 ordered segments override the default speed, which covers the
// long quiet stretches before the open, the lunch break, and the overnight
// gap without a special case in the caller.
//
// `due()` advances a cursor, so a stream that moves forward pays O(1) per call
// and only a rewind recomputes from the origin. All arithmetic is integer.

#include <cstdint>

namespace pitlane {

struct PaceSeg {
    int64_t from_us{0};
    int64_t to_us{0};
    int32_t speed_ppm{1000000};
};

class ReplayPace {
public:
    static constexpr int kSegments = 16;
    static constexpr int32_t kRealtime = 1000000;

    void reset() noexcept {
        n_ = 0;
        speed_ = kRealtime;
        started_ = false;
        origin_event_ = 0;
        origin_wall_ = 0;
        cur_event_ = 0;
        cur_wall_ = 0;
    }

    void set_speed(int32_t ppm) noexcept { speed_ = ppm > 0 ? ppm : kRealtime; }
    int32_t speed() const noexcept { return speed_; }

    // Segments must be added in order and must not overlap. Returns false on a
    // bad range, an overlap, or a full table.
    bool add_segment(int64_t from_us, int64_t to_us, int32_t speed_ppm) noexcept {
        if (n_ >= kSegments || to_us <= from_us || speed_ppm <= 0) return false;
        if (n_ > 0 && from_us < seg_[n_ - 1].to_us) return false;
        seg_[n_].from_us = from_us;
        seg_[n_].to_us = to_us;
        seg_[n_].speed_ppm = speed_ppm;
        ++n_;
        return true;
    }

    void clear_segments() noexcept { n_ = 0; }
    int segments() const noexcept { return n_; }
    const PaceSeg* segment(int i) const noexcept { return i >= 0 && i < n_ ? &seg_[i] : nullptr; }

    // Pins one event timestamp to one wall timestamp. Call once before the
    // first due() or after a seek.
    void start(int64_t event_us, int64_t wall_us) noexcept {
        origin_event_ = event_us;
        origin_wall_ = wall_us;
        cur_event_ = event_us;
        cur_wall_ = wall_us;
        seg_cursor_ = 0;
        started_ = true;
    }

    bool started() const noexcept { return started_; }

    // Absolute wall deadline for `event_us`. An event before the origin keeps
    // the origin deadline.
    int64_t due(int64_t event_us) noexcept {
        if (!started_) return 0;
        if (event_us < origin_event_) return origin_wall_;
        if (event_us == cur_event_) return cur_wall_;
        if (event_us < cur_event_) {
            cur_event_ = origin_event_;
            cur_wall_ = origin_wall_;
            seg_cursor_ = 0;
        }
        advance(cur_event_, event_us);
        cur_event_ = event_us;
        return cur_wall_;
    }

    // How long to wait before the event is due. Zero means it is due now.
    int64_t wait(int64_t event_us, int64_t wall_now_us) noexcept {
        const int64_t deadline = due(event_us);
        return deadline > wall_now_us ? deadline - wall_now_us : 0;
    }

    // Speed in force at one media timestamp.
    int32_t speed_at(int64_t event_us) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (event_us >= seg_[i].from_us && event_us < seg_[i].to_us) return seg_[i].speed_ppm;
        }
        return speed_;
    }

    bool in_break(int64_t event_us) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (event_us >= seg_[i].from_us && event_us < seg_[i].to_us) return true;
        }
        return false;
    }

private:
    void advance(int64_t from_us, int64_t to_us) noexcept {
        if (to_us <= from_us) return;
        int64_t pos = from_us;
        int i = 0;
        while (i < n_ && seg_[i].to_us <= pos) ++i;
        while (pos < to_us) {
            int32_t sp = speed_;
            int64_t stop = to_us;
            if (i < n_) {
                if (seg_[i].from_us <= pos) {
                    sp = seg_[i].speed_ppm;
                    if (seg_[i].to_us < stop) stop = seg_[i].to_us;
                } else if (seg_[i].from_us < stop) {
                    stop = seg_[i].from_us;
                }
            }
            if (sp <= 0) sp = kRealtime;
            cur_wall_ += (stop - pos) * kRealtime / sp;
            pos = stop;
            if (i < n_ && pos >= seg_[i].to_us) ++i;
        }
        seg_cursor_ = i;
    }

    PaceSeg seg_[kSegments]{};
    int n_{0};
    int32_t speed_{kRealtime};
    int seg_cursor_{0};
    bool started_{false};
    int64_t origin_event_{0};
    int64_t origin_wall_{0};
    int64_t cur_event_{0};
    int64_t cur_wall_{0};
};

}  // namespace pitlane
