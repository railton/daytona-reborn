#include "app/link_codec.h"

namespace app::online {

namespace {
void put_var(std::string &out, size_t n) {
    while (n >= 0x80) {
        out += char(uint8_t(n) | 0x80);
        n >>= 7;
    }
    out += char(uint8_t(n));
}

bool get_var(const uint8_t *&p, const uint8_t *end, size_t &n) {
    n = 0;
    for (int shift = 0; shift < 28; shift += 7) {
        if (p == end) return false;
        const uint8_t c = *p++;
        n |= size_t(c & 0x7f) << shift;
        if (c < 0x80) return true;
    }
    return false;
}
} // namespace

void LinkCodec::reset() {
    hist_[0].clear();
    hist_[1].clear();
}

const uint8_t *LinkCodec::ref(uint8_t r) const {
    if (r == 0 || r > 16) return nullptr;
    const auto &h = hist_[(r - 1) / 8];
    const size_t off = size_t((r - 1) % 8) * kSlot;
    return off + kSlot <= h.size() ? h.data() + off : nullptr;
}

void LinkCodec::push(const uint8_t *block, size_t n) {
    hist_[1].swap(hist_[0]);
    hist_[0].assign(block, block + n);
}

void LinkCodec::encode(const uint8_t *f, size_t n, std::string &out) {
    if (n >= 2 && n <= 0xffff && f[0] == 0xfc) {
        out += char(1);
        out += char(uint8_t(n >> 8));
        out += char(uint8_t(n));
        out += char(f[1]);
        return;
    }
    if (n < 2 || f[0] > 8 || (n - 1) % kSlot != 0) {
        out += char(0);
        out.append(reinterpret_cast<const char *>(f), n);
        return;
    }
    out += char(2);
    out += char(f[0]);
    const uint8_t *b = f + 1;
    const size_t len = n - 1;
    for (size_t off = 0; off < len; off += kSlot) {
        const uint8_t *cur = b + off;
        // the fewest differing bytes: the same slot of the previous block first (usually the answer)
        const uint8_t same = uint8_t(1 + off / kSlot);
        uint8_t best = 0;
        size_t best_n = kSlot + 1;
        auto consider = [&](uint8_t r) {
            size_t d = 0;
            if (r == 0) {
                for (size_t i = 0; i < kSlot; ++i) d += cur[i] != 0;
            } else if (const uint8_t *x = ref(r)) {
                for (size_t i = 0; i < kSlot; ++i) d += cur[i] != x[i];
            } else {
                return false;
            }
            if (d < best_n) best = r, best_n = d;
            return d == 0;
        };
        if (!(same <= 16 && consider(same)))
            for (uint8_t r = 0; r <= 16; ++r)
                if (r != same && consider(r)) break;
        out += char(best);
        const uint8_t *x = ref(best);
        auto at = [&](size_t i) { return uint8_t(x ? cur[i] ^ x[i] : cur[i]); };
        // (zero run, literals)*: a literal run ends at three zeros in a row, or zeros to the end
        for (size_t i = 0; i < kSlot;) {
            size_t j = i;
            while (j < kSlot && at(j) == 0) ++j;
            put_var(out, j - i);
            i = j;
            while (j < kSlot) {
                if (at(j) == 0) {
                    if (j + 2 < kSlot && at(j + 1) == 0 && at(j + 2) == 0) break;
                    size_t k = j;
                    while (k < kSlot && at(k) == 0) ++k;
                    if (k == kSlot) break;
                }
                ++j;
            }
            put_var(out, j - i);
            for (; i < j; ++i) out += char(at(i));
        }
    }
    push(b, len);
}

bool LinkCodec::decode(const uint8_t *m, size_t n, std::string &out) {
    if (n == 0) return false;
    const uint8_t *p = m + 1, *end = m + n;
    switch (m[0]) {
    case 0:
        if (n - 1 > kMaxFrame) return false;
        out.append(reinterpret_cast<const char *>(p), n - 1);
        return true;
    case 1: {
        if (n != 4) return false;
        const size_t len = size_t(m[1]) << 8 | m[2];
        if (len < 2 || len > kMaxFrame) return false;
        std::string token(len, '\0');
        token[0] = char(0xfc);
        token[1] = char(m[3]);
        out += token;
        return true;
    }
    case 2: break;
    default: return false;
    }
    if (p == end) return false;
    std::string frame(1, char(*p++));
    std::vector<uint8_t> block;
    while (p < end) {
        if (block.size() + kSlot > kMaxFrame) return false;
        const uint8_t r = *p++;
        if (r > 16) return false;
        const uint8_t *x = ref(r);
        if (r != 0 && !x) return false; // a reference this side does not have: the streams disagree
        for (size_t i = 0; i < kSlot;) {
            size_t zeros = 0, literals = 0;
            if (!get_var(p, end, zeros) || !get_var(p, end, literals)) return false;
            if (zeros > kSlot - i || literals > kSlot - i - zeros || size_t(end - p) < literals) return false;
            for (size_t k = 0; k < zeros; ++k) block.push_back(x ? x[i + k] : 0);
            i += zeros;
            for (size_t k = 0; k < literals; ++k) block.push_back(uint8_t(x ? p[k] ^ x[i + k] : p[k]));
            p += literals;
            i += literals;
        }
    }
    if (block.empty()) return false;
    frame.append(reinterpret_cast<const char *>(block.data()), block.size());
    push(block.data(), block.size());
    out += frame;
    return true;
}

} // namespace app::online
