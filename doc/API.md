# pitlane API

This is the call guide. The project introduction is `README.md` in the repository root. The Chinese guide is `API_CN.md` in this directory. When a public call changes, update both guides together.

Include one header:

```cpp
#include "pitlane/pitlane.hpp"
```

Everything is in the namespace `pitlane`. Link `libpitlane.a`, or link the shared library with `-lpitlane`. The shared library's soname is `libpitlane.so.1`. The headers are the same for both.

Prices and quantities are integers. Timestamps are microseconds. A side value of 1 buys and 2 sells. Most calls report failure by a `false`, a negative count, or a `Why` other than `Why::None`. They do not throw for ordinary rejects. A full ring or a full queue fails the write and leaves the previous contents in place.

## Open a shared desk

One file mapping holds the desk. The writer creates it. Other processes attach to the same path.

```cpp
pitlane::MapInfo map{};
if (pitlane::map_create("desk.bin", pitlane::desk_bytes(), &map) != 0) return 1;
pitlane::Desk* desk = pitlane::desk_view(&map, 1);
if (!desk) return 1;
```

`map_create` creates or extends the file and does not wipe an existing mapping. `desk_view` returns the desk only when the magic is `PITLANE1` and the version is `kDeskVersion`, which is 2. The second argument formats a still-blank file when it is non-zero. A file with the wrong magic is left untouched and the call returns null. After the magic is written, do not format the mapping again.

Another process attaches with `map_attach` and passes 0 to `desk_view`. `map_release` unmaps and closes the file. Each ring has one writer. Readers do not share a cursor.

`desk->clock_us` is the session clock in microseconds. `DeskLoop::beat` stores the time it is given there.

## Publish and read

`Desk` contains eight rings: `intents`, `reports`, `quotes`, `algos`, `controls`, `pulses`, `prints`, and `queries`. Their capacities are `kIntentCap` 1024, `kReportCap` 1024, `kQuoteCap` 2048, `kAlgoCap` 256, `kControlCap` 256, `kPulseCap` 512, `kPrintCap` 512, and `kQueryCap` 256. Cash and positions occupy `cash` and `hold`, 256 rows each.

`publish` takes the message and a reader mask. A mask of 0 means the first reader to acknowledge the slot claims it. Any other mask lists the readers that must acknowledge before the slot can be reused. Reader numbers are 0 through 63. The session loop is reader 7, `kLoopReader`.

```cpp
pitlane::Intent in{};
in.tag = 1;
in.name = 1;
in.acct = 2;
in.px = 100;
in.qty = 5;
in.stamp_us = 1;
in.side = static_cast<uint8_t>(pitlane::Side::Buy);
in.act = static_cast<uint8_t>(pitlane::Act::Enter);
in.style = static_cast<uint8_t>(pitlane::Style::Day);

const uint64_t readers = 1ull << pitlane::kLoopReader;
if (!desk->intents.publish(in, readers)) {
    // The ring is full. The new order was not stored.
}
```

A reader copies one slot with `at`. The position is absolute, starting at 0. `fresh` is true when this reader has not acknowledged it. `ack` records that acknowledgement. `latest` copies the newest published item. `tail` is the next position a writer will use.

```cpp
pitlane::Intent got{};
bool fresh = false;
if (desk->intents.at(0, pitlane::kLoopReader, got, &fresh) && fresh) {
    desk->intents.ack(0, pitlane::kLoopReader);
}
```

`Intent`, `Report`, and `Quote` are defined in `types.hpp`. The other slot types, `AlgoJob`, `ControlNote`, `Pulse`, `TapePrint`, and `QueryNote`, are defined in `wire.hpp`. A payload larger than those slots uses `Parcel<Bytes>` on a ring the caller owns. `publish_parcel` fails when the length is greater than `Bytes`.

## Run the session loop

`DeskLoop` drains the rings as reader 7, fires timers, and stops when it sees a control message whose `kind` is 1. `beat` returns immediately once the loop has stopped. A timer id of 0 is rejected. A timer with `every_us` of 0 fires once. There are 32 timer slots.

