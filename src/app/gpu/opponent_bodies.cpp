#include "app/gpu/opponent_bodies.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <locale>
#include <array>
#include <map>
#include <optional>
#include <utility>

namespace app {

// One body's faces from the stream: the count (1..max_faces), then each face.
static bool read_faces(std::istream &in, int max_faces, std::vector<OpponentBodies::Face> &body) {
    int face_count;
    if (!(in >> face_count) || face_count < 1 || face_count > max_faces) return false;
    body.resize(size_t(face_count));
    for (auto &face : body) {
        int count, surface;
        if (!(in >> count >> surface >> face.shade) || count < 3 || count > 4 || surface < 0 || surface >= 10 ||
            !(face.shade >= 0 && face.shade <= 2)) return false;
        face.count = uint8_t(count);
        face.surface = uint8_t(surface);
        for (int i = 0; i < count; ++i) {
            auto &v = face.vertices[i];
            for (float &x : v)
                if (!(in >> x) || !std::isfinite(x)) return false;
            if (std::abs(v[0]) > 3 || std::abs(v[1]) > 3 || std::abs(v[2]) > 3 || v[3] < 0 || v[3] > 1 || v[4] < 0 ||
                v[4] > 1) return false;
        }
    }
    return true;
}

bool OpponentBodies::load(const std::string &directory, int max_faces) {
    const auto root = std::filesystem::u8path(directory);
    std::ifstream meshes(root / "bodies.m2car");
    meshes.imbue(std::locale::classic());
    std::string version;
    if (!(meshes >> version) || version != "M2CAR1") return false;
    decltype(bodies_) bodies;
    for (auto &body : bodies)
        if (!read_faces(meshes, max_faces, body)) return false;
    meshes >> std::ws;
    if (!meshes.eof()) return false;
    // optional, the player's car: wheel.m2car, "M2WHEEL1" then one wheel in
    // the game wheel's own space (the hub at 0, the axle along x, the outer
    // face towards +x: a right wheel; mirrored for the left), its textures
    // the atlas's second body's surfaces
    wheel_.clear();
    if (std::ifstream wheels(root / "wheel.m2car"); wheels) {
        wheels.imbue(std::locale::classic());
        std::string tag;
        std::vector<Face> wheel;
        if (wheels >> tag && tag == "M2WHEEL1" && read_faces(wheels, max_faces, wheel) && (wheels >> std::ws).eof())
            wheel_ = std::move(wheel);
    }
    wheel_radius_ = 0;
    for (const auto &f : wheel_)
        for (unsigned i = 0; i < f.count; ++i)
            wheel_radius_ = std::max(wheel_radius_, std::sqrt(f.vertices[i][1] * f.vertices[i][1] +
                                                              f.vertices[i][2] * f.vertices[i][2]));

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

// The player's car outside the race: the mission select screen turns two of
// it (automatic and manual liveries), drawn from the same models but not
// keyed (MobileIdentity), so known by their ROM objects (Revision A): the
// bodies and their glass. Their wheels are keyed as the race's.
bool is_select_body(const rt::GeoPoly &p) {
    return !p.mobile_key.part && (p.object_oba == 0x95ff9du || p.object_oba == 0x960daeu);
}
bool is_select_glass(const rt::GeoPoly &p) {
    return !p.mobile_key.part && (p.object_oba == 0x961bbfu || p.object_oba == 0x961cb0u);
}
bool is_player_body(const rt::GeoPoly &p) {
    return (!p.mobile_key.slot && p.mobile_key.part == 0x7d68u) || is_select_body(p);
}
bool is_player_glass(const rt::GeoPoly &p) {
    return (!p.mobile_key.slot && p.mobile_key.part == 0x7d94u) || is_select_glass(p);
}

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

void OpponentBodies::set_player(uint32_t texture_base) {
    player_ = true;
    texture_base_ = texture_base;
    // body 0's face normals (Newell), turned outward: the winding that points
    // most faces away from the body's centre is taken for all
    player_normals_.clear();
    if (bodies_.empty() || bodies_[0].empty()) return;
    float centre[3] = {0, 0, 0};
    int corners = 0;
    for (const auto &f : bodies_[0])
        for (unsigned i = 0; i < f.count; ++i, ++corners)
            for (int a = 0; a < 3; ++a) centre[a] += f.vertices[i][a];
    for (float &c : centre) c /= float(corners);
    int out = 0;
    for (const auto &f : bodies_[0]) {
        float n[3] = {0, 0, 0}, mid[3] = {0, 0, 0};
        for (unsigned i = 0; i < f.count; ++i) {
            const auto &p = f.vertices[i], &q = f.vertices[(i + 1) % f.count];
            n[0] += (p[1] - q[1]) * (p[2] + q[2]);
            n[1] += (p[2] - q[2]) * (p[0] + q[0]);
            n[2] += (p[0] - q[0]) * (p[1] + q[1]);
            for (int a = 0; a < 3; ++a) mid[a] += p[a] / float(f.count);
        }
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        std::array<float, 3> u{0, 1, 0};
        if (len > 1e-12f) u = {n[0] / len, n[1] / len, n[2] / len};
        out += (u[0] * (mid[0] - centre[0]) + u[1] * (mid[1] - centre[1]) + u[2] * (mid[2] - centre[2])) > 0 ? 1 : -1;
        player_normals_.push_back(u);
    }
    if (out < 0)
        for (auto &u : player_normals_) u = {-u[0], -u[1], -u[2]};
}

// luma = a + w . n over the original body's faces this frame (object space,
// normals turned away from the body's centre); kept from the last frame
// when they are too few to tell.
// luma = a + w . n over one car's original body faces (object space, normals
// turned away from the body's centre); false when they are too few to tell.
static bool fit_light(const std::vector<const rt::GeoPoly *> &faces, float fit[4]) {
    if (faces.size() < 8) return false;
    float centre[3] = {0, 0, 0};
    int corners = 0;
    for (const auto *p : faces)
        for (int k = 0; k < p->source_num_vertices; ++k, ++corners)
            for (int a = 0; a < 3; ++a) centre[a] += p->source_v[k][a];
    for (float &c : centre) c /= float(corners);
    double m[4][5] = {}; // normal equations for (a, wx, wy, wz), the right-hand side in column 4
    for (const auto *p : faces) {
        float n[3] = {0, 0, 0}, mid[3] = {0, 0, 0};
        const int count = p->source_num_vertices;
        for (int k = 0; k < count; ++k) {
            const float *a = p->source_v[k], *b = p->source_v[(k + 1) % count];
            n[0] += (a[1] - b[1]) * (a[2] + b[2]);
            n[1] += (a[2] - b[2]) * (a[0] + b[0]);
            n[2] += (a[0] - b[0]) * (a[1] + b[1]);
            for (int x = 0; x < 3; ++x) mid[x] += a[x] / float(count);
        }
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (!(len > 1e-12f)) continue;
        float turn = 1.0f / len;
        if ((n[0] * (mid[0] - centre[0]) + n[1] * (mid[1] - centre[1]) + n[2] * (mid[2] - centre[2])) < 0) turn = -turn;
        const double row[4] = {1.0, n[0] * turn, n[1] * turn, n[2] * turn};
        for (int r = 0; r < 4; ++r) {
            for (int s = 0; s < 4; ++s) m[r][s] += row[r] * row[s];
            m[r][4] += row[r] * p->luma;
        }
    }
    for (int col = 0; col < 4; ++col) { // Gauss-Jordan, partial pivoting
        int pivot = col;
        for (int r = col + 1; r < 4; ++r)
            if (std::abs(m[r][col]) > std::abs(m[pivot][col])) pivot = r;
        if (std::abs(m[pivot][col]) < 1e-6) return false;
        for (int s = 0; s < 5; ++s) std::swap(m[col][s], m[pivot][s]);
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const double f = m[r][col] / m[col][col];
            for (int s = col; s < 5; ++s) m[r][s] -= f * m[col][s];
        }
    }
    for (int r = 0; r < 4; ++r) fit[r] = float(m[r][4] / m[r][r]);
    return true;
}

// Each car's fit (by its body's emission), and the brightest of them as the
// reference: the select screen dims the car not chosen, and so must the new
// body. A car this frame cannot tell keeps the last reference's fit.
void OpponentBodies::fit_player_light(const std::vector<rt::GeoPoly> &scene) {
    std::map<uint32_t, std::vector<const rt::GeoPoly *>> cars;
    for (const auto &p : scene)
        if (is_player_body(p) && p.source_num_vertices >= 3) cars[p.object_command].push_back(&p);
    light_fits_.clear();
    float reference = 0;
    for (const auto &[command, faces] : cars) {
        std::array<float, 4> fit;
        if (!fit_light(faces, fit.data())) continue;
        light_fits_[command] = fit;
        reference = std::max(reference, fit[0] + std::sqrt(fit[1] * fit[1] + fit[2] * fit[2] + fit[3] * fit[3]));
        std::copy(fit.begin(), fit.end(), light_fit_);
        light_fitted_ = true;
    }
    if (reference > 1.0f) light_reference_ = reference;
}

const std::vector<rt::GeoPoly> *OpponentBodies::replace(const std::vector<rt::GeoPoly> &scene) {
    if (atlas_.empty()) return nullptr;
    if (player_) fit_player_light(scene);
    // A command is one emission, including all its faces. Identity comes from
    // the runtime's established opponent slots for supported revisions.
    std::map<uint32_t, const rt::GeoPoly *> candidates;
    std::map<uint32_t, size_t> replaced;
    for (const auto &poly : scene) {
        const auto key = poly.mobile_key;
        if (player_) { // the player's car: its body (slot 0, or the select screen's); its glass goes with it
            if (!poly.object_command || poly.object_direct) continue;
            if (is_player_glass(poly)) replaced.emplace(poly.object_command, 0);
            if (!is_player_body(poly) || poly.object_matrix[11] <= 0.f) continue;
            candidates.try_emplace(poly.object_command, &poly);
            continue;
        }
        if (!rt::is_opponent_body_part(key.part) || !poly.object_command || poly.object_direct) continue;
        // Every distance, the contact range included: a nearer cut-off showed
        // the original car again whenever the player touched an opponent.
        if (poly.object_matrix[11] <= 0.f) continue;
        candidates.try_emplace(poly.object_command, &poly);
    }
    if (candidates.empty()) return nullptr;

    scene_.clear();
    for (const auto &[command, source] : candidates) {
        const auto slot = player_ ? std::optional<unsigned>(0u) : rt::MobileIdentity::slot_index(source->mobile_key.slot);
        if (!slot) continue;
        const size_t variant = *slot % bodies_.size();
        const auto &m = source->object_matrix;
        const size_t first = scene_.size();
        // the game's light on the new faces: luma = a + w . n, the brightest
        // direction (a + |w|) drawing the texture as made (shade 1)
        // this car's fit (else the last one), over the brightest car's
        const float *fit = light_fit_;
        if (const auto it = light_fits_.find(command); it != light_fits_.end()) fit = it->second.data();
        const float bright = player_ && light_fitted_ ? light_reference_ : 0.0f;
        for (size_t f = 0; f < bodies_[variant].size(); ++f) {
            const auto &face = bodies_[variant][f];
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
                out.ox = v[0], out.oy = v[1], out.oz = v[2];
                depth += out.p[0];
            }
            if (std::all_of(vertices, vertices + face.count, [](const auto &v) { return v.p[0] <= near_z; }))
                continue;
            const auto &a = vertices[0], &b = vertices[1], &c = vertices[2];
            const float ux = b.x - a.x, uy = b.y - a.y, uz = b.p[0] - a.p[0];
            const float vx = c.x - a.x, vy = c.y - a.y, vz = c.p[0] - a.p[0];
            const float facing = (uy * vz - uz * vy) * a.x + (uz * vx - ux * vz) * a.y +
                                 (ux * vy - uy * vx) * a.p[0];
            const bool away = facing >= 0;
            if (away && !player_) continue;
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
            poly.local_texture = away ? kShadowOnlyTexture : uint8_t(texture_base_ + variant * 10 + face.surface + 1);
            poly.luma = uint8_t(std::lround(face.shade * 127));
            if (player_) { // the original's identity, and the game's light on this face
                poly.mobile_key = source->mobile_key;
                poly.object_command = source->object_command;
                poly.object_oba = source->object_oba;
                poly.object_focus_x = source->object_focus_x;
                poly.object_focus_y = source->object_focus_y;
                std::copy_n(source->object_matrix, 12, poly.object_matrix);
                if (bright > 1.0f && f < player_normals_.size()) {
                    const auto &n = player_normals_[f];
                    const float *w = fit + 1;
                    const float lit = (fit[0] + w[0] * n[0] + w[1] * n[1] + w[2] * n[2]) / bright;
                    poly.luma = uint8_t(std::lround(std::clamp(lit, 0.08f, 1.0f) * 127));
                }
            }
            for (unsigned i = 0; i < count; ++i) {
                vertices[i].x *= source->object_focus_x;
                vertices[i].y *= source->object_focus_y;
                poly.v[i] = vertices[i];
            }
        }
        if (scene_.size() > first) replaced[command] = scene_.size() - first;
    }
    // The player's own wheels (wheel.m2car) in place of the game's: one per
    // wheel emission, through that wheel's matrix (its spin and steering),
    // mirrored for the left ones, raised along the body's up axis to stand on
    // the same ground as the game's wheel (radius 0.371)
    if (player_ && !wheel_.empty() && !candidates.empty()) {
        // a wheel's body: the nearest (the select screen turns two cars)
        auto body_of = [&](const rt::GeoPoly &w) {
            const rt::GeoPoly *best = candidates.begin()->second;
            float d_best = 1e30f;
            for (const auto &[command, body] : candidates) {
                const float *b = body->object_matrix;
                const float dx = b[9] - w.object_matrix[9], dy = b[10] - w.object_matrix[10], dz = b[11] - w.object_matrix[11];
                if (dx * dx + dy * dy + dz * dz < d_best) d_best = dx * dx + dy * dy + dz * dz, best = body;
            }
            return best;
        };
        const float lift = wheel_radius_ - 0.371f;
        std::map<uint32_t, const rt::GeoPoly *> wheels;
        for (const auto &p : scene) {
            const uint32_t part = p.mobile_key.part;
            if (p.mobile_key.slot || !p.object_command || p.object_direct ||
                !(part == 0x810cu || part == 0x8198u || part == 0x8210u || part == 0x8288u)) continue;
            wheels.try_emplace(p.object_command, &p);
        }
        for (const auto &[command, source] : wheels) {
            const auto &m = source->object_matrix;
            const rt::GeoPoly *body = body_of(*source);
            const float *bm = body->object_matrix;
            const float up[3] = {bm[3], bm[4], bm[5]};
            // the wheel as lit as its car: its brightest over the brightest car's (a dimmed car, dim wheels)
            float dim = 1.0f;
            if (const auto it = light_fits_.find(body->object_command); it != light_fits_.end() && light_reference_ > 1.0f) {
                const auto &f = it->second;
                dim = std::clamp((f[0] + std::sqrt(f[1] * f[1] + f[2] * f[2] + f[3] * f[3])) / light_reference_, 0.08f, 1.0f);
            }
            const bool left = source->mobile_key.part == 0x810cu || source->mobile_key.part == 0x8210u;
            const size_t first = scene_.size();
            for (const auto &face : wheel_) {
                rt::GeoVertex vertices[8];
                float depth = 0;
                for (unsigned i = 0; i < face.count; ++i) {
                    const auto &v = face.vertices[left ? face.count - 1 - i : i]; // mirrored: the winding kept
                    const float x = left ? -v[0] : v[0];
                    auto &out = vertices[i];
                    out.x = x * m[0] + v[1] * m[3] + v[2] * m[6] + m[9] + up[0] * lift;
                    out.y = x * m[1] + v[1] * m[4] + v[2] * m[7] + m[10] + up[1] * lift;
                    out.p[0] = x * m[2] + v[1] * m[5] + v[2] * m[8] + m[11] + up[2] * lift;
                    out.p[1] = v[3] * 8;
                    out.p[2] = v[4] * 8;
                    out.ox = x, out.oy = v[1], out.oz = v[2];
                    depth += out.p[0];
                }
                if (std::all_of(vertices, vertices + face.count, [](const auto &v) { return v.p[0] <= near_z; }))
                    continue;
                const auto &a = vertices[0], &b = vertices[1], &cc = vertices[2];
                const float ux = b.x - a.x, uy = b.y - a.y, uz = b.p[0] - a.p[0];
                const float vx = cc.x - a.x, vy = cc.y - a.y, vz = cc.p[0] - a.p[0];
                const bool away = (uy * vz - uz * vy) * a.x + (uz * vx - ux * vz) * a.y + (ux * vy - uy * vx) * a.p[0] >= 0;
                const unsigned count = clip_near(vertices, face.count);
                if (count < 3) continue;
                auto &poly = scene_.emplace_back();
                poly.z = rt::Geo::z_sort_value(depth / face.count, source->object_z_adjust);
                std::copy_n(source->viewport, 4, poly.viewport);
                std::copy_n(source->center, 2, poly.center);
                poly.window = source->window;
                poly.num_vertices = uint8_t(count);
                poly.local_texture = away ? kShadowOnlyTexture : uint8_t(texture_base_ + 10 + face.surface + 1);
                poly.luma = uint8_t(std::lround(std::clamp(face.shade * dim, 0.0f, 2.0f) * 127));
                poly.mobile_key = source->mobile_key;
                poly.object_command = source->object_command;
                poly.object_oba = source->object_oba;
                poly.object_focus_x = source->object_focus_x;
                poly.object_focus_y = source->object_focus_y;
                std::copy_n(source->object_matrix, 12, poly.object_matrix);
                for (unsigned i = 0; i < count; ++i) {
                    vertices[i].x *= source->object_focus_x;
                    vertices[i].y *= source->object_focus_y;
                    poly.v[i] = vertices[i];
                }
            }
            replaced[command] = scene_.size() - first; // the game's wheel goes
        }
    }
    if (candidates.empty() || replaced.empty()) return nullptr;

