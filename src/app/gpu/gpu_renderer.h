// Hardware renderer (SDL_GPU): draws a frame on the GPU instead of the CPU
// rasterizer (src/runtime/raster.cpp, the exact reference) and tilemap
// drawing (Video::draw): the back tilemap layers, the Model 2 3D layer, the
// front tilemap layers (Video's external-3D desktop mode).
//
// The tilemap layers are composed per pixel (m2.hlsl ps_tiles_back,
// ps_tiles_front) from the pixmaps the CPU decodes, uploaded by the rows of
// tiles that changed, and the frame's tile registers and pens. With the HUD
// moved to the widescreen edges the front layers come from the CPU instead.
//
// Polygons are projected exactly as model2_3d_project, drawn as a fan from
// their first vertex in the rasterizer's order (window, then z, newest first)
// and clipped to their window; the depth test stands in for the rasterizer's
// "first polygon to fill a pixel wins". Solid polygons take their palette
// colour from the CPU; textured ones are shaded per pixel by a port of the
// rasterizer's draw_tex_span (m2.hlsl) reading texture RAM, the luma RAM and
// the colour translation from storage buffers.
#pragma once

#include "app/gpu/hires_textures.h"
#include "app/gpu/opponent_bodies.h"
#include "runtime/video.h"

#include <SDL3/SDL.h>

#include <array>
#include <string>
#include <vector>

namespace app {

class GpuRenderer {
public:
    // Creates the pipelines for drawing into textures of `format`. False (with
    // error()) if the GPU or its shader format is not usable.
    // msaa (an enhancement): samples a pixel, 1 (off, the exact path), 2, 4
    // or 8; the polygons' edges and the cut-out textures' (alpha to coverage)
    // are smoothed, the frame resolved into the target. Fewer if the GPU
    // cannot (msaa()). Not with the ray-traced shadows (their passes read
    // the depth of single-sample pixels).
    bool init(SDL_GPUDevice *dev, SDL_GPUTextureFormat format, int msaa = 1);
    // The player's car body (experimental, app/gpu/opponent_bodies.h): a
    // folder with bodies.m2car and liveries.rgba, read by the next init();
    // "" the game's own car. M2_PLAYER_BODY wins.
    void set_player_body(const std::string &directory) { player_body_dir_ = directory; }
    int msaa() const { return msaa_; }
    // Texture filtering (an enhancement): 1 the game's own (the exact path),
    // 2 to 16 anisotropic: on surfaces seen at a grazing angle (the road, the
    // walls) the mip level follows the footprint's narrow side and up to n
    // samples are averaged along its long side, so the far road stays sharp
    // without shimmering. Opaque textures only.
    void set_anisotropy(int n);
    // The cars (enhancements, both off: the exact path). smooth: each corner
    // of a car's face takes the light the game gave the faces around it
    // (those within 50 degrees: creases stay sharp), blended across the face,
    // instead of one light per face. reflect: the paint reflects the course's
    // own sky (the back tilemap layers this frame, blurred) and the ground,
    // more at grazing angles, and the game's sun as a highlight.
    void set_car_shading(bool smooth, bool reflect) { smooth_cars_ = smooth, car_reflect_ = reflect; }
    // sharp: up close, the cars' numbers and logos keep crisp, rounded edges
    // instead of the bilinear blur (m2.hlsl fetch_texel, bit 31)
    void set_car_textures(bool sharp) { sharp_car_textures_ = sharp; }
    // Smooth 2D (an enhancement): the tilemap layers (menus, HUD, text)
    // magnified with smooth curves and diagonals instead of square pixels
    // (m2.hlsl smooth_vote), the CPU's front layers (HUD at the edges) too.
    void set_smooth_2d(bool on) { smooth_2d_ = on; }
    // no high-resolution textures any more (load_hires_textures loads a pack)
    void clear_hires_textures();
    // The cars' shadows on the road and the walls (an enhancement, any GPU):
    // the cars drawn from the game's sun into a depth map, two cascades of
    // 2048 x 2048 (up to 40 camera units, up to 140), which the scenery's
    // pixels look up (m2.hlsl car_shadow_light).
    // replace_game_shadow: the game's own checker shadow under the player's
    // car left out while these are drawn.
    void set_car_shadows(bool on, bool replace_game_shadow = false) {
        car_shadows_ = on, hide_game_shadow_ = replace_game_shadow;
    }
    // The game's light vector (Geo::light, camera space), each frame, for the reflections.
    void set_light(const std::array<float, 3> &light) { game_light_ = light; }
    // The display passes (enhancements, both 0 = off: a plain blit). The
    // frame `source` (its part uv_w x uv_h, mip level `lod`) is drawn into
    // `dest` of `target` (cleared to black around it): bloom > 0 adds the glow
    // of its bright parts (light above `threshold`, blurred at a quarter of
    // the window), crt > 0 the look of an arcade monitor (a beam per game
    // line, `lines` of them, an aperture grille, darker corners), faded out
    // where the window has too few pixels a line. False if it cannot.
    struct PostFrame {
        SDL_GPUTexture *source = nullptr;
        float uv_w = 1, uv_h = 1, lod = 0;
        SDL_GPUTexture *target = nullptr;
        SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        SDL_Rect dest{};
        int lines = 384;
        float bloom = 0, crt = 0, threshold = 0.55f;
    };
    bool present(SDL_GPUCommandBuffer *cmd, const PostFrame &frame);
    // The quality measurement (app/gpu/quality_probe.h): a stand-in for a
    // race frame at pixel_w x pixel_h through the polygon shader, with this
    // renderer's MSAA and texture filtering. `layers` tilted textured planes
    // (a road seen from the driver's seat) cover the whole frame back to
    // front, so that each pixel is shaded `layers` times; texture RAM, luma
    // RAM and colour translation hold blocks of noise (nothing of the game).
    // Then this renderer is only for measuring: render() would show the noise.
    bool draw_benchmark(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, int pixel_w, int pixel_h, int layers);
    // High-resolution textures (experimental, app/gpu/hires_textures.h): the
    // pack's upscaled textures replace the game's where they are magnified on
    // screen and the texture RAM holds the very texels they were made from.
    // False (with error()) if the pack cannot be read; "" unloads it.
    bool load_hires_textures(const std::string &path);
    size_t hires_entries() const { return hires_.entries.size(); }
    size_t hires_live() const { return hires_live_; } // in use for the last frame's texture RAM
    void shutdown();
    bool ok() const { return poly_pipe_ != nullptr; }
    const std::string &error() const { return error_; }

