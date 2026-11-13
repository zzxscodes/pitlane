#pragma once

namespace pitlane {

// Library release. This is the number in the shared-library file name.
// kDeskVersion is the on-disk layout of a mapping file, and it moves on its own.

#define PITLANE_VERSION_MAJOR 2
#define PITLANE_VERSION_MINOR 19
#define PITLANE_VERSION_PATCH 0
#define PITLANE_VERSION_STRING "2.19.0"

inline constexpr int kVersionMajor = PITLANE_VERSION_MAJOR;
inline constexpr int kVersionMinor = PITLANE_VERSION_MINOR;
inline constexpr int kVersionPatch = PITLANE_VERSION_PATCH;
inline constexpr char kVersion[] = PITLANE_VERSION_STRING;

}  // namespace pitlane
