#include "pitlane/util/textutil.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iconv.h>

namespace pitlane {
namespace {

void copy_cap(char* dst, int cap, const char* src, int n) noexcept {
    if (cap <= 0) return;
    if (n >= cap) n = cap - 1;
    if (n < 0) n = 0;
    for (int i = 0; i < n; ++i) dst[i] = src[i];
    dst[n] = 0;
}

bool bad_token(const char* s) noexcept {
    if (!s) return true;
    for (const char* p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') return true;
    }
    return false;
}

}  // namespace

void trim_inplace(char* s) noexcept {
    if (!s) return;
    char* b = s;
    while (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n') ++b;
    char* e = b;
    while (*e) ++e;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
    const int n = static_cast<int>(e - b);
    for (int i = 0; i < n; ++i) s[i] = b[i];
    s[n] = 0;
}

bool contains_ci(const char* hay, const char* needle) noexcept {
    if (!hay || !needle || !*needle) return false;
    for (const char* h = hay; *h; ++h) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b) {
            char ca = *a;
            char cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) break;
            ++a;
            ++b;
        }
        if (*b == 0) return true;
    }
    return false;
}

int split_text(const char* s, char sep, char out[][64], int cap, bool drop_empty) noexcept {
    if (!s || !out || cap <= 0) return 0;
    int n = 0;
    const char* p = s;
    while (*p && n < cap) {
        const char* start = p;
        while (*p && *p != sep) ++p;
        const int len = static_cast<int>(p - start);
        if (!(drop_empty && len == 0)) {
            copy_cap(out[n], 64, start, len);
            ++n;
        }
        if (*p == sep) ++p;
    }
    return n;
}

bool join_text(const char* const* parts, int n, char sep, char* out, int cap) noexcept {
    if (!out || cap <= 0) return false;
    int w = 0;
    out[0] = 0;
    if (!parts || n <= 0) return true;
    for (int i = 0; i < n; ++i) {
        if (i > 0) {
            if (w + 1 >= cap) return false;
            out[w++] = sep;
        }
        const char* p = parts[i] ? parts[i] : "";
        while (*p) {
            if (w + 1 >= cap) return false;
            out[w++] = *p++;
        }
    }
    out[w] = 0;
    return true;
}

bool pad_left(const char* s, char fill, int width, char* out, int cap) noexcept {
    if (!out || cap <= 0 || width < 0 || width >= cap) return false;
    const int len = s ? static_cast<int>(std::strlen(s)) : 0;
    int pad = width - len;
    if (pad < 0) pad = 0;
    if (pad + len >= cap) return false;
    for (int i = 0; i < pad; ++i) out[i] = fill;
    for (int i = 0; i < len; ++i) out[pad + i] = s[i];
    out[pad + len] = 0;
    return true;
}

uint32_t hash32(const void* data, std::size_t n) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    uint32_t h = 2166136261u;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

uint32_t hash_str(const char* s) noexcept {
    if (!s) return hash32("", 0);
    return hash32(s, std::strlen(s));
}

bool read_lines(const char* path, char lines[][160], int cap, int* n) noexcept {
    if (n) *n = 0;
    if (!path || !lines || cap <= 0) return false;
    FILE* fp = std::fopen(path, "r");
    if (!fp) return false;
    int count = 0;
    char buf[256];
    bool ok = true;
    while (std::fgets(buf, sizeof buf, fp)) {
        const std::size_t len = std::strlen(buf);
        if (len == sizeof(buf) - 1 && buf[len - 1] != '\n') {
            ok = false;
            break;
        }
        if (count >= cap) {
            ok = false;
            break;
        }
        copy_cap(lines[count], 160, buf, static_cast<int>(len));
        trim_inplace(lines[count]);
        ++count;
    }
    std::fclose(fp);
    if (n) *n = count;
    return ok;
}

bool write_text(const char* path, const char* text) noexcept {
    if (!path || !text) return false;
    FILE* fp = std::fopen(path, "w");
    if (!fp) return false;
    const std::size_t n = std::strlen(text);
    const bool ok = std::fwrite(text, 1, n, fp) == n;
    std::fclose(fp);
    return ok;
}

bool gbk_to_utf8(const char* in, std::size_t in_n, char* out, std::size_t out_cap) noexcept {
    if (!in || !out || out_cap < 2) return false;
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == reinterpret_cast<iconv_t>(-1)) return false;
    char* src = const_cast<char*>(in);
    std::size_t left = in_n;
    char* dst = out;
    std::size_t room = out_cap - 1;
    const std::size_t rc = iconv(cd, &src, &left, &dst, &room);
    iconv_close(cd);
    if (rc == static_cast<std::size_t>(-1)) return false;
    *dst = 0;
    return true;
}