    // Draws the frame into `target` (at least w x h times `scale`, created
    // with COLOR_TARGET usage): the back tilemap layers, the 3D polygons, the
    // front tilemap layers, all from `video` in external-3D mode. scale > 1
    // (the internal resolution enhancement): the 3D drawn at w x h times
    // scale, textures a mip level finer per doubling, the tilemaps' pixels
    // repeated.
    bool render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, int w, int h,
                const rt::Video &video, int scale = 1, int pixel_w = 0, int pixel_h = 0,
                bool sharp_textures = false, const std::vector<rt::GeoPoly> *pose_polys = nullptr);

    // Ray tracing (app/gpu/dxr_scene.h): when set, each render fills `out`
    // with the drawn polygons as camera-space triangles (9 floats each: x, y
    // already times the focal length, z), in the fan order the GPU draws.
    void collect_triangles(std::vector<float> *out) { triangles_ = out; }
    // Polygons that only cast shadows (rt::Geo::shadow_casters: behind the
    // camera, never drawn), added to the triangles of the next render.
    void set_shadow_casters(const std::vector<rt::GeoPoly> *casters) { casters_ = casters; }

    // Ray-traced sun shadows (app/gpu/dxr.hlsl). Once enabled, each render
    // also writes the view z of the polygon that won each pixel (0 where a
    // front layer covers the 3D) into a texture of the target's size; the
    // DXR pass turns it into a light texture, and apply_shadow multiplies
    // the target by it. Never enabled, nothing of this exists or runs.
    bool enable_shadows();
    struct ShadowFrame {
        void *view_z = nullptr, *light = nullptr; // ID3D12Resource of each
        int width = 0, height = 0;                // the target, in pixels
        float to_screen[2] = {1, 1};              // screen units per pixel
        float origin[2] = {0, 0};                 // screen position of the camera axis
        float focus[2] = {1, 1};                  // the polygons' focal lengths
        const std::vector<float> *cutout_xyz = nullptr;        // cut-out triangles (9 floats each)
        const std::vector<uint32_t> *cutout_records = nullptr; // their texture: info.x, info.y, u0 v0 u1 v1 u2 v2
        const void *tex0 = nullptr, *tex1 = nullptr;          // texture RAM sheets (0x80000 words each)
        uint64_t tex_generation = 0;
    };
    // The last render's, or nullptr when it drew no shadow inputs.
    const ShadowFrame *shadow_frame() const { return shadow_ready_ ? &shadow_frame_ : nullptr; }
    // diagnostic: the frame's cameras (origin x y, focus x y, window: polygons)
    const std::vector<std::pair<std::array<float, 5>, uint32_t>> &shadow_cameras() const { return cameras_; }
    uint32_t shadow_main_camera() const { return main_camera_; }
    bool apply_shadow(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target);

private:
    std::vector<float> *triangles_ = nullptr;
    const std::vector<rt::GeoPoly> *casters_ = nullptr;
    bool shadows_ = false, shadow_ready_ = false;
    SDL_GPUTextureFormat frame_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUGraphicsPipeline *z_pipe_ = nullptr, *z_front_pipe_ = nullptr, *z_quad_pipe_ = nullptr,
                            *shade_pipe_ = nullptr;
    SDL_GPUTexture *view_z_ = nullptr, *light_ = nullptr;
    ShadowFrame shadow_frame_;
    int frame_w_ = 0, frame_h_ = 0;
    std::vector<std::pair<std::array<float, 5>, uint32_t>> cameras_; // origin x, y, focus x, y, window: polygons
    struct ShadowPoly { uint32_t first, count, batch, camera, tri_first, tri_count; bool cutout; }; // per drawn polygon
    struct ZDraw { uint32_t first, count, batch; };                                    // view z pass runs
    std::vector<ShadowPoly> shadow_polys_;
    std::vector<float> cutout_xyz_;        // cut-out triangles, 9 floats each
    std::vector<uint32_t> cutout_records_; // 8 words each: texture info.x, info.y, then u, v of each corner
    std::vector<ZDraw> z_draws_;
    uint32_t main_camera_ = ~0u;
    bool ensure_shadow_targets(int pixel_w, int pixel_h);
    struct PolyVertex {
        float x, y, depth;
        float ooz, uoz, voz; // render_one's p[0..2]: 1/z, u/z, v/z
        float r, g, b, a;
        uint32_t info[4], more[2]; // texture state (m2.hlsl PolyIn)
        // the cars' shading (set_car_shading), each divided by z for a
        // perspective-correct blend: the corner's light (0..255), normal and
        // camera-space position
        float light_oz = 0, normal_oz[3] = {0, 0, 0}, position_oz[3] = {0, 0, 0};
    };
    bool smooth_cars_ = false, car_reflect_ = false, sharp_car_textures_ = false, car_shadows_ = false,
         hide_game_shadow_ = false, smooth_2d_ = false;
    // the shadow map: its pipeline, texture (and a 1 x 1 stand-in), sampler,
    // the casters' vertices after the polygons' in verts_, the fragment uniform
    SDL_GPUGraphicsPipeline *shadow_pipe_ = nullptr;
    SDL_GPUTexture *shadow_map_ = nullptr, *shadow_none_ = nullptr;
    SDL_GPUSampler *shadow_sampler_ = nullptr;
    SDL_GPUTextureFormat shadow_format_ = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    static constexpr uint32_t kShadowMapW = 4096, kShadowMapH = 2048; // two square cascades side by side
    uint32_t shadow_first_ = 0, shadow_near_ = 0, shadow_far_ = 0;     // the casters' vertices, near then far
    std::vector<size_t> casters_now_; // this frame's casting polygons (indices into the scene)
    float car_light_[36] = {};        // m2.hlsl CarLight
    // the real sky the paint reflects (m2.hlsl ps_sky_env): the back layers in
    // a small texture with mip levels (a 1 x 1 stand-in), and the cars' camera
    // in it: axis u, v, focal length / size u, v (0: no car this frame)
    SDL_GPUGraphicsPipeline *sky_env_pipe_ = nullptr;
    SDL_GPUTexture *sky_env_ = nullptr, *sky_none_ = nullptr;
    SDL_GPUSampler *sky_sampler_ = nullptr;
    static constexpr uint32_t kSkyEnvW = 512, kSkyEnvH = 256, kSkyEnvLevels = 6;
    float sky_camera_[4] = {};
    void shadow_casters(const std::vector<rt::GeoPoly> &polys);
    std::array<float, 3> game_light_{0, 0, 1};
    struct CarCorner { float light, normal[3]; };
    std::vector<int32_t> car_first_;    // per polygon: its first corner in car_corners_, -1 if not a car's
    std::vector<CarCorner> car_corners_;
    void shade_cars(const std::vector<rt::GeoPoly> &polys);
    // the display passes: shaders, pipelines (the glow's, the target's per format), glow textures
    SDL_GPUShader *post_vs_ = nullptr, *post_ps_ = nullptr;
    SDL_GPUGraphicsPipeline *post_bright_pipe_ = nullptr, *post_blur_pipe_ = nullptr;
    std::vector<std::pair<SDL_GPUTextureFormat, SDL_GPUGraphicsPipeline *>> post_pipes_;
    SDL_GPUSampler *post_sampler_ = nullptr;
    SDL_GPUTexture *glow_[2] = {nullptr, nullptr};
    int glow_w_ = 0, glow_h_ = 0;
    bool post_failed_ = false;
    bool init_post();
    hires::Pack hires_;
    SDL_GPUBuffer *hires_index_ = nullptr, *hires_data_ = nullptr;
    bool hires_data_uploaded_ = false;
    uint64_t hires_generation_ = ~0ull; // the texture RAM generation hires_index_ was checked against
    bool hires_index_ready_ = false;    // hires_index_ holds that generation's index on the GPU
    hires::Indexer hires_indexer_;      // keeps the index up to date, rehashing what changed
    size_t hires_live_ = 0;
    std::vector<uint32_t> hires_words_;
    void hires_upload(SDL_GPUCommandBuffer *cmd, const rt::VideoMem &mem);
    bool bench_filled_ = false;
    struct QuadVertex {
        float x, y, u, v;
    };
    struct Batch { // consecutive polygons sharing one clip rectangle
        uint32_t first, count;
        SDL_Rect clip;
    };

