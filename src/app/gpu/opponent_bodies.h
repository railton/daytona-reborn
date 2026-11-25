#pragma once

#include "runtime/geo.h"

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
    // A null result means the original scene can be used directly.
    const std::vector<rt::GeoPoly> *replace(const std::vector<rt::GeoPoly> &scene);

    struct Face {
        uint8_t count = 0;
        uint8_t surface = 0;
        float shade = 1;
        std::array<std::array<float, 5>, 4> vertices{};
    };
private:
    std::array<std::vector<Face>, 3> bodies_;
    std::vector<Face> wheel_; // the player's wheel (wheel.m2car), empty: the game's
    float wheel_radius_ = 0;  // its tyre's radius (the farthest corner from the axle)
    std::vector<uint8_t> atlas_;
    std::vector<rt::GeoPoly> scene_;
    bool player_ = false;
    uint32_t texture_base_ = 0;
    std::vector<std::array<float, 3>> player_normals_; // body 0's faces, outward, model space
    float light_fit_[4] = {0, 0, 0, 0};                // a, w (the last good fit)
    bool light_fitted_ = false;
    std::map<uint32_t, std::array<float, 4>> light_fits_; // this frame's, by body emission
    float light_reference_ = 0;                            // the brightest car's a + |w|
    void fit_player_light(const std::vector<rt::GeoPoly> &scene);
};

} // namespace app
