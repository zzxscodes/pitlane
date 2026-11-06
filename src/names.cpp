#include "pitlane/names.hpp"

#include <cstring>

namespace pitlane {
namespace {

bool copy_sym(char* dst, const char* src) noexcept {
    if (!src || !src[0]) return false;
    std::strncpy(dst, src, 31);
    dst[31] = 0;
    return true;
}

bool ends_with(const char* s, const char* suf) noexcept {
    if (!s || !suf) return false;
    const std::size_t n = std::strlen(s);
    const std::size_t m = std::strlen(suf);
    if (m > n) return false;
    return std::memcmp(s + (n - m), suf, m) == 0;
}

template <typename T, int N>
bool push_id(T (&arr)[N], uint8_t& count, uint32_t id) noexcept {
    if (id == 0 || id > 65535 || count >= N) return false;
    for (uint8_t i = 0; i < count; ++i) {
        if (arr[i] == id) return true;
    }
    arr[count++] = static_cast<T>(id);
    return true;
}

}  // namespace

bool NameBook::insert(const char* symbol, uint8_t kind, uint32_t* id) noexcept {
    if (find(symbol)) return false;
    if (used_ >= kCap) return false;
    NameCard& card = card_[used_];
    card = NameCard{};
    if (!copy_sym(card.symbol, symbol)) return false;
    card.kind = kind;
    card.used = 1;
    card.id = used_ + 1;
    ++used_;
    if (id) *id = card.id;
    return true;
}

NameCard* NameBook::find(const char* symbol) noexcept {
    return const_cast<NameCard*>(static_cast<const NameBook*>(this)->find(symbol));
}

const NameCard* NameBook::find(const char* symbol) const noexcept {
    if (!symbol) return nullptr;
    for (uint32_t i = 0; i < used_; ++i) {
        if (card_[i].used && std::strcmp(card_[i].symbol, symbol) == 0) return &card_[i];
    }
    return nullptr;
}

NameCard* NameBook::at(uint32_t id) noexcept {
    return const_cast<NameCard*>(static_cast<const NameBook*>(this)->at(id));
}

const NameCard* NameBook::at(uint32_t id) const noexcept {
    if (id == 0 || id > used_ || !card_[id - 1].used) return nullptr;
    return &card_[id - 1];
}

bool NameBook::link(uint32_t from, uint32_t to, int kind) noexcept {
    NameCard* card = at(from);
    if (!card || !at(to) || from == to) return false;
    switch (kind) {
        case 0: return push_id(card->related, card->n_related, to);
        case 1: return push_id(card->underlying, card->n_underlying, to);
        case 2: return push_id(card->derived, card->n_derived, to);
        case 3: return push_id(card->track_underlying, card->n_track_underlying, to);
        case 4: return push_id(card->track_derived, card->n_track_derived, to);
        default: return false;
    }
}

bool NameBook::same_symbol(uint32_t id, const char* symbol) const noexcept {
    const NameCard* card = at(id);
    return card && symbol && std::strcmp(card->symbol, symbol) == 0;
}

void NameBook::set_session(uint32_t id, int open_min, int close_min) noexcept {
    if (NameCard* card = at(id)) {
        card->open_min = open_min;
        card->close_min = close_min;
    }
}

bool NameBook::in_session(uint32_t id, int minute) const noexcept {
    const NameCard* card = at(id);
    if (!card) return false;
    if (card->open_min == 0 && card->close_min == 0) return true;
    if (card->open_min < card->close_min) return minute >= card->open_min && minute < card->close_min;
    if (card->open_min > card->close_min) return minute >= card->open_min || minute < card->close_min;
    return false;
}

void NameBook::set_quote(uint32_t id, int64_t bid, int64_t ask, int64_t last) noexcept {
    if (NameCard* card = at(id)) {
        card->bid = bid;
        card->ask = ask;
        card->last = last;
    }
}

void NameBook::set_factor(uint32_t id, int64_t value) noexcept {
    if (NameCard* card = at(id)) card->factor = value;
}

bool NameBook::set_prop(uint32_t id, const char* key, const char* val) noexcept {
    NameCard* card = at(id);
    if (!card || !key || !key[0] || !val) return false;
    for (uint8_t i = 0; i < card->n_prop; ++i) {
        if (std::strcmp(card->prop_key[i], key) == 0) {
            std::strncpy(card->prop_val[i], val, 31);
            card->prop_val[i][31] = 0;
            return true;
        }
    }
    if (card->n_prop >= 4) return false;
    std::strncpy(card->prop_key[card->n_prop], key, 15);
    card->prop_key[card->n_prop][15] = 0;
    std::strncpy(card->prop_val[card->n_prop], val, 31);
    card->prop_val[card->n_prop][31] = 0;
    ++card->n_prop;
    return true;
}

const char* NameBook::prop(uint32_t id, const char* key) const noexcept {
    const NameCard* card = at(id);
    if (!card || !key) return nullptr;
    for (uint8_t i = 0; i < card->n_prop; ++i) {
        if (std::strcmp(card->prop_key[i], key) == 0) return card->prop_val[i];
    }
    return nullptr;
}

int NameBook::list_kind(uint8_t kind, uint32_t* out, int cap) const noexcept {
    int n = 0;
    for (uint32_t i = 0; i < used_ && n < cap; ++i) {
        if (card_[i].used && card_[i].kind == kind) {
            if (out) out[n] = card_[i].id;
            ++n;
        }
    }
    return n;
}

int NameBook::list_suffix(const char* suffix, uint32_t* out, int cap) const noexcept {
    int n = 0;
    for (uint32_t i = 0; i < used_ && n < cap; ++i) {
        if (card_[i].used && ends_with(card_[i].symbol, suffix)) {
            if (out) out[n] = card_[i].id;
            ++n;
        }
    }
    return n;
}

}  // namespace pitlane
