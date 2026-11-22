// Point batching must preserve the original three scalar rasterizer writes.
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>
#define private public
#include "runtime/geo.h"
#undef private
#include "../src/runtime/geo.cpp"
#include "../src/compat/bit_cast.h"
namespace {
bool same_poly(const rt::GeoPoly &a, const rt::GeoPoly &b) {
    if (a.z != b.z || a.luma != b.luma || a.texlod != b.texlod || a.window != b.window ||
        a.reverse != b.reverse || a.num_vertices != b.num_vertices) return false;
    for (unsigned i = 0; i < 4; ++i)
        if (a.texheader[i] != b.texheader[i] || a.viewport[i] != b.viewport[i]) return false;
    for (unsigned i = 0; i < 2; ++i) if (a.center[i] != b.center[i]) return false;
    for (unsigned i = 0; i < a.num_vertices; ++i) {
        if (compat::bit_cast<uint32_t>(a.v[i].x) != compat::bit_cast<uint32_t>(b.v[i].x) ||
            compat::bit_cast<uint32_t>(a.v[i].y) != compat::bit_cast<uint32_t>(b.v[i].y)) return false;
        for (unsigned j = 0; j < 3; ++j)
            if (compat::bit_cast<uint32_t>(a.v[i].p[j]) != compat::bit_cast<uint32_t>(b.v[i].p[j])) return false;
    }
    return true;
}
void reset(rt::Geo &geo, unsigned trial, unsigned index, bool triangle) {
    const uint16_t *texture = geo.raster_.texture_rom;
    const uint32_t texture_mask = geo.raster_.texture_rom_mask;
    geo.raster_ = {};
    auto &r = geo.raster_;
    r.texture_rom = texture;
    r.texture_rom_mask = texture_mask;
    r.master_z_clip = 0xff;
    r.cur_command = 1;
    r.command_index = index;
    r.command_buffer[0] = trial % 64;
    r.command_buffer[1] = (trial * 7) % 64;
    r.command_buffer[8] = 0x20600u | (triangle ? 2u : 1u);
    r.command_buffer[9] = (trial & 255u) << 15;
    r.command_buffer[10] = 0x3f8000;
    auto point = [&](unsigned at, float x, float y) {
        r.command_buffer[at] = compat::bit_cast<uint32_t>(x) >> 8;
        r.command_buffer[at + 1] = compat::bit_cast<uint32_t>(y) >> 8;
        r.command_buffer[at + 2] = compat::bit_cast<uint32_t>(10.0f) >> 8;
    };
    point(2, -20.0f, -20.0f); point(5, -20.0f, 20.0f);
    point(11, 20.0f, -20.0f); point(14, 20.0f, 20.0f);
    for (auto &planes : r.clip_plane) {
        planes[0].normal = {1, 0, {2, 0, 0}};
        planes[1].normal = {-1, 0, {2, 0, 0}};
        planes[2].normal = {0, 1, {2, 0, 0}};
        planes[3].normal = {0, -1, {2, 0, 0}};
    }
    geo.polys.clear(); geo.pushed.clear(); geo.record_pushes = (trial & 8) != 0;
}
bool windshield_reflection_follows_unique_glass(rt::Geo &geo) {
    rt::Geo::ReferencePrimitive reflected;
    reflected.oba = 4;
    reflected.command = 1;
    reflected.mode = 1;
    reflected.focus_x = reflected.focus_y = 1;
    reflected.matrix = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 10};
    reflected.attributes = (1u << 17) | (1u << 10) | (3u << 8);
    reflected.master_z_clip = 0xff;
    reflected.z_adjust = 0x3f800000;
    reflected.geometry.texture_header = {0x4409, 0, 7064, 0};
    reflected.geometry.vertex_count = 3;
    reflected.fallback_vertices[0] = {0, 0, {10, 0, 0}, 0, 0, 0};
    reflected.fallback_vertices[1] = {1, 0, {10, 0, 0}, 1, 0, 0};
    reflected.fallback_vertices[2] = {0, 1, {10, 0, 0}, 0, 1, 0};

    auto glass = reflected;
    glass.rom = true;
    glass.command = 3;
    glass.mobile_key.part = 0x7d94;
    glass.geometry.texture_header = {};

    std::array<std::array<float, 12>, 4> poses;
    poses.fill(reflected.matrix);
    poses[2][9] = 3;
    poses[2][11] = 12;
    std::vector<rt::GeoPoly> polys;
    geo.reference_primitives_ = {reflected, glass};
    geo.bind_windshield_reflection();
    geo.replay_instance_reference(polys, true, poses);
    if (polys.size() != 2 || polys[0].object_command != 1 ||
        polys[0].v[0].x != 3 || polys[0].v[0].pz != 12 || polys[0].z != polys[1].z)
        return false;

