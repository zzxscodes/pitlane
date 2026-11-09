#pragma once

#include <cstdint>

namespace pitlane {

struct NameCard {
    uint32_t id{0};
    char symbol[32]{};
    uint8_t kind{0};
    int open_min{0};
    int close_min{0};
    int64_t bid{0};
    int64_t ask{0};
    int64_t last{0};
    int64_t factor{0};
    uint16_t related[8]{};
    uint16_t underlying[4]{};
    uint16_t derived[8]{};
    uint16_t track_underlying[4]{};
    uint16_t track_derived[8]{};
    uint8_t n_related{0};
    uint8_t n_underlying{0};
    uint8_t n_derived{0};
    uint8_t n_track_underlying{0};
    uint8_t n_track_derived{0};
    char prop_key[4][16]{};
    char prop_val[4][32]{};
    uint8_t n_prop{0};
    uint8_t used{0};
};

class NameBook {
public:
    static constexpr uint32_t kCap = 256;

    bool insert(const char* symbol, uint8_t kind, uint32_t* id) noexcept;
    NameCard* find(const char* symbol) noexcept;
    const NameCard* find(const char* symbol) const noexcept;
    NameCard* at(uint32_t id) noexcept;
    const NameCard* at(uint32_t id) const noexcept;
    bool link(uint32_t from, uint32_t to, int kind) noexcept;
    bool same_symbol(uint32_t id, const char* symbol) const noexcept;
    void set_session(uint32_t id, int open_min, int close_min) noexcept;
    bool in_session(uint32_t id, int minute) const noexcept;
    void set_quote(uint32_t id, int64_t bid, int64_t ask, int64_t last) noexcept;
    void set_factor(uint32_t id, int64_t value) noexcept;
    bool set_prop(uint32_t id, const char* key, const char* val) noexcept;
    const char* prop(uint32_t id, const char* key) const noexcept;
    int list_kind(uint8_t kind, uint32_t* out, int cap) const noexcept;
    int list_suffix(const char* suffix, uint32_t* out, int cap) const noexcept;
    uint32_t size() const noexcept { return used_; }

private:
    NameCard card_[kCap]{};
    uint32_t used_{0};
};

}  // namespace pitlane
