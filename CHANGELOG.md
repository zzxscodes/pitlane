# Changelog

## [2.11.0] - 2026-11-12

### Added

- Static web API docs under `doc/site/` (sidebar by section, EN/ZH); rebuild with `python3 doc/gen_site.py`
- Startup `PitConf` / `conf_load` / `conf_normalize` / `desk_apply_conf` (INI-like file, defaults in code, hints on clamp)
- `FanRing::tune` soft capacity at desk init (power-of-two ≤ compile-time Cap); hot path uses tuned mask only

### Changed

- Library release version is 2.11.0

## [2.10.0] - 2026-11-12

### Added

- `risk_merge` / `AcctRiskTab` account risk overlay (stricter positive limits; miss fail-closed)
- `gate_ask` cash / inventory / `RiskCaps` path before flow; failures leave flow and inflight unchanged
- `BasketBook` / `basket_gap` / `basket_topup` per-name target ledger
- `TraderAcctTab` / `acct_idle` trader→accounts map and delete-time idle check
- `board_lot_*` and `MarginBook` spot / margin quantity helpers
- `QueryFold` `(acct, kind)` coalesce with optional `page` token; `pending_kind`
- `LaneBook` / `lane_pick` fixed counter→`LaneRule` table
- `BrokerWatch::due` list-only timeouts; `inflight_scan` composer
- `sheet` / `recon_cancel_qty` partial cancel under PullWait; late fill stays PullWait until flat
- `sign_begin` multi-module sign composition helper; `cash_enough` account buying-power predicate
- `QuoteFold::top` / `apply_ordered` / `clear`; `SeqGate::gap_width` / `reset`; `seq_start`
- `MacAllow` (≤3) and `RoleBits` byte/bit helpers

### Changed

- Library release version is 2.10.0

## [2.9.0] - 2026-11-12

### Added

- `PrioSnap` priority triple buffer and `PinBank` multi-reader snapshots with per-slot refcounts (fail-closed)
- `PhaseGate` / `ActiveSum` signed-ticket phase gate and counter rotate after drain
- `IdClaim` fixed-capacity id state table (Wait / Live / Final)
- `infer_lots` / `CumSnap` per-price lot inference from cumulative volume/turnover
- `FifoLot` / `AcbBook` / `acb_apply` FIFO lot queue and average-cost position book
- `IntEmaDev` integer EMA mean + absolute-deviation pair

### Changed

- Library release version is 2.9.0

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
