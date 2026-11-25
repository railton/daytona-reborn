// High-resolution textures (app/gpu/hires_textures.h): the pack's reader
// and the shader's index; the maker's parsing (app/hires_maker.h).
#include "app/gpu/hires_textures.h"
#include "app/hires_maker.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool value, const char *message) {
    if (!value) ++failures, std::fprintf(stderr, "FAIL: %s\n", message);
}

struct Spec {
    uint32_t sheet, x, y, w, h, flags;
    uint64_t hash;
};

// A pack file with these entries, each one's data filled with its index.
std::vector<uint8_t> make_pack(const std::vector<Spec> &specs, uint32_t version = 1, uint32_t scale = 4) {
    std::vector<uint8_t> out;
    auto put = [&](const void *p, size_t n) { out.insert(out.end(), (const uint8_t *)p, (const uint8_t *)p + n); };
    auto u32 = [&](uint32_t v) { put(&v, 4); };
    auto u64 = [&](uint64_t v) { put(&v, 8); };
    put("M2HT", 4);
    u32(version), u32(uint32_t(specs.size())), u32(scale);
    uint64_t offset = 0;
    std::vector<uint8_t> data;
    for (size_t i = 0; i < specs.size(); ++i) {
        const Spec &s = specs[i];
        u32(s.sheet), u32(s.x), u32(s.y), u32(s.w), u32(s.h), u32(s.flags), u64(s.hash), u64(offset);
        const uint64_t bytes = uint64_t(s.w) * s.h * scale * scale * ((s.flags & 1) ? 2 : 1);
        data.insert(data.end(), size_t(bytes), uint8_t(i));
        offset += bytes;
    }
    put(data.data(), data.size());
    return out;
}

std::string write(const std::string &name, const std::vector<uint8_t> &bytes) {
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    if (std::FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
    return path;
}

} // namespace

