#pragma once

#include "pitlane/desk/types.hpp"

#include <cstdint>

namespace pitlane {

// A local command snapshots the ticket first. A fill is an outside fact and moves the revision,
// so a later rewind cannot cover it. A cancel the counter has already accepted is not rewound here.

struct Ticket {
    uint64_t tag{0};
    Phase phase{Phase::None};
    int64_t qty{0};
    int64_t filled{0};
    int64_t px{0};
    uint64_t rev{0};
};

struct TicketShot {
    uint64_t tag{0};
    Phase phase{Phase::None};
    int64_t qty{0};
    int64_t filled{0};
    int64_t px{0};
    uint64_t rev{0};
};

struct TicketMove {
    TicketShot before{};
    uint64_t after_rev{0};
    uint8_t armed{0};
};

inline bool ticket_open(Phase phase) noexcept {
    return phase == Phase::Working || phase == Phase::Partial;
}

inline TicketShot ticket_shoot(const Ticket& ticket) noexcept {
    TicketShot shot;
    shot.tag = ticket.tag;
    shot.phase = ticket.phase;
    shot.qty = ticket.qty;
    shot.filled = ticket.filled;
    shot.px = ticket.px;
    shot.rev = ticket.rev;
    return shot;
}

inline bool ticket_pull(Ticket& ticket, TicketMove& move) noexcept {
    move = TicketMove{};
    if (!ticket_open(ticket.phase) || ticket.rev == UINT64_MAX) return false;
    move.before = ticket_shoot(ticket);
    ticket.phase = Phase::PullWait;
    ++ticket.rev;
    move.after_rev = ticket.rev;
    move.armed = 1;
    return true;
}

inline bool ticket_amend(Ticket& ticket, int64_t px, int64_t qty, TicketMove& move) noexcept {
    move = TicketMove{};
    if (!ticket_open(ticket.phase) || px <= 0 || qty <= ticket.filled || ticket.rev == UINT64_MAX) return false;
    move.before = ticket_shoot(ticket);
    ticket.px = px;
    ticket.qty = qty;
    ++ticket.rev;
    move.after_rev = ticket.rev;
    move.armed = 1;
    return true;
}

inline bool ticket_rewind(Ticket& ticket, TicketMove& move) noexcept {
    if (!move.armed || ticket.tag != move.before.tag || ticket.rev != move.after_rev) return false;
    if (ticket.rev == UINT64_MAX) return false;
    ticket.phase = move.before.phase;
    ticket.qty = move.before.qty;
    ticket.filled = move.before.filled;
    ticket.px = move.before.px;
    ++ticket.rev;
    move.armed = 0;
    return true;
}

inline bool ticket_fill(Ticket& ticket, int64_t qty) noexcept {
    if (qty <= 0 || ticket.rev == UINT64_MAX) return false;
    if (ticket.phase != Phase::Working && ticket.phase != Phase::Partial && ticket.phase != Phase::PullWait) return false;
    if (ticket.filled > ticket.qty || qty > ticket.qty - ticket.filled) return false;
    ticket.filled += qty;
    if (ticket.phase != Phase::PullWait) {
        ticket.phase = ticket.filled == ticket.qty ? Phase::Filled : Phase::Partial;
    }
    ++ticket.rev;
    return true;
}

}  // namespace pitlane