```cpp
void on_quote(void*, const pitlane::Quote&) {}

pitlane::DeskLoop loop;
loop.add_timer(1, /*when_us=*/1000, /*every_us=*/0);
pitlane::LoopHooks hooks{};
hooks.on_quote = on_quote;
loop.beat(*desk, /*now_us=*/1000, hooks);
```

The hooks are function pointers: `on_quote`, `on_report`, `on_pulse`, `on_print`, `on_algo`, `on_query`, `on_control`, and `on_timer`. Only the hooks you set are called. Messages published for this loop must include reader bit 7, or use a mask of 0.

## Prepare an order

`IdMint` builds a restart-safe id. The fields are site, kind, the last digit of the trading day, a ten-second slot, and a serial number, packed into one 64-bit integer. The kind is defined by the caller.

```cpp
pitlane::IdMint mint(/*site=*/1, /*kind=*/1);
uint64_t tag = mint.next(/*day_index=*/4, /*sec_of_day=*/10 * 3600);
```

`normalize_gate` fixes offset and direction before an order is stored. Cash products are forced to open. On SHFE and INE a plain close becomes close-yesterday. On the other futures exchanges, close-today and close-yesterday become a plain close. Wire sides 0, 2, 5, and 6 are long. Wire sides 1, 3, 4, and 7 are short. A limit price of 0 is rejected. `Why` on the result says why.

```cpp
pitlane::GateForm form{};
std::snprintf(form.symbol, sizeof form.symbol, "rb2410.SHF");
form.side = static_cast<uint8_t>(pitlane::Side::Sell);
form.off = static_cast<int8_t>(pitlane::Off::Close);
form.px_mode = static_cast<int8_t>(pitlane::PxMode::Limit);
form.px = 3500;
form.qty = 2;
pitlane::GateResult gate = pitlane::normalize_gate(form);
```

`split_lots` turns one futures quantity into slices. The modes are `TodayThenYdayThenOpen`, `CloseOnly`, and `YdayThenStop`. Any other mode returns -1. A slice above the maximum lot is cut, and a slice below the minimum lot is dropped.

`commodity_plan` and `index_plan` each return one next order toward a target. Where today and yesterday are split, yesterday is closed first. `index_plan` uses yesterday equal to position minus quantity opened today. `plan_px` uses the ask for a buy and the bid for a sell.

`lock_future` and `lock_cash_sell` freeze the quantity a close or a stock sell needs. `book_fill` applies a fill back onto today, yesterday, and the total.

## Match on the paper venue

`PaperVenue` handles 48 names, `kNames`, and 192 live orders, `kTickets`. A name must be less than 48. An order is judged only after its send delay has elapsed, against the book at that moment. A cancel waits for its cancel delay.

```cpp
pitlane::VenueCfg cfg{};
cfg.lot = 1;
pitlane::PaperVenue venue(cfg);
const pitlane::Rung asks[] = {{100, 5}};
venue.set_levels(1, nullptr, 0, asks, 1);

pitlane::Report reps[4]{};
pitlane::VenueOut out = venue.admit(in, in.stamp_us, reps, 4);
```

`admit` writes reports into the caller buffer and sets `out.why`. `on_clock` promotes staged orders whose delay has elapsed and completes cancels that are due. `on_print` and `on_tape` reduce the quantity ahead of a resting order. A print whose aggressive side equals the resting order's side does not fill it. Pass 0 as the aggressor to keep every print. `on_shown` applies a later change in displayed size without counting the same trade twice.

`Style::Day` fills the whole remainder at its limit, even past the size shown there. `Style::Passive` expires if it would cross. `Style::Immediate` keeps only the shown size. `Style::All` expires the whole order when the shown size is short. A self-trade against the same account stops the order.

Before 09:30 the delay tables are zero. After that, `session_band` splits the day at 10:00, 10:30, 11:00, 11:30, 13:30, 14:00, 14:30, and 15:00. `SimClock` uses those nine bands unless sample points are loaded. A negative result from `SimClock::go` means the order did not reach the book. The caller applies that result. `admit` does not read the clock itself.

`auction_clear` picks the price with the largest matched quantity. `tie_higher` selects the higher price on a tie. Otherwise the price closer to the reference is used.

## Names, signals, and the counter

