// Object-space positions must survive the command buffer and link reuse.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#define private public
#include "runtime/geo.h"
#undef private
#include "../src/runtime/geo.cpp"
#include "../src/compat/bit_cast.h"

namespace {
void reset(rt::Geo &geo, std::vector<uint16_t> &texture) {
    geo.raster_ = {};
    auto &r = geo.raster_;
    r.texture_rom = texture.data();
    r.texture_rom_mask = 0xff;
    r.cur_command = 1;
    r.command_index = 17;
    r.master_z_clip = 0xff;
    r.command_buffer[0] = 0;      // texture points
    r.command_buffer[1] = 0;      // texture header
    r.command_buffer[8] = 0x20601; // quad, link type 0
    r.command_buffer[9] = 0;
    r.command_buffer[10] = 0x3f8000;
    for (auto &planes : r.clip_plane) {
        for (auto &p : planes) { p.normal = {1, 0, {0, 0, 0}}; p.distance = -1e30f; }
    }
    geo.polys.clear();
    geo.cur_oba = 0x00800001;
    geo.cur_obc = 4;
    geo.cur_object_command = 7;
    geo.cur_mode = 0;
    geo.cur_direct = 0;
    for (int i = 0; i < 12; ++i) geo.cur_matrix[i] = float(i + 1);
    geo.cur_focus_x = 2.0f;
    geo.cur_focus_y = 3.0f;
}
void point(uint32_t *b, unsigned at, float v) {
    b[at] = rt::f2u(v) >> 8;
}

bool check_pose_facing(rt::Geo &geo) {
    rt::Geo::ReferencePrimitive primitive;
    primitive.rom = true;
    primitive.attributes = 0x300;
    primitive.geometry.vertex_count = 3;
    primitive.geometry.vertices[0].position = {1, 0, 0};
    primitive.geometry.vertices[1].position = {0, 0, 0};
    primitive.geometry.vertices[2].position = {0, 1, 0};
    primitive.geometry.normal = {0, 0, 1};
    primitive.focus_x = primitive.focus_y = 1;
    primitive.master_z_clip = 0xff;
    primitive.matrix = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 10};
    for (auto &plane : primitive.planes) {
        plane.normal = {0, 0, {1, 0, 0}};
    }
    geo.enable_instance_reference(true);
    for (bool rom_normal : {true, false}) {
        primitive.geometry.has_rom_normal = rom_normal;
        primitive.face_word = 0x00800000; // deliberately stale backface bit
        geo.reference_primitives_ = {primitive};
        geo.replay_instance_reference();
        if (geo.polys.size() != 1) return false;

        // A 180-degree turn around the vertical axis flips the face.
        auto &opposite = geo.reference_primitives_.front();
        opposite.matrix[0] = opposite.matrix[8] = -1;
        opposite.face_word = 0;
        geo.replay_instance_reference();
        if (!geo.polys.empty()) return false;

        opposite.attributes |= 1u << 17; // double-sided
        geo.replay_instance_reference();
        if (geo.polys.size() != 1) return false;
    }
    return true;
}

