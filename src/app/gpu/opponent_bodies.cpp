#include "app/gpu/opponent_bodies.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <locale>
#include <map>
#include <utility>

namespace app {

bool OpponentBodies::load(const std::string &directory) {
    const auto root = std::filesystem::u8path(directory);
    std::ifstream meshes(root / "bodies.m2car");
    meshes.imbue(std::locale::classic());
    std::string version;
    if (!(meshes >> version) || version != "M2CAR1") return false;
    decltype(bodies_) bodies;
    for (auto &body : bodies) {
        int face_count;
        if (!(meshes >> face_count) || face_count < 1 || face_count > 1024) return false;
        body.resize(size_t(face_count));
        for (auto &face : body) {
            int count, surface;
            if (!(meshes >> count >> surface >> face.shade) || count < 3 || count > 4 ||
                surface < 0 || surface >= 10 || !(face.shade >= 0 && face.shade <= 2)) return false;
            face.count = uint8_t(count);
            face.surface = uint8_t(surface);
            for (int i = 0; i < count; ++i) {
                auto &v = face.vertices[i];
                for (float &x : v)
                    if (!(meshes >> x) || !std::isfinite(x)) return false;
                if (std::abs(v[0]) > 3 || std::abs(v[1]) > 3 || std::abs(v[2]) > 3 ||
                    v[3] < 0 || v[3] > 1 || v[4] < 0 || v[4] > 1) return false;
            }
        }
    }
    meshes >> std::ws;
    if (!meshes.eof()) return false;

    std::ifstream image(root / "liveries.rgba", std::ios::binary | std::ios::ate);
    if (!image || image.tellg() != atlas_bytes) return false;
    std::vector<uint8_t> atlas(atlas_bytes);
    image.seekg(0);
    if (!image.read(reinterpret_cast<char *>(atlas.data()), atlas.size())) return false;
    bodies_ = std::move(bodies);
    atlas_ = std::move(atlas);
    return true;
}

namespace {

constexpr float near_z = .1f;

// Sutherland-Hodgman against z = near_z; a quad gives at most five corners.
unsigned clip_near(rt::GeoVertex (&v)[8], unsigned count) {
    if (std::all_of(v, v + count, [](const auto &p) { return p.p[0] > near_z; })) return count;
    rt::GeoVertex out[8];
    unsigned n = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto &a = v[i], &b = v[(i + 1) % count];
        const bool ain = a.p[0] > near_z, bin = b.p[0] > near_z;
        if (ain) out[n++] = a;
        if (ain != bin) {
            const float t = (near_z - a.p[0]) / (b.p[0] - a.p[0]);
            auto &c = out[n++];
            c.x = a.x + (b.x - a.x) * t;
            c.y = a.y + (b.y - a.y) * t;
            c.p[0] = near_z;
            c.p[1] = a.p[1] + (b.p[1] - a.p[1]) * t;
            c.p[2] = a.p[2] + (b.p[2] - a.p[2]) * t;
        }
    }
    std::copy_n(out, n, v);
    return n;
}

} // namespace

const std::vector<rt::GeoPoly> *OpponentBodies::replace(const std::vector<rt::GeoPoly> &scene) {
    if (atlas_.empty()) return nullptr;
    // A command is one emission, including all its faces. Identity comes from
    // the runtime's established opponent slots for supported revisions.
    std::map<uint32_t, const rt::GeoPoly *> candidates;
    for (const auto &poly : scene) {
        const auto key = poly.mobile_key;
        if (!rt::is_opponent_body_part(key.part) || !poly.object_command || poly.object_direct) continue;
        // Every distance, the contact range included: a nearer cut-off showed
        // the original car again whenever the player touched an opponent.
        if (poly.object_matrix[11] <= 0.f) continue;
        candidates.try_emplace(poly.object_command, &poly);
    }
    if (candidates.empty()) return nullptr;

    scene_.clear();
    std::map<uint32_t, size_t> replaced;
    for (const auto &[command, source] : candidates) {
        const auto slot = rt::MobileIdentity::slot_index(source->mobile_key.slot);
        if (!slot) continue;
        const size_t variant = *slot % bodies_.size();
        const auto &m = source->object_matrix;
        const size_t first = scene_.size();
        for (const auto &face : bodies_[variant]) {
            rt::GeoVertex vertices[8];
            float depth = 0;
            for (unsigned i = 0; i < face.count; ++i) {
                const auto &v = face.vertices[i];
                auto &out = vertices[i];
                out.x = v[0] * m[0] + v[1] * m[3] + v[2] * m[6] + m[9];
                out.y = v[0] * m[1] + v[1] * m[4] + v[2] * m[7] + m[10];
                out.p[0] = v[0] * m[2] + v[1] * m[5] + v[2] * m[8] + m[11];
                out.p[1] = v[3] * 8;
                out.p[2] = v[4] * 8;
                depth += out.p[0];
            }
            if (std::all_of(vertices, vertices + face.count, [](const auto &v) { return v.p[0] <= near_z; }))
                continue;
            const auto &a = vertices[0], &b = vertices[1], &c = vertices[2];
            const float ux = b.x - a.x, uy = b.y - a.y, uz = b.p[0] - a.p[0];
            const float vx = c.x - a.x, vy = c.y - a.y, vz = c.p[0] - a.p[0];
            const float facing = (uy * vz - uz * vy) * a.x + (uz * vx - ux * vz) * a.y +
                                 (ux * vy - uy * vx) * a.p[0];
            if (facing >= 0) continue;
            // A face crossing the near plane (an opponent against the player's
            // car) is cut there rather than dropped, so the body has no holes.
            const unsigned count = clip_near(vertices, face.count);
            if (count < 3) continue;
            auto &poly = scene_.emplace_back();
            poly.z = rt::Geo::z_sort_value(depth / face.count, source->object_z_adjust);
            std::copy_n(source->viewport, 4, poly.viewport);
            std::copy_n(source->center, 2, poly.center);
            poly.window = source->window;
            poly.num_vertices = uint8_t(count);
            poly.local_texture = uint8_t(variant * 10 + face.surface + 1);
            poly.luma = uint8_t(std::lround(face.shade * 127));
            for (unsigned i = 0; i < count; ++i) {
                vertices[i].x *= source->object_focus_x;
                vertices[i].y *= source->object_focus_y;
                poly.v[i] = vertices[i];
            }
        }
        if (scene_.size() > first) replaced.emplace(command, scene_.size() - first);
    }
    if (replaced.empty()) return nullptr;

    // Keep the original wheel, shadow and other emissions. Only substitute
    // the same body command in the same camera window.
    scene_.reserve(scene_.size() + scene.size());
    unsigned removed = 0;
    for (const auto &poly : scene) {
        if (replaced.contains(poly.object_command)) ++removed;
        else scene_.push_back(poly);
    }

    static std::FILE *trace = [] {
        const char *file = std::getenv("M2_OPPONENT_BODY_TRACE");
        return file && *file ? std::fopen(file, "w") : nullptr;
    }();
    if (trace) {
        unsigned counts[3] = {}, near = 0;
        for (const auto &[command, count] : replaced) {
            const auto &poly = *candidates.at(command);
            const auto slot = rt::MobileIdentity::slot_index(poly.mobile_key.slot);
            counts[*slot % 3] += unsigned(count);
            near += poly.object_matrix[11] <= 3.4f; // the old cut-off range
        }
        std::fprintf(trace, "%zu,%u,%u,%u,%u,%u\n", scene.size(), removed, counts[0], counts[1], counts[2], near);
    }
    return &scene_;
}

} // namespace app