    SDL_GPUDevice *dev_ = nullptr;
    SDL_GPUGraphicsPipeline *poly_pipe_ = nullptr, *quad_pipe_ = nullptr, *back_pipe_ = nullptr, *front_pipe_ = nullptr,
                            *quad_smooth_pipe_ = nullptr;
    SDL_GPUSampler *sampler_ = nullptr;
    SDL_GPUTexture *front_ = nullptr; // the front layers from the CPU (HUD at the edges)
    SDL_GPUTexture *depth_ = nullptr;
    int depth_w_ = 0, depth_h_ = 0, front_w_ = 0, front_h_ = 0;
    int msaa_ = 1;          // samples a pixel (init)
    SDL_GPUSampleCount samples_ = SDL_GPU_SAMPLECOUNT_1;
    uint32_t aniso_bits_ = 0; // log2 of the anisotropy, in the material's info.x bits 26-28
    // msaa > 1: the frame drawn here, resolved into resolve_, copied into the target
    SDL_GPUTexture *msaa_color_ = nullptr, *resolve_ = nullptr;
    int msaa_w_ = 0, msaa_h_ = 0;
    SDL_GPUBuffer *vbuf_ = nullptr, *qbuf_ = nullptr;
    // storage buffers: texture RAM (both sheets), luma RAM, colour translation with gamma
    SDL_GPUBuffer *texram_ = nullptr, *luma_ = nullptr, *xlat_ = nullptr;
    OpponentBodies opponent_bodies_, player_body_;
    bool have_opponents_ = false, have_player_ = false;
    std::string player_body_dir_;
    std::vector<uint8_t> atlas_cpu_; // the opponents' atlas (or blank), then the player's
    SDL_GPUBuffer *opponent_atlas_ = nullptr;
    bool opponent_atlas_uploaded_ = false;
    uint64_t texram_generation_ = ~0ull; // the texture RAM generation texram_ holds
    // the tilemaps: pixmaps (4 x 512 x 512 u16), header + pens + tile words
    SDL_GPUBuffer *tilepix_ = nullptr, *tiledata_ = nullptr;
    uint64_t tile_instance_ = 0, tile_generation_ = 0; // the Video and its tile generation tilepix_ holds
    uint64_t sky_generation_ = 0; // the sky panorama tilepix_ holds (Video::sky_generation)
    struct Rows { uint32_t first, count; }; // pixmap rows (of pixels) to upload, per layer
    Rows tile_rows_[4];
    std::vector<uint32_t> tiledata_words_;
    std::vector<uint32_t> xlat_table_;
    uint32_t vbuf_size_ = 0;
    SDL_GPUTransferBuffer *upload_ = nullptr;
    uint32_t upload_size_ = 0;
    std::vector<PolyVertex> verts_;
    std::vector<Batch> batches_;
    std::vector<size_t> order_;
    uint8_t gamma_[256];
    std::string error_;

    SDL_GPUShader *shader(const unsigned char *spv, size_t spv_len, const unsigned char *dxil, size_t dxil_len,
                          const char *msl, const char *dxil_entry, SDL_GPUShaderStage stage, int uniforms, int samplers,
                          int storage_buffers = 0);
    bool ensure(int w, int h, int pixel_w, int pixel_h, uint32_t vert_bytes);
    void build(const rt::Video &video, int w, int h, int scale, bool sharp_textures,
               const std::vector<rt::GeoPoly> &polys);
};

} // namespace app