`NameBook` stores at most 256 instruments. `insert` writes an id starting at 1, and returns false when the symbol already exists or the book is full. `link` records a relation: 0 related, 1 underlying, 2 derived, 3 track underlying, 4 track derived. `set_session` allows the open minute to be later than the close minute for a session that crosses midnight.

`PulseBoard` keeps 128 subscriptions and a queue of 256. `watch` starts a subscription. `offer` fails when nothing matches or the queue is full. `drain` copies the queued pulses out.

`BrokerWatch::screen` checks quantity, side, notional, and the number of live orders. `remember` tracks a sent order. `expire` drops orders that missed their acknowledgement limit, passed their lifetime, or waited too long after a cancel. A limit of 0 or less turns that check off.

`QueryFold` accepts at most 4 engines and 32 accounts, with 16 queued requests per engine. `push` fails when the engine, the account, or the queue is out of range. `fold` merges the queues, and `pending` lists each account once. The library does not sleep between pages. The caller decides when to query again.

`session_date` moves a clock reading onto its trading day. From 08:00 through 18:00 the shift is 0. After 18:00, Monday shifts back 3 days and other days shift back 1. In the early morning, Monday shifts back 2 days and other days shift back 1. Weekends and days in `ClosedDays` are then skipped.

## Helpers

`QuoteFold::apply` writes a batch of `Add`, `Set`, and `Del` edits. One invalid edit leaves the previous levels in place. `copy` returns the best price first. `PrintLog::push` stores a print and does not take a book. `DayRoll::on_msg` updates the current minute and returns false when the timestamp moves backward. Time is microseconds. A minute older than one day is dropped.

```cpp
pitlane::QuoteFold<8> book;
const pitlane::FoldEdit edits[] = {{pitlane::FoldOp::Add, 1, 10, 4}};
book.apply(edits, 1);
```

Side 1 is the bid and side 2 is the ask. `Add` fails when that price already exists. `Set` with quantity 0 removes a level. `Del` of a missing price succeeds.

`band_at_hold` keeps a price that sits exactly on the next band's start in the lower band. `walk_up` and `walk_down` then take ticks from that band. After a step lands on the next start, later steps use the new band. `clamp_px` applies only when the upper limit is above the lower limit. `span_index` returns the span that contains a minute, or -1. An overnight span has an end at or before its begin. `session_part` returns open, lunch, or closed for a `SessionRow`.

`life_next` returns the next `Phase`. `Phase::None` means the event is not allowed from that phase. A fill while a cancel is waiting stays in `PullWait`. `Resume` and `ResumePartial` move a rejected cancel back to working or partial.

`margin_need` is multiplier times price times quantity times the margin numerator, divided by the margin denominator. `charge_class` uses the cash formula for stock, bond, fund, tech, index, and repo, and the rate formula for the other products. `scale_of` supplies the multiplier and the margin fraction.

`rest_room` is the quantity still allowed to rest on one leg. When both legs are sent together the result is 0. `cut_hedge` reduces the other leg after one leg is cut. `slice_weight` rounds half up and returns false when the new slice is not positive. `align_leg` divides by the feed coefficient and rounds to the tick, half up. `pair_qty_ok` requires 180 on a 688 or 689 code and 80 on other Shanghai or Shenzhen codes. `chase_px` walks the price up for a buy and down for a sell.

`IdBind::put` stores a pending link and fails when the table is full or the counter key already exists. `confirm` marks it linked. `erase` leaves a tomb so a later put can reuse the slot. A local id of 0 is rejected.

`bind_fill` accumulates quantity and amount on the early fields while the order is still unset, sent, or acknowledged. `bind_status` copies those fields into the order and turns an acknowledgement with a fill into a partial. `bind_final` holds a filled or cancelled status while the local remainder is still above the counter remainder. `bind_catchup` applies that status once the remainder is 0. `quote_repost` is false while a cancel is out and the new price equals the fill price.

`lane_accepts` reads a `LaneRule`. A pull needs `pull`. An enter needs the price type and the offset bit. `lane_can_amend` reads `amend`. `Cause` values from 10 are field checks, from 40 are cash and position, from 70 are risk, and from 100 are outside this process. `cause_band` returns that group. `cause_text` is a short fixed word.

