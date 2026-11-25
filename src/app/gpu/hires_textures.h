// High-resolution textures (an experimental enhancement, off unless a pack
// is given): the game's textures, upscaled on the player's own computer from
// their own ROM set (tools/m2texcap captures them, scripts/hires_textures.py
// upscales them with an AI upscaler the player provides and writes the pack).
// A pack is never shared nor committed: it is made of the game's textures.
//
// A Model 2 texture is a rectangle of 4-bit texels in one of the two texture
// RAM sheets (2048 x 1024 each), turned into a colour per pixel by a luma
// table and the polygon's palette colour (m2.hlsl). An upscaled texture keeps
// that: it holds tones (0..255 for texels 0..15, times 17), so the game's luma
// and colour still apply. A pack entry is the rectangle where its texture was
// seen and the hash of its texels; in the game it is used only while the
// texture RAM holds those very texels there (checked when texture RAM changes).
//
// A cut-out texture (texel 15 transparent, drawn by the game's renderer 3)
// has its opacity upscaled as well: its entry is flagged, and its data is
// the tones (the transparent texels filled from their neighbours before
// upscaling) then as many opacities (0 transparent .. 255 opaque).
//
// Pack file (little endian): "M2HT", u32 version (1), u32 entries, u32 scale;
// per entry: u32 sheet, x, y, w, h (texels), u32 flags (kCutout), u64 hash,
// u64 data offset; then the data: per entry (w * scale) x (h * scale) tones,
// rows top down, and for a cut-out one as many opacities after them.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace app::hires {

inline constexpr uint32_t kSheetWords = 0x80000; // one texture RAM sheet, in u32 (VideoMem tex0, tex1)
inline constexpr uint32_t kCellsPerSheet = 64 * 32; // 32 x 32-texel cells of a 2048 x 1024 sheet
inline constexpr uint32_t kCutout = 1;               // Entry::flags: a cut-out texture, its opacity after its tones
inline constexpr uint32_t kIndexCutout = 1u << 15;   // in the index's w | h << 16 word (m2.hlsl hires_index)
inline constexpr uint32_t kSlots = 4;                // index slots per cell (m2.hlsl kHiresSlots)

// m2.hlsl get_texel: the texel at (x, y) of the rectangle at (base_x, base_y).
inline uint32_t texel(const uint32_t *sheet_words, uint32_t base_x, uint32_t base_y, int x, int y) {
    int x2 = int(base_x) + x, y2 = int(base_y) + y;
    if (x2 >= 1024) x2 -= 1024, y2 ^= 1024; // sheets are mapped 2048x1024, stored 1024x2048
    const uint32_t offset = uint32_t((y2 / 2) * 512 + (x2 / 2));
    uint32_t t = sheet_words[(offset >> 1) & 0x7ffffu];
    if (offset & 1u) t >>= 16;
    if ((y & 1) == 0) t >>= 8;
    if ((x & 1) == 0) t >>= 4;
    return t & 0x0fu;
}

// FNV-1a over the size and every texel of a rectangle.
inline uint64_t hash(const uint32_t *sheet_words, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint64_t v = 1469598103934665603ull;
    auto mix = [&v](uint32_t b) { v = (v ^ b) * 1099511628211ull; };
    mix(w), mix(h);
    for (uint32_t j = 0; j < h; ++j)
        for (uint32_t i = 0; i < w; ++i) mix(texel(sheet_words, x, y, int(i), int(j)));
    return v;
}

// A polygon's level-0 texture rectangle, from its texture header (as
// m2gpu::pack_polygon_material decodes it).
struct Rect {
    uint32_t sheet, x, y, w, h;
};
inline Rect rect_of(uint16_t th0, uint16_t th2) {
    return {(th2 >> 12) & 1u, 32u * (th2 & 0x3fu), 32u * ((th2 >> 6) & 0x1fu), 32u << (th0 & 7u),
            32u << ((th0 >> 3) & 7u)};
}

struct Entry {
    uint32_t sheet, x, y, w, h, flags;
    uint64_t hash, offset;
};

