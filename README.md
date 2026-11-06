# pitlane

The Chinese introduction is in `README_CN.md`. API usage is in `doc/API.md` and `doc/API_CN.md`. When the library changes, update each language pair together.

pitlane is a C++17 library for trading processes on the same machine. It gives those processes one shared workspace, and on that workspace it handles orders, positions, trading days, and paper matching. Prices are integers. Times are microseconds.

The name comes from the pit lane beside a racetrack. Orders and market data enter this lane, where they are checked, queued, and matched, and then handed to an outside counter or strategy. The library itself does not connect to an exchange.

## What it does

Strategy, market data, and order entry often run in different processes. pitlane lets them exchange orders, reports, quotes, signals, and queries through one file mapping. Message size is fixed, and capacity is fixed at compile time. When a ring is full, the next write fails. The library does not grow the ring, and it does not overwrite a message that has not been acknowledged.

Order slots, ledger rows, and live orders sit in fixed arrays. Cash and positions are updated in place, and the critical section is short.

Include `include/pitlane/pitlane.hpp` and link `libpitlane.a`. Types and functions live in the `pitlane` namespace. The interface is designed on its own and cannot replace another trading library that is already running.

## Shared region

A mapping file starts with the magic `PITLANE1`. The current version is 2. `desk_view` opens a file only when both the magic and the version match. A blank file is initialized once, and only when the call allows creation. An older file with a different version is not treated as the current layout.

One shared region holds eight rings.

| Contents | Capacity |
| --- | ---: |
| Orders | 1024 |
| Reports | 1024 |
| Quotes | 2048 |
| Algo orders | 256 |
| Control | 256 |
| Signals | 512 |
| Prints | 512 |
| Queries | 256 |

Cash and positions each have 256 rows.

Each ring has one writer. Each reader remembers the next position it will read. A publish can name the readers that must acknowledge the message. With no readers named, the first reader to acknowledge takes the message. Once readers are named, the slot can be reused only after all of them have acknowledged it. The session loop uses reader bit 7. A message meant for that loop must set this bit, or it must name no reader.

A control message of kind 1 stops the session loop. A timer with a repeat interval of 0 fires once.

Packets larger than these slots stay out of the shared region. The caller sends them on its own ring. A send that exceeds the given capacity fails.

## Orders, positions, and queries

An order id is a 64-bit integer. The fields are site, kind, the last digit of the trading day, a ten-second slot, and a serial number. They are stored as bit fields, not concatenated as decimal text. The caller defines the kind.

The instrument directory holds at most 256 names, numbered from 1. A name can point at related instruments, and it can record its underlying, its derivatives, and what it tracks. A night session may open later than it closes, which means the session crosses midnight.

Before an order is stored, offset and direction are normalized. Stocks, bonds, funds, STAR Market names, indexes, and repos are treated as opens. On SHFE and INE, a plain close becomes a close of yesterday. On the other futures exchanges, close-today and close-yesterday become a plain close. Buy, margin buy, buy-to-cover, and creation are long. Sell, short sell, sell-to-repay, and redemption are short. A limit order with a price of 0 is rejected.

One futures quantity can be split into several orders in three ways: close today, then close yesterday, then open the rest; close only; or close yesterday only. Any other way fails. Each slice stays within the maximum lot, and a slice smaller than the minimum lot is not sent.

Moving a position toward a target sends one order at a time. Where today and yesterday are stored apart, yesterday is closed first. Commodity futures also check that the open does not pass the open limit. Equity-index futures use the net position, and they close yesterday only. Yesterday here is the position minus the quantity already opened today.

Close-today, close-yesterday, and a plain close freeze different quantities. A stock sell reduces the quantity available to sell. A fill then writes today, yesterday, and the total back according to the offset and direction at the time of the order.

Before an order goes to the counter, the library can limit the quantity, the notional, and the number of orders still working. Working orders are checked against three limits: no acknowledgement for too long, a lifetime that has expired, and a cancel that has waited too long. An expired order leaves the working list.

Position and cash queries are queued per engine, for at most 4 engines and 32 accounts. Requests for the same account become one query. A new request fails when the queue is full. The caller decides when to ask for the next page.

## Market data and the trading day

