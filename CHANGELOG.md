# Changelog

## [2.8.0] - 2026-11-12

### Added

- `SeqField` seqlock publisher for small hot-path records
- `LatestKeep` per-key market-data coalesce (latest wins)
- `DualLane` request/report sequence + heartbeat window helpers
- `PaceLatch` / `window_back` parent pacing hysteresis
- Study flow: `ofi_step` / `OfiWin`, `SecMove`, `TradeMidGap`, aggressor `FootBin`
- Wire mold extras: `CapRow` / `WireMode` / `FastHead` / presence bits / `clamp_wire_i64` / `ByteCode`
- `IntEma` Q16 integer EMA

### Changed

- Library release version is 2.8.0

## [2.7.0] - 2026-11-12

### Added

- `SendJoinRing` multi-producer fail-full send ring with generation-packed tail
- `SlotGate` resync gate beside `OwnPair` (`need_seed` / `on_drop` / `seeded`)
- `SigBook::find4` with dual-bucket prefetch; SSE2 signature vector compare
- `assume_aligned` in `hint.hpp`; `-mprefer-vector-width=256 -mno-avx512f` under `PITLANE_NATIVE`
- Host hardenings: `mem_raise_limit` / `mem_lock_range`, `set_fifo_thread` / `set_rr_thread`, `pin_process_mask`, `numa_local_node`, THP `defrag=never`

### Changed

- Library release version is 2.7.0
- `ByteArena` uses `numa_bind_range` (STRICT) and `hugepage_count_ensure` like desk maps

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