`list_cause` checks the open list or the close list. `FlowWindow::allow` counts the new event and allows it only while the count is still below `limit`. The span is in the same unit as the timestamps. `count_ratio_ok` passes when the sample is still below `min_n`, and a ratio equal to the cap also passes. `cash_ratio_ok` fails when the cash ratio is equal to the cap. `cancel_ok` and `reject_ok` fail on an equal ratio. `fill_ok` passes on an equal floor. `px_shift` is the signed distance from the last price, scaled by `den`. `deviation_cause` uses the absolute distance. `wash_hit` matches the same instrument and the opposite side when the account matches, or when the group is not 0 and matches. The prices must meet or cross.

`ByteFrame::begin` reserves four bytes. `end` writes the payload length there and returns the total size, or -1. `frame_read` fails when the buffer is shorter than that length. `seq_feed` returns 0 for the next id, a positive gap when ids were skipped, and -1 for an old id. The first id starts the watch. `SeqHold::start` sets the next id that `take` will release. `offer` keeps an id only inside the next `Cap` ids. `SpanMark::mark` rejects a negative tick. `between` fails while either node is unset.

## Caller-owned helpers

These calls do not attach to the desk. The caller keeps the storage, passes the numbers, and decides what to do with the result.

`IdSlip::issue` returns the next id. `undo` returns that id only while it is still the latest one and `keep` has not been called. `WireMold::seal` copies a prefix into the caller's buffer. `put_u64` and `put_i64` write little-endian bytes at an offset inside that prefix. `read_block` accepts a block at least `know` bytes long and rejects a shorter one. `block_end` steps past the declared block, including bytes this build does not read.

`StayShed::drop` uses the caller's time unit. A stay below the target clears drop mode. The first stay at or above the target starts the window and is kept. After the window, a late item is dropped. A stay below the target leaves drop mode.

`floor_chunk` moves a negative tick away from zero to the block start. `ChunkMap<Ticks,Blocks>` keeps quantity in blocks of `Ticks` prices, with at most `Blocks` blocks. `Ticks` is 8 to 256 and a multiple of 64. `Blocks` is 1 to 64. A quantity of 0 clears a tick. A new block is taken only for a positive quantity. A full directory fails and leaves the old quantities in place. `chain_link` appends a caller-owned node and returns a slot plus its reuse count. `chain_unlink` fails when the reuse count does not match. `chain_zero` leaves the node linked with quantity 0. `chain_take` reduces that quantity.

`ticket_pull` and `ticket_amend` run only from `Working` or `Partial`. Each one stores the previous ticket and bumps the revision. `ticket_rewind` restores that snapshot only while the revision is still the one written by the command. `ticket_fill` also bumps the revision, so a fill blocks a pending rewind. A fill during `PullWait` stays in `PullWait`.

`slice_open` divides a parent quantity by the number of intervals in the horizon, at most one million cuts. When one cut would be below the lot and the total still covers one lot, the plan becomes a single cut. The last cut receives the remainder. `slice_poll` waits until the due time. `slice_restore` returns an unfinished child to the pool. `slice_late` adds the fill, takes what it can from the pool, and returns how much was removed from children already scheduled. `basket_make` accepts `Create`, `Pause`, `Resume`, `Stop`, and `Update`. The kind is at most 15 characters. A byte count above `max_bytes` fails. The payload itself is not stored.

`recon_fill` applies one venue fill id once. An unknown order is built from `size` and reported as an acknowledgement, then a fill. Quantity past the order increments `over` and is not added to `filled`. A repeated id emits `Dup` and does not change the order. `recon_cancel` marks a known, unfinished order cancelled.

`FlightQueue<Cap>` holds 2 to 64 commands, ordered by due time. `push` fails when the queue is full, or when the tag or the instrument is 0. `take` removes only commands for that instrument whose due time has arrived.

`BarFold` ignores a tick the caller marks as not tradable, a non-positive price, a negative quantity, or a negative timestamp. The bucket is the timestamp minus its remainder modulo the span. A later bucket closes the current bar. An older tick is rejected and the bar stays. Empty spans are not created. `seal` emits the bar that is still open.

