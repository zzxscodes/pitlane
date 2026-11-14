# Changelog

## [2.22.0] - 2026-11-14

### Added
- `WalView` / `wal_open` / `wal_append` / `wal_recover`: a fixed-size, file-backed write-ahead log. The tail is the commit marker; CRC32C rejects a torn record and recovery truncates to the last good sequence. `wal_sync` is an explicit group commit.
- `SnapRing<T, Cap>` and `snap_open` / `snap_view`: a 64-byte fixed header plus sequence-locked cells for cross-language, read-only snapshots. Readers map the same file and read `magic`, `version`, `slot_bytes`, `stride`, `cap` without the C++ type.
- `TscClock`: one-time calibration of the TSC against `CLOCK_MONOTONIC_RAW`; `now_ns`, `now_us`, `publish(Desk&, offset_us)`, and `publish(UsSlot&)`. The hot path reads `rdtsc` only.
- `TailBoard`, `tail_note`, `tail_report`, `tail_arm`, and `TailTape`: per-lane TSC histograms, budget stalls, and a freeze-on-trigger tape for tail outliers.
- `ShadowStats` / `ShadowBook`: paired real and shadow fills, per-algorithm slippage in ticks, missed/extra/fill classification, and percentile reports.
- `ReplayTape`, `replay_capture_all`, `replay_ring`, and `FaultPlan`: deterministic replay of a captured ring sequence with publish-drop and no-ack injection. A no-ack run fills the ring and stops instead of overwriting.
- `MboBook`: market-by-order builder with fixed order storage, add/modify/cancel/trade by id, best and depth derivation, and an invariant check.
- `RouteBook::unbind`, `RouteBook::generation`, `route_control`, `StratEngine::routes`, `StratEngine::pin_cpu`, and `on_routes_changed`: route reload without restarting the desk and optional strategy core pinning.
- `tests/alloc`: replaces the global allocator and asserts that the new hot paths (`wal_append_item`, snapshot publish/read, `TscClock::now_*`, `tail_note`, `ShadowStats::note`, `MboBook::apply`, `replay_ring`, `FanRing` publish/take) neither allocate nor free after startup, and that the descriptor count does not change.

### Notes
- Library release version is 2.22.0. Desk layout stays `kDeskVersion` 5; the WAL and snapshot files use their own magic and version.
- All new containers are caller-owned and allocation-free. No network path is involved.

## [2.21.1] - 2026-11-13

### Fixed
- The heartbeat pulse is published from a cold function. g++ 11 at `-O3` otherwise treated the value-initialized padding as uninitialized and failed the build under `-Werror`.

### Notes
- Library release version is 2.21.1. Desk layout stays `kDeskVersion` 5.

## [2.21.0] - 2026-11-13

### Added
- Gateway and strategy roles from `the reference gateway`: `Engine` timers, controls, symbols, and heartbeat; `MdEngine` subscription drain with a five-failure cap and a copied subscription list; `TdEngine` order, asset, position, and algo handling plus `risk_filter` and working-order timeouts; `StratEngine` picks the same lanes a strategy context picks, and publishes orders, subscribes, and queries.
- Desk layout 5: `subs`, `signals`, `mkt_ords`, and `rebuilds`. `FanRing::poll` skips a slot that is not addressed to the reader.
- `SimRole`, `order_delay`, `tape_speed`, and `follow_clock` for the four-role paper path. `timer_due` is the matcher clock step. `adjust_offset` closes the opposite book before an open. `order_precheck` rejects a bad child before it is published.

### Changed
- The CTP gateway lives in `examples/simnow/ctp` and is built only into `pitlane_simnow`. The library does not include or link a broker API.

### Notes
- Library release version is 2.21.0. Desk layout is `kDeskVersion` 5.

## [2.20.0] - 2026-11-13

### Added
- Print matching on the paper venue: `print_hits`, `same_side_print`, `ext_match_one`, `match_quote`, and `PaperVenue::on_ext`. A same-direction print still counts, at the print price; the other direction counts at the order price. Quote crosses are the active bid and ask paths.
- `split_against` closes the opposite book first. Shanghai and INE keep today and yesterday apart; other venues use one close. The shortfall opens, cut by the maximum lot.
- `Cause::NoYday`. A limit enter with `fee_den` set freezes notional plus the class fee through `prelock_ok`.
- `apply_open_fill`, `apply_close_fill`, and `roll_session` show how today, yesterday, and the total move. `add_session_us` walks only continuous sessions.
- `price_at_quote_vwap`, `pre_px`, `post_px`, and `chasing_px` return a price from the book and the tick. `next_child_qty` cuts the next child when the parent is too far ahead once in-flight quantity is counted, and shares a lag by the completion rate.

### Notes
- Library release version is 2.20.0. Desk layout stays `kDeskVersion` 4.

## [2.19.0] - 2026-11-13

### Added
- `Center` is the trading-center step: market-data engine, quote book, strategy engine, then a live trader or the paper lane, plus a heartbeat pulse and an engine route allow-list (`RouteAllow`, zero bits = NoFilter).
- `TapeMd` replays a caller-owned quote tape (tape engine role).
- `examples/simnow/conf/paper.conf` runs that center with no broker and reports T2T.

### Changed
- The CTP trader, when `new_password` is set, rotates an expired counter password on login error 141 and logs in again. Leave that key only in the gitignored conf.

### Notes
- Library release version is 2.19.0. Desk layout stays `kDeskVersion` 4.

## [2.18.0] - 2026-11-13