    // Keep the original wheel, shadow and other emissions. Only substitute
    // the same body command in the same camera window.
    // The player's wheels: scaled by kPlayerWheelScale about their centre
    // (the mean of their corners, camera space), then raised along the body's
    // up axis so that they stand on the same ground.
    auto is_player_wheel = [](const rt::GeoPoly &p) {
        const uint32_t part = p.mobile_key.part;
        return !p.mobile_key.slot && (part == 0x810cu || part == 0x8198u || part == 0x8210u || part == 0x8288u);
    };
    std::map<std::pair<uint32_t, uint32_t>, std::array<float, 4>> wheel_centre; // (part, command) -> sum xyz, count
    float up[3] = {0, 0, 0};
    if (player_ && !candidates.empty()) {
        const float *m = candidates.begin()->second->object_matrix;
        up[0] = m[3], up[1] = m[4], up[2] = m[5]; // the body's +y in camera space, with its scale
        for (const auto &p : scene) {
            if (!is_player_wheel(p)) continue;
            auto &c = wheel_centre[{p.mobile_key.part, p.object_command}];
            for (int k = 0; k < p.num_vertices; ++k) {
                c[0] += p.v[k].x / p.object_focus_x, c[1] += p.v[k].y / p.object_focus_y, c[2] += p.v[k].p[0];
                c[3] += 1;
            }
        }
    }
    // the game's own reflection on the player's windows (Geo's window
    // reflection: an emission of oba 4, texture 0x4409 / 7064, drawn with the
    // glass's projection) goes with the glass it lies on
    std::vector<const float *> glass_matrices;
    if (player_)
        for (const auto &p : scene)
            if (is_player_glass(p)) glass_matrices.push_back(p.object_matrix);
    auto is_window_reflection = [&](const rt::GeoPoly &p) {
        if (p.object_oba != 4 || p.texheader[0] != 0x4409 || p.texheader[2] != 7064) return false;
        for (const float *m : glass_matrices)
            if (std::equal(m, m + 12, p.object_matrix)) return true;
        return false;
    };
    constexpr float kHornetWheelRadius = 0.371f;
    scene_.reserve(scene_.size() + scene.size());
    unsigned removed = 0;
    for (const auto &poly : scene) {
        if (replaced.contains(poly.object_command) || (player_ && is_window_reflection(poly))) {
            ++removed;
            continue;
        }
        scene_.push_back(poly);
        if (!player_ || !is_player_wheel(poly)) continue;
        const auto it = wheel_centre.find({poly.mobile_key.part, poly.object_command});
        if (it == wheel_centre.end() || it->second[3] < 1) continue;
        const float s = kPlayerWheelScale, lift = (s - 1.0f) * kHornetWheelRadius;
        const float c[3] = {it->second[0] / it->second[3], it->second[1] / it->second[3], it->second[2] / it->second[3]};
        auto &out = scene_.back();
        for (int k = 0; k < out.num_vertices; ++k) {
            auto &v = out.v[k];
            const float p[3] = {v.x / out.object_focus_x, v.y / out.object_focus_y, v.p[0]};
            float q[3];
            for (int a = 0; a < 3; ++a) q[a] = c[a] + s * (p[a] - c[a]) + up[a] * lift;
            v.x = q[0] * out.object_focus_x, v.y = q[1] * out.object_focus_y, v.p[0] = q[2];
        }
    }

    static std::FILE *trace = [] {
        const char *file = std::getenv("M2_OPPONENT_BODY_TRACE");
        return file && *file ? std::fopen(file, "w") : nullptr;
    }();
    if (trace && !player_) {
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