struct Pack {
    uint32_t scale = 0;
    std::vector<Entry> entries;
    std::vector<uint8_t> data;
    std::string error;

    bool load(const std::string &path) {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) return error = "cannot open " + path, false;
        auto rd = [&](void *p, size_t n) { return std::fread(p, 1, n, f) == n; };
        char magic[4];
        uint32_t version = 0, count = 0;
        bool ok = rd(magic, 4) && std::memcmp(magic, "M2HT", 4) == 0 && rd(&version, 4) && version == 1 &&
                  rd(&count, 4) && rd(&scale, 4) && scale >= 1 && scale <= 8 && count < 100000;
        entries.resize(ok ? count : 0);
        uint64_t need = 0;
        for (uint32_t i = 0; ok && i < count; ++i) {
            Entry &e = entries[i];
            ok = rd(&e.sheet, 4) && rd(&e.x, 4) && rd(&e.y, 4) && rd(&e.w, 4) && rd(&e.h, 4) && rd(&e.flags, 4) &&
                 rd(&e.hash, 8) && rd(&e.offset, 8) && e.sheet < 2 && e.w >= 32 && e.w <= 256 && e.h >= 32 &&
                 e.h <= 256 && (e.w & (e.w - 1)) == 0 && (e.h & (e.h - 1)) == 0 && e.x % 32 == 0 &&
                 e.y % 32 == 0 && e.x + e.w <= 2048 && e.y + e.h <= 1024 &&
                 e.flags <= kCutout;
            const uint64_t planes = (e.flags & kCutout) ? 2 : 1;
            if (ok) need = std::max<uint64_t>(need, e.offset + planes * e.w * e.h * scale * scale);
        }
        ok = ok && need < (1ull << 31);
        if (ok) {
            data.resize(size_t(need));
            ok = rd(data.data(), data.size());
        }
        std::fclose(f);
        if (!ok) entries.clear(), data.clear(), error = "not a valid high-resolution texture pack: " + path;
        return ok;
    }

    // The shader's index (m2.hlsl hires_index): per sheet and 32-texel cell
    // (sheet * kCellsPerSheet + y / 32 * 64 + x / 32), kSlots slots of two
    // words: an entry's first data byte, and w | h << 16, kIndexCutout for a
    // cut-out (0: none). Textures of different sizes can start at the same
    // place (36 of the 361 places the game's races use; 3 at most). Only the
    // entries whose texels the texture RAM holds now, at their place.
    // Returns how many.
    size_t index(const uint32_t *tex0, const uint32_t *tex1, std::vector<uint32_t> &out) const {
        out.assign(size_t(2) * kCellsPerSheet * kSlots * 2, 0u);
        size_t live = 0;
        for (const Entry &e : entries) {
            const uint32_t cell = e.sheet * kCellsPerSheet + (e.y / 32) * 64 + e.x / 32;
            const uint32_t key = e.w | (e.h << 16) | ((e.flags & kCutout) ? kIndexCutout : 0u);
            uint32_t *slot = &out[size_t(cell) * kSlots * 2], *free = nullptr;
            bool there = false;
            for (uint32_t k = 0; k < kSlots; ++k) {
                there |= slot[k * 2 + 1] == key;
                if (!slot[k * 2 + 1] && !free) free = &slot[k * 2];
            }
            if (there || !free) continue; // that texture already there, or no slot left
            if (hash(e.sheet ? tex1 : tex0, e.x, e.y, e.w, e.h) != e.hash) continue;
            free[0] = uint32_t(e.offset);
            free[1] = key;
            ++live;
        }
        return live;
    }
};

// The texture RAM words that hold one 32 x 32-texel cell (texel's layout):
// 16 rows of 8 words. fn(word index) for each.
template <class Fn> inline void cell_words(uint32_t cx, uint32_t cy, Fn fn) {
    for (uint32_t r = 0; r < 16; ++r) {
        uint32_t x2 = cx * 32, y2 = cy * 32 + r * 2;
        if (x2 >= 1024) x2 -= 1024, y2 ^= 1024;
        const uint32_t first = ((y2 / 2) * 512 + x2 / 2) >> 1;
        for (uint32_t k = 0; k < 8; ++k) fn((first + k) & (kSheetWords - 1));
    }
}