bool check_pose_presentation(rt::Geo &geo) {
    rt::MobileIdentity identity;
    geo.set_mobile_identity(&identity);
    geo.enable_pose_presentation();
    const rt::BackgroundTransform base{1,0,0, 0,1,0, 0,0,1, 0,0,10};
    geo.previous_scene_.clear();
    geo.current_scene_.clear();
    for (unsigned i = 0; i < 3; ++i) {
        rt::Geo::SceneObject scenery{{{0x800001u+i, 3, 0}, base}, {}};
        scenery.geometry.model_view[9] = i;
        geo.previous_scene_.push_back(scenery);
        scenery.geometry.model_view[9] += 1;
        geo.current_scene_.push_back(scenery);
    }
    rt::Geo::SceneObject car{{{0x800008, 1, 0}, base}, {0x93dc, 0x506680, 0}};
    geo.previous_scene_.push_back(car);
    car.geometry.model_view[9] = 2;
    geo.current_scene_.push_back(car);
    geo.reference_primitives_.clear();

    geo.previous_stamp_ = rt::SnapshotStamp{9, 100, false, 7};
    geo.set_tick(10);
    const rt::SnapshotStamp newest{10, 200, false, 7};
    geo.stamp_pose_scene(newest);
    if (!geo.present_pose_scene(250, 100) || geo.sampled_matrices_.size() != 4 ||
        geo.sampled_matrices_[0][9] != 0.5f || geo.sampled_matrices_[3][9] != 1.0f) return false;
    // Past the latest snapshot the same replay draws the latest poses.
    if (!geo.present_pose_scene(300, 100) || geo.sampled_matrices_[3][9] != 2.0f) return false;

    geo.current_scene_[3].geometry.model_view[9] = 20;
    if (!geo.present_pose_scene(250, 100) || geo.sampled_matrices_[3][9] != 20) return false;
    identity.reset_slot(0x506680, 10);
    geo.background_motion_.reset();
    geo.stamp_pose_scene(newest);
    return geo.present_pose_scene(250, 100) == nullptr;
}

// Opponent wheels follow the sampled chassis with their native offset, also
// when two wheel emissions share one key (review of #81, point 1).
bool check_wheel_carry(rt::Geo &geo) {
    rt::MobileIdentity identity;
    geo.set_mobile_identity(&identity);
    geo.enable_pose_presentation();
    const rt::BackgroundTransform base{1,0,0, 0,1,0, 0,0,1, 0,0,10};
    geo.previous_scene_.clear();
    geo.current_scene_.clear();
    geo.background_motion_.reset();
    for (unsigned i = 0; i < 3; ++i) {
        rt::Geo::SceneObject scenery{{{0x800001u+i, 3, 0}, base}, {}};
        geo.previous_scene_.push_back(scenery);
        geo.current_scene_.push_back(scenery);
    }
    rt::Geo::SceneObject chassis{{{0x800010, 1, 0}, base}, {rt::opponent_wheel_chassis_part(rt::opponent_wheel_mid), 0x506680, 0}};
    geo.previous_scene_.push_back(chassis);
    chassis.geometry.model_view[9] = 2;
    geo.current_scene_.push_back(chassis);
    for (float dx : {1.5f, -1.5f}) {
        rt::Geo::SceneObject wheel{{{0x800020, 1, 0}, base}, {rt::opponent_wheel_mid, 0x506680, 0}};
        wheel.geometry.model_view[9] = 2 + dx;
        wheel.geometry.model_view[10] = -0.5;
        geo.current_scene_.push_back(wheel);
    }
    geo.reference_primitives_.clear();
    geo.previous_stamp_ = rt::SnapshotStamp{9, 100, false, 7};
    geo.set_tick(10);
    geo.stamp_pose_scene(rt::SnapshotStamp{10, 200, false, 7});
    if (!geo.present_pose_scene(250, 100) || geo.sampled_matrices_.size() != 6) return false;
    const float body = geo.sampled_matrices_[3][9];
    if (!(body > 0.0f && body < 2.0f)) return false; // the chassis is interpolated
    const float offsets[2] = {1.5f, -1.5f};
    for (int w = 0; w < 2; ++w) {
        const auto &m = geo.sampled_matrices_[4 + w];
        if (std::fabs(m[9] - (body + offsets[w])) > 1e-4f || std::fabs(m[10] + 0.5f) > 1e-4f ||
            std::fabs(m[11] - 10.0f) > 1e-4f) return false;
    }
    return true;
}