int main() {
    using namespace app::hires;
    // a texture RAM with a pattern; hashes of its rectangles
    std::vector<uint32_t> tex0(kSheetWords), tex1(kSheetWords);
    for (uint32_t i = 0; i < kSheetWords; ++i) tex0[i] = i * 2654435761u, tex1[i] = ~i * 40503u;
    const uint64_t h64 = hash(tex0.data(), 0, 0, 64, 64);
    const uint64_t h32 = hash(tex0.data(), 0, 0, 32, 32);
    const uint64_t h128 = hash(tex0.data(), 0, 0, 128, 64);
    const uint64_t hcut = hash(tex1.data(), 256, 128, 32, 64);
    check(h64 != h32 && h64 != h128, "the hash takes the size");
    check(texel(tex0.data(), 0, 0, 1, 0) <= 15, "a texel is 4 bits");

    // a valid pack: three textures starting at the same place, a cut-out
    // one, and one whose texels are not there now
    const std::vector<Spec> specs = {{0, 0, 0, 64, 64, 0, h64},    {0, 0, 0, 32, 32, 0, h32},
                                     {0, 0, 0, 128, 64, 0, h128},  {1, 256, 128, 32, 64, kCutout, hcut},
                                     {0, 512, 0, 32, 32, 0, 12345}};
    Pack pack;
    check(pack.load(write("test_hires_ok.m2tex", make_pack(specs))), "a valid pack loads");
    check(pack.entries.size() == 5 && pack.scale == 4, "its entries and scale");
    check(pack.data.size() == size_t(64 * 64 + 32 * 32 + 128 * 64 + 2 * 32 * 64 + 32 * 32) * 16, "its data, a cut-out's twice");

    std::vector<uint32_t> index;
    const size_t live = pack.index(tex0.data(), tex1.data(), index);
    check(live == 4, "four are in texture RAM, the fifth's texels are not");
    check(index.size() == size_t(2) * kCellsPerSheet * kSlots * 2, "the index's size");
    auto slot = [&](uint32_t sheet, uint32_t x, uint32_t y, uint32_t k) {
        return &index[(size_t(sheet * kCellsPerSheet + (y / 32) * 64 + x / 32) * kSlots + k) * 2];
    };
    check(slot(0, 0, 0, 0)[1] == (64u | 64u << 16) && slot(0, 0, 0, 1)[1] == (32u | 32u << 16) &&
              slot(0, 0, 0, 2)[1] == (128u | 64u << 16) && slot(0, 0, 0, 3)[1] == 0,
          "three textures at one place take three slots");
    check(slot(0, 0, 0, 1)[0] == 64 * 64 * 16, "each slot points at its texture's data");
    check(slot(1, 256, 128, 0)[1] == (32u | 64u << 16 | kIndexCutout), "a cut-out is flagged");
    check(slot(0, 512, 0, 0)[1] == 0, "a texture whose texels changed is left out");
    // the same texture twice: one slot
    std::vector<Spec> twice = {{0, 0, 0, 64, 64, 0, h64}, {0, 0, 0, 64, 64, 0, h64}};
    Pack pack2;
    check(pack2.load(write("test_hires_twice.m2tex", make_pack(twice))), "a pack listing one texture twice loads");
    check(pack2.index(tex0.data(), tex1.data(), index) == 1 && slot(0, 0, 0, 1)[1] == 0, "listed twice, one slot");

    // not valid: each is refused, nothing kept
    auto refused = [&](const std::vector<uint8_t> &bytes, const char *what) {
        Pack p;
        check(!p.load(write("test_hires_bad.m2tex", bytes)) && p.entries.empty() && p.data.empty() && !p.error.empty(), what);
    };
    std::vector<uint8_t> bytes = make_pack(specs);
    bytes[0] = 'X';
    refused(bytes, "a wrong magic is refused");
    refused(make_pack(specs, 2), "another version is refused");
    refused(make_pack(specs, 1, 0), "scale 0 is refused");
    bytes = make_pack(specs);
    bytes.resize(bytes.size() - 1);
    refused(bytes, "a short file is refused");
    refused(make_pack({{0, 0, 0, 48, 64, 0, 1}}), "a width that is no size the game has is refused");
    refused(make_pack({{0, 2032, 0, 32, 32, 0, 1}}), "a rectangle past the sheet is refused");
    refused(make_pack({{2, 0, 0, 32, 32, 0, 1}}), "a third sheet is refused");
    refused(make_pack({{0, 0, 0, 32, 32, 2, 1}}), "unknown flags are refused");
    {
        Pack p;
        check(!p.load((std::filesystem::temp_directory_path() / "test_hires_missing.m2tex").string()), "no file");
    }

    // the maker's progress lines and the Dockerfile's base image
    uint64_t done = 0, total = 0;
    check(app::HiresMaker::parse_progress("m2texcap: progress 3000/51000", "m2texcap", done, total) && done == 3000 &&
              total == 51000,
          "the capture's progress");
    check(app::HiresMaker::parse_progress("  hires_textures: progress 12/665", "hires_textures", done, total) &&
              done == 12 && total == 665,
          "the upscaling's progress");
    check(!app::HiresMaker::parse_progress("hires_textures: progress 9/0", "hires_textures", done, total), "total 0");
    check(!app::HiresMaker::parse_progress("hires_textures: progress 9/8", "hires_textures", done, total), "past the total");
    check(!app::HiresMaker::parse_progress("m2texcap: progress 1/2", "hires_textures", done, total), "another tool");
    check(app::HiresMaker::dockerfile_base("# a comment\n  FROM pytorch/pytorch:2.7.1-cuda12.8-cudnn9-runtime AS base\nRUN x\n") ==
              "pytorch/pytorch:2.7.1-cuda12.8-cudnn9-runtime",
          "the base image");
    check(app::HiresMaker::dockerfile_base("RUN x\n").empty(), "no FROM");

    if (failures) return 1;
    std::printf("hires: ok\n");
    return 0;
}