`act_mul` uses `jump_den / jump_num` as the price multiplier. A cash dividend changes that factor only when `income` is set, and only when the previous close is above the dividend. `mul_fold` multiplies actions whose `ymd` is at or before the as-of date. `scale_px` truncates toward zero. `traded_move` returns the price change after the raw jump is removed. A 1-into-2 split uses jump `1/2`. When the ex price is exactly half of the previous close, the traded move is 0.

## Frames, pacing, and snapshots

These calls do not open a socket and do not send an order. The caller keeps the bytes, the clock, and the decision.

`digest_of` covers the bytes it is given. `Sum8` is the low 8 bits of the sum. `Crc16` is CRC-16/CCITT-FALSE. `Crc32` is the ISO-HDLC CRC-32. `digest_write` stores that value little-endian. `StreamAsm<Cap>` appends into its own buffer, with `Cap` from 16 to 65536. `FrameSpec` reads a little-endian body length at `len_off` inside a header of 4 to 32 bytes. `pull` waits until the header, body, and checksum are all present. A body longer than `max_body`, or a frame that cannot fit, returns `BadLen` and leaves the buffer unchanged. A bad checksum returns `BadDig`, drops that frame, and keeps the bytes after it. A good frame stays at the front until `commit`. `append` fails while a frame is still held.

`SeqGate<Cap>` keeps 2 to 64 later sequence numbers. `start` sets the next expected number, and 0 is rejected. `live` releases a number only when it is exactly the next one. A hole records the missing range and parks the later number. A live number inside the open hole is ignored. `replay` accepts only the next missing number. When the hole is filled, consecutive parked numbers are released, and a later hole is opened if the side list still has one. A full side list or a short output buffer leaves the gate unchanged. The gate does not send a resend request.

`token_open` fills a bucket to `burst`. `token_take` adds `rate` tokens for each `per` units of time, capped at the burst. Time that moves backward does not add tokens. A request above the burst fails. `gcra_open` stores an interval and a tolerance. `gcra_allow` passes when `now + tolerance` reaches the theoretical arrival time, then moves that time forward by one interval. `TokenBook` and `GcraBook` hold 1 to 64 keys. Key 0 is rejected. A new key is refused when every slot is still inside the idle time. An idle time of 0 never reuses a slot. A failed first take does not keep the new key.

`sign_ready` is true when the reject count is 0 and the pass count has reached `need`. A later pass does not clear a reject. `sign_reset` clears both counts and leaves `need` as it is. `hold_reserve` on an opening order takes the quantity and one cancel credit. A closing order takes neither. `hold_cancel` applies to an opening hold and returns the unfilled quantity. The cancel credit is returned only when `rejected_here` is set. `hold_done` returns that credit when an opening order is fully filled and the fill is not marked outside. `hold_late` on an opening order returns the unfilled quantity and the credit. On a closing order it only clears the hold. `hold_cancel_late` returns the credit and leaves the hold open, so a later full fill does not return it again.

`key_num` and `key_text` keep the interface name with the counter key. A numeric key and a text key are not the same. Text longer than 23 characters, or an interface name longer than 15, is rejected and the previous key is left unchanged. `AckBook<Cap>` holds 2 to 64 rows. `put` fails when the table is full or the key is already present. `link` moves a row from waiting for a place acknowledgement to linked. `arm_cancel` starts the cancel clock from the linked state. `sweep` lists due local ids and does not change the rows. A row is due when its age is greater than its own limit. A second sweep inside `gap` returns no rows. A short output buffer leaves the sweep clock unchanged.

`lat_mark` stores a non-negative tick only when marking is enabled. `lat_span` fails while either node is unset. `skew_sample` stores half of the desk time minus the local time, with integer division toward zero. `skew_apply` adds that integer. `LatHist<Buckets>` uses 4 to 32 buckets. A value of 0 or 1 lands in bucket 0. Larger values use the highest power of two that is still inside the value, and the last bucket holds anything above it. `cum` adds the counts from bucket 0 through the given bucket.

`SnapPub<T, Slots>` stores 2 or 3 inline slots. One writer calls `begin_write`, fills that slot, and `publish` makes it visible. `pin` holds the published slot for one reader. A second pin fails until `unpin`. With 2 slots, a write fails while the reader still holds one and the other is published. With 3 slots, the writer may replace a slot that was published and then passed over. The library does not start a thread.

