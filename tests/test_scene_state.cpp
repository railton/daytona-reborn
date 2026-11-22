#include "runtime/scene_cache.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (0)
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
Bytes bytes(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    CHECK(file.good());
    return Bytes(std::istreambuf_iterator<char>(file), {});
}
struct Reader {
    Bytes data;
    size_t offset = 0;
    uint32_t word(unsigned count = 4) {
        CHECK(offset + count <= data.size());
        uint32_t result = 0;
        for (unsigned i = 0; i < count; ++i) result |= uint32_t(data[offset++]) << (8*i);
        return result;
    }
    float real() {
        const uint32_t bits = word();
        float result;
        std::memcpy(&result, &bits, sizeof result);
        return result;
    }
};
rt::GeoPoly triangle(uint32_t command, uint16_t texture, float tx) {
    rt::GeoPoly p{};
    for (auto &v : p.v) v = rt::GeoVertex{};
    p.object_command = command; p.object_oba = 0x800001; p.object_obc = 4; p.object_mode = 1;
    p.num_vertices = 3;
    p.v[0].ox = -1; p.v[0].oy = -1;
    p.v[1].ox = 1; p.v[1].oy = -1;
    p.v[2].oy = 1;
    for (unsigned i = 0; i < 3; ++i) p.v[i].oz = 10;
    p.object_matrix[0] = p.object_matrix[4] = p.object_matrix[8] = 1;
    p.object_matrix[9] = tx;
    p.object_focus_x = p.object_focus_y = 40;
    for (unsigned i = 0; i < 4; ++i) p.texheader[i] = texture + i;
    p.luma = uint8_t(texture); p.texlod = -int(texture);
    p.center[0] = 32; p.center[1] = 352;
    p.viewport[0] = -3; p.viewport[1] = 7; p.viewport[2] = 64; p.viewport[3] = 60;
    return p;
}
void check_export(const fs::path &path, const std::vector<rt::GeoPoly> &expected) {
    Reader r{bytes(path)};
    CHECK(r.word() == 0x3243534d); CHECK(r.word() == 1);
    CHECK(r.word() == expected.size()); CHECK(r.word() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto &p = expected[i];
        CHECK(r.word() == i); CHECK(r.word() == p.object_oba); CHECK(r.word() == p.object_obc);
        CHECK(r.word(1) == p.object_mode); CHECK(r.word() == 1);
        for (auto value : p.texheader) CHECK(r.word() == value);
        CHECK(r.word(1) == p.luma); CHECK(r.word() == uint32_t(p.texlod));
        for (auto value : p.center) CHECK(r.word(2) == uint16_t(value));
        for (auto value : p.viewport) CHECK(r.word(2) == uint16_t(value));
        CHECK(r.word() == 3);
        for (unsigned vertex = 0; vertex < 3; ++vertex) {
            const auto &v = p.v[vertex];
            for (float value : {v.ox, v.oy, v.oz, v.p[1], v.p[2]}) CHECK(r.real() == value);
        }
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        CHECK(r.word() == i);
        for (float value : expected[i].object_matrix) CHECK(r.real() == value);
        CHECK(r.real() == expected[i].object_focus_x);
        CHECK(r.real() == expected[i].object_focus_y);
    }
    CHECK(r.offset == r.data.size());
}
int main(int argc, char **argv) {
    try {
        CHECK(argc == 2);
        fs::path folder;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 10 && folder.empty(); ++attempt) {
            auto candidate = fs::path(argv[1]) / ("scene-state-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (fs::create_directory(candidate)) folder = candidate;
        }
        CHECK(!folder.empty());
        rt::SceneCache cache;
        auto a = triangle(1, 10, -3), b = triangle(2, 20, 3);
        cache.frame({a,b}, 0, nullptr);
        CHECK(cache.unique_meshes() == 1);
        const auto mesh = cache.instances()[0].mesh;
        const auto old_states = cache.instances()[0].primitive_states;
        CHECK(old_states.size() == 1 && old_states[0].texheader[0] == 10);
        CHECK(cache.instances()[1].primitive_states[0].texheader[0] == 20);
        const auto first = folder / "first.msc2";
        cache.export_binary(first.string().c_str()); check_export(first, {a,b});
        const auto first_memory = cache.copy_binary();
        CHECK(first_memory == bytes(first));
        const auto image1 = folder / "first.ppm";
        cache.render_ppm(image1.string().c_str(),64,64);
        a.texheader[0] = 41; a.luma = 77; a.texlod = -5; a.viewport[0] = -7;
        cache.frame({b,a}, 1, nullptr);
        CHECK(cache.unique_meshes() == 1 && cache.instances()[1].mesh == mesh);
        CHECK(cache.instances()[0].primitive_states[0].texheader[0] == 20);
        CHECK(cache.instances()[1].primitive_states[0].texheader[0] == 41);
        CHECK(old_states[0].texheader[0] == 10);
        const auto second = folder / "second.msc2";
        cache.export_binary(second.string().c_str()); check_export(second, {b,a});
        const auto second_memory = cache.copy_binary();
        CHECK(second_memory == bytes(second) && second_memory != first_memory);
        CHECK(first_memory == bytes(first));
        const auto image2 = folder / "second.ppm";
        cache.render_ppm(image2.string().c_str(),64,64);
        CHECK(bytes(image1) != bytes(image2));
        cache.frame({a,b}, 2, nullptr);
        const auto reordered = folder / "reordered.ppm";
        cache.render_ppm(reordered.string().c_str(),64,64);
        CHECK(bytes(image2) == bytes(reordered));
        auto linked = a;
        linked.texheader[0] = 88;
        cache.frame({a,linked}, 3, nullptr);
        CHECK(cache.instances().size() == 1);
        CHECK(cache.instances()[0].primitive_states.size() == 2);
        CHECK(cache.instances()[0].primitive_states[0].texheader[0] == 41);
        CHECK(cache.instances()[0].primitive_states[1].texheader[0] == 88);
        CHECK(cache.unique_meshes() == 2);
        cache.frame({}, 4, nullptr);
        const auto empty = folder / "empty.msc2";
        cache.export_binary(empty.string().c_str()); check_export(empty, {});
        CHECK(cache.copy_binary() == bytes(empty));
        CHECK(cache.unique_meshes() == 2);
        std::cout << "scene state and MSC2 v1 snapshot passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
