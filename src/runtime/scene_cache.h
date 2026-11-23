#pragma once

// Persistent object-mesh cache, the first step towards a resident GPU scene.
// It stores the first visible variant of each ROM object
// (positions before the model-view matrix and UVs) and a
// per-frame instance matrix and visual state, so geometry can be uploaded once.
//
// Scope: polygon ROM only (oba bit 0x00800000). Direct data and the two polygon
// RAM windows keep the historical per-frame list. obc is a link-prefix length,
// not an instance identity. It remains a variant discriminator in the mesh key.
// object_command separates emissions inside a frame, even with identical poses.
// Its identity is frame-local. Cull-dependent variants still require work
// before this prototype can serve as the final GPU cache.

#include "runtime/geo.h"
#include "runtime/msc2_priority.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>
#include <limits>
#include <memory>

namespace rt {

class SceneCache {
public:
    struct Vertex {
        float x = 0, y = 0, z = 0;
        float u = 0, v = 0;
    };
    struct PrimitiveState {
        uint32_t texheader[4] = {};
        uint8_t luma = 0;
        int32_t texlod = 0;
        int16_t center[2] = {0, 0};
        int16_t viewport[4] = {0, 0, 0, 0};
    };
    struct Primitive {
        std::vector<Vertex> vertices;
    };
    struct Mesh {
        uint32_t id = 0;
        uint32_t key_oba = 0;
        uint32_t key_obc = 0;
        uint8_t key_mode = 0;
        uint32_t first_frame = 0;
        uint64_t last_used = 0; // cache frame count when an instance last drew it
        uint32_t vertices = 0;
        std::vector<Primitive> primitives;
    };
    struct Instance {
        std::vector<PrimitiveState> primitive_states;
        uint32_t mesh = 0;
        uint32_t command = 0; // frame-local provenance (not part of MSC2 v1)
        float matrix[12] = {};
        float focus_x = 1.0f, focus_y = 1.0f;
    };

