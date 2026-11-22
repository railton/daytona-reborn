// MSC2 v1 is a little-endian, frame-local scene snapshot.
#pragma once

#include "runtime/scene_cache.h"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rt::msc2 {

struct Primitive {
    SceneCache::PrimitiveState state;
    std::vector<SceneCache::Vertex> vertices;
};
struct Mesh {
    uint32_t id, oba, obc;
    uint8_t mode;
    std::vector<Primitive> primitives;
};
struct Instance {
    uint32_t mesh;
    std::array<float, 12> matrix;
    float focus_x, focus_y;
};
struct Snapshot {
    std::vector<Mesh> meshes;
    std::vector<Instance> instances;
};

namespace detail {
struct Reader {
    std::span<const uint8_t> bytes;

    void require_count(uint32_t count, size_t width) const {
        if (count > bytes.size() / width)
            throw std::runtime_error("truncated MSC2 record");
    }

    uint32_t word(unsigned width = 4) {
        require_count(1, width);
        uint32_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= uint32_t(bytes[i]) << (8 * i);
        bytes = bytes.subspan(width);
        return value;
    }

    int16_t signed_short() {
        return std::bit_cast<int16_t>(uint16_t(word(2)));
    }

    float real() {
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
        return std::bit_cast<float>(word());
    }
};
} // namespace detail

inline Snapshot read(std::span<const uint8_t> bytes) {
    detail::Reader r{bytes};
    if (r.word() != 0x3243534d || r.word() != 1)
        throw std::runtime_error("expected MSC2 version 1");
    const auto mesh_count = r.word();
    const auto instance_count = r.word();
    r.require_count(instance_count, 60);
    const auto instance_bytes = size_t(instance_count) * 60;
    detail::Reader mesh_reader{r.bytes.first(r.bytes.size() - instance_bytes)};
    detail::Reader instance_reader{r.bytes.last(instance_bytes)};
    mesh_reader.require_count(mesh_count, 17);

    Snapshot snapshot;
    snapshot.meshes.reserve(mesh_count);
    std::unordered_set<uint32_t> ids;
    for (uint32_t i = 0; i < mesh_count; ++i) {
        Mesh mesh;
        mesh.id = mesh_reader.word();
        if (!ids.insert(mesh.id).second)
            throw std::runtime_error("duplicate MSC2 mesh id");
        mesh.oba = mesh_reader.word();
        mesh.obc = mesh_reader.word();
        mesh.mode = uint8_t(mesh_reader.word(1));
        const auto primitive_count = mesh_reader.word();
        mesh_reader.require_count(primitive_count, 37);
        mesh.primitives.reserve(primitive_count);
        for (uint32_t j = 0; j < primitive_count; ++j) {
            Primitive p;
            for (auto &field : p.state.texheader) field = mesh_reader.word();
            p.state.luma = uint8_t(mesh_reader.word(1));
            p.state.texlod = std::bit_cast<int32_t>(mesh_reader.word());
            for (auto &field : p.state.center) field = mesh_reader.signed_short();
            for (auto &field : p.state.viewport) field = mesh_reader.signed_short();
            const auto vertex_count = mesh_reader.word();
            mesh_reader.require_count(vertex_count, 20);
            p.vertices.reserve(vertex_count);
            for (uint32_t k = 0; k < vertex_count; ++k) {
                p.vertices.push_back({mesh_reader.real(), mesh_reader.real(),
                                      mesh_reader.real(), mesh_reader.real(),
                                      mesh_reader.real()});
            }
            mesh.primitives.push_back(std::move(p));
        }
        snapshot.meshes.push_back(std::move(mesh));
    }
    if (!mesh_reader.bytes.empty())
        throw std::runtime_error("unexpected bytes in MSC2 mesh table");

    snapshot.instances.reserve(instance_count);
    for (uint32_t i = 0; i < instance_count; ++i) {
        Instance instance;
        instance.mesh = instance_reader.word();
        if (!ids.contains(instance.mesh))
            throw std::runtime_error("missing MSC2 instance mesh");
        for (auto &field : instance.matrix) field = instance_reader.real();
        instance.focus_x = instance_reader.real();
        instance.focus_y = instance_reader.real();
        snapshot.instances.push_back(instance);
    }
    return snapshot;
}

} // namespace rt::msc2
