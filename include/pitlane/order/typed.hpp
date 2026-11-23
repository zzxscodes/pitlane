#pragma once

#include <cstdint>

namespace pitlane {

// Compile-time order tags. Illegal transitions are not callable.
// Empty tags add no storage. The helpers do not send an order.

struct TagDraft {};
struct TagReady {};
struct TagLive {};
struct TagDone {};

template <class Tag>
struct TypedTicket {
    uint64_t id{0};
    int64_t leaves{0};
};

template <class Tag>
inline uint64_t ticket_id(const TypedTicket<Tag>& t) noexcept {
    return t.id;
}

template <class Tag>
inline int64_t ticket_leaves(const TypedTicket<Tag>& t) noexcept {
    return t.leaves;
}

inline TypedTicket<TagDraft> ticket_draft(uint64_t id, int64_t leaves) noexcept {
    TypedTicket<TagDraft> t;
    t.id = id;
    t.leaves = leaves;
    return t;
}

inline TypedTicket<TagReady> mark_ready(TypedTicket<TagDraft> t) noexcept {
    TypedTicket<TagReady> r;
    r.id = t.id;
    r.leaves = t.leaves;
    return r;
}

inline TypedTicket<TagLive> go_live(TypedTicket<TagReady> t) noexcept {
    TypedTicket<TagLive> r;
    r.id = t.id;
    r.leaves = t.leaves;
    return r;
}

inline TypedTicket<TagLive> apply_fill(TypedTicket<TagLive> t, int64_t qty) noexcept {
    if (qty > 0 && t.leaves >= qty) t.leaves -= qty;
    else if (qty > 0) t.leaves = 0;
    return t;
}

inline TypedTicket<TagDone> seal(TypedTicket<TagLive> t) noexcept {
    TypedTicket<TagDone> r;
    r.id = t.id;
    r.leaves = 0;
    (void)t;
    return r;
}

}  // namespace pitlane
