#pragma once

// 行情回放控制：把一段行情时间轴映射到墙上时间，支持播放、暂停、单步、跳转和倍速。
// 只负责“什么时候把下一帧交给引擎”，不关心行情存在哪里；录音由 TapeRecorder 负责。
// 时间由调用方传入，便于在真实运行与测试中使用同一套状态机。

#include <cstdint>

namespace pitlane {

enum ReplayState : uint8_t {
    kReplayIdle = 0,
    kReplayPlaying = 1,
    kReplayPaused = 2,
    kReplayDone = 3
};

inline const char* replay_state_name(uint8_t s) noexcept {
    switch (s) {
        case kReplayPlaying: return "playing";
        case kReplayPaused: return "paused";
        case kReplayDone: return "done";
        default: return "idle";
    }
}

struct ReplayReport {
    uint8_t state{kReplayIdle};
    uint8_t pad[7]{};
    int64_t from_us{0};
    int64_t to_us{0};
    int64_t cursor_us{0};
    int64_t speed_ppm{1000000};
    int32_t progress_ppm{0};
    int32_t pad2{0};
    uint64_t frames{0};
};

class ReplayCtl {
public:
    void load(int64_t from_us, int64_t to_us, int64_t speed_ppm = 1000000) noexcept {
        from_ = from_us;
        to_ = to_us > from_us ? to_us : from_us;
        cursor_ = from_;
        speed_ = speed_ppm > 0 ? speed_ppm : 1000000;
        state_ = kReplayIdle;
        anchor_wall_ = 0;
        anchor_tape_ = from_;
        frames_ = 0;
    }

    bool play(int64_t now_us) noexcept {
        if (to_ <= from_) return false;
        if (state_ == kReplayDone) return false;
        state_ = kReplayPlaying;
        anchor_wall_ = now_us;
        anchor_tape_ = cursor_;
        return true;
    }

    bool pause() noexcept {
        if (state_ != kReplayPlaying) return false;
        state_ = kReplayPaused;
        return true;
    }

    bool stop() noexcept {
        state_ = kReplayIdle;
        cursor_ = from_;
        anchor_wall_ = 0;
        anchor_tape_ = from_;
        frames_ = 0;
        return true;
    }

    // 跳转到指定行情时间；播放中会以当前墙上时间重新锚定。
    bool seek(int64_t tape_us, int64_t now_us) noexcept {
        if (to_ <= from_) return false;
        if (tape_us < from_) tape_us = from_;
        if (tape_us > to_) tape_us = to_;
        cursor_ = tape_us;
        anchor_wall_ = now_us;
        anchor_tape_ = tape_us;
        if (state_ == kReplayDone && tape_us < to_) state_ = kReplayPaused;
        return true;
    }

    // 单步：暂停状态下按给定行情时长前进。
    bool step(int64_t tape_us) noexcept {
        if (tape_us <= 0) return false;
        if (state_ == kReplayPlaying) return false;
        cursor_ += tape_us;
        if (cursor_ >= to_) {
            cursor_ = to_;
            state_ = kReplayDone;
        } else {
            state_ = kReplayPaused;
            anchor_tape_ = cursor_;
        }
        return true;
    }

    // 播放推进。返回 true 表示产生了一个新的回放时刻（tape_us）。
    bool advance(int64_t now_us, int64_t& tape_us) noexcept {
        if (state_ != kReplayPlaying) return false;
        const int64_t wall = now_us - anchor_wall_;
        if (wall < 0) return false;
        const int64_t span = wall * speed_ / 1000000;
        int64_t next = anchor_tape_ + span;
        if (next >= to_) {
            next = to_;
            cursor_ = to_;
            state_ = kReplayDone;
        } else {
            cursor_ = next;
        }
        tape_us = next;
        ++frames_;
        return true;
    }

    uint8_t state() const noexcept { return state_; }
    int64_t cursor() const noexcept { return cursor_; }
    int64_t speed() const noexcept { return speed_; }
    bool set_speed(int64_t speed_ppm) noexcept {
        if (speed_ppm <= 0) return false;
        speed_ = speed_ppm;
        anchor_tape_ = cursor_;
        return true;
    }
    void reanchor(int64_t now_us) noexcept {
        anchor_wall_ = now_us;
        anchor_tape_ = cursor_;
    }

    ReplayReport report() const noexcept {
        ReplayReport r{};
        r.state = state_;
        r.from_us = from_;
        r.to_us = to_;
        r.cursor_us = cursor_;
        r.speed_ppm = speed_;
        r.frames = frames_;
        const int64_t span = to_ - from_;
        r.progress_ppm = span > 0 ? static_cast<int32_t>((cursor_ - from_) * 1000000 / span) : 0;
        return r;
    }

private:
    int64_t from_{0};
    int64_t to_{0};
    int64_t cursor_{0};
    int64_t speed_{1000000};
    int64_t anchor_wall_{0};
    int64_t anchor_tape_{0};
    uint64_t frames_{0};
    uint8_t state_{kReplayIdle};
};

} // namespace pitlane
