# Changelog

## 1.0.0

Current release state.

### Web console

- The Market tab pairs a sortable, filterable watchlist with the chart. It is
  backed by one `GET /api/quotes` read of the fixed quote rings, shows session
  open/high/low/change and tick count, and keeps the chart, ticket, and depth on
  the same selected product.
- The console adds seven product workspaces (futures, stocks, HK, options, ETF,
  bonds, US). Each filters quotes, positions, and orders by instrument class,
  shows class parameters and the book, and sends through the same direct ticket
  and risk gate; the class workspace follows the selected product.
- The Trade tab adds an account KPI strip, a quote board, quick price buttons
  (opponent, bid, ask, last, upper/lower limit where a depth book exists), a
  quantity stepper with flat shortcuts, a notional and margin estimate, an
  optional order confirmation, and a fill-progress order blotter with cancel-all.
- The Algorithm tab carries the TWAP, conditional, chase, basket-netting, and
  unlock tickets. The Portfolio tab carries pair orders, packages, and file
  import. The Margin/credit tab reads broker credit limits and submits the
  securities-credit business axis (margin buy, short sell, cover, repay,
  subscribe, redeem) through the same order endpoint.
- Positions gain per-row close and reverse actions that submit through the same
  manual order path and risk gate.
- The header shows the local clock and trading-session badge; tab, instrument,
  timeframe, and funding account are persisted; keyboard shortcuts cover tab
  switching (`Alt+1`..`9`/`0`), buy/sell (`b`/`s`), instrument search (`f`),
  refresh (`r`), and help (`?`, `Esc`).
- `GET /api/quotes` is documented in both web API guides. The watchlist, the
  quote board, and the product workspaces read it once per refresh instead of
  polling one product at a time.

### Kernel and desk

- C++17 kernel built around one `PITLANE1` memory-mapped desk: fixed-size
  quotes, intents, reports, execs, cash, and hold rows, with one writer per ring
  and fail-closed publishing.
- `Intent` carries `biz` and `px_mode` inside the 56-byte frame; `Report` is 64
  bytes and echoes the force-close reason and business kind.
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
  The market tab pairs a sortable watchlist with the chart, and the trade tab
  carries an account strip, quote board, and a fill-progress order blotter.
- Conditional orders: a fixed-size book on the trading thread arms on last,
  bid, or ask crossing a trigger price, then fires once through the same manual
  direct path (pause/stop gate, quantity and notional caps, rate limit, and RMS).
- Chase orders: a fixed-size book keeps one child at the opposite touch and
  re-prices it after a configurable step, or takes once per remainder without
  re-pricing; stopping cancels the working child.
- `GET /api/cond`, `POST /api/cond`, `GET /api/chase`, and `POST /api/chase`
  expose both books; the Trade tab adds cards to arm/cancel conditions and
  start/stop chases, with the existing i18n and polling.
- `--record-block FILE` writes a block tape per subscribed instrument so the
  Data tab can load a live session; `--record` still writes the CSV quote tape.
- Night sessions stay on one clock: a block tape adds a day when `exch_us`
  wraps at midnight, and the CSV readers keep the post-midnight rows for
  research, backtests, and history; the CTP trading adapter confirms the
  settlement notice before the night and morning opens and after a `90`
  rejection, so a resident process keeps trading across the trading-day switch.

### Adapters and tooling

- CTP market-data and trading adapters forward offsets, price types, hedge
  flags, business kinds, and exercise requests without rewriting them; anything
  the counter cannot accept is refused locally with a denied report.
- `pitlane-trading` runs paper, backtest, research, full-flow, and resident live
  console stages; the block tape, C ABI, and `python/pitlane` wrappers share the
  same formats.
- `ctest` covers the desk, books, order layers, ops, study, adapters, tape, C
  ABI, and the trading stages.
