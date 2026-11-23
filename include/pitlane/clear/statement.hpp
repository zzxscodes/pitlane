#pragma once

// External statement import and three-way reconciliation.
//
// A custodian or broker statement is a small CSV: one row per instrument with
// quantity and money columns. `recon_three_way` lines the internal book up
// against statement A and statement B and reports the pairwise differences, so
// a break can be attributed to the book or to one of the two statements.
//
// Loading allocates inside std::FILE parsing only; evaluation is fixed
// capacity and allocation-free. The console calls both from its own thread on
// a clearing snapshot, never from the trading thread.

#include "pitlane/clear/clearing.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pitlane {

inline constexpr int kStmtMax = 32;
inline constexpr int64_t kReconMissing = INT64_MIN;

struct StmtRow {
    uint32_t name{0};
    int64_t qty{0};
    int64_t equity{0};
    int64_t realized{0};
    int64_t unrealized{0};
};

// One imported statement. Names are instrument ids; a name that appears twice
// keeps the last row.
struct StmtBook {
    static constexpr int kMax = kStmtMax;

    bool load_csv(const char* path) noexcept {
        n_ = 0;
        if (!path || !path[0]) return false;
        std::FILE* f = std::fopen(path, "r");
        if (!f) return false;
        char line[512];
        while (n_ < kMax && std::fgets(line, sizeof line, f)) {
            char* p = line;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p == '#' || *p == '\0' || *p == '\r' || *p == '\n') continue;
            char* end = nullptr;
            const long name = std::strtol(p, &end, 10);
            if (end == p || name <= 0) continue;  // header or malformed
            if (*end != ',') continue;
            int64_t v[4] = {0, 0, 0, 0};
            char* q = end + 1;
            bool ok = true;
            for (int i = 0; i < 4; ++i) {
                if (*q == '\0' || *q == '\r' || *q == '\n') break;
                const long long x = std::strtoll(q, &end, 10);
                if (end == q) {
                    ok = false;
                    break;
                }
                v[i] = static_cast<int64_t>(x);
                q = end;
                if (*q == ',') ++q;
                else break;
            }
            if (!ok) continue;
            StmtRow r{};
            r.name = static_cast<uint32_t>(name);
            r.qty = v[0];
            r.equity = v[1];
            r.realized = v[2];
            r.unrealized = v[3];
            StmtRow* slot = find(r.name);
            if (slot) {
                *slot = r;
            } else {
                rows_[n_++] = r;
            }
        }
        std::fclose(f);
        return true;
    }

    int count() const noexcept { return n_; }

    const StmtRow* at(int i) const noexcept {
        return i >= 0 && i < n_ ? &rows_[i] : nullptr;
    }

    StmtRow* find(uint32_t name) noexcept {
        for (int i = 0; i < n_; ++i) {
            if (rows_[i].name == name) return &rows_[i];
        }
        return nullptr;
    }

    const StmtRow* find(uint32_t name) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (rows_[i].name == name) return &rows_[i];
        }
        return nullptr;
    }

private:
    StmtRow rows_[kMax]{};
    int n_{0};
};

// Sources are indexed 0 internal, 1 statement A, 2 statement B.
struct ReconLine {
    uint32_t name{0};
    uint8_t present{0};  // bit0 internal, bit1 A, bit2 B
    int64_t qty[3]{};
    int64_t equity[3]{};
    int64_t realized[3]{};
    int64_t unrealized[3]{};
    // Pairwise differences: 0 internal-A, 1 internal-B, 2 A-B.
    int64_t dqty[3]{};
    int64_t dequity[3]{};
    bool qty_bad{false};
    bool money_bad{false};
};

struct ReconOut {
    int count{0};
    int qty_breaks{0};
    int money_breaks{0};
    ReconLine line[kStmtMax]{};
};