// The rev needle (0x8d6a0f, 0.3 in front of the eye, turned by the revs) is
// a camera-space overlay: while the camera moves it keeps its latest pose
// instead of being moved with the scenery between ticks.
bool check_needle_overlay(rt::Geo &geo) {
    rt::MobileIdentity identity;
    geo.set_mobile_identity(&identity);
    geo.enable_pose_presentation();
    geo.previous_scene_.clear();
    geo.current_scene_.clear();
    geo.background_motion_.reset();
    for (unsigned i = 0; i < 3; ++i) {
        rt::BackgroundTransform before{1,0,0, 0,1,0, 0,0,1, float(i),0,10};
        rt::BackgroundTransform after = before;
        after[10] = 1; // the camera pitches: the scenery rises one unit
        geo.previous_scene_.push_back({{{0x800001u+i, 3, 0}, before}, {}});
        geo.current_scene_.push_back({{{0x800001u+i, 3, 0}, after}, {}});
    }
    const float c = std::cos(0.6f) * 0.05f, s = std::sin(0.6f) * 0.05f;
    const rt::BackgroundTransform needle{c,s,0, -s,c,0, 0,0,0.05f, 0.02f,0.11f,0.3f};
    geo.previous_scene_.push_back({{{0x8d6a0fu, 1, 0}, needle}, {}});
    geo.current_scene_.push_back({{{0x8d6a0fu, 1, 0}, needle}, {}});
    geo.reference_primitives_.clear();
    geo.previous_stamp_ = rt::SnapshotStamp{9, 100, false, 7};
    geo.set_tick(10);
    geo.stamp_pose_scene(rt::SnapshotStamp{10, 200, false, 7});
    if (!geo.present_pose_scene(250, 100) || geo.sampled_matrices_.size() != 4) return false;
    if (!(geo.sampled_matrices_[0][10] > 0.0f && geo.sampled_matrices_[0][10] < 1.0f)) return false;
    for (unsigned j = 0; j < 12; ++j)
        if (std::fabs(geo.sampled_matrices_[3][j] - float(needle[j])) > 1e-6f) return false;
    return true;
}

bool check_opponent_level_of_detail(rt::Geo &geo) {
    // An opponent switching from its far (0x9000) to its mid (0x8c00) body
    // between ticks keeps slot and generation: it must be interpolated from the
    // far pose, not drawn at its latest position as scenery (a doubled car).
    rt::MobileIdentity identity;
    geo.set_mobile_identity(&identity);
    geo.enable_pose_presentation();
    geo.previous_scene_.clear();
    geo.current_scene_.clear();
    geo.background_motion_.reset();
    for (unsigned i = 0; i < 3; ++i) {
        const rt::BackgroundTransform still{1,0,0, 0,1,0, 0,0,1, float(i),0,10};
        geo.previous_scene_.push_back({{{0x800001u+i, 3, 0}, still}, {}});
        geo.current_scene_.push_back({{{0x800001u+i, 3, 0}, still}, {}});
    }
    const rt::BackgroundTransform far{1,0,0, 0,1,0, 0,0,1, 0,0,30};
    const rt::BackgroundTransform mid{1,0,0, 0,1,0, 0,0,1, 0,0,29};
    geo.previous_scene_.push_back({{{0x900001u, 3, 0}, far}, {0x9000u, 0x507000u, 2}});
    geo.current_scene_.push_back({{{0x8c0001u, 3, 0}, mid}, {0x8c00u, 0x507000u, 2}});
    geo.reference_primitives_.clear();
    geo.previous_stamp_ = rt::SnapshotStamp{9, 100, false, 7};
    geo.set_tick(10);
    geo.stamp_pose_scene(rt::SnapshotStamp{10, 200, false, 7});
    if (!geo.present_pose_scene(250, 100) || geo.sampled_matrices_.size() != 4) return false;
    const float z = geo.sampled_matrices_[3][11];
    return z > 29.01f && z < 29.99f;
}

