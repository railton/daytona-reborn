#pragma once

#include "runtime/geo.h"

#include <algorithm>
#include <map>
#include <array>
#include <string>
#include <vector>

namespace app {

// The optional pack is read from the user's machine once, at GPU startup.
class OpponentBodies {
public:
    static constexpr uint32_t atlas_bytes = 3 * 10 * (256 * 128 + 128 * 64 + 64 * 32 + 32 * 16) * 4;

    // max_faces: per body (the opponents' 1024; the player's car may have
    // more, its one body drawn once)
    bool load(const std::string &directory, int max_faces = 1024);
    // why load() refused the folder ("" when it loaded): the file, the face and the rule, for the log
    const std::string &error() const { return error_; }
    // why wheel.m2car was left out (the game's wheels drawn instead); "" when it loaded or there is none
    const std::string &wheel_error() const { return wheel_error_; }
    // The player's car instead of the opponents' (M2_PLAYER_BODY, experimental):
    // the first body takes the place of the player's body (0x7d68) and its
    // glass (0x7d94), the wheels, shadow and lights kept; its textures are
    // the atlas's surfaces from texture_base (GpuRenderer appends this pack's
    // atlas after the opponents').
    // The player's faces are lit by the game's own light: each frame the
    // original body's faces (their object-space normals and the luma the
    // game lit them with) fit luma = a + w . n, which then lights the new
    // faces from their own normals. They keep the original's identity (part,
    // command, focus, object-space corners), so the renderer's car shading,
    // reflections and shadows apply to them too.
    void set_player(uint32_t texture_base);
    static constexpr uint32_t kPlayerTextureBase = 30; // local textures above it are the player's
    // a player body's face turned away from the camera: not drawn, but it
    // casts the car's shadow (the sun sees faces the camera does not)
    static constexpr uint8_t kShadowOnlyTexture = 255;
    // a player body's surfaces by role (m2.hlsl): 6 glass, reflects 15 %;
    // 7 and 8 (wheel wells; grille mesh, spoilers, dark trim) reflect nothing
    static constexpr uint32_t kPlayerGlassSurface = 6;
    static constexpr uint32_t kPlayerMatteSurfaces[2] = {7, 8};
    // The player's wheels (kept, the game's own) drawn this much bigger with
    // a new body, standing on the same ground (Hornet: radius 0.371 model units)
    static constexpr float kPlayerWheelScale = 1.10f;
    const std::vector<uint8_t> &atlas() const { return atlas_; }
    // car.ini: glass_opacity=0.25..1 (glass_see_through=1: 0.5): the player's
    // glass (surface 6) drawn that much covered, the rest see-through (the
    // car has an interior behind it); 1, opaque, without car.ini
    bool glass_see_through() const { return glass_opacity_ < 1.0f; }
    float glass_opacity() const { return glass_opacity_; }
    // The light the player's car casts its shadow by (its travel, camera
    // space), or none: a face turned from the camera is kept (shadow only)
    // only when it faces this light.
    void set_shadow_light(const float *travel) {
        shadow_light_ = travel != nullptr;
        if (travel) std::copy_n(travel, 3, shadow_travel_);
    }
    // A null result means the original scene can be used directly.
    const std::vector<rt::GeoPoly> *replace(const std::vector<rt::GeoPoly> &scene);
    // The player's new faces' corners, per polygon of the last replace()'s
    // scene: their smooth normal (camera space) and light, made once from the
    // body's shape (valid false: the renderer finds them, as for any car).
    struct CornerLight {
        bool valid = false;
        float light[4] = {};
        float normal[4][3] = {};
    };
    const std::vector<CornerLight> &corners() const { return corners_; }
    const std::vector<rt::GeoPoly> &scene() const { return scene_; }

    struct Face {
        uint8_t count = 0;
        uint8_t surface = 0;
        bool decal = false; // SURFACE 10..19 in the file: drawn in front of the faces it lies on
        float shade = 1;
        std::array<std::array<float, 5>, 4> vertices{};
    };
private:
    std::array<std::vector<Face>, 3> bodies_;
    std::string error_, wheel_error_;
    std::vector<Face> wheel_; // the player's wheel (wheel.m2car), empty: the game's
    float wheel_radius_ = 0;  // its tyre's radius (the farthest corner from the axle)
    struct WheelAxles { bool valid = false; float front_z = 0, rear_z = 0, half_track = 0; };
    WheelAxles wheel_axles_; // M2WHEEL2: where the new car's wheels go (body space)
    std::vector<uint8_t> atlas_;
    std::vector<rt::GeoPoly> scene_;
    bool player_ = false;
    float glass_opacity_ = 1.0f;
    bool shadow_light_ = false;
    float shadow_travel_[3] = {0, 0, 1};
    bool casts_shadow(float nx, float ny, float nz) const {
        return shadow_light_ && nx * shadow_travel_[0] + ny * shadow_travel_[1] + nz * shadow_travel_[2] < 0;
    }
    uint32_t texture_base_ = 0;
    std::vector<std::array<float, 3>> player_normals_; // body 0's faces, outward, model space
    std::vector<std::array<float, 3>> wheel_normals_;  // the wheel's, outward
    // per face, its corners' smooth normals (model space), body 0's and the wheel's
    std::vector<std::array<std::array<float, 3>, 4>> body_corners_, wheel_corners_;
    std::vector<CornerLight> corners_; // per scene_ polygon
    // The cockpit view paints the game's own bonnet with the body's surfaces
    // seen from above: per cell of a grid over the body (x, z), the surface
    // of its highest face; per surface its texture as a plane over (x, z).
    static constexpr int kTopGridX = 48, kTopGridZ = 96;
    std::vector<int8_t> top_surface_;       // kTopGridX * kTopGridZ, -1: none
    std::vector<bool> top_face_;            // body 0's faces highest over some cell (the skin seen from above)
    float top_box_[4] = {0, 0, 0, 0};        // x0, z0, x1, z1
    struct Plane { bool valid = false; float u[3], v[3]; }; // u = u0 x + u1 z + u2
    std::array<Plane, 10> top_planes_{};
    void build_top_view();
    bool paint_cockpit(const rt::GeoPoly &source, rt::GeoPoly &out) const;
    float light_fit_[4] = {0, 0, 0, 0};                // a, w (the last good fit)
    bool light_fitted_ = false;
    std::map<uint32_t, std::array<float, 4>> light_fits_; // this frame's, by body emission
    float light_reference_ = 0;                            // the brightest car's a + |w|
    void fit_player_light(const std::vector<rt::GeoPoly> &scene);
};

} // namespace app