// Compares the clearing snapshot with up to two statements. A tolerance is
// inclusive: a difference equal to it is not a break. Missing rows are absent,
// never zero.
inline bool recon_three_way(const ClearingReport& rep, const StmtBook* a, const StmtBook* b, int64_t qty_tol,
                            int64_t money_tol, ReconOut& out) noexcept {
    out = ReconOut{};
    if (qty_tol < 0) qty_tol = 0;
    if (money_tol < 0) money_tol = 0;

    auto pick = [&out](uint32_t name) -> ReconLine* {
        for (int i = 0; i < out.count; ++i) {
            if (out.line[i].name == name) return &out.line[i];
        }
        if (out.count >= kStmtMax) return nullptr;
        ReconLine& l = out.line[out.count++];
        l = ReconLine{};
        l.name = name;
        l.qty[0] = l.qty[1] = l.qty[2] = kReconMissing;
        l.equity[0] = l.equity[1] = l.equity[2] = kReconMissing;
        l.realized[0] = l.realized[1] = l.realized[2] = kReconMissing;
        l.unrealized[0] = l.unrealized[1] = l.unrealized[2] = kReconMissing;
        l.dqty[0] = l.dqty[1] = l.dqty[2] = kReconMissing;
        l.dequity[0] = l.dequity[1] = l.dequity[2] = kReconMissing;
        return &l;
    };

    for (int i = 0; i < rep.rows; ++i) {
        const ClearingRow& row = rep.row[i];
        ReconLine* l = pick(row.name);
        if (!l) break;
        l->present |= 1u;
        l->qty[0] = row.long_qty - row.short_qty;
        l->realized[0] = row.realized;
        l->unrealized[0] = row.unrealized;
        l->equity[0] = row.realized + row.unrealized - row.fees;
    }
    if (a) {
        for (int i = 0; i < a->count(); ++i) {
            const StmtRow* r = a->at(i);
            if (!r) continue;
            ReconLine* l = pick(r->name);
            if (!l) break;
            l->present |= 2u;
            l->qty[1] = r->qty;
            l->equity[1] = r->equity;
            l->realized[1] = r->realized;
            l->unrealized[1] = r->unrealized;
        }
    }
    if (b) {
        for (int i = 0; i < b->count(); ++i) {
            const StmtRow* r = b->at(i);
            if (!r) continue;
            ReconLine* l = pick(r->name);
            if (!l) break;
            l->present |= 4u;
            l->qty[2] = r->qty;
            l->equity[2] = r->equity;
            l->realized[2] = r->realized;
            l->unrealized[2] = r->unrealized;
        }
    }

    for (int i = 0; i < out.count; ++i) {
        ReconLine& l = out.line[i];
        const int pa[3][2] = {{0, 1}, {0, 2}, {1, 2}};
        for (int p = 0; p < 3; ++p) {
            const int x = pa[p][0];
            const int y = pa[p][1];
            if (!(l.present & (1u << x)) || !(l.present & (1u << y))) continue;
            l.dqty[p] = l.qty[x] - l.qty[y];
            l.dequity[p] = l.equity[x] - l.equity[y];
            const int64_t money = l.dequity[p];
            const int64_t dr = l.realized[x] - l.realized[y];
            const int64_t du = l.unrealized[x] - l.unrealized[y];
            if (l.dqty[p] > qty_tol || l.dqty[p] < -qty_tol) l.qty_bad = true;
            if (money > money_tol || money < -money_tol || dr > money_tol || dr < -money_tol ||
                du > money_tol || du < -money_tol) {
                l.money_bad = true;
            }
        }
        if (l.qty_bad) ++out.qty_breaks;
        if (l.money_bad) ++out.money_breaks;
    }
    return true;
}

// Writes the three-way table. Money and quantity columns are in their native
// units; a missing source prints an empty cell.
inline bool recon_csv(const ReconOut& out, const char* path) noexcept {
    if (!path || !path[0]) return false;
    std::FILE* f = std::fopen(path, "w");
    if (!f) return false;
    std::fprintf(f,
                 "name,present,qty_i,qty_a,qty_b,dqty_ia,dqty_ib,dqty_ab,"
                 "equity_i,equity_a,equity_b,deq_ia,deq_ib,deq_ab,qty_bad,money_bad\n");
    for (int i = 0; i < out.count; ++i) {
        const ReconLine& l = out.line[i];
        std::fprintf(f, "%u,%u,", static_cast<unsigned>(l.name), static_cast<unsigned>(l.present));
        for (int s = 0; s < 3; ++s) {
            if (l.qty[s] == kReconMissing) std::fputc(',', f);
            else std::fprintf(f, "%lld,", static_cast<long long>(l.qty[s]));
        }
        for (int p = 0; p < 3; ++p) {
            if (l.dqty[p] == kReconMissing) std::fputc(',', f);
            else std::fprintf(f, "%lld,", static_cast<long long>(l.dqty[p]));
        }
        for (int s = 0; s < 3; ++s) {
            if (l.equity[s] == kReconMissing) std::fputc(',', f);
            else std::fprintf(f, "%lld,", static_cast<long long>(l.equity[s]));
        }
        for (int p = 0; p < 3; ++p) {
            if (l.dequity[p] == kReconMissing) std::fputc(',', f);
            else std::fprintf(f, "%lld,", static_cast<long long>(l.dequity[p]));
        }
        std::fprintf(f, "%d,%d\n", l.qty_bad ? 1 : 0, l.money_bad ? 1 : 0);
    }
    std::fclose(f);
    return true;
}

}  // namespace pitlane