bool parse_flat_json(const char* text, FlatMap* out) noexcept {
    if (!text || !out) return false;
    out->n = 0;
    const char* p = text;
    while (*p == ' ' || *p == '\n' || *p == '\t') ++p;
    if (*p != '{') return false;
    ++p;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == ',') ++p;
        if (*p == '}') return true;
        if (*p != '"') return false;
        ++p;
        const char* ks = p;
        while (*p && *p != '"') ++p;
        if (*p != '"') return false;
        const int klen = static_cast<int>(p - ks);
        ++p;
        while (*p == ' ') ++p;
        if (*p != ':') return false;
        ++p;
        while (*p == ' ') ++p;
        if (*p != '"') return false;
        ++p;
        const char* vs = p;
        while (*p && *p != '"') ++p;
        if (*p != '"') return false;
        const int vlen = static_cast<int>(p - vs);
        ++p;
        if (out->n >= FlatMap::kPairs) return false;
        copy_cap(out->key[out->n], FlatMap::kKey, ks, klen);
        copy_cap(out->val[out->n], FlatMap::kVal, vs, vlen);
        ++out->n;
    }
    return false;
}

bool write_flat_json(const FlatMap& in, char* out, int cap) noexcept {
    if (!out || cap < 3) return false;
    int w = 0;
    out[w++] = '{';
    for (int i = 0; i < in.n; ++i) {
        if (bad_token(in.key[i]) || bad_token(in.val[i])) return false;
        if (i > 0) {
            if (w + 1 >= cap) return false;
            out[w++] = ',';
        }
        const int kn = static_cast<int>(std::strlen(in.key[i]));
        const int vn = static_cast<int>(std::strlen(in.val[i]));
        if (w + kn + vn + 5 >= cap) return false;
        out[w++] = '"';
        for (int c = 0; c < kn; ++c) out[w++] = in.key[i][c];
        out[w++] = '"';
        out[w++] = ':';
        out[w++] = '"';
        for (int c = 0; c < vn; ++c) out[w++] = in.val[i][c];
        out[w++] = '"';
    }
    if (w + 1 >= cap) return false;
    out[w++] = '}';
    out[w] = 0;
    return true;
}

bool parse_csv(const char* text, CellGrid* out) noexcept {
    if (!text || !out) return false;
    out->rows = 0;
    out->cols = 0;
    const char* p = text;
    int row = 0;
    while (*p && row < CellGrid::kRows) {
        int col = 0;
        while (*p && *p != '\n' && col < CellGrid::kCols) {
            if (*p == '"') {
                ++p;
                const char* s = p;
                while (*p && *p != '"') ++p;
                copy_cap(out->cell[row][col], CellGrid::kLen, s, static_cast<int>(p - s));
                if (*p == '"') ++p;
            } else {
                const char* s = p;
                while (*p && *p != ',' && *p != '\n') ++p;
                int n = static_cast<int>(p - s);
                if (n > 0 && s[n - 1] == '\r') --n;
                copy_cap(out->cell[row][col], CellGrid::kLen, s, n);
            }
            ++col;
            if (*p == ',') ++p;
        }
        if (col > out->cols) out->cols = col;
        ++row;
        if (*p == '\n') ++p;
    }
    out->rows = row;
    return *p == 0;
}

bool load_lag_csv(const char* text, LagTable* table) noexcept {
    if (!table) return false;
    CellGrid grid;
    if (!parse_csv(text, &grid)) return false;
    bool any = false;
    for (int r = 0; r < grid.rows; ++r) {
        if (grid.cell[r][0][0] == 0) continue;
        char* end = nullptr;
        const long band = std::strtol(grid.cell[r][0], &end, 10);
        if (end == grid.cell[r][0] || band < 0 || band > 8) continue;
        table->go_us[band] = std::strtoll(grid.cell[r][1], nullptr, 10);
        table->pull_us[band] = std::strtoll(grid.cell[r][2], nullptr, 10);
        any = true;
    }
    return any;
}

const char* flat_get(const FlatMap& map, const char* key) noexcept {
    if (!key) return nullptr;
    for (int i = 0; i < map.n; ++i) {
        if (std::strcmp(map.key[i], key) == 0) return map.val[i];
    }
    return nullptr;
}

}  // namespace pitlane
