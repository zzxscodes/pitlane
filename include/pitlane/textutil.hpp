#pragma once

#include "pitlane/lags.hpp"

#include <cstddef>
#include <cstdint>

namespace pitlane {

struct CellGrid {
    static constexpr int kRows = 32;
    static constexpr int kCols = 12;
    static constexpr int kLen = 48;
    char cell[kRows][kCols][kLen]{};
    int rows{0};
    int cols{0};
};

struct FlatMap {
    static constexpr int kPairs = 32;
    static constexpr int kKey = 32;
    static constexpr int kVal = 64;
    char key[kPairs][kKey]{};
    char val[kPairs][kVal]{};
    int n{0};
};

void trim_inplace(char* s) noexcept;
bool contains_ci(const char* hay, const char* needle) noexcept;
int split_text(const char* s, char sep, char out[][64], int cap, bool drop_empty) noexcept;
bool join_text(const char* const* parts, int n, char sep, char* out, int cap) noexcept;
bool pad_left(const char* s, char fill, int width, char* out, int cap) noexcept;
uint32_t hash32(const void* data, std::size_t n) noexcept;
uint32_t hash_str(const char* s) noexcept;
bool read_lines(const char* path, char lines[][160], int cap, int* n) noexcept;
bool write_text(const char* path, const char* text) noexcept;
bool gbk_to_utf8(const char* in, std::size_t in_n, char* out, std::size_t out_cap) noexcept;
bool parse_flat_json(const char* text, FlatMap* out) noexcept;
bool write_flat_json(const FlatMap& in, char* out, int cap) noexcept;
bool parse_csv(const char* text, CellGrid* out) noexcept;
bool load_lag_csv(const char* text, LagTable* table) noexcept;
const char* flat_get(const FlatMap& map, const char* key) noexcept;

}  // namespace pitlane