A subscription receives every instrument unless it is changed to receive only instruments allowed in advance. A missing limit-up or limit-down price is filled from the fallback price supplied by the caller. A raw price is divided by the given denominator. A record marked `S` is a session switch and is dropped.

A market-data time is moved back onto the trading day it belongs to. From 08:00 through 18:00 it stays on the calendar day. After 18:00, Monday moves back 3 days and every other day moves back 1 day. In the early morning, Monday moves back 2 days and every other day moves back 1 day. Weekends and closed days are then skipped.

Paper orders and cancels can use a different delay in each part of the day. Before 09:30 the delay is 0. After that, the day is split at 10:00, 10:30, 11:00, 11:30, 13:30, 14:00, 14:30, and 15:00, which makes nine bands. From 15:00 onward the last band is used. A set of samples can be used instead, and the delay is interpolated. A negative order delay means the order did not reach the book.

## Paper matching

Paper matching handles 48 instruments at once, with at most 192 orders still working. A new order waits out its order delay, and only then does the book at that moment decide a fill, a rest, or an expiry. A cancel waits out its cancel delay in the same way.

A day order that reaches its own limit fills the whole remainder, which may be larger than the quantity shown at that price. An order that only wants to rest expires as soon as it would trade through the opposite price. An immediate order takes only the quantity then visible, and the rest expires. An all-or-nothing order expires entirely when the visible quantity does not cover it.

The fill price is the passive price. A price level is removed as soon as its quantity reaches 0. If the buy and the sell belong to the same account and would trade with each other, the order stops. A call auction picks the price that matches the most quantity. When two prices match the same quantity, the higher price can be chosen, or the price closer to a reference.

After an order rests, prints reduce the quantity ahead of it. A print whose aggressive side is the same as the resting order does not fill it. When the displayed quantity at a price changes later, prints already counted are not subtracted again.

## Helpers for market data, orders, and counters

These calls sit beside the shared desk. A market-data process, an order process, and a counter adapter can use them. The library still does not connect to an exchange.

A quote book takes a batch of level changes and keeps the previous book when any edit in the batch is invalid. Prints are stored on their own log, so a print does not change quote levels. A minute roll keeps high, low, and volume, and it ignores a clock that moves backward.

Price walks move an integer price across tick bands. Session calls say which span a minute belongs to, including a session that crosses midnight.

An order moves only through the transitions the life table allows. Margin and fees follow the product class. Slice, hedge, board-lot, and chase calls prepare the next child order. The caller still decides whether to send it.

A bind table maps a local id to a counter id and holds a fill that arrives before its status. A channel rule lists the price types and offsets that channel accepts. Reject codes for local checks and for the counter share one numbering.

List, flow, ratio, price-distance, and self-trade checks only report a reason. Framing writes the length after the body is known. A sequence watch reports a gap, and a short hold releases messages in id order. A span mark stores timestamps beside a message.

## Build and install

CMake 3.16 or newer is required, together with a C++17 compiler. From the repository root:

```bash
cmake -S pitlane -B pitlane/build
cmake --build pitlane/build
cd pitlane/build && ctest --output-on-failure
```

Run the tests from the build directory. To check for address errors and undefined behavior, build a second tree:

```bash
cmake -S pitlane -B pitlane/build-san -DPITLANE_SANITIZE=ON
cmake --build pitlane/build-san
cd pitlane/build-san && ctest --output-on-failure
```

The build enables the ordinary warnings and treats them as errors. Install into a chosen prefix:

```bash
cmake --install pitlane/build --prefix /opt/pitlane
```

One build produces the static library `libpitlane.a` and the shared library `libpitlane.so`. The code is built position-independent, so the static library can also be linked into another shared library. To build the static library only, configure with `-DPITLANE_BUILD_SHARED=OFF`.

Other programs include the same headers. Link the shared library with `-lpitlane`, and make sure `libpitlane.so` can be found in the install directory at run time. The install also places the headers under `include/pitlane`.

`examples/demo.cpp` creates a temporary mapping, writes one buy order, and fills it in the paper matcher. After the build:

```bash
./pitlane/build/pitlane_demo
```

## Limits

Each ring may have only one writing process. Each reader keeps its own read position.

Do not initialize a mapping again after its magic has been written.

Instrument numbers used by paper matching must be less than 48. Signals allow 128 subscriptions and a queue of 256. A new signal is rejected once that queue is full.
