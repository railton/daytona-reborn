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
        if (!(in >> count >> surface >> face.shade) || count < 3 || count > 4 || surface < 0 || surface >= 20 ||
            !(face.shade >= 0 && face.shade <= 2)) return false;
        face.count = uint8_t(count);
        // 10..19: a decal on the skin (a line, a sticker) with surface - 10's texture, drawn in front of
        // the faces it lies on (the game's order is by each face's mean depth: a big face beside a
        // thin strip on it would cover it)
        face.decal = surface >= 10;
        face.surface = uint8_t(surface % 10);
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
    // the atlas's second body's surfaces. "M2WHEEL2 FRONT_Z REAR_Z HALF_TRACK"
    // also places the wheels: the wheel's origin (its x = 0 plane, on the
    // axle) at those body-space axles and |x| instead of the game car's (z
    // +1.4135 and -1.399, |x| 0.525: the game's wheel origin is its inner
    // face); for a car of another size, the drawing only, the physics stay
    wheel_.clear();
    wheel_axles_ = {};
    if (std::ifstream wheels(root / "wheel.m2car"); wheels) {
        wheels.imbue(std::locale::classic());
        std::string tag;
        std::vector<Face> wheel;
        WheelAxles axles;
        bool ok = wheels >> tag && (tag == "M2WHEEL1" || tag == "M2WHEEL2");
        if (ok && tag == "M2WHEEL2") {
            ok = wheels >> axles.front_z >> axles.rear_z >> axles.half_track && std::abs(axles.front_z) <= 3 &&
                 std::abs(axles.rear_z) <= 3 && axles.front_z > axles.rear_z + 1.0f && axles.half_track > 0.3f &&
                 axles.half_track < 1.2f;
            axles.valid = ok;
        }
        if (ok && read_faces(wheels, max_faces, wheel) && (wheels >> std::ws).eof()) {
            wheel_ = std::move(wheel);
            wheel_axles_ = axles;
        }
    }
    wheel_radius_ = 0;
    for (const auto &f : wheel_)
        for (unsigned i = 0; i < f.count; ++i)
            wheel_radius_ = std::max(wheel_radius_, std::sqrt(f.vertices[i][1] * f.vertices[i][1] +
                                                              f.vertices[i][2] * f.vertices[i][2]));

    // optional: car.ini, "key=value" lines: glass_see_through=1, glass_opacity=F
    glass_opacity_ = 1.0f;
    if (std::ifstream ini(root / "car.ini"); ini)
        for (std::string line; std::getline(ini, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "glass_see_through=1") glass_opacity_ = 0.5f;
            else if (line.rfind("glass_opacity=", 0) == 0) {
                char *end = nullptr;
                const float f = std::strtof(line.c_str() + 14, &end);
                if (end && !*end && f >= 0.25f && f <= 1.0f) glass_opacity_ = f;
            }
        }

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
// The cockpit view (VR2) draws its own model of the bonnet and dashboard
// (Revision A's ROM object 0x94fff9, keyed as the body), in the body's frame
// with the camera inside the car. Its bonnet and wings (the livery texture
// 0x6412, and 0x4400 at 0x1a9a) are painted with the new body's surfaces;
// the dashboard stays as it is.
bool is_cockpit_bonnet(const rt::GeoPoly &p) {
    return !p.mobile_key.slot && p.mobile_key.part == 0x7d68u && p.object_oba == 0x94fff9u &&
           (p.texheader[0] == 0x6412u || (p.texheader[0] == 0x4400u && p.texheader[2] == 0x1a9au));
}
bool is_player_body(const rt::GeoPoly &p) {
    return (!p.mobile_key.slot && p.mobile_key.part == 0x7d68u && p.object_oba != 0x94fff9u) || is_select_body(p);
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

// Faces' normals (Newell), turned outward: the winding that points most
// faces away from the centre is taken for all; and each face's area.
static void outward_normals(const std::vector<OpponentBodies::Face> &faces, std::vector<std::array<float, 3>> &normals,
                            std::vector<float> &areas) {
    normals.clear();
    areas.clear();
    if (faces.empty()) return;
    float centre[3] = {0, 0, 0};
    int corners = 0;
    for (const auto &f : faces)
        for (unsigned i = 0; i < f.count; ++i, ++corners)
            for (int a = 0; a < 3; ++a) centre[a] += f.vertices[i][a];
    for (float &c : centre) c /= float(corners);
    int out = 0;
    for (const auto &f : faces) {
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
        normals.push_back(u);
        areas.push_back(len * 0.5f);
    }
    if (out < 0)
        for (auto &u : normals) u = {-u[0], -u[1], -u[2]};
}

// Each face's corners' smooth normals, as the renderer's smooth car shading
// makes them (the faces sharing the corner's position whose normal is
// within 50 degrees of the face's, weighted by area), once: the body does
// not change shape, and doing it each frame for 20,000 faces cost 2.5 ms.
static void smooth_corners(const std::vector<OpponentBodies::Face> &faces, const std::vector<std::array<float, 3>> &normals,
                           const std::vector<float> &areas, std::vector<std::array<std::array<float, 3>, 4>> &out) {
    out.assign(faces.size(), {});
    std::map<std::array<float, 3>, std::vector<uint32_t>> at;
    for (size_t f = 0; f < faces.size(); ++f)
        for (unsigned i = 0; i < faces[f].count; ++i)
            at[{faces[f].vertices[i][0], faces[f].vertices[i][1], faces[f].vertices[i][2]}].push_back(uint32_t(f));
    constexpr float kCrease = 0.6428f; // cos 50 degrees, as the renderer's
    for (size_t f = 0; f < faces.size(); ++f) {
        const auto &self = normals[f];
        for (unsigned i = 0; i < faces[f].count; ++i) {
            float n[3] = {0, 0, 0};
            for (uint32_t g : at[{faces[f].vertices[i][0], faces[f].vertices[i][1], faces[f].vertices[i][2]}]) {
                const auto &o = normals[g];
                if (o[0] * self[0] + o[1] * self[1] + o[2] * self[2] < kCrease) continue;
                for (int a = 0; a < 3; ++a) n[a] += o[a] * areas[g];
            }
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            out[f][i] = len > 1e-12f ? std::array<float, 3>{n[0] / len, n[1] / len, n[2] / len} : self;
        }
    }
}

// A model-space normal through an object matrix (its rotation), unit length.
static void to_camera(const float *m, const float n[3], float out[3]) {
    out[0] = n[0] * m[0] + n[1] * m[3] + n[2] * m[6];
    out[1] = n[0] * m[1] + n[1] * m[4] + n[2] * m[7];
    out[2] = n[0] * m[2] + n[1] * m[5] + n[2] * m[8];
    const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (len > 1e-12f)
        for (int a = 0; a < 3; ++a) out[a] /= len;
}

void OpponentBodies::set_player(uint32_t texture_base) {
    player_ = true;
    texture_base_ = texture_base;
    std::vector<float> areas;
    outward_normals(wheel_, wheel_normals_, areas);
    smooth_corners(wheel_, wheel_normals_, areas, wheel_corners_);
    player_normals_.clear();
    if (bodies_.empty() || bodies_[0].empty()) return;
    outward_normals(bodies_[0], player_normals_, areas);
    smooth_corners(bodies_[0], player_normals_, areas, body_corners_);
    build_top_view();
}

// For the cockpit view: which surface body 0 shows from above at each (x, z),
// and the surfaces whose texture is a plane over (x, z), fitted from their
// corners (u = a x + b z + c; kept only when it fits every corner).
void OpponentBodies::build_top_view() {
    top_surface_.assign(size_t(kTopGridX) * kTopGridZ, -1);
    top_planes_ = {};
    const auto &body = bodies_[0];
    if (body.empty()) return;
    float box[4] = {1e9f, 1e9f, -1e9f, -1e9f};
    for (const auto &f : body)
        for (unsigned i = 0; i < f.count; ++i) {
            box[0] = std::min(box[0], f.vertices[i][0]), box[1] = std::min(box[1], f.vertices[i][2]);
            box[2] = std::max(box[2], f.vertices[i][0]), box[3] = std::max(box[3], f.vertices[i][2]);
        }
    std::copy_n(box, 4, top_box_);
    const float cw = (box[2] - box[0]) / kTopGridX, ch = (box[3] - box[1]) / kTopGridZ;
    if (!(cw > 0 && ch > 0)) return;
    std::vector<float> height(top_surface_.size(), -1e9f);
    for (const auto &f : body) {
        // each triangle of the face over the cells whose centre it covers
        for (unsigned t = 1; t + 1 < f.count; ++t) {
            const auto &a = f.vertices[0], &b = f.vertices[t], &c = f.vertices[t + 1];
            const float det = (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]);
            if (std::abs(det) < 1e-9f) continue;
            const int i0 = std::max(0, int((std::min({a[0], b[0], c[0]}) - box[0]) / cw));
            const int i1 = std::min(kTopGridX - 1, int((std::max({a[0], b[0], c[0]}) - box[0]) / cw));
            const int j0 = std::max(0, int((std::min({a[2], b[2], c[2]}) - box[1]) / ch));
            const int j1 = std::min(kTopGridZ - 1, int((std::max({a[2], b[2], c[2]}) - box[1]) / ch));
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) {
                    const float x = box[0] + (i + .5f) * cw, z = box[1] + (j + .5f) * ch;
                    const float s = ((x - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (z - a[2])) / det;
                    const float r = ((b[0] - a[0]) * (z - a[2]) - (x - a[0]) * (b[2] - a[2])) / det;
                    if (s < 0 || r < 0 || s + r > 1) continue;
                    const float y = a[1] + s * (b[1] - a[1]) + r * (c[1] - a[1]);
                    const size_t k = size_t(j) * kTopGridX + i;
                    if (y > height[k]) height[k] = y, top_surface_[k] = int8_t(f.surface);
                }
        }
    }
    for (int surface = 0; surface < 10; ++surface) {
        if (surface == int(kPlayerGlassSurface)) continue; // seen through, not a paint
        double m[3][3] = {}, ru[3] = {}, rv[3] = {};
        int corners = 0;
        for (const auto &f : body) {
            if (f.surface != surface) continue;
            for (unsigned i = 0; i < f.count; ++i, ++corners) {
                const double q[3] = {f.vertices[i][0], f.vertices[i][2], 1};
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) m[r][c] += q[r] * q[c];
                    ru[r] += q[r] * f.vertices[i][3], rv[r] += q[r] * f.vertices[i][4];
                }
            }
        }
        if (corners < 3) continue;
        // Cramer's rule on the 3 x 3 normal equations
        auto det3 = [](const double a[3][3]) {
            return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                   a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        };
        const double d = det3(m);
        if (std::abs(d) < 1e-12) continue;
        Plane plane;
        for (int k = 0; k < 3; ++k) {
            double mu[3][3], mv[3][3];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) mu[r][c] = c == k ? ru[r] : m[r][c], mv[r][c] = c == k ? rv[r] : m[r][c];
            plane.u[k] = float(det3(mu) / d), plane.v[k] = float(det3(mv) / d);
        }
        bool fits = true;
        for (const auto &f : body) {
            if (f.surface != surface) continue;
            for (unsigned i = 0; i < f.count && fits; ++i) {
                const auto &q = f.vertices[i];
                fits = std::abs(plane.u[0] * q[0] + plane.u[1] * q[2] + plane.u[2] - q[3]) < 2e-3f &&
                       std::abs(plane.v[0] * q[0] + plane.v[1] * q[2] + plane.v[2] - q[4]) < 2e-3f;
            }
        }
        plane.valid = fits;
        top_planes_[size_t(surface)] = plane;
    }
}

