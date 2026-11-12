# Changelog

## [2.6.0] - 2026-11-12

### Added

- FanRing in-place `claim` / `publish_claimed` and const `view` without copying the body
- `ObjectPool` fixed freelist, `StickyCounter`, caller-owned `CoreBank`, `WirePx` scale helpers
- CMake package export (`find_package(pitlane)`), `pitlane::pitlane` aliases, test/example build gates
- GitHub Actions CI, LICENSE (MIT), CHANGELOG, CMake package export, `.clang-format`, `.editorconfig`

### Changed

- Library release version is 2.6.0
- `kDeskVersion` is 3 (`FanRing` in-place claim state)

## [2.5.0] - 2026-11-11

### Added

- Explicit huge-page mappings (hugetlbfs / `MAP_HUGETLB`), no THP
- Host helpers: affinity, realtime scheduling, IRQ affinity, `mlockall`, NUMA `mbind`, `thp_disable`
- Explicit SIMD paths, adaptive micro-batch sizing, `RankBits` / `QuadTab`
- Process-wide integer TSC scale, `SeqPublishRing`, decision scratch helpers

## [2.3.0] - 2026-11-10

### Added

- `ack_span`, `shown_before`, `take_span`, `ExtremeWindow`
