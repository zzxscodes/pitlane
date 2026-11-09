#pragma once

#include "pitlane/desk/types.hpp"

namespace pitlane {

// Legal order transitions. life_next returns Phase::None when the event is not allowed.
// No successful event lands on Phase::None.

enum class LifeEvent : uint8_t {
    Submit = 1,
    Accept = 2,
    Fill = 3,
    Finish = 4,
    AskPull = 5,
    ConfirmPull = 6,
    Reject = 7,
    Expire = 8,
    Resume = 9,
    ResumePartial = 10
};

inline Phase life_next(Phase from, LifeEvent ev) noexcept {
    switch (ev) {
        case LifeEvent::Submit:
            return from == Phase::None ? Phase::Staged : Phase::None;
        case LifeEvent::Accept:
            return from == Phase::Staged ? Phase::Working : Phase::None;
        case LifeEvent::Fill:
            if (from == Phase::Working) return Phase::Partial;
            if (from == Phase::Partial || from == Phase::PullWait) return from;
            return Phase::None;
        case LifeEvent::Finish:
            if (from == Phase::Working || from == Phase::Partial || from == Phase::PullWait) return Phase::Filled;
            return Phase::None;
        case LifeEvent::AskPull:
            if (from == Phase::Working || from == Phase::Partial) return Phase::PullWait;
            return Phase::None;
        case LifeEvent::ConfirmPull:
            return from == Phase::PullWait ? Phase::Cancelled : Phase::None;
        case LifeEvent::Reject:
            return from == Phase::Staged ? Phase::Denied : Phase::None;
        case LifeEvent::Expire:
            if (from == Phase::Staged || from == Phase::Working || from == Phase::Partial) return Phase::Expired;
            return Phase::None;
        case LifeEvent::Resume:
            return from == Phase::PullWait ? Phase::Working : Phase::None;
        case LifeEvent::ResumePartial:
            return from == Phase::PullWait ? Phase::Partial : Phase::None;
    }
    return Phase::None;
}

inline const char* phase_text(Phase phase) noexcept {
    switch (phase) {
        case Phase::None: return "none";
        case Phase::Staged: return "staged";
        case Phase::Working: return "working";
        case Phase::Partial: return "partial";
        case Phase::PullWait: return "pull";
        case Phase::Filled: return "filled";
        case Phase::Cancelled: return "cancel";
        case Phase::Denied: return "denied";
        case Phase::Expired: return "expired";
    }
    return "unknown";
}

}  // namespace pitlane