// One of the cockpit's bonnet faces painted with the surface the new body
// shows from above at its centre; false when that surface is not a plane
// over (x, z) (the face is then left as the game drew it).
bool OpponentBodies::paint_cockpit(const rt::GeoPoly &source, rt::GeoPoly &out) const {
    if (top_surface_.empty() || !source.num_vertices) return false;
    float x = 0, z = 0;
    for (int i = 0; i < source.num_vertices; ++i) x += source.v[i].ox, z += source.v[i].oz;
    x /= source.num_vertices, z /= source.num_vertices;
    const float cw = (top_box_[2] - top_box_[0]) / kTopGridX, ch = (top_box_[3] - top_box_[1]) / kTopGridZ;
    const int i = std::clamp(int((x - top_box_[0]) / cw), 0, kTopGridX - 1);
    const int j = std::clamp(int((z - top_box_[1]) / ch), 0, kTopGridZ - 1);
    // where the highest face is not a paint seen from above (the glass, a
    // wing's edge), the nearest cell that is, within 8 cells
    int surface = -1;
    float best = 1e30f;
    for (int dj = -8; dj <= 8; ++dj)
        for (int di = -8; di <= 8; ++di) {
            const int ci = i + di, cj = j + dj;
            if (ci < 0 || cj < 0 || ci >= kTopGridX || cj >= kTopGridZ) continue;
            const int s = top_surface_[size_t(cj) * kTopGridX + ci];
            const float d = (di * cw) * (di * cw) + (dj * ch) * (dj * ch);
            if (s >= 0 && top_planes_[size_t(s)].valid && d < best) best = d, surface = s;
        }
    if (surface < 0) return false;
    const auto &plane = top_planes_[size_t(surface)];
    out = rt::GeoPoly{};
    out.z = source.z;
    std::copy_n(source.viewport, 4, out.viewport);
    std::copy_n(source.center, 2, out.center);
    out.window = source.window;
    out.num_vertices = source.num_vertices;
    out.local_texture = uint8_t(texture_base_ + surface + 1);
    // the game's light on this face, over the brightest it gives the bonnet
    const float lit = light_reference_ > 1.0f ? float(source.luma) / light_reference_ : 1.0f;
    out.luma = uint8_t(std::lround(std::clamp(lit, 0.08f, 1.0f) * 127));
    out.mobile_key = source.mobile_key;
    out.object_command = source.object_command;
    out.object_oba = source.object_oba;
    out.object_focus_x = source.object_focus_x;
    out.object_focus_y = source.object_focus_y;
    std::copy_n(source.object_matrix, 12, out.object_matrix);
    for (int k = 0; k < source.num_vertices; ++k) {
        auto v = source.v[k];
        v.p[1] = std::clamp(plane.u[0] * v.ox + plane.u[1] * v.oz + plane.u[2], 0.0f, 1.0f) * 8;
        v.p[2] = std::clamp(plane.v[0] * v.ox + plane.v[1] * v.oz + plane.v[2], 0.0f, 1.0f) * 8;
        out.v[k] = v;
    }
    return true;
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
        if ((is_player_body(p) || is_cockpit_bonnet(p)) && p.source_num_vertices >= 3) cars[p.object_command].push_back(&p);
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
    bool cockpit = false;
    for (const auto &poly : scene) {
        const auto key = poly.mobile_key;
        if (player_) { // the player's car: its body (slot 0, or the select screen's); its glass goes with it
            if (!poly.object_command || poly.object_direct) continue;
            if (is_player_glass(poly)) replaced.emplace(poly.object_command, 0);
            if (is_cockpit_bonnet(poly)) {
                cockpit = true;
                continue;
            }
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
    if (candidates.empty() && !cockpit) return nullptr;

    scene_.clear();
    corners_.clear();
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
            const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx; // outward
            const bool away = nx * a.x + ny * a.y + nz * a.p[0] >= 0;
            // turned from the camera: kept only to cast the shadow, and only facing the sun
            if (away && (!player_ || !casts_shadow(nx, ny, nz))) continue;
            // A face crossing the near plane (an opponent against the player's
            // car) is cut there rather than dropped, so the body has no holes.
            const bool whole = std::all_of(vertices, vertices + face.count, [](const auto &v) { return v.p[0] > near_z; });
            const unsigned count = clip_near(vertices, face.count);
            if (count < 3) continue;
            auto &poly = scene_.emplace_back();
            auto &corner = corners_.emplace_back();
            float sort_depth = depth / face.count;
            if (face.decal) { // its nearest corner, 2 cm nearer still
                sort_depth = vertices[0].p[0];
                for (unsigned i = 1; i < count; ++i) sort_depth = std::min(sort_depth, vertices[i].p[0]);
                sort_depth = std::max(sort_depth - 0.02f, near_z);
            }
            poly.z = rt::Geo::z_sort_value(sort_depth, source->object_z_adjust);
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
                // its corners' smooth normals and the light at them, from the shape
                if (!away && whole && variant == 0 && f < body_corners_.size()) {
                    corner.valid = true;
                    for (unsigned i = 0; i < count; ++i) {
                        const auto &n = body_corners_[f][i];
                        to_camera(m, n.data(), corner.normal[i]);
                        float lit = float(poly.luma) / 127;
                        if (bright > 1.0f) {
                            const float *w = fit + 1;
                            lit = std::clamp((fit[0] + w[0] * n[0] + w[1] * n[1] + w[2] * n[2]) / bright, 0.08f, 1.0f);
                        }
                        corner.light[i] = lit * 127;
                    }
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
            // M2WHEEL2: the hub moved to the new car's axle and track, along
            // the body's own axes (its height and the game's steering kept)
            float shift[3] = {0, 0, 0};
            if (wheel_axles_.valid) {
                const float d[3] = {m[9] - bm[9], m[10] - bm[10], m[11] - bm[11]};
                const float ax[3] = {bm[0], bm[1], bm[2]}, az[3] = {bm[6], bm[7], bm[8]};
                const float xx = ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2];
                const float zz = az[0] * az[0] + az[1] * az[1] + az[2] * az[2];
                if (xx > 1e-12f && zz > 1e-12f) {
                    const float lx = (d[0] * ax[0] + d[1] * ax[1] + d[2] * ax[2]) / xx;
                    const float lz = (d[0] * az[0] + d[1] * az[1] + d[2] * az[2]) / zz;
                    const float dx = (lx < 0 ? -wheel_axles_.half_track : wheel_axles_.half_track) - lx;
                    const float dz = (lz > 0 ? wheel_axles_.front_z : wheel_axles_.rear_z) - lz;
                    for (int a = 0; a < 3; ++a) shift[a] = ax[a] * dx + az[a] * dz;
                }
            }
            const size_t first = scene_.size();
            for (const auto &face : wheel_) {
                rt::GeoVertex vertices[8];
                float depth = 0;
                for (unsigned i = 0; i < face.count; ++i) {
                    const auto &v = face.vertices[left ? face.count - 1 - i : i]; // mirrored: the winding kept
                    const float x = left ? -v[0] : v[0];
                    auto &out = vertices[i];
                    out.x = x * m[0] + v[1] * m[3] + v[2] * m[6] + m[9] + up[0] * lift + shift[0];
                    out.y = x * m[1] + v[1] * m[4] + v[2] * m[7] + m[10] + up[1] * lift + shift[1];
                    out.p[0] = x * m[2] + v[1] * m[5] + v[2] * m[8] + m[11] + up[2] * lift + shift[2];
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
                const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                const bool away = nx * a.x + ny * a.y + nz * a.p[0] >= 0;
                if (away && !casts_shadow(nx, ny, nz)) continue;
                const bool whole = std::all_of(vertices, vertices + face.count, [](const auto &v) { return v.p[0] > near_z; });
                const unsigned count = clip_near(vertices, face.count);
                if (count < 3) continue;
                auto &poly = scene_.emplace_back();
                auto &corner = corners_.emplace_back();
                poly.z = rt::Geo::z_sort_value(depth / face.count, source->object_z_adjust);
                std::copy_n(source->viewport, 4, poly.viewport);
                std::copy_n(source->center, 2, poly.center);
                poly.window = source->window;
                poly.num_vertices = uint8_t(count);
                poly.local_texture = away ? kShadowOnlyTexture : uint8_t(texture_base_ + 10 + face.surface + 1);
                poly.luma = uint8_t(std::lround(std::clamp(face.shade * dim, 0.0f, 2.0f) * 127));
                const size_t wf = size_t(&face - wheel_.data());
                if (!away && whole && wf < wheel_corners_.size()) { // mirrored: x turned, the corners reversed
                    corner.valid = true;
                    for (unsigned i = 0; i < count; ++i) {
                        const auto &n = wheel_corners_[wf][left ? face.count - 1 - i : i];
                        const float nm[3] = {left ? -n[0] : n[0], n[1], n[2]};
                        to_camera(m, nm, corner.normal[i]);
                        corner.light[i] = poly.luma;
                    }
                }
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
    if ((candidates.empty() || replaced.empty()) && !cockpit) return nullptr;

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
        if (player_ && is_cockpit_bonnet(poly)) {
            rt::GeoPoly painted;
            scene_.push_back(paint_cockpit(poly, painted) ? painted : poly);
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
    corners_.resize(scene_.size()); // the game's own polygons: found by the renderer
    return &scene_;
}

} // namespace app


