#pragma once

namespace pitlane {

// Library release. This is the number in the shared-library file name.
// kDeskVersion is the on-disk layout of a mapping file, and it moves on its own.

inline constexpr int kVersionMajor = 2;
inline constexpr int kVersionMinor = 1;
inline constexpr int kVersionPatch = 0;
inline constexpr char kVersion[] = "2.1.0";

}  // namespace pitlane
