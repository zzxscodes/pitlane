#pragma once

#include "pitlane/desk/region.hpp"
#include "pitlane/util/hint.hpp"
#include "pitlane/util/spin.hpp"

#include <atomic>
#include <cstdint>
#include <ctime>

namespace pitlane {

// Process/engine role aligned with the reference design::shmdb::Engine:
// SetUp → PreRun → (LoopTask until stop) → PostRun.
// Owns no heap on the hot path; Desk is caller-attached.

class Engine {
public:
    Engine() = default;
    virtual ~Engine() = default;

    void bind_desk(Desk* desk) noexcept { desk_ = desk; }
    Desk* desk() const noexcept { return desk_; }

    virtual bool set_up() { return desk_ != nullptr; }
    virtual void pre_run() {}
    virtual void loop_task() {}
    virtual void post_run() {}
    virtual bool stop_condition() { return stop_.load(std::memory_order_acquire); }

    void request_stop() noexcept { stop_.store(true, std::memory_order_release); }
    bool stopped() const noexcept { return stop_.load(std::memory_order_acquire); }

    // Blocking run. sleep_ns==0 spins with cpu_relax.
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

protected:
    Desk* desk_{nullptr};
    std::atomic<bool> stop_{false};
};

} // namespace pitlane
