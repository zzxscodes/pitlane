#pragma once

#include "pitlane/desk/types.hpp"

#include <cstdint>

namespace pitlane {

enum class Product : int8_t {
    Unknown = -1,
    Stock = 0,
    Future = 1,
    Bond = 2,
    Option = 3,
    Fund = 4,
    Tech = 5,
    Index = 6,
    Repo = 7
};

enum class Off : int8_t { Open = 0, Close = 1, CloseToday = 2, CloseYday = 3 };

enum class BookDir : int8_t { Long = 0, Short = 1 };

enum class PxMode : int8_t { Unknown = -1, Limit = 0, Market = 1 };

enum class CloseStyle : int8_t { Plain = 0, SplitYday = 1 };

struct GateForm {
    char symbol[32]{};
    int wire_side{-1};  // 0 buy, 1 sell, 2 margin buy, 3 short sell, 4 sell-to-repay, 5 buy-to-cover, 6 create, 7 redeem
    uint8_t side{0};    // used when wire_side < 0: 1 buy, 2 sell
    int8_t off{0};
    int8_t px_mode{-1};
    int64_t px{0};
    int64_t qty{0};
};

struct GateResult {
    Why why{Why::None};
    Product product{Product::Unknown};
    Off off{Off::Open};
    BookDir dir{BookDir::Long};
    PxMode mode{PxMode::Unknown};
    int64_t px{0};
    int64_t qty{0};
};

bool cash_product(Product product) noexcept;
Product classify_symbol(const char* symbol) noexcept;
bool product_root(const char* symbol, char* out, int cap) noexcept;
CloseStyle close_style_of(const char* symbol) noexcept;
int wire_to_dir(int wire_side) noexcept;
GateResult normalize_gate(const GateForm& in) noexcept;

bool final_phase(uint8_t phase) noexcept;
bool hanging_phase(uint8_t phase) noexcept;
bool running_phase(uint8_t phase) noexcept;
const char* phase_text(uint8_t phase) noexcept;
const char* why_text(Why why) noexcept;

int exchange_id(const char* code) noexcept;
const char* exchange_text(int id) noexcept;
const char* holder_for(const char* symbol, const char* sh_holder, const char* sz_holder) noexcept;

}  // namespace pitlane