bool check_rom_extraction() {
    // Start with two points. A triangle with link type 3 becomes the first
    // side of the following quad. The third primitive is culled by link 0.
    const uint32_t triangle = 0xf8040302;
    const uint32_t quad = 0x0001f201; // signed header offset -1
    const uint32_t culled = 0x00020002;
    std::array<uint32_t, 64> words{};
    size_t n = 0;
    auto append_point = [&](float x, float y, float z) {
        for (float value : {x, y, z}) words[n++] = rt::f2u(value);
    };
    append_point(1, 0, 5);
    append_point(2, 0, 5);
    words[n++] = triangle;
    append_point(0, 0, 2);
    append_point(3, 1, 5);
    append_point(99, 99, 99); // triangle padding
    words[n++] = quad;
    append_point(0, 0, 3);
    append_point(4, 1, 5);
    append_point(5, 0, 5);
    words[n++] = culled;
    append_point(0, 0, 4);
    append_point(6, 1, 5);
    append_point(99, 99, 99);
    words[n++] = 0;
    words[n++] = 0;
    words[n++] = 0;
    words[n++] = 0;
    words[n++] = quad; // beyond the end link
    std::vector<uint8_t> polygons(sizeof words), textures(128);
    std::memcpy(polygons.data(), words.data(), sizeof words);
    for (unsigned i = 0; i < 64; ++i) textures[2*i] = uint8_t(i);
    std::array<uint32_t, 0x8000> buffer{};
    auto storage = std::make_unique<rt::Geo>(polygons, textures, buffer.data());
    auto &geo = *storage;
    std::vector<rt::RomObjectEmission> emissions;
    geo.set_rom_object_sink([](void *context, const rt::RomObjectEmission &object) {
        static_cast<std::vector<rt::RomObjectEmission> *>(context)->push_back(object);
    }, &emissions);
    buffer[0] = 7u << 23;
    buffer[2] = 1u << 23;
    buffer[3] = 2; buffer[4] = 52; buffer[5] = 0x800000; buffer[6] = 5;
    buffer[7] = 1u << 23;
    buffer[8] = 2; buffer[9] = 52; buffer[10] = 0x1800000; buffer[11] = 1;
    buffer[12] = 15u << 23;
    geo.geo_.matrix[0] = geo.geo_.matrix[4] = geo.geo_.matrix[8] = 1;
    geo.geo_.matrix[9] = 10;
    for (unsigned mode = 0; mode < 4; ++mode) {
        for (unsigned count : {5u, 2u, 0u}) {
            buffer[1] = mode;
            buffer[6] = count;
            emissions.clear();
            geo.parse(0);
            if (emissions.size() != 1) return false;
            const auto &object = emissions.front();
            if (object.command_word != 2 || object.command != 1 || object.mode != mode ||
                object.oba != 0x800000 || object.obc != (count ? count : 0xfffff) ||
                object.tpa != 2 || object.tha != 52 || object.matrix[9] != 10 ||
                object.primitives.size() != (count == 2 ? 2u : 3u)) return false;
            const auto &a = object.primitives[0];
            const auto &b = object.primitives[1];
            const std::array<float, 3> old_second{2, 0, 5};
            const std::array<float, 3> carried{3, 1, 5};
            if (a.attributes != triangle || b.attributes != quad || a.vertex_count != 3 ||
                b.vertex_count != 4 || a.vertices[0].position != old_second ||
                a.vertices[2].position != carried || b.vertices[0].position != old_second ||
                b.vertices[1].position != carried || a.vertices[2].u != 7 || a.vertices[2].v != 6 ||
                a.tpa != 2 || a.tha != 52 || a.texture_header[0] != 52 ||
                b.tpa != 8 || b.tha != 52 || b.texture_header[3] != 55 ||
                a.has_rom_normal != (mode < 2) || a.normal[0] != 0 || a.normal[1] != 0 ||
                (mode < 2 ? a.normal[2] != 2 : std::abs(a.normal[2]) != 1)) return false;
            if (object.primitives.size() == 3) {
                const auto &c = object.primitives[2];
                if (c.attributes != culled || c.tpa != 16 || c.tha != 48 ||
                    c.texture_header[0] != 48 || c.vertex_count != 3) return false;
            }
        }
    }
    return true;
}
} // namespace