`SigBook<Buckets, Keys>` uses a power-of-two bucket count from 2 to 256, and 1 to 1024 keys. Each bucket has 8 slots. A signature of 0 means empty, and a computed 0 is stored as 1. The signature is written after the key index. `find` returns a 1-based index, or 0. `put` of a key that is already present returns true. When both buckets are full, an insert moves an older key toward its other bucket at most 8 times. If that fails, the previous keys remain. One writer inserts.

`Grace<Readers>` tracks 1 to 64 readers. `online` records the current epoch. `offline` stores 0 and that reader no longer blocks. `quiesce` records the epoch after the caller has dropped shared pointers. `bump` advances the epoch and returns the new value. `elapsed` returns false while any online reader is still behind, and it does not wait. A caller who is also in the reader list must `quiesce` before `elapsed`, otherwise that caller is still behind. `RetireList<Cap>` holds 2 to 64 tokens in insertion order. `reap` releases a prefix of tokens that have elapsed and stops at the first one that has not. A short output buffer leaves the list unchanged. The list does not free the caller's memory.

## Where the calls live

| Header | What it covers |
| --- | --- |
| `region.hpp` | The desk, the mapping, and the ring capacities |
| `ring.hpp` | `FanRing` |
| `types.hpp` | `Intent`, `Report`, `Quote`, sides, styles, phases, and `Why` |
| `wire.hpp` | Algo, control, pulse, print, query, and parcel slots |
| `runtime.hpp` | `DeskLoop` |
| `ids.hpp` | `IdMint` |
| `codec.hpp` | Symbol class, offset, and direction |
| `lots.hpp` | Splitting one quantity into orders |
| `plan.hpp` | The next order, resting room, slices, and chase prices |
| `ledger.hpp` | Freezes, margin, fees, fills, login ids, and the working slot |
| `venue.hpp` | `PaperVenue` |
| `lags.hpp` | Session bands and sampled delay |
| `simclock.hpp` | `SimClock` |
| `cross.hpp` | The limit book and the call auction |
| `names.hpp` | `NameBook` |
| `signal.hpp` | `PulseBoard` |
| `broker.hpp` | `BrokerWatch` and `QueryFold` |
| `calendar.hpp` | Trading days and session times |
| `feed.hpp` | Subscription mask, price scale, and raw ticks |
| `cash.hpp` | Cash and position rows |
| `logline.hpp` | `Log` |
| `quote.hpp` | Quote batches, the print log, and minute rolls |
| `steps.hpp` | Tick bands and price walks |
| `life.hpp` | Order transitions |
| `bind.hpp` | Counter ids and out-of-order reports |
| `lane.hpp` | Channel rules and `Cause` |
| `judge.hpp` | List, flow, ratio, deviation, and self-trade checks |
| `frame.hpp` | Frames, sequence gaps, and reordering |
| `span.hpp` | Timestamps beside a message |
| `numeric.hpp` | Wide products used by the checks |
| `mold.hpp` | Prepared frames, block length, and an id that can be returned |
| `shed.hpp` | Sojourn drop |
| `chunk.hpp` | Sparse price blocks and cancel handles |
| `ticket.hpp` | Command snapshot and revision |
| `parent.hpp` | Parent time slices and a basket header |
| `recon.hpp` | Local and counter fill alignment |
| `inbound.hpp` | Commands waiting for their due time |
| `bar.hpp` | Bars from ticks the caller marks tradable |
| `corp.hpp` | Corporate-action price factors |
| `stream.hpp` | Frame checksums and byte-stream reassembly |
| `seqgate.hpp` | Sequence gap recovery |
| `pace.hpp` | Token buckets and theoretical arrival times |
| `sign.hpp` | Check counts and opening quota |
| `ack.hpp` | Place and cancel acknowledgement timers |
| `trace.hpp` | Latency samples, clock offset, and buckets |
| `snap.hpp` | Snapshot slots published as a whole |
| `sig.hpp` | Signature buckets |
| `grace.hpp` | Reader epochs and a retire list |