    void frame(const std::vector<GeoPoly> &polys, uint64_t frame, const char *log_path,
               bool reference_export = false) {
        ++frames_;
        uint32_t new_meshes = 0, reused = 0, vertices = 0, instances = 0;
        last_instances_.clear();
        reference_priority_.clear();
        // Keep first-emission order; never iterate an unordered map to emit instances.
        std::vector<std::vector<const GeoPoly *>> groups;
        std::unordered_map<uint32_t, size_t> commands;
        for (const GeoPoly &poly : polys) {
            ++polys_;
            vertices += poly.num_vertices;
            const bool rom_object = (poly.object_oba & 0x00800000) && !(poly.object_oba & 0x01000000);
            if (poly.object_direct || (!rom_object && !reference_export)) continue;
            if (!poly.object_command) {
                // Legacy/synthetic inputs without provenance cannot safely be
                // merged by address or matrix: preserve each primitive separately.
                groups.push_back({&poly});
            } else {
                auto [it, inserted] = commands.emplace(poly.object_command, groups.size());
                if (inserted) groups.emplace_back();
                groups[it->second].push_back(&poly);
            }
        }
        for (const auto &list : groups) {
            uint64_t h = 1469598103934665603ull;
            for (const GeoPoly *poly : list) {
                const unsigned nv = poly->source_num_vertices ? poly->source_num_vertices : poly->num_vertices;
                // Preserve primitive boundaries in the fingerprint. Two triangles
                // and a six-vertex polygon must not alias as one flat stream.
                h ^= nv; h *= 1099511628211ull;
                for (unsigned k = 0; k < nv; ++k) {
                    const float *sv = poly->source_num_vertices ? poly->source_v[k] : nullptr;
                    const GeoVertex &g = poly->v[k];
                    const float vals[5] = {sv ? sv[0] : g.ox, sv ? sv[1] : g.oy, sv ? sv[2] : g.oz,
                                           sv ? sv[3] : g.p[1], sv ? sv[4] : g.p[2]};
                    for (float f : vals) {
                        uint32_t bits; std::memcpy(&bits, &f, sizeof bits);
                        h ^= bits; h *= 1099511628211ull;
                    }
                }
            }
            const uint64_t key = make_key(*list[0]) ^ (h * 0x9e3779b97f4a7c15ull);
            const auto range = meshes_.equal_range(key);
            auto it = meshes_.end();
            for (auto candidate = range.first; candidate != range.second; ++candidate) {
                if (geometry_equal(candidate->second, list)) {
                    it = candidate;
                    break;
                }
            }
            const bool existed = it != meshes_.end();
            if (!existed) {
                Mesh mesh;
                mesh.id = next_mesh_id_++;
                mesh.key_oba = list[0]->object_oba;
                mesh.key_obc = list[0]->object_obc;
                mesh.key_mode = list[0]->object_mode;
                mesh.first_frame = uint32_t(frame);
                it = meshes_.emplace(key, std::move(mesh));
                ++new_meshes;
                ++unique_;
            }
            if (!existed) {
                for (const GeoPoly *poly : list) {
                    Primitive prim;
                    const unsigned nv = poly->source_num_vertices ? poly->source_num_vertices : poly->num_vertices;
                    prim.vertices.reserve(nv);
                    for (unsigned k = 0; k < nv; ++k) {
                        if (poly->source_num_vertices) {
                            prim.vertices.push_back({poly->source_v[k][0], poly->source_v[k][1], poly->source_v[k][2],
                                                     poly->source_v[k][3], poly->source_v[k][4]});
                        } else {
                            const GeoVertex &v = poly->v[k];
                            prim.vertices.push_back({v.ox, v.oy, v.oz, v.p[1], v.p[2]});
                        }
                    }
                    it->second.vertices += nv;
                    resident_vertices_ += nv;
                    ++resident_primitives_;
                    it->second.primitives.push_back(std::move(prim));
                }
            } else {
                reused += uint32_t(list.size());
                reused_polys_ += uint64_t(list.size());
            }
            Instance inst;
            inst.primitive_states.reserve(list.size());
            for (const GeoPoly *poly : list) {
                PrimitiveState state;
                std::copy(std::begin(poly->texheader), std::end(poly->texheader), std::begin(state.texheader));
                state.luma = poly->luma;
                state.texlod = poly->texlod;
                std::copy(std::begin(poly->center), std::end(poly->center), std::begin(state.center));
                std::copy(std::begin(poly->viewport), std::end(poly->viewport), std::begin(state.viewport));
                inst.primitive_states.push_back(state);
                if (reference_export)
                    reference_priority_.push_back({uint32_t(poly->window), uint32_t(poly->z),
                                                   uint32_t(poly - polys.data())});
            }
            it->second.last_used = frames_;
            inst.mesh = it->second.id;
            inst.command = list[0]->object_command;
            std::copy(std::begin(list[0]->object_matrix), std::end(list[0]->object_matrix), std::begin(inst.matrix));
            inst.focus_x = list[0]->object_focus_x;
            inst.focus_y = list[0]->object_focus_y;
            last_instances_.push_back(std::move(inst));
            ++instances;
        }
        // Geometry the game builds per frame (moving shadows, bent trunks,
        // canyon fill) never repeats, so a cache that only grows reached
        // 15 000 meshes and about 280 MB over a five-lap race. Forget what
        // no instance has drawn for kKeepFrames; the frame's own instances
        // are always kept, so exports are unchanged.
        if (frames_ % kSweepFrames == 0) {
            for (auto it = meshes_.begin(); it != meshes_.end();) {
                if (frames_ - it->second.last_used > kKeepFrames) {
                    resident_vertices_ -= it->second.vertices;
                    resident_primitives_ -= it->second.primitives.size();
                    it = meshes_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (log_path && *log_path) {
            std::FILE *f = std::fopen(log_path, "ab");
            if (f) {
                if (frames_ == 1) {
                    std::fprintf(f, "frame,polys,unique,new,reused,vertices,resident_vertices,resident_primitives,instances\n");
                }
                std::fprintf(f, "%llu,%llu,%zu,%u,%u,%u,%llu,%llu,%u\n",
                             (unsigned long long)frame, (unsigned long long)polys_,
                             meshes_.size(), new_meshes, reused, vertices,
                             (unsigned long long)resident_vertices_,
                             (unsigned long long)resident_primitives_, instances);
                std::fclose(f);
            }
        }
    }

    size_t unique_meshes() const { return meshes_.size(); }
    uint64_t polys() const { return polys_; }
    uint64_t reused_polys() const { return reused_polys_; }
    uint64_t frames() const { return frames_; }
    uint32_t last_instances() const { return uint32_t(last_instances_.size()); }
    const std::vector<Instance> &instances() const { return last_instances_; }

    // CPU preview of the resident scene: object positions transformed by the
    // per-frame instance matrix, then divided by view z. Not a texture-correct
    // renderer; it validates the persistent mesh/matrix path.
    void render_ppm(const char *path, int w = 496, int h = 384) const {
        if (!path || !*path) return;
        std::vector<uint32_t> image(size_t(w) * h, 0xff101018u);
        std::vector<float> depth(size_t(w) * h, 1e30f);
        std::unordered_map<uint32_t, const Mesh *> by_id;
        for (const auto &kv : meshes_) by_id[kv.second.id] = &kv.second;
        auto project = [](const Vertex &v, const Instance &in, const PrimitiveState &p,
                          float &sx, float &sy, float &z) {
            const float *m = in.matrix;
            const float x = m[0] * v.x + m[3] * v.y + m[6] * v.z + m[9];
            const float y = m[1] * v.x + m[4] * v.y + m[7] * v.z + m[10];
            const float zz = m[2] * v.x + m[5] * v.y + m[8] * v.z + m[11];
            if (zz <= 1e-6f) return false;
            z = zz;
            sx = float(p.center[0]) + (x * in.focus_x) / zz;
            sy = float(384 - p.center[1]) - (y * in.focus_y) / zz;
            return true;
        };
        for (const Instance &in : last_instances_) {
            auto mi = by_id.find(in.mesh);
            if (mi == by_id.end()) continue;
            for (size_t i = 0; i < mi->second->primitives.size(); ++i) {
                const Primitive &p = mi->second->primitives[i];
                const PrimitiveState &state = in.primitive_states[i];
                const uint32_t color = 0xff000000u |
                    ((state.texheader[0] * 2654435761u) & 0xffffffu);
                for (size_t k = 1; k + 1 < p.vertices.size(); ++k) {
                    float x0, y0, z0, x1, y1, z1, x2, y2, z2;
                    if (!project(p.vertices[0], in, state, x0, y0, z0) ||
                        !project(p.vertices[k], in, state, x1, y1, z1) ||
                        !project(p.vertices[k + 1], in, state, x2, y2, z2)) continue;
                    const int minx = std::max(0, int(std::floor(std::min({x0, x1, x2}))));
                    const int maxx = std::min(w - 1, int(std::ceil(std::max({x0, x1, x2}))));
                    const int miny = std::max(0, int(std::floor(std::min({y0, y1, y2}))));
                    const int maxy = std::min(h - 1, int(std::ceil(std::max({y0, y1, y2}))));
                    if (minx > maxx || miny > maxy) continue;
                    const float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
                    if (std::abs(area) < 1e-9f) continue;
                    for (int y = miny; y <= maxy; ++y) {
                        for (int x = minx; x <= maxx; ++x) {
                            const float px = x + 0.5f, py = y + 0.5f;
                            const float w0 = ((x1 - px) * (y2 - py) - (y1 - py) * (x2 - px)) / area;
                            const float w1 = ((x2 - px) * (y0 - py) - (y2 - py) * (x0 - px)) / area;
                            const float w2 = 1.0f - w0 - w1;
                            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                            const float z = w0 * z0 + w1 * z1 + w2 * z2;
                            const size_t at = size_t(y) * w + x;
                            if (z < depth[at]) { depth[at] = z; image[at] = color; }
                        }
                    }
                }
            }
        }
        std::FILE *f = std::fopen(path, "wb");
        if (!f) return;
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (uint32_t c : image) {
            const unsigned char rgb[3] = {static_cast<unsigned char>(c >> 16),
                                          static_cast<unsigned char>(c >> 8),
                                          static_cast<unsigned char>(c)};
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
    }

    // Export for a GPU viewer: little-endian, no pointers, game data not
    // embedded beyond the mesh vertices already held in memory.
    bool export_binary(const char *path, bool exclusive = false,
                       msc2::SnapshotFingerprint *fingerprint = nullptr) const {
        if (!path || !*path) return false;
        const auto by_id = binary_mesh_index();
        std::unique_ptr<std::FILE, decltype(&std::fclose)> file(
            std::fopen(path, exclusive ? "wbx" : "wb"), &std::fclose);
        if (!file) return false;
        msc2::SnapshotFingerprint measured;
        const auto write = [&](const void *data, size_t size, size_t count) {
            if (std::fwrite(data, size, count, file.get()) != count)
                throw std::runtime_error("scene export write failed");
            if (fingerprint) measured.append({static_cast<const uint8_t *>(data), size * count});
        };
        write_binary(by_id, write);
        if (std::fclose(file.release()) != 0)
            throw std::runtime_error("scene export close failed");
        if (fingerprint) *fingerprint = measured;
        return true;
    }

    std::vector<uint8_t> copy_binary() const {
        std::vector<uint8_t> result;
        write_binary(binary_mesh_index(), [&](const void *data, size_t size, size_t count) {
            const auto *first = static_cast<const uint8_t *>(data);
            result.insert(result.end(), first, first + size * count);
        });
        return result;
    }

    void export_reference_priority(std::FILE *file) const {
        for (const auto key : reference_priority_) {
            const auto bytes = msc2::encode_priority(key);
            if (std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size())
                throw std::runtime_error("reference draw priority write failed");
        }
    }

private:
    using BinaryMeshIndex = std::unordered_map<uint32_t, const Mesh *>;

    BinaryMeshIndex binary_mesh_index() const {
        if (last_instances_.size() > std::numeric_limits<uint32_t>::max())
            throw std::length_error("scene export instance count exceeds MSC2 v1 range");
        BinaryMeshIndex by_id;
        for (const auto &kv : meshes_) by_id[kv.second.id] = &kv.second;
        return by_id;
    }

    template<class Write>
    void write_binary(const BinaryMeshIndex &by_id, Write &&write) const {
        const uint32_t magic = 0x3243534du; // "MSC2"
        const uint32_t version = 1;
        const uint32_t mesh_count = uint32_t(last_instances_.size());
        const uint32_t instance_count = uint32_t(last_instances_.size());
        write(&magic, 4, 1); write(&version, 4, 1);
        write(&mesh_count, 4, 1); write(&instance_count, 4, 1);
        // MSC2 v1 stores state on meshes. Bake each current occurrence into a
        // file-local record while the resident cache keeps shared geometry.
        for (uint32_t id = 0; id < mesh_count; ++id) {
            const Instance &instance = last_instances_[id];
            const Mesh &m = *by_id.at(instance.mesh);
            const uint32_t prim_count = uint32_t(m.primitives.size());
            write(&id, 4, 1); write(&m.key_oba, 4, 1);
            write(&m.key_obc, 4, 1); write(&m.key_mode, 1, 1);
            write(&prim_count, 4, 1);
            for (size_t i = 0; i < m.primitives.size(); ++i) {
                const Primitive &p = m.primitives[i];
                const PrimitiveState &state = instance.primitive_states[i];
                const uint32_t vcount = uint32_t(p.vertices.size());
                write(state.texheader, 4, 4);
                write(&state.luma, 1, 1); write(&state.texlod, 4, 1);
                write(state.center, 2, 2); write(state.viewport, 2, 4);
                write(&vcount, 4, 1);
                for (const Vertex &v : p.vertices) {
                    const float xyzu[5] = {v.x, v.y, v.z, v.u, v.v};
                    write(xyzu, 4, 5);
                }
            }
        }
        for (uint32_t id = 0; id < instance_count; ++id) {
            const Instance &in = last_instances_[id];
            write(&id, 4, 1);
            write(in.matrix, 4, 12);
            write(&in.focus_x, 4, 1); write(&in.focus_y, 4, 1);
        }
    }

    std::vector<msc2::DrawPriority> reference_priority_;
    // A fingerprint selects candidates; exact geometry selects a reusable mesh.
    // Per-instance primitive state is refreshed separately every frame.
    static bool geometry_equal(const Mesh &mesh, const std::vector<const GeoPoly *> &list) {
        if (mesh.key_oba != list[0]->object_oba || mesh.key_obc != list[0]->object_obc ||
            mesh.key_mode != list[0]->object_mode || mesh.primitives.size() != list.size()) return false;
        for (size_t i = 0; i < list.size(); ++i) {
            const GeoPoly &poly = *list[i];
            const auto &vertices = mesh.primitives[i].vertices;
            const unsigned nv = poly.source_num_vertices ? poly.source_num_vertices : poly.num_vertices;
            if (vertices.size() != nv) return false;
            for (unsigned k = 0; k < nv; ++k) {
                const Vertex &v = vertices[k];
                const float stored[5] = {v.x, v.y, v.z, v.u, v.v};
                const GeoVertex &g = poly.v[k];
                const float *sv = poly.source_num_vertices ? poly.source_v[k] : nullptr;
                const float incoming[5] = {sv ? sv[0] : g.ox, sv ? sv[1] : g.oy,
                    sv ? sv[2] : g.oz, sv ? sv[3] : g.p[1], sv ? sv[4] : g.p[2]};
                // Match the fingerprint's float-bit semantics, including signed
                // zeros and NaN payloads; never compare struct padding.
                if (std::memcmp(stored, incoming, sizeof stored) != 0) return false;
            }
        }
        return true;
    }

    static uint64_t make_key(const GeoPoly &poly) {
        // obc is a link-prefix/LOD discriminator here, not a unique mesh id.
        return (uint64_t(poly.object_oba & 0x1ffffff) << 24) |
               (uint64_t(poly.object_obc & 0xfffff) << 4) |
               uint64_t(poly.object_mode & 3);
    }

    static constexpr uint64_t kKeepFrames = 1200; // 20 s at 60 Hz, a little over one Beginner lap
    static constexpr uint64_t kSweepFrames = 60;
    std::unordered_multimap<uint64_t, Mesh> meshes_;
    std::vector<Instance> last_instances_;
    uint64_t frames_ = 0, polys_ = 0, reused_polys_ = 0;
    uint64_t resident_vertices_ = 0, resident_primitives_ = 0;
    uint32_t next_mesh_id_ = 0;
    size_t unique_ = 0;
};

} // namespace rt