// Pack::index kept up to date as texture RAM changes: a copy of the RAM as
// last checked tells which 32 x 32 cells changed, and only the entries over
// a changed cell are hashed again (a full check at first). Its output equals
// Pack::index's (tests/test_hires.cpp).
class Indexer {
public:
    size_t refresh(const Pack &pack, const uint32_t *tex0, const uint32_t *tex1, std::vector<uint32_t> &out) {
        const uint32_t *ram[2] = {tex0, tex1};
        if (pack_ != &pack || seen_[0].size() != kSheetWords) { // first: everything
            pack_ = &pack;
            for (int s = 0; s < 2; ++s) seen_[s].assign(ram[s], ram[s] + kSheetWords);
            match_.assign(pack.entries.size(), 0);
            for (size_t i = 0; i < pack.entries.size(); ++i) {
                const Entry &e = pack.entries[i];
                match_[i] = hash(ram[e.sheet], e.x, e.y, e.w, e.h) == e.hash;
            }
            last_rehashed_ = pack.entries.size();
        } else {
            changed_.assign(size_t(2) * kCellsPerSheet, 0);
            bool any = false;
            for (uint32_t s = 0; s < 2; ++s)
                for (uint32_t cy = 0; cy < 32; ++cy)
                    for (uint32_t cx = 0; cx < 64; ++cx) {
                        bool diff = false;
                        cell_words(cx, cy, [&](uint32_t w) { diff |= seen_[s][w] != ram[s][w]; });
                        if (!diff) continue;
                        changed_[s * kCellsPerSheet + cy * 64 + cx] = 1;
                        cell_words(cx, cy, [&](uint32_t w) { seen_[s][w] = ram[s][w]; });
                        any = true;
                    }
            last_rehashed_ = 0;
            for (size_t i = 0; any && i < pack.entries.size(); ++i) {
                const Entry &e = pack.entries[i];
                bool touched = false;
                for (uint32_t y = e.y / 32; !touched && y < (e.y + e.h) / 32; ++y)
                    for (uint32_t x = e.x / 32; !touched && x < (e.x + e.w) / 32; ++x)
                        touched = changed_[e.sheet * kCellsPerSheet + y * 64 + x];
                if (!touched) continue;
                match_[i] = hash(ram[e.sheet], e.x, e.y, e.w, e.h) == e.hash;
                ++last_rehashed_;
            }
        }
        // the slots, as Pack::index fills them
        out.assign(size_t(2) * kCellsPerSheet * kSlots * 2, 0u);
        size_t live = 0;
        for (size_t i = 0; i < pack.entries.size(); ++i) {
            if (!match_[i]) continue;
            const Entry &e = pack.entries[i];
            const uint32_t cell = e.sheet * kCellsPerSheet + (e.y / 32) * 64 + e.x / 32;
            const uint32_t key = e.w | (e.h << 16) | ((e.flags & kCutout) ? kIndexCutout : 0u);
            uint32_t *slot = &out[size_t(cell) * kSlots * 2], *free = nullptr;
            bool there = false;
            for (uint32_t k = 0; k < kSlots; ++k) {
                there |= slot[k * 2 + 1] == key;
                if (!slot[k * 2 + 1] && !free) free = &slot[k * 2];
            }
            if (there || !free) continue;
            free[0] = uint32_t(e.offset);
            free[1] = key;
            ++live;
        }
        return live;
    }
    void reset() { pack_ = nullptr; }
    size_t last_rehashed() const { return last_rehashed_; } // entries hashed by the last refresh

private:
    const Pack *pack_ = nullptr;
    std::vector<uint32_t> seen_[2];
    std::vector<uint8_t> match_, changed_;
    size_t last_rehashed_ = 0;
};

} // namespace app::hires
