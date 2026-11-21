#pragma once

// Instrument reference data. The web console uses it to group the watch list
// by product class and to size clearing fees, margin, and multipliers.
//
// Format: instrument = <name> <class> <ccy> <tick> <mult> <margin_ppm> <fee_ppm>
//                      [min_fee=<n>] [stamp_ppm=<n>] [close_fee_ppm=<n>]
//                      [close_today_fee_ppm=<n>] [fx_ppm=<n>] [short=1] [gw=<gateway>]
//                      [lot=<n>] [min_qty=<n>] [max_qty=<n>]
//                      [tickb=<lo>:<hi>:<step>,<lo>:<hi>:<step>,...]
//
// `lot` is the whole lot for an order, `min_qty` and `max_qty` bound one
// order, and `max_qty=0` means no cap. A `tickb` band applies from `lo` up to
// `hi`, and `hi=0` makes the last band open-ended; bands are sorted by `lo` and
// invalid entries are dropped. With no valid band the scalar `tick` is used.

#include "pitlane/book/steps.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pitlane {

enum InstrClass : uint8_t {
    kInstrAStock = 0,
    kInstrHkStock = 1,
    kInstrFuture = 2,
    kInstrOption = 3,
    kInstrUsStock = 4,
    kInstrEtf = 5,
    kInstrBond = 6,
    kInstrOther = 7,
    kInstrClassCount = 8,
};

inline const char* instr_class_name(uint8_t klass) noexcept {
    switch (klass) {
        case kInstrAStock: return "astock";
        case kInstrHkStock: return "hkstock";
        case kInstrFuture: return "future";
        case kInstrOption: return "option";
        case kInstrUsStock: return "usstock";
        case kInstrEtf: return "etf";
        case kInstrBond: return "bond";
        default: return "other";
    }
}

inline bool instr_class_from_name(const char* s, uint8_t& klass) noexcept {
    if (!s) return false;
    for (uint8_t k = 0; k < kInstrClassCount; ++k) {
        if (std::strcmp(s, instr_class_name(k)) == 0) {
            klass = k;
            return true;
        }
    }
    return false;
}

// Stocks and funds cannot open a short in this example; futures and options can.
inline bool instr_allows_short(uint8_t klass) noexcept { return klass == kInstrFuture || klass == kInstrOption; }

struct InstrSpec {
    uint32_t name{0};
    uint8_t klass{kInstrAStock};
    char ccy[8]{};
    int64_t tick{1};
    int64_t mult{1};
    int64_t margin_ppm{0};
    int64_t fee_ppm{0};
    int64_t close_fee_ppm{-1};        // closing yesterday, -1 uses fee_ppm
    int64_t close_today_fee_ppm{-1};  // closing today, -1 uses the close fee
    int64_t fx_ppm{0};                // base units per currency unit, 0 = unset
    int64_t min_fee{0};
    int64_t stamp_ppm{0};
    bool allow_short{false};
    char gw[24]{};
    int64_t lot{1};       // whole order lot
    int64_t min_qty{1};   // smallest single order
    int64_t max_qty{0};   // largest single order, 0 = no cap
    PxBand band[8]{};     // tick bands, ascending by lo
    int band_n{0};

    int64_t px_step(int64_t px) const noexcept {
        if (band_n > 0) {
            const PxBand* b = band_for(band, band_n, px);
            if (b && b->step > 0) return b->step;
        }
        return tick;
    }

    bool qty_ok(int64_t qty) const noexcept {
        if (qty <= 0 || lot <= 0 || qty % lot != 0) return false;
        const int64_t lo = min_qty > 0 ? min_qty : lot;
        if (qty < lo) return false;
        if (max_qty > 0 && qty > max_qty) return false;
        return true;
    }
};

class InstrumentRegistry {
public:
    static constexpr int kMaxInstruments = 1024;