    geo.replay_instance_reference(polys, true);
    if (polys.size() != 2 || polys[0].v[0].x != 0 || polys[0].v[0].pz != 10)
        return false;
    const auto native = polys[0];

    auto another_reflection = reflected;
    another_reflection.command = 2;
    auto another_glass = glass;
    another_glass.command = 4;
    auto different_window = glass;
    different_window.window = 1;
    for (auto group : {std::vector{reflected, glass, another_reflection},
                       std::vector{reflected, glass, another_glass},
                       std::vector{reflected, different_window}}) {
        geo.reference_primitives_ = std::move(group);
        geo.bind_windshield_reflection();
        geo.replay_instance_reference(polys, true, poses);
        if (polys.empty() || !same_poly(polys[0], native))
            return false;
    }
    return true;
}
} // namespace

int main() {
    std::vector<uint8_t> polygons(128), textures(128);
    for (unsigned i = 0; i < textures.size(); ++i) textures[i] = uint8_t(i * 7);
    std::array<uint32_t, 0x8000> buffer{};
    auto scalar = std::make_unique<rt::Geo>(polygons, textures, buffer.data());
    auto batch = std::make_unique<rt::Geo>(polygons, textures, buffer.data());
    // A mobile key does not move a verified overlay, or shadow a car using
    // the same key. An unverified ROM address keeps the scenery transform.
    const rt::BackgroundTransform camera{
        1, 0, 0, 0, 1, 0, 0, 0, 1, 2, 0, 0,
    };
    const rt::BackgroundTransform overlay_pose{
        .02, 0, 0, 0, .02, 0, 0, 0, .02, 0, 0, 1.5,
    };
    const rt::MobileKey key{1, 2, 3};
    const rt::Geo::SceneObject hud{{{0x8d5da4u, 0, 0}, overlay_pose}, key};
    auto scenery = hud;
    scenery.key = {};
    scenery.geometry.geometry[0] = 0x00800000u;
    auto car = scenery;
    car.key = key;
    car.geometry.model_view = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 10};
    auto out_of_envelope = hud;
    out_of_envelope.key = {};
    out_of_envelope.geometry.model_view[0] = .031;
    batch->previous_scene_ = {hud, car};
    car.geometry.model_view[9] = 3.0;
    batch->current_scene_ = {hud, scenery, car, out_of_envelope};
    batch->previous_stamp_ = rt::SnapshotStamp{1, 100, false, 1};
    batch->current_stamp_ = rt::SnapshotStamp{2, 200, false, 1};
    batch->background_motion_ = rt::BackgroundMotion{camera, 1, 1, 0};
    if (!batch->present_pose_scene(150, 0) ||
        batch->sampled_matrices_[0][9] != 0.0f ||
        batch->sampled_matrices_[1][9] != -1.0f ||
        batch->sampled_matrices_[2][9] != 1.5f ||
        batch->sampled_matrices_[3][9] != -1.0f) {
        std::fprintf(stderr, "camera-space overlay leaked into world motion or mobile matching\n");
        return 1;
    }

    // A reused full mobile key does not establish that the mesh is unchanged.
    batch->current_scene_[2].geometry.geometry[0] ^= 1;
    if (!batch->present_pose_scene(150, 0)) {
        std::fprintf(stderr, "expected the scene to retain its valid background\n");
        return 1;
    }
    if (batch->sampled_matrices_[2][9] != float(car.geometry.model_view[9])) {
        std::fprintf(stderr, "changed mobile mesh borrowed an earlier pose (expected %g, got %g)\n",
                     car.geometry.model_view[9], double(batch->sampled_matrices_[2][9]));
        return 1;
    }
    batch->current_scene_[2] = car;

    auto mobile_identity = std::make_unique<rt::MobileIdentity>();
    batch->set_mobile_identity(mobile_identity.get());
    batch->enable_pose_presentation();
    batch->set_tick(2);
    auto static_hud = hud;
    static_hud.key = {};
    batch->previous_scene_ = {static_hud};
    batch->current_scene_ = {static_hud};
    for (uint32_t i = 0; i < 3; ++i) {
        rt::Geo::SceneObject track{
            {{0x00800010u + i, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1, double(i), 0, 10}}, {},
        };
        batch->previous_scene_.push_back(track);
        track.geometry.model_view[9] += 2.0;
        batch->current_scene_.push_back(track);
    }
    batch->background_motion_.reset();
    batch->stamp_pose_scene({2, 200, false, 1});
    if (!batch->background_motion_ || batch->background_motion_->transform != camera ||
        batch->background_motion_->support != 3 || batch->background_motion_->common_objects != 3) {
        std::fprintf(stderr, "camera-space overlay participated in camera fitting\n");
        return 1;
    }
    if (!batch->present_pose_scene(150, 0)) {
        std::fprintf(stderr, "continuous camera motion did not produce an interpolated pose\n");
        return 1;
    }

    auto translated = batch->current_scene_;
    for (auto &object : translated) {
        if (object.geometry.geometry[0] != 0x8d5da4u)
            object.geometry.model_view[9] += 11;
    }
    batch->previous_scene_ = batch->current_scene_;
    batch->current_scene_.swap(translated);
    batch->previous_stamp_ = batch->current_stamp_;
    batch->background_motion_.reset();
    batch->set_tick(3);
    batch->stamp_pose_scene({3, 300, false, 1});
    if (batch->background_motion_ || batch->present_pose_scene(250, 0)) {
        std::fprintf(stderr, "a large camera translation produced an interpolated pose\n");
        return 1;
    }

    batch->current_scene_ = {static_hud};
    for (uint32_t i = 0; i < 3; ++i) {
        batch->current_scene_.push_back({
            {{0x00800010u + i, 0, 0}, {0, 1, 0, -1, 0, 0, 0, 0, 1, 0, double(i) + 2, 10}}, {},
        });
    }
    batch->previous_stamp_ = rt::SnapshotStamp{2, 200, false, 1};
    batch->background_motion_.reset();
    batch->set_tick(3);
    batch->stamp_pose_scene({3, 300, false, 1});
    if (batch->background_motion_ || batch->present_pose_scene(250, 0)) {
        std::fprintf(stderr, "a 90-degree camera cut produced an interpolated pose\n");
        return 1;
    }
    constexpr unsigned cases = 6000;
    unsigned kept = 0;
    const unsigned slots[] = {0, 1, 2, 5, 11, 14};
    for (unsigned trial = 0; trial < cases; ++trial) {
        const unsigned index = slots[trial % std::size(slots)];
        const bool triangle = (trial / std::size(slots)) % 2 != 0 && index != 14;
        reset(*scalar, trial, index, triangle); reset(*batch, trial, index, triangle);
        rt::GeoVertex point{float(1 + trial % 40), float(int(trial % 51) - 25), {10, 0, 0}};
        if (trial % 17 == 0) { // Window command: force the generic fallback.
            scalar->raster_.cur_command = batch->raster_.cur_command = 3;
            scalar->raster_.command_index = batch->raster_.command_index = 3;
        }
        scalar->model2_3d_push(&scalar->raster_, compat::bit_cast<uint32_t>(point.x) >> 8);
        scalar->model2_3d_push(&scalar->raster_, compat::bit_cast<uint32_t>(point.y) >> 8);
        scalar->model2_3d_push(&scalar->raster_, compat::bit_cast<uint32_t>(point.p[0]) >> 8);
        batch->model2_3d_push_point(&batch->raster_, point);
        const auto &a = scalar->raster_, &b = batch->raster_;
        bool same = a.cur_command == b.cur_command && a.command_index == b.command_index &&
                    a.poly_list_index == b.poly_list_index && a.min_z == b.min_z && a.max_z == b.max_z &&
                    compat::bit_cast<uint32_t>(a.polygon_z) == compat::bit_cast<uint32_t>(b.polygon_z) &&
                    std::memcmp(a.command_buffer, b.command_buffer, sizeof(a.command_buffer)) == 0 &&
                    scalar->pushed == batch->pushed && scalar->polys.size() == batch->polys.size();
        for (size_t i = 0; same && i < scalar->polys.size(); ++i) same = same_poly(scalar->polys[i], batch->polys[i]);
        if (!same) { std::fprintf(stderr, "geometry point stream differs at case %u\n", trial); return 1; }
        kept += unsigned(scalar->polys.size());
    }
    if (!kept) return 1;
    if (!windshield_reflection_follows_unique_glass(*batch)) {
        std::fprintf(stderr, "windshield reflection pose or fallback differs\n");
        return 1;
    }
    std::printf("geometry point batches: %u scalar-equivalent states, %u kept polygons passed\n", cases, kept);
}
