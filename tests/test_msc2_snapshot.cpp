#include "runtime/msc2_snapshot.h"

#include <algorithm>
#include <iostream>
#include <string_view>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (0)

using Bytes = std::vector<uint8_t>;

void word(Bytes &out, uint32_t value, unsigned width = 4) {
    for (unsigned i = 0; i < width; ++i) out.push_back(uint8_t(value >> (8 * i)));
}

void real(Bytes &out, float value) {
    word(out, std::bit_cast<uint32_t>(value));
}

Bytes header(uint32_t meshes, uint32_t instances) {
    Bytes out;
    for (uint32_t value : {0x3243534du, 1u, meshes, instances}) word(out, value);
    return out;
}

Bytes shared_mesh_snapshot(uint32_t mesh = 7, float coordinate_offset = 0.f) {
    auto out = header(2, 2);
    word(out, mesh); word(out, 0x800001); word(out, 3); word(out, 1, 1); word(out, 1);
    for (uint32_t value : {5u, 6u, 7u, 8u}) word(out, value);
    word(out, 42, 1); word(out, uint32_t(-3));
    for (int16_t value : {-9, 100, -2, 4, 496, 384}) word(out, uint16_t(value), 2);
    word(out, 3);
    for (unsigned i = 0; i < 15; ++i) real(out, float(i) - 5.5f + coordinate_offset);
    // Legacy exports could contain inactive resident variants.
    word(out, 4); word(out, 0x800002); word(out, 5); word(out, 2, 1); word(out, 0);
    // Two visible instances share mesh 7, independent of record order.
    for (unsigned i = 0; i < 2; ++i) {
        word(out, mesh);
        for (unsigned j = 0; j < 14; ++j) real(out, float(i * 14 + j) + coordinate_offset);
    }
    return out;
}

void rejects(const Bytes &bytes, std::string_view reason = {}) {
    try {
        rt::msc2::read(bytes);
    } catch (const std::runtime_error &error) {
        CHECK(reason.empty() || reason == error.what());
        return;
    }
    throw std::runtime_error("malformed snapshot was accepted");
}

void check_owned_scene(const rt::msc2::Snapshot &scene, uint32_t mesh, float offset) {
    const auto &active_mesh = scene.meshes.at(0);
    CHECK(active_mesh.id == mesh);
    CHECK(active_mesh.primitives.at(0).vertices.at(0).x == -5.5f + offset);
    const auto &instance = scene.instances.at(1);
    CHECK(instance.mesh == mesh && instance.matrix[11] == 25.f + offset);
    CHECK(instance.focus_y == 27.f + offset);
}

void check_pair_ownership() {
    const auto first_bytes = shared_mesh_snapshot();
    const auto second_bytes = shared_mesh_snapshot(900, 100.f);
    CHECK(first_bytes.size() == second_bytes.size());

    for (const bool reversed : {false, true}) {
        rt::msc2::Snapshot first, second;
        {
            auto input = reversed ? second_bytes : first_bytes;
            auto &older = reversed ? second : first;
            auto &newer = reversed ? first : second;
            older = rt::msc2::read(input);
            const auto &replacement = reversed ? first_bytes : second_bytes;
            // Reuse the same allocation for the next read and poison it before release.
            std::copy(replacement.begin(), replacement.end(), input.begin());
            newer = rt::msc2::read(input);
            std::fill(input.begin(), input.end(), uint8_t{0xA5});
            check_owned_scene(first, 7, 0.f);
            check_owned_scene(second, 900, 100.f);
        }
        check_owned_scene(first, 7, 0.f);
        first.meshes[0].primitives[0].vertices[0].x = 12345.f;
        first.instances[1].matrix[11] = 6789.f;
        check_owned_scene(second, 900, 100.f);
    }
}

int main() {
    try {
        auto empty = rt::msc2::read(header(0, 0));
        CHECK(empty.meshes.empty() && empty.instances.empty());
        const auto bytes = shared_mesh_snapshot();
        const auto scene = rt::msc2::read(bytes);
        CHECK(scene.meshes.size() == 2 && scene.instances.size() == 2);
        CHECK(scene.meshes[0].id == 7 && scene.meshes[1].id == 4);
        CHECK(scene.meshes[0].oba == 0x800001 && scene.meshes[0].obc == 3 && scene.meshes[0].mode == 1);
        CHECK(scene.meshes[1].primitives.empty());
        const auto &p = scene.meshes[0].primitives.at(0);
        CHECK(p.state.texheader[3] == 8 && p.state.luma == 42 && p.state.texlod == -3);
        CHECK(p.state.center[0] == -9 && p.state.viewport[0] == -2 && p.state.viewport[3] == 384);
        CHECK(p.vertices.size() == 3 && p.vertices[0].x == -5.5f && p.vertices[2].v == 8.5f);
        CHECK(scene.instances[0].mesh == 7 && scene.instances[1].mesh == 7);
        CHECK(scene.instances[1].matrix[11] == 25.f && scene.instances[1].focus_y == 27.f);

        for (size_t size = 0; size < bytes.size(); ++size)
            rejects(Bytes(bytes.begin(), bytes.begin() + size));
        auto bad = bytes;
        bad.insert(bad.end() - 120, 0);
        rejects(bad, "unexpected bytes in MSC2 mesh table");
        bad = bytes;
        bad[4] = 2; rejects(bad, "expected MSC2 version 1");
        bad = bytes;
        bad[bytes.size() - 120] = 9; rejects(bad, "missing MSC2 instance mesh");
        bad = bytes;
        bad[bytes.size() - 120 - 17] = 7; rejects(bad, "duplicate MSC2 mesh id");
        // Last byte of the mesh, instance, primitive and vertex counts.
        for (size_t count_byte : {11u, 15u, 32u, 69u}) {
            bad = bytes;
            bad[count_byte] = 0xff;
            rejects(bad, "truncated MSC2 record");
        }
        check_pair_ownership();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
