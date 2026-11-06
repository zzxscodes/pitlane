#include "pitlane/logline.hpp"

namespace pitlane {

void Log::bind(void (*fn)(int level, const char* line, void* user), void* user) noexcept {
    fn_ = fn;
    user_ = user;
}

void Log::write(int level, const char* line) noexcept {
    if (level > min_) {
        ++dropped_;
        return;
    }
    ++emitted_;
    if (fn_) fn_(level, line ? line : "", user_);
}

}  // namespace pitlane