    bool load(const char* path) noexcept {
        n_ = 0;
        std::FILE* f = std::fopen(path, "r");
        if (!f) return false;
        char line[320];
        while (std::fgets(line, sizeof line, f)) {
            char* p = line;
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p || *p == '#' || *p == '\n' || *p == '\r') continue;
            char* eq = std::strchr(p, '=');
            if (!eq) continue;
            *eq = 0;
            char* key_end = eq;
            while (key_end > p && (key_end[-1] == ' ' || key_end[-1] == '\t')) --key_end;
            *key_end = 0;
            if (std::strcmp(p, "instrument") != 0) continue;
            unsigned name = 0;
            char klass[12]{}, ccy[8]{};
            long long tick = 1, mult = 1, margin = 0, fee = 0;
            int used = 0;
            if (std::sscanf(eq + 1, " %u %11s %7s %lld %lld %lld %lld %n", &name, klass, ccy, &tick, &mult, &margin,
                            &fee, &used) < 7)
                continue;
            uint8_t k = kInstrAStock;
            if (!instr_class_from_name(klass, k)) continue;
            if (name == 0 || tick <= 0 || mult <= 0 || margin < 0 || fee < 0) continue;
            if (n_ >= kMaxInstruments || find(name) >= 0) continue;
            InstrSpec s{};
            s.name = static_cast<uint32_t>(name);
            s.klass = k;
            std::snprintf(s.ccy, sizeof s.ccy, "%s", ccy);
            s.tick = tick;
            s.mult = mult;
            s.margin_ppm = margin;
            s.fee_ppm = fee;
            s.allow_short = instr_allows_short(k);
            const char* rest = eq + 1 + used;
            s.min_fee = opt_i64(rest, "min_fee=", 0);
            s.stamp_ppm = opt_i64(rest, "stamp_ppm=", 0);
            s.close_fee_ppm = opt_i64(rest, "close_fee_ppm=", -1);
            s.close_today_fee_ppm = opt_i64(rest, "close_today_fee_ppm=", -1);
            s.fx_ppm = opt_i64(rest, "fx_ppm=", 0);
            s.lot = opt_i64(rest, "lot=", 1);
            s.min_qty = opt_i64(rest, "min_qty=", s.lot);
            s.max_qty = opt_i64(rest, "max_qty=", 0);
            if (s.lot <= 0 || s.min_qty < 0 || s.max_qty < 0) continue;
            if (s.max_qty > 0 && s.max_qty < (s.min_qty > 0 ? s.min_qty : s.lot)) continue;
            if (s.min_qty == 0) s.min_qty = s.lot;
            s.band_n = parse_bands(rest, s.band, 8);
            const char* short_opt = std::strstr(rest, "short=");
            if (short_opt) s.allow_short = std::strtol(short_opt + 6, nullptr, 10) != 0;
            const char* gw = std::strstr(rest, "gw=");
            if (gw) {
                gw += 3;
                std::size_t i = 0;
                while (gw[i] && gw[i] != ' ' && gw[i] != '\t' && gw[i] != '\r' && gw[i] != '\n' &&
                       i < sizeof s.gw - 1) {
                    s.gw[i] = gw[i];
                    ++i;
                }
                s.gw[i] = 0;
            }
            specs_[n_++] = s;
        }
        std::fclose(f);
        return true;
    }

    int count() const noexcept { return n_; }
    const InstrSpec& spec(int i) const noexcept { return specs_[i]; }

    int find(uint32_t name) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (specs_[i].name == name) return i;
        }
        return -1;
    }

    const InstrSpec* by_name(uint32_t name) const noexcept {
        const int i = find(name);
        return i < 0 ? nullptr : &specs_[i];
    }

    int64_t px_step(uint32_t name, int64_t px) const noexcept {
        const InstrSpec* s = by_name(name);
        return s ? s->px_step(px) : 0;
    }

    bool qty_ok(uint32_t name, int64_t qty) const noexcept {
        const InstrSpec* s = by_name(name);
        return s && s->qty_ok(qty);
    }

private:
    static int parse_bands(const char* rest, PxBand* out, int cap) noexcept {
        const char* p = std::strstr(rest, "tickb=");
        if (!p) return 0;
        p += 6;
        int n = 0;
        while (n < cap && *p) {
            long long lo = 0;
            long long hi = 0;
            long long step = 0;
            int used = 0;
            if (std::sscanf(p, " %lld:%lld:%lld%n", &lo, &hi, &step, &used) < 3) break;
            if (lo >= 0 && hi >= 0 && step > 0 && (hi == 0 || hi > lo)) {
                if (n == 0 || lo > out[n - 1].lo) {
                    out[n].lo = lo;
                    out[n].hi = hi == 0 ? INT64_MAX : hi;
                    out[n].step = step;
                    ++n;
                }
            }
            if (used <= 0) break;
            p += used;
            if (*p == ',') ++p;
            else break;
        }
        return n;
    }

    static int64_t opt_i64(const char* rest, const char* key, int64_t dflt) noexcept {
        const char* p = std::strstr(rest, key);
        if (!p) return dflt;
        return std::strtoll(p + std::strlen(key), nullptr, 10);
    }

    InstrSpec specs_[kMaxInstruments]{};
    int n_{0};
};

}  // namespace pitlane