int main() {
    std::vector<uint8_t> polygons(128), textures(128);
    std::vector<uint16_t> texture(256);
    std::array<uint32_t, 0x8000> buffer{};
    auto storage = std::make_unique<rt::Geo>(polygons, textures, buffer.data());
    auto &geo = *storage;
    reset(geo, texture);
    auto &r = geo.raster_;

    // Transformed points in command-buffer slots.
    point(r.command_buffer, 2, 10.0f); point(r.command_buffer, 5, 20.0f);
    point(r.command_buffer, 11, 30.0f); point(r.command_buffer, 14, 40.0f);
    for (unsigned at : {2u, 5u, 11u, 14u}) r.command_buffer[at + 2] = rt::f2u(5.0f) >> 8;
    // Object-space positions are stored beside the command buffer.
    const float obj[4][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12}};
    const unsigned slots[4] = {2, 5, 11, 14};
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 3; ++k) r.object_pos[slots[i]][k] = obj[i][k];

    geo.model2_3d_process_polygon<4>(&r, 0x20601);
    if (geo.polys.size() != 1) { std::fprintf(stderr, "poly missing\n"); return 1; }
    const rt::GeoPoly &p = geo.polys.back();
    const int order[4] = {1, 0, 2, 3}; // command slot order -> GeoPoly v[]
    for (int i = 0; i < 4; ++i) {
        const float *o = obj[order[i]];
        if (p.v[i].ox != o[0] || p.v[i].oy != o[1] || p.v[i].oz != o[2]) {
            std::fprintf(stderr, "object position %d: %g %g %g != %g %g %g\n", i,
                         p.v[i].ox, p.v[i].oy, p.v[i].oz, o[0], o[1], o[2]);
            return 1;
        }
    }
    if (p.object_command != 7 || p.object_matrix[0] != 1.0f || p.object_matrix[11] != 12.0f ||
        p.object_focus_x != 2.0f || p.object_focus_y != 3.0f) {
        std::fprintf(stderr, "object state not copied\n");
        return 1;
    }
    // Exercise actual object commands, including commands emitting no polygon.
    // Their emission ordinal must be allocated before culling, then reset per frame.
    auto command_storage = std::make_unique<rt::Geo>(polygons, textures, buffer.data());
    auto &commands = *command_storage;
    commands.geo_.mode = 2;
    uint32_t args[4] = {0, 0, 0x00800000, 1}; // zero-filled ROM: end link
    commands.render_frame_start();
    for (uint32_t expected = 1; expected <= 2; ++expected) {
        commands.geo_object_data(&commands.geo_, 1u << 23, {args, 4, 0});
        if (commands.cur_object_command != expected || !commands.polys.empty()) {
            std::fprintf(stderr, "empty object command identity missing\n"); return 1;
        }
    }
    commands.render_frame_start();
    commands.geo_object_data(&commands.geo_, 1u << 23, {args, 4, 0});
    if (commands.cur_object_command != 1) {
        std::fprintf(stderr, "command identity not reset for new frame\n"); return 1;
    }
    if (!check_rom_extraction()) {
        std::fprintf(stderr, "pre-cull ROM extraction failed\n");
        return 1;
    }
    if (!check_pose_facing(geo)) {
        std::fprintf(stderr, "pose-dependent backface culling failed\n");
        return 1;
    }
    if (!check_pose_presentation(geo)) {
        std::fprintf(stderr, "scene pose blending or discontinuity guard failed\n");
        return 1;
    }
    if (!check_wheel_carry(geo)) {
        std::fprintf(stderr, "opponent wheels did not follow the sampled chassis\n");
        return 1;
    }
    if (!check_needle_overlay(geo)) {
        std::fprintf(stderr, "the rev needle moved with the scenery between ticks\n");
        return 1;
    }
    if (!check_opponent_level_of_detail(geo)) {
        std::fprintf(stderr, "an opponent changing level of detail was not interpolated\n");
        return 1;
    }
    std::puts("object positions, command provenance, all four ROM modes and chain endings OK");
    return 0;
}