### Added
- `WireLog`: one slot per line, hot path stores type tags and raw values, a cold thread formats and writes (trading-system-notes async log). A full ring drops the line.
- Broker-neutral `Account` / `StratCfg` / `load_account_conf` in `desk/session.hpp`, plus `InstMap`.

### Changed
- CTP is only the optional adapter under `include/pitlane/adapter/ctp` (`pitlane_ctp`). It is not a framework module.
- SimNow example logs to `run/wire.log` and stops when the counter rejects login or the front stays silent.

### Fixed
- `div_wide` is an inline function, so more than one translation unit can include it.

### Notes
- Library release version is 2.18.0. Desk layout stays `kDeskVersion` 4.

## [2.17.0] - 2026-11-12

### Added
- Desk layout `kDeskVersion` 4: `pack_reqs` / `pack_rsps` / `logs` FanRings (package lane / log report size roles)
- trade lane-scale compile-time caps (`kIntentCap`/`kReportCap`/`kAlgoCap` 32768, `kHoldCap` 4096, …)
- `PaperVenue` SimClock wiring (`VenueCfg::clock` / `set_clock`); larger name/ticket/depth tables
- `TradeLane` respects `tape_first` (Match.use_ext): prints before intents when set
- `StratHook` `on_algo` / package / log / control; `strat_stage_algo` / `strat_pause` / `strat_resume` / `desk_log_publish`
- Cold loaders `load_facts` / `load_hours`; RouteBook grows to 256×16
- `DeskLoop` drains package and log rings

### Changed
- HoldBoard / CashBoard open-address maps cover hold/cash soft caps
- Docs and example conf updated for desk v4 / 2.17.0

### Notes
- Library release version is 2.17.0

## [2.16.1] - 2026-11-12

### Added

- `pulse_bridge` (`PulseBoard` → desk `pulses`); `load_holidays` cold file loader
- Deeper FeedHub/Yard/WantMask, TradeLane tape, StratHook+`DeskLoop`, and holiday-file tests

### Changed

- Library release version is 2.16.1

## [2.16.0] - 2026-11-12

### Added

- Local Mongo roles: `FactSeed` / `HoursSeed` / `seed_facts` / `seed_hours` / `seed_holidays` into Yard + `ClosedDays`
- Local middleware roles: `TopicBus` / `BusNote` / `topic_id` (same-machine topic pub/sub); `RouteBook` (algo route table table)
- Tests for shelf/bus/route; perf samples `topic_bus` / `venue_admit`

### Changed

- `DrillCue::Closed` value is 10 (`NotTrading`); removed `Warn`
- README / API wording cleaned up for natural EN/ZH
- Library release version is 2.16.0

## [2.15.0] - 2026-11-12

### Added

- Local MD/TD/strategy framework hubs (pitlane names): `FeedHub` / `feed_push_*`, `TradeLane` / `lane_beat`, `StratHook` / `strat_bind_loop` / `strat_stage_*`, `CashSeed` / `HoldSeed` / `seed_cash` / `seed_hold`
- PitConf `[venue]` aligned with `paper matcher` `match.ini`: go/pull lags (`go_usN` / `pull_usN` or `delayN` / `cancel_delayN` seconds), `rigid_rule` / `use_ext`, `rigid_extra`, `trade_open`, lot/rate/notional, `cancel_pow_x1000`; `conf_venue_cfg`
- `VenueCfg::rigid`

### Changed

- Library release version is 2.15.0

## [2.14.0] - 2026-11-12

### Added

- Paper drill surface aligned with `paper matcher` forced fill/session state roles: `DrillCue` / `DrillBook`, `PaperVenue::{force_fill,force_pull,force_deny,hang,drill_apply}`, `Why::Closed` / `VenueCfg::trade_open`
- `TouchMode` / `resolve_px` / `tape_keep`; `HoldLine::{sellable,frozen,frozen_yday}` + `sellable_of` / `sell_room` / `acct_ready`

### Changed

- Library release version is 2.14.0

## [2.13.0] - 2026-11-12

### Added

- `Yard` v2 (`PITYARD2`): startup pool-scale startup map — symbol table, quote/order/trade/rebuild/mix tapes (`YardMark` / `YardTick` / `YardPrint`), `YardFacts` / `YardHours` tables; bind + publish/latest APIs; hot path allocation-free; fail-closed when full
- PitConf `[yard]` (incl. `mix_*`) and `conf_yard_plan`; `[study]` / `StudyPlan` / `conf_study_plan` for study pool-scale factor sizing
- `TickGrid` / `FactorSeries` / `NamedFactorRing` study helpers

### Changed

- `QueryFold::kEngines` 16 → 32 (covers cts strategy engine count+trading engine count with headroom)
- Library release version is 2.13.0
- README QueryFold engine/account counts match headers (32 / 64)

## [2.12.0] - 2026-11-12

### Added

- Two-phase ledger freeze (`prelock_*`), credit/liability pools + `option_free`
- `CurvePlan` / `lag_share_remaining`, `HysteresisGate` / `end_flat_pull`
- `study/fair.hpp` integer fair / bias / pressure score
- `wash_plan` STP disposition list, `freeze_px`, `auction_estimate_fill`, `maker_only_style`
- `PulseBoard::copy_subs`, `util/breach.hpp` first-breach / pool watermark
- PitConf `[host]`/`[shed]`/`[pace]`, `conf_load_ex` line errors, `conf_apply_host`, cash/hold soft `tune`
- GitHub Pages workflow, pkg-config, install of `pitlane.conf` + `doc/site`, `CONTRIBUTING`/`SECURITY`, `HOST_TUNING` docs

### Changed

- Library release version is 2.12.0

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
