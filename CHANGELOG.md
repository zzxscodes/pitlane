# Changelog

## [1.0.0] - 2026-11-21

First release considered production-ready.

### Kernel and desk

- Header-only C++17 kernel designed around one `PITLANE1` memory-mapped desk:
  fixed-size quotes, intents, reports, execs, cash, and hold rows, with one
  writer per ring and fail-closed publishing.
- Desk layout `kDeskVersion` 8 adds the `execs` lane; `Intent` carries `biz` and
  `px_mode` inside the 56-byte frame; `Report` is 64 bytes and echoes the
  force-close reason and business kind.
- `Off` follows the seven CTP codes exactly: `Open 0`, `Close 1`, `ForceClose 2`,
  `CloseToday 3`, `CloseYday 4`, `ForceOff 5`, `LocalForceClose 6`.
- `Biz` covers ordinary trade, margin buy, short sell, buy-to-cover,
  sell-to-repay, subscribe, redeem, cash repay, return stock, odd-lot transfer,
  collateral in/out, lock, unlock, split, and merge.
- `Style::BestFive`, `PxMode::OppBest`, `PxMode::OwnBest`, and `Hedge::Covered`;
  option exercise and abandon travel on `desk/execs`.
- `Rms` runs between the strategy and the trading path: factor signal -> EMS
  parent and slices -> `Rms` and `PreTradeGuard` -> `Oms` -> venue. Manual,
  basket, algo, and strategy orders share the same gate.

### Execution and operations

- `Oms`, `Ems`, `Sor`, `Pms`, `Rms`, `PreTradeGuard`, `IdMint`, WAL, snapshots,
  and tail telemetry cover the full order lifecycle.
- `OpsKind::RiskLimits` with `GET`/`POST /api/risk/limits` lets the console read
  and override every live `RmsLimits` field, the halt flag, and the precheck
  flag; `StatusSnapshot::risk` publishes them and `/api/status` renders `rms`.
- `StatusSnapshot` and `/api/status` expose `ack_timeouts`, `life_timeouts`, and
  `pull_timeouts`.
- The console closes the loop from tape to running strategy: factor research,
  backtests with equity and drawdown curves, block tapes, strategy instances,
  manual and algo tickets, pair packages, credit, clearing, accounts, and risk.

### Adapters and tooling

- CTP market-data and trading adapters forward offsets, price types, hedge
  flags, business kinds, and exercise requests without rewriting them; anything
  the counter cannot accept is refused locally with a denied report.
- `pitlane-trading` runs paper, backtest, research, full-flow, and resident live
  console stages; the block tape, C ABI, and `python/pitlane` wrappers share the
  same formats.
- `ctest` covers the desk, books, order layers, ops, study, adapters, tape, C
  ABI, and the trading stages.
