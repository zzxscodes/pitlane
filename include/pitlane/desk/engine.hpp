#pragma once

#include "pitlane/desk/region.hpp"
#include "pitlane/util/hint.hpp"
#include "pitlane/util/spin.hpp"

#include <atomic>
#include <cstdint>
#include <ctime>

namespace pitlane {

// Process role aligned with the reference design::shmdb::Engine:
// set_up → pre_run → (loop_task until stop) → post_run.
// Timers, control notes, and the heartbeat stay on the engine. No heap on the hot path.

class Engine {
public:
    static constexpr int kTimers = 16;
    static constexpr int kSymbols = 64;

    Engine() = default;
    virtual ~Engine() = default;

    void bind_desk(Desk* desk) noexcept { desk_ = desk; }
    Desk* desk() const noexcept { return desk_; }

    void set_engine_id(uint32_t id) noexcept { engine_id_ = id; }
    uint32_t engine_id() const noexcept { return engine_id_; }
    void set_app_name(const char* name) noexcept { slot_text(app_name_, sizeof app_name_, name); }
    const char* app_name() const noexcept { return app_name_; }

    virtual bool set_up() { return desk_ != nullptr; }
    virtual void pre_run() {}
    virtual void loop_task() {}
    virtual void post_run() {}
    virtual bool stop_condition() { return stop_.load(std::memory_order_acquire); }
    virtual void on_timer(uint64_t id, int64_t now_us) {
        (void)id;
        (void)now_us;
    }
    virtual void on_control(const ControlNote& note) { (void)note; }

    void request_stop() noexcept { stop_.store(true, std::memory_order_release); }
    bool stopped() const noexcept { return stop_.load(std::memory_order_acquire); }

    int64_t get_time_now() const noexcept {
        return desk_ ? desk_->clock_us.load(std::memory_order_acquire) : 0;
    }

    bool add_timer(uint64_t id, int64_t when_us, int64_t every_us) noexcept {
        if (id == 0) return false;
        for (int i = 0; i < kTimers; ++i) {
            if (timer_[i].live && timer_[i].id == id) {
                timer_[i].when_us = when_us;
                timer_[i].every_us = every_us;
                return true;
            }
        }
        for (int i = 0; i < kTimers; ++i) {
            if (timer_[i].live) continue;
            timer_[i] = Timer{id, when_us, every_us, 1};
            ++timers_live_;
            return true;
        }
        return false;
    }

    bool add_symbol(uint32_t name, const char* code) noexcept {
        if (name == 0 || !code || !code[0]) return false;
        for (int i = 0; i < kSymbols; ++i) {
            if (sym_[i].used && sym_[i].name == name) {
                slot_text(sym_[i].code, sizeof sym_[i].code, code);
                return true;
            }
        }
        for (int i = 0; i < kSymbols; ++i) {
            if (sym_[i].used) continue;
            sym_[i].used = 1;
            sym_[i].name = name;
            slot_text(sym_[i].code, sizeof sym_[i].code, code);
            return true;
        }
        return false;
    }

    uint32_t find_symbol(const char* code) const noexcept {
        if (!code || !code[0]) return 0;
        for (int i = 0; i < kSymbols; ++i) {
            if (!sym_[i].used) continue;
            const char* a = sym_[i].code;
            const char* b = code;
            while (*a && *a == *b) {
                ++a;
                ++b;
            }
            if (*a == 0 && *b == 0) return sym_[i].name;
        }
        return 0;
    }

    bool report_log(int32_t level, const char* line) noexcept {
        if (!desk_) return false;
        LogNote n{};
        n.level = level;
        n.stamp_us = get_time_now();
        slot_text(n.line, static_cast<int>(sizeof n.line), line);
        return desk_->logs.publish(n, 0);
    }

    // Control notes, timers, and an optional heartbeat. Reader 64 and above skips controls.
    void housekeep() noexcept {
        if (!desk_) return;
        const int64_t now = get_time_now();
        if (control_reader < 64u) {
            ring_take(desk_->controls, control_cur_, control_reader, false, [&](const ControlNote& n) {
                on_control(n);
                if (n.kind == 1) request_stop();
            });
        }
        if (timers_live_ > 0) {
            for (int i = 0; i < kTimers; ++i) {
                Timer& t = timer_[i];
                if (!t.live || t.when_us > now) continue;
                on_timer(t.id, now);
                if (t.every_us > 0) t.when_us += t.every_us;
                else {
                    t.live = 0;
                    --timers_live_;
                }
            }
        }
        if (beat_us > 0 && (last_beat_us_ == 0 || now - last_beat_us_ >= beat_us)) emit_heartbeat(now);
    }

    // Blocking run. sleep_ns==0 spins with cpu_relax. The caller owns any extra thread.
    void run(int64_t sleep_ns = 100000) {
        pre_run();
        while (!stop_condition()) {
            loop_task();
            if (sleep_ns > 0) {
                timespec ts{0, sleep_ns};
                nanosleep(&ts, nullptr);
            } else {
                cpu_relax();
            }
        }
        post_run();
    }

    unsigned control_reader{64};
    uint64_t beat_mask{0};
    int64_t beat_us{0};

protected:
    Desk* desk_{nullptr};
    std::atomic<bool> stop_{false};

private:
    struct Timer {
        uint64_t id{0};
        int64_t when_us{0};
        int64_t every_us{0};
        uint8_t live{0};
    };
    struct Sym {
        uint32_t name{0};
        uint8_t used{0};
        char code[32]{};
    };
    Timer timer_[kTimers]{};
    Sym sym_[kSymbols]{};
    uint64_t control_cur_{0};
    int timers_live_{0};
    int64_t last_beat_us_{0};
    uint64_t beats_{0};
    uint32_t engine_id_{0};
    char app_name_[32]{};

    // Not inlined: g++ 11 -O3 otherwise treats Pulse padding as uninitialized in copy_bytes.
    PITLANE_NOINLINE PITLANE_COLD void emit_heartbeat(int64_t now) noexcept {
        Pulse beat{};
        beat.stamp_us = now;
        beat.value = static_cast<int64_t>(beats_ + 1);
        slot_text(beat.topic, static_cast<int>(sizeof beat.topic), "hb");
        if (!desk_ || !desk_->pulses.publish(beat, beat_mask)) return;
        ++beats_;
        last_beat_us_ = now;
    }
};

} // namespace pitlane
